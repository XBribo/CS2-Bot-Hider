// plugin.cpp
// All reverse-engineered constants (offsets, vtable slots, schema candidates) live in version_targets.h

#include "plugin.h"
#include "bot_info.h"
#include "personas.h"
#include "fake_client_manager.h"
#include "ping_display.h"
#include "serversideclient_ref.h"
#include "slot_publisher.h"
#include "version_targets.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>

#include <Windows.h>

#include <iserver.h>
#include <eiface.h>
#include <tier1/utlvector.h>
#include <tier1/convar.h>

#if !defined(_WIN32) || !defined(_WIN64)
#error "CS2-Bot-Hider is Windows x64 only."
#endif

SH_DECL_HOOK6_void(IServerGameClients, OnClientConnected, SH_NOATTRIB, 0,
                   CPlayerSlot, const char *, uint64, const char *, const char *, bool);
SH_DECL_HOOK4_void(IServerGameClients, ClientPutInServer, SH_NOATTRIB, 0,
                   CPlayerSlot, char const *, int, uint64);
SH_DECL_HOOK1(IVEngineServer, CreateFakeClient, SH_NOATTRIB, 0, CPlayerSlot, const char *);
SH_DECL_HOOK3(INetworkGameServer, StartChangeLevel, SH_NOATTRIB, 0,
              CUtlVector<INetworkGameClient *> *, const char *, const char *, void *);
SH_DECL_HOOK3_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool, bool, bool);

namespace cs2bh
{

    HiderPlugin g_Plugin;

} // namespace cs2bh

PLUGIN_EXPOSE(cs2bh::HiderPlugin, cs2bh::g_Plugin);

// Interface globals — populated in Load()

IVEngineServer *engine = nullptr;
ICvar *icvar = nullptr;
IServerGameClients *gameclients = nullptr;
IServerGameDLL *server = nullptr;
extern INetworkServerService *g_pNetworkServerService;

namespace cs2bh
{

    static std::set<std::string> g_ExpectedNames;

    // Resolve CServerSideClient* for a slot via the CUtlVector at +592
    static void *ResolveClientBySlot(int slot)
    {
        if (!g_pNetworkServerService)
            return nullptr;
        auto *gs = g_pNetworkServerService->GetIGameServer();
        if (!gs)
            return nullptr;
        auto *vec = reinterpret_cast<CUtlVector<void *> *>(
            reinterpret_cast<unsigned char *>(gs) + targets::kClientListOffset);
        int count = vec->Count();
        if (count < 0 || count > 256 || slot < 0 || slot >= count)
            return nullptr;
        return vec->Element(slot);
    }

    // Engine-side m_Name overwrite via CUtlString::Set
    static const char *OverwriteEngineName(HiderPlugin *plugin, void *pClient, const char *newName)
    {
        if (!plugin->m_pUtlStringSet || !pClient || !newName || !newName[0])
            return nullptr;
        void *pUtlString =
            reinterpret_cast<unsigned char *>(pClient) + ssc::OFFSET_m_Name;
        plugin->m_pUtlStringSet(pUtlString, newName);
        return *reinterpret_cast<const char **>(pUtlString);
    }

    // Schema field writes
    // TODO: replace with SchemaSystemTypeScope::FindDeclaredClass

    static void WriteControllerPing(void *controller, uint32_t ping)
    {
        if (!controller)
            return;
        auto *raw = reinterpret_cast<unsigned char *>(controller);
        *reinterpret_cast<uint32_t *>(raw + targets::kSchemaFallback_m_iPing) = ping;
        // TODO: call CCSPlayerController::NetworkStateChanged(offset, -1, -1)
    }

    // Stamp a string_t (pooled name) for m_iszPlayerName
    // For v0.1.0 we leave this write disabled and rely on CServerSideClient::m_Name instead
    static void WriteControllerPlayerName(void * /*controller*/, const char * /*name*/)
    {
        // intentional no-op for v0.1.0
    }

    // Stamp synthetic SteamID64 into the controller
    static void WriteControllerSteamId(void * /*controller*/, uint64_t /*sid64*/)
    {
        // intentional no-op for v0.1.0
    }

    // Walk EntitySystem for the controller pointer
    // For v0.1.0 we use pszName + slot bookkeeping only
    static void *ResolveControllerBySlot(int /*slot*/)
    {
        return nullptr;
    }

    // Hook bodies

    void HiderPlugin::Hook_OnClientConnected_Post(CPlayerSlot slot, const char *pszName, uint64 xuid,
                                                  const char * /*pszNetworkID*/, const char * /*pszAddress*/,
                                                  bool bFakePlayer)
    {
        if (m_bSelfDisabled)
            RETURN_META(MRES_IGNORED);
        if (!bFakePlayer)
            RETURN_META(MRES_IGNORED);
        int idx = slot.Get();
        if (idx < 0 || idx >= PersonaPool::kMaxSlots)
            RETURN_META(MRES_IGNORED);

        // Match incoming pszName against expected_set populated by CFC PRE
        bool managed = false;
        if (pszName && pszName[0])
        {
            auto it = g_ExpectedNames.find(pszName);
            if (it != g_ExpectedNames.end())
            {
                managed = true;
                Personas().MarkSlotManaged(idx, pszName);
                g_ExpectedNames.erase(it);
            }
            else if (Personas().IsSlotManaged(idx))
            {
                managed = true;
            }
        }
        if (!managed)
            RETURN_META(MRES_IGNORED);

        // Resolve the bot_info.json config for this persona
        auto *cfg = BotInfo().FindByName(pszName);
        uint64_t cfgSid = (cfg && cfg->SteamId64 != 0) ? cfg->SteamId64 : 0;
        const char *cfgCross = cfg ? cfg->CrosshairCode.c_str() : nullptr;

        Manager().AdoptSlot(idx, pszName, cfgSid, cfgCross);

        void *pClient = ResolveClientBySlot(idx);
        if (!pClient)
        {
            META_CONPRINTF("[BOTHIDER] error: ResolveClientBySlot null slot=%d\n", idx);
            RETURN_META(MRES_IGNORED);
        }

        // Flip m_bFakePlayer
        auto *raw = reinterpret_cast<unsigned char *>(pClient);
        if (ssc::IsFakePlayerSet(pClient))
        {
            ssc::ClearFakePlayer(pClient);
        }

        // Write real SteamID64 into CServerSideClient.m_SteamID
        if (cfgSid != 0)
        {
            *reinterpret_cast<uint64_t *>(raw + ssc::OFFSET_m_SteamID) = cfgSid;
            META_CONPRINTF("[BOTHIDER] slot=%d steamid64=%llu written at +%d\n",
                           idx, (unsigned long long)cfgSid, ssc::OFFSET_m_SteamID);
        }
        else
        {
            META_CONPRINTF("[BOTHIDER] slot=%d name='%s' not in bot_info.json — no steamid write\n",
                           idx, pszName ? pszName : "<null>");
        }

        META_CONPRINTF("[BOTHIDER] OCC done slot=%d name='%s'\n", idx, pszName ? pszName : "<null>");
        RETURN_META(MRES_IGNORED);
    }

    void HiderPlugin::Hook_ClientPutInServer_Post(CPlayerSlot slot, char const *pszName,
                                                  int type, uint64 /*xuid*/)
    {
        if (m_bSelfDisabled)
            RETURN_META(MRES_IGNORED);
        if (type != 1)
            RETURN_META(MRES_IGNORED);
        int idx = slot.Get();
        if (idx < 0 || idx >= PersonaPool::kMaxSlots)
            RETURN_META(MRES_IGNORED);
        if (!Personas().IsSlotManaged(idx))
            RETURN_META(MRES_IGNORED);

        void *pClient = ResolveClientBySlot(idx);
        if (!pClient)
            RETURN_META(MRES_IGNORED);

        // Re-flip byte 160 + re-write SteamID
        if (ssc::IsFakePlayerSet(pClient))
        {
            ssc::ClearFakePlayer(pClient);
        }
        auto *raw = reinterpret_cast<unsigned char *>(pClient);
        auto *entry = BotInfo().FindByName(pszName);
        if (entry && entry->SteamId64 != 0)
        {
            *reinterpret_cast<uint64_t *>(raw + ssc::OFFSET_m_SteamID) = entry->SteamId64;
        }
        META_CONPRINTF("[BOTHIDER] CPiS safety-net slot=%d name='%s'\n", idx, pszName ? pszName : "<null>");
        RETURN_META(MRES_IGNORED);
    }

    // Replace the name with the next name from bot_info.json
    CPlayerSlot HiderPlugin::Hook_CreateFakeClient_Pre(const char *netname)
    {
        if (m_bSelfDisabled)
        {
            RETURN_META_VALUE(MRES_IGNORED, CPlayerSlot(-1));
        }

        std::string persona;

        // Pick a bot_info.json config
        auto *entry = BotInfo().PickForBot(netname);
        if (entry)
        {
            persona = entry->Name;
        }
        else
        {
            persona = Personas().PickFromRoster();
        }
        if (persona.empty())
        {
            RETURN_META_VALUE(MRES_IGNORED, CPlayerSlot(-1));
        }

        g_ExpectedNames.insert(persona);
        static thread_local std::string s_PersonaBuffer;
        s_PersonaBuffer = persona;

        META_CONPRINTF("[BOTHIDER] CFC PRE override '%s' -> '%s'\n",
                       netname ? netname : "<null>", s_PersonaBuffer.c_str());
        const char *personaCStr = s_PersonaBuffer.c_str();
        RETURN_META_VALUE_NEWPARAMS(MRES_HANDLED, CPlayerSlot(-1),
                                    &IVEngineServer::CreateFakeClient, (personaCStr));
    }

    CUtlVector<INetworkGameClient *> *HiderPlugin::Hook_StartChangeLevel_Pre(
        const char *mapName, const char *landmark, void * /*changelevelState*/)
    {
        if (m_bSelfDisabled)
        {
            RETURN_META_VALUE(MRES_IGNORED, nullptr);
        }
        g_ExpectedNames.clear();
        Manager().ReleaseAll();
        BotInfo().ResetAssignments();
        META_CONPRINTF("[BOTHIDER] StartChangeLevel PRE — map='%s' landmark='%s'\n",
                       mapName ? mapName : "?", landmark ? landmark : "");
        RETURN_META_VALUE(MRES_IGNORED, nullptr);
    }

    // Tick driver
    void HiderPlugin::Hook_GameFrame_Post(bool simulating, bool /*bFirst*/, bool /*bLast*/)
    {
        if (m_bSelfDisabled || !simulating)
            RETURN_META(MRES_IGNORED);
        Manager().OnTick();

        // Drain CSS -> C++ write commands posted via shared memory.
        Publisher().DrainCommands(
            // SET_SID: write SteamID64 into CServerSideClient.m_SteamID
            [this](int slot, uint64_t sid)
            {
                if (!Manager().IsManaged(slot))
                    return;
                void *pClient = ResolveClientBySlot(slot);
                if (!pClient)
                    return;
                *reinterpret_cast<uint64_t *>(
                    reinterpret_cast<unsigned char *>(pClient) + ssc::OFFSET_m_SteamID) = sid;
                Manager().SetSyntheticSid(slot, sid);
                Publisher().UpdateSyntheticSid(slot, sid);
            },
            // SET_PERSONA: overwrite engine-side m_Name via CUtlString::Set
            [this](int slot, const char *name)
            {
                if (!Manager().IsManaged(slot) || !name || !name[0])
                    return;
                void *pClient = ResolveClientBySlot(slot);
                if (!pClient)
                    return;
                OverwriteEngineName(this, pClient, name);
                Personas().MarkSlotManaged(slot, name);
                Publisher().UpdatePersonaName(slot, name);
            });
        RETURN_META(MRES_IGNORED);
    }

    void HiderPlugin::OnLevelInit(char const *pMapName, char const *, char const *,
                                  char const *, bool, bool)
    {
        auto *gameServer = g_pNetworkServerService
                               ? g_pNetworkServerService->GetIGameServer()
                               : nullptr;
        if (gameServer && gameServer != m_pHookedGameServer)
        {
            if (m_StartChangeLevelHookId != 0)
            {
                SH_REMOVE_HOOK_ID(m_StartChangeLevelHookId);
                m_StartChangeLevelHookId = 0;
            }
            m_StartChangeLevelHookId = SH_ADD_HOOK_MEMFUNC(
                INetworkGameServer, StartChangeLevel, gameServer,
                this, &HiderPlugin::Hook_StartChangeLevel_Pre, false /* PRE */);
            m_pHookedGameServer = static_cast<void *>(gameServer);
            META_CONPRINTF("[BOTHIDER] StartChangeLevel hook attached to %p (id %d)\n",
                           static_cast<void *>(gameServer), m_StartChangeLevelHookId);
        }
        META_CONPRINTF("[BOTHIDER] OnLevelInit map=%s\n", pMapName ? pMapName : "?");
    }

    void HiderPlugin::OnLevelShutdown()
    {
        g_ExpectedNames.clear();
        Manager().ReleaseAll();
        BotInfo().ResetAssignments();
        META_CONPRINTF("[BOTHIDER] OnLevelShutdown — state drained\n");
    }

    bool HiderPlugin::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool /*late*/)
    {
        PLUGIN_SAVEVARS();

        GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
        GET_V_IFACE_CURRENT(GetEngineFactory, icvar, ICvar, CVAR_INTERFACE_VERSION);
        GET_V_IFACE_ANY(GetServerFactory, gameclients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS);
        GET_V_IFACE_ANY(GetServerFactory, server, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);
        GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkServerService, INetworkServerService,
                        NETWORKSERVERSERVICE_INTERFACE_VERSION);

        g_pCVar = icvar;
        g_SMAPI->AddListener(this, this);

        // Resolve CUtlString::Set from tier0.dll
        HMODULE tier0 = GetModuleHandleA("tier0.dll");
        if (tier0)
        {
            m_pUtlStringSet = reinterpret_cast<CUtlStringSetFn>(
                GetProcAddress(tier0, targets::kSym_CUtlString_Set));
        }
        if (!m_pUtlStringSet)
        {
            META_CONPRINTF("[BOTHIDER] warning: CUtlString::Set unresolved — name overwrite disabled\n");
        }
        else
        {
            META_CONPRINTF("[BOTHIDER] CUtlString::Set resolved at %p\n",
                           reinterpret_cast<void *>(m_pUtlStringSet));
        }

        Manager().Init();

        // Open the shared-memory bridge for the C# CSS plugin
        if (Publisher().Init())
        {
            META_CONPRINTF("[BOTHIDER] shared memory '%s' mapped\n", shm::kMappingName);
        }
        else
        {
            META_CONPRINTF("[BOTHIDER] warning: shared memory init failed — CSS bridge disabled\n");
        }

        // Load bot identity data from JSON config
        std::string jsonPath = g_SMAPI->GetBaseDir();
        jsonPath += "/addons/BotHider/bot_info.json";
        META_CONPRINTF("[BOTHIDER] loading JSON from: %s\n", jsonPath.c_str());
        if (BotInfo().Load(jsonPath.c_str()))
        {
            META_CONPRINTF("[BOTHIDER] bot_info.json loaded — %zu entries\n", BotInfo().Count());
        }
        else
        {
            META_CONPRINTF("[BOTHIDER] warning: bot_info.json not found or parse error at '%s' — "
                           "CFC PRE will fall back to curated roster\n",
                           jsonPath.c_str());
        }

        SH_ADD_HOOK(IServerGameClients, OnClientConnected, gameclients,
                    SH_MEMBER(this, &HiderPlugin::Hook_OnClientConnected_Post), true);
        SH_ADD_HOOK(IServerGameClients, ClientPutInServer, gameclients,
                    SH_MEMBER(this, &HiderPlugin::Hook_ClientPutInServer_Post), true);
        SH_ADD_HOOK(IVEngineServer, CreateFakeClient, engine,
                    SH_MEMBER(this, &HiderPlugin::Hook_CreateFakeClient_Pre), false);
        SH_ADD_HOOK(IServerGameDLL, GameFrame, server,
                    SH_MEMBER(this, &HiderPlugin::Hook_GameFrame_Post), true);

        META_CONPRINTF("[BOTHIDER] loaded — m_bFakePlayer offset=%d, OCC=#%d CPiS=#%d\n",
                       ssc::OFFSET_m_bFakePlayer,
                       targets::kVTSlot_OnClientConnected,
                       targets::kVTSlot_ClientPutInServer);
        return true;
    }

    bool HiderPlugin::Unload(char * /*error*/, size_t /*maxlen*/)
    {
        SH_REMOVE_HOOK(IServerGameClients, OnClientConnected, gameclients,
                       SH_MEMBER(this, &HiderPlugin::Hook_OnClientConnected_Post), true);
        SH_REMOVE_HOOK(IServerGameClients, ClientPutInServer, gameclients,
                       SH_MEMBER(this, &HiderPlugin::Hook_ClientPutInServer_Post), true);
        SH_REMOVE_HOOK(IVEngineServer, CreateFakeClient, engine,
                       SH_MEMBER(this, &HiderPlugin::Hook_CreateFakeClient_Pre), false);
        SH_REMOVE_HOOK(IServerGameDLL, GameFrame, server,
                       SH_MEMBER(this, &HiderPlugin::Hook_GameFrame_Post), true);

        if (m_StartChangeLevelHookId != 0)
        {
            SH_REMOVE_HOOK_ID(m_StartChangeLevelHookId);
            m_StartChangeLevelHookId = 0;
        }
        m_pHookedGameServer = nullptr;
        Manager().ReleaseAll();
        Publisher().Shutdown();
        return true;
    }

} // namespace cs2bh
