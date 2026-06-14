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
#include "sig_scan.h"
#include "schema_resolver.h"
#include "inline_hook.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <array>
#include <unordered_map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <dlfcn.h>
#include <strings.h>
#endif

#include <iserver.h>
#include <eiface.h>
#include <tier1/utlvector.h>
#include <tier1/convar.h>

SH_DECL_HOOK6_void(IServerGameClients, OnClientConnected, SH_NOATTRIB, 0,
                   CPlayerSlot, const char *, uint64, const char *, const char *, bool);
SH_DECL_HOOK4_void(IServerGameClients, ClientPutInServer, SH_NOATTRIB, 0,
                   CPlayerSlot, char const *, int, uint64);
SH_DECL_HOOK5_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0,
                   CPlayerSlot, ENetworkDisconnectionReason, const char *, uint64, const char *);
SH_DECL_HOOK1(IVEngineServer, CreateFakeClient, SH_NOATTRIB, 0, CPlayerSlot, const char *);
SH_DECL_HOOK3(INetworkGameServer, StartChangeLevel, SH_NOATTRIB, 0,
              CUtlVector<INetworkGameClient *> *, const char *, const char *, void *);
SH_DECL_HOOK3_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool, bool, bool);
SH_DECL_HOOK3_void(ICvar, DispatchConCommand, SH_NOATTRIB, 0,
                   ConCommandRef, const CCommandContext &, const CCommand &);

namespace cs2bh
{

    HiderPlugin g_Plugin;

} // namespace cs2bh

PLUGIN_EXPOSE(cs2bh::HiderPlugin, cs2bh::g_Plugin);

// Interface globals

IVEngineServer *engine = nullptr;
ICvar *icvar = nullptr;
IServerGameClients *gameclients = nullptr;
IServerGameDLL *server = nullptr;
extern INetworkServerService *g_pNetworkServerService;

// GameResourceServiceServerV001
static void *g_pGameResourceService = nullptr;

// * UTIL_Remove(CEntityInstance*)
// Used to destroy the CCSPlayerController a kicked bot leaves behind

#if defined(_WIN32)
using UtilRemoveFn = void(__fastcall *)(void * /*CEntityInstance*/);
#else
using UtilRemoveFn = void (*)(void * /*CEntityInstance*/);
#endif
static UtilRemoveFn g_pfnUtilRemove = nullptr;

// Cross-check anchor: address of the CGameEntitySystem singleton global that UTIL_Remove references.
static void **g_ppEntSysGlobal = nullptr;

// * FIX: inline detour on SV_KickOneFromTeam
#if defined(_WIN32)
using KickOneFn = char(__fastcall *)(int /*team*/);
#else
using KickOneFn = char (*)(int /*team*/);
#endif
static cs2bh::hook::InlineHook g_KickHook;
static KickOneFn g_pfnKickOneTramp = nullptr;

namespace cs2bh
{
    bool ServerHasHltvClient();
}

// Detour
#if defined(_WIN32)
static char __fastcall Detour_KickOneFromTeam(int team)
#else
static char Detour_KickOneFromTeam(int team)
#endif
{
    // Phantom kick: Windows callers expect success here. Linux must keep the
    // caller's fallback pruning path alive, otherwise no real slot is freed.
    if (team == 0 && cs2bh::ServerHasHltvClient())
#if defined(_WIN32)
        return 1;
#else
        return 0;
#endif
    return g_pfnKickOneTramp ? g_pfnKickOneTramp(team) : 0;
}

namespace cs2bh
{

    // Maps an expected fake-client name to the bot_info entry chosen for it at CreateFakeClient time
    static std::unordered_map<std::string, const BotEntry *> g_ExpectedNames;

    // Per-slot bound bot_info entry
    static std::array<const BotEntry *, PersonaPool::kMaxSlots> g_SlotEntry{};

    // Maps where bots stay disguised, loaded from map_whitelist.json
    static std::vector<std::string> g_DisguiseWhitelist;

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

    // Count online human clients
    static int CountHumanClients()
    {
        if (!g_pNetworkServerService)
            return 0;
        auto *gs = g_pNetworkServerService->GetIGameServer();
        if (!gs)
            return 0;
        auto *vec = reinterpret_cast<CUtlVector<void *> *>(
            reinterpret_cast<unsigned char *>(gs) + targets::kClientListOffset);
        int count = vec->Count();
        if (count < 0 || count > 256)
            return 0;
        int humans = 0;
        for (int i = 0; i < count; ++i)
        {
            void *pClient = vec->Element(i);
            if (!pClient)
                continue;
            void *netChan = *reinterpret_cast<void **>(
                reinterpret_cast<unsigned char *>(pClient) + ssc::OFFSET_m_NetChannel);
            if (netChan)
                ++humans;
        }
        return humans;
    }

    // True if any connected client is the SourceTV/HLTV client
    bool ServerHasHltvClient()
    {
        if (!g_pNetworkServerService)
            return false;
        auto *gs = g_pNetworkServerService->GetIGameServer();
        if (!gs)
            return false;
#if defined(_WIN32)
        __try
        {
#endif
            auto *vec = reinterpret_cast<CUtlVector<void *> *>(
                reinterpret_cast<unsigned char *>(gs) + targets::kClientListOffset);
            int count = vec->Count();
            if (count < 0 || count > 256)
                return false;
            for (int i = 0; i < count; ++i)
            {
                void *pClient = vec->Element(i);
                if (!pClient)
                    continue;
                if (*reinterpret_cast<unsigned char *>(
                        reinterpret_cast<unsigned char *>(pClient) + ssc::OFFSET_m_bIsHLTV))
                    return true;
            }
            return false;
#if defined(_WIN32)
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#endif
    }

    // True if sid is already live on any connected client other than exceptSlot
    static bool IsSteamIdInUseByOther(uint64_t sid, int exceptSlot)
    {
        if (sid == 0 || !g_pNetworkServerService)
            return false;
        auto *gs = g_pNetworkServerService->GetIGameServer();
        if (!gs)
            return false;
        auto *vec = reinterpret_cast<CUtlVector<void *> *>(
            reinterpret_cast<unsigned char *>(gs) + targets::kClientListOffset);
        int count = vec->Count();
        if (count < 0 || count > 256)
            return false;
        for (int i = 0; i < count; ++i)
        {
            if (i == exceptSlot)
                continue;
            void *pClient = vec->Element(i);
            if (!pClient)
                continue;
            uint64_t other = *reinterpret_cast<uint64_t *>(
                reinterpret_cast<unsigned char *>(pClient) + ssc::OFFSET_m_SteamID);
            if (other == sid)
                return true;
        }
        return false;
    }

    // Resolve a SteamID for slot that collides with other client
    static uint64_t MakeUniqueSteamId(int slot, uint64_t desired)
    {
        if (desired != 0 && !IsSteamIdInUseByOther(desired, slot))
            return desired;

        // Scan bot_info entries for a non-colliding SteamID64
        for (const auto &e : BotInfo().All())
        {
            if (e.SteamId64 != 0 && !IsSteamIdInUseByOther(e.SteamId64, slot))
                return e.SteamId64;
        }

        // Bump the AccountId off a base until it is free
        uint64_t base = desired != 0 ? desired : BotInfoStore::kSteamId64Base + 1;
        for (int bump = 1; bump <= 4096; ++bump)
        {
            uint64_t candidate = base + static_cast<uint64_t>(bump);
            if (!IsSteamIdInUseByOther(candidate, slot))
                return candidate;
        }
        return desired; // give up
    }

    // Resolve UTIL_Remove from the server module
    static void ResolveUtilRemoveAndEntSys(const nlohmann::json &gamedata, const sig::ModuleInfo &serverModule)
    {
        if (!serverModule)
        {
            META_CONPRINTF("[BOTHIDER] warning: %s module unresolved for signature scan\n",
                           targets::kServerModuleName);
            return;
        }
        std::string sigStr = sig::FindPlatformSig(gamedata, "UTIL_Remove");
        std::vector<uint8_t> bytes;
        std::vector<bool> wild;
        if (sigStr.empty() || !sig::ParseSigString(sigStr, bytes, wild))
        {
            META_CONPRINTF("[BOTHIDER] warning: UTIL_Remove %s sig missing/malformed in gamedata.json\n",
                           sig::PlatformName());
            return;
        }
        auto *hit = static_cast<unsigned char *>(sig::FindPatternIn(serverModule, bytes, wild));
        if (!hit)
            return;
        g_pfnUtilRemove = reinterpret_cast<UtilRemoveFn>(hit);

        // Windows: mov rcx, [rip+disp32]
        // Linux:   lea rax, [rip+disp32]; mov rdi, [rax]
        for (size_t i = 0; i + 7 <= bytes.size(); ++i)
        {
            bool isWindowsMov = bytes[i] == 0x48 && bytes[i + 1] == 0x8B && bytes[i + 2] == 0x0D;
            bool isLinuxLea = bytes[i] == 0x48 && bytes[i + 1] == 0x8D && bytes[i + 2] == 0x05;
            if (isWindowsMov || isLinuxLea)
            {
                unsigned char *dispAt = hit + i + 3; // first byte of disp32
                int32_t disp = *reinterpret_cast<int32_t *>(dispAt);
                unsigned char *instrEnd = dispAt + 4; // RIP points past the instr
                g_ppEntSysGlobal = reinterpret_cast<void **>(instrEnd + disp);
                break;
            }
        }
    }

    // Override member offsets from gamedata.json; missing entries keep their fallback
    static void LoadMemberOffsets(const nlohmann::json &gamedata)
    {
        using sig::FindPlatformOffset;
        // CServerSideClient layout — shifts as a block across game updates
        ssc::OFFSET_m_UserIDString = FindPlatformOffset(gamedata, "CServerSideClient::m_UserIDString", ssc::OFFSET_m_UserIDString);
        ssc::OFFSET_m_Name = FindPlatformOffset(gamedata, "CServerSideClient::m_Name", ssc::OFFSET_m_Name);
        ssc::OFFSET_m_nClientSlot = FindPlatformOffset(gamedata, "CServerSideClient::m_nClientSlot", ssc::OFFSET_m_nClientSlot);
        ssc::OFFSET_m_nEntityIndex = FindPlatformOffset(gamedata, "CServerSideClient::m_nEntityIndex", ssc::OFFSET_m_nEntityIndex);
        ssc::OFFSET_m_Server = FindPlatformOffset(gamedata, "CServerSideClient::m_Server", ssc::OFFSET_m_Server);
        ssc::OFFSET_m_NetChannel = FindPlatformOffset(gamedata, "CServerSideClient::m_NetChannel", ssc::OFFSET_m_NetChannel);
        ssc::OFFSET_m_nSignonState = FindPlatformOffset(gamedata, "CServerSideClient::m_nSignonState", ssc::OFFSET_m_nSignonState);
        ssc::OFFSET_m_pAttachedTo = FindPlatformOffset(gamedata, "CServerSideClient::m_pAttachedTo", ssc::OFFSET_m_pAttachedTo);
        ssc::OFFSET_m_bFakePlayer = FindPlatformOffset(gamedata, "CServerSideClient::m_bFakePlayer", ssc::OFFSET_m_bFakePlayer);
        ssc::OFFSET_m_UserID = FindPlatformOffset(gamedata, "CServerSideClient::m_UserID", ssc::OFFSET_m_UserID);
        ssc::OFFSET_m_SteamID = FindPlatformOffset(gamedata, "CServerSideClient::m_SteamID", ssc::OFFSET_m_SteamID);
        ssc::OFFSET_m_bIsHLTV = FindPlatformOffset(gamedata, "CServerSideClient::m_bIsHLTV", ssc::OFFSET_m_bIsHLTV);
        // CNetworkGameServerBase::m_Clients vector base
        targets::kClientListOffset = FindPlatformOffset(gamedata, "CNetworkGameServerBase::m_Clients", targets::kClientListOffset);
    }

    // Resolve SV_KickOneFromTeam by sig and install the team-0 kick-guard detour
    static void InstallKickGuardHook(const nlohmann::json &gamedata, const sig::ModuleInfo &serverModule)
    {
        if (!serverModule)
            return;
        std::string sigStr = sig::FindPlatformSig(gamedata, "SV_KickOneFromTeam");
        std::vector<uint8_t> bytes;
        std::vector<bool> wild;
        if (sigStr.empty() || !sig::ParseSigString(sigStr, bytes, wild))
        {
            META_CONPRINTF("[BOTHIDER] warning: SV_KickOneFromTeam sig missing — GOTV kick-guard disabled\n");
            return;
        }
        void *target = sig::FindPatternIn(serverModule, bytes, wild);
        if (!target)
        {
            META_CONPRINTF("[BOTHIDER] warning: SV_KickOneFromTeam sig not found — GOTV kick-guard disabled\n");
            return;
        }
#if defined(_WIN32)
        constexpr size_t kStealLen = 19;
#else
        // Linux gamedata points at the function body after the RIP-relative precheck.
        constexpr size_t kStealLen = 15;
#endif
        if (!g_KickHook.Install(target, reinterpret_cast<void *>(&Detour_KickOneFromTeam), kStealLen))
        {
            META_CONPRINTF("[BOTHIDER] warning: SV_KickOneFromTeam detour install failed — GOTV kick-guard disabled\n");
            return;
        }
        g_pfnKickOneTramp = reinterpret_cast<KickOneFn>(g_KickHook.Trampoline());
        META_CONPRINTF("[BOTHIDER] GOTV kick-guard installed at %p\n", target);
    }

    // SEH-isolated single reads, used by the controller-resolution walk
    static bool SehReadPtr(const void *addr, void **out)
    {
#if defined(_WIN32)
        __try
        {
            *out = *reinterpret_cast<void *const *>(addr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *out = nullptr;
            return false;
        }
#else
        if (!addr)
        {
            *out = nullptr;
            return false;
        }
        *out = *reinterpret_cast<void *const *>(addr);
        return true;
#endif
    }
    // Treat addr as a char** , deref then copy
    static bool SehReadCStr(const void *addr, char *out, size_t cap)
    {
#if defined(_WIN32)
        __try
        {
            const char *p = *reinterpret_cast<const char *const *>(addr);
            if (!p)
            {
                out[0] = '\0';
                return false;
            }
            size_t i = 0;
            for (; i + 1 < cap && p[i]; ++i)
                out[i] = p[i];
            out[i] = '\0';
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out[0] = '\0';
            return false;
        }
#else
        if (!addr)
        {
            out[0] = '\0';
            return false;
        }
        const char *p = *reinterpret_cast<const char *const *>(addr);
        if (!p)
        {
            out[0] = '\0';
            return false;
        }
        size_t i = 0;
        for (; i + 1 < cap && p[i]; ++i)
            out[i] = p[i];
        out[i] = '\0';
        return true;
#endif
    }

    // Resolve a CEntityInstance* by entity index
    static void *ResolveEntityInstance(int entityIndex, char *classnameOut, size_t classnameCap)
    {
        if (classnameOut && classnameCap)
            classnameOut[0] = '\0';
        if (!g_pGameResourceService || entityIndex <= 0 || entityIndex >= 0x8000)
            return nullptr;

        void *entSys = nullptr;
        if (!SehReadPtr(reinterpret_cast<unsigned char *>(g_pGameResourceService) +
                            targets::kEntSys_OffsetInGameResSvc,
                        &entSys) ||
            !entSys)
            return nullptr;

        void *chunk = nullptr;
        const void *chunkSlot = reinterpret_cast<unsigned char *>(entSys) +
                                targets::kEntSys_IdentityChunksOffset +
                                (entityIndex / targets::kEntListChunkSize) * sizeof(void *);
        if (!SehReadPtr(chunkSlot, &chunk) || !chunk)
            return nullptr;

        unsigned char *identity = reinterpret_cast<unsigned char *>(chunk) +
                                  (entityIndex % targets::kEntListChunkSize) * targets::kEntIdentity_Size;
        if (classnameOut && classnameCap)
            SehReadCStr(identity + 0x20, classnameOut, classnameCap);

        void *instance = nullptr;
        if (!SehReadPtr(identity + targets::kEntIdentity_InstanceOffset, &instance) || !instance)
            return nullptr;
        return instance;
    }

    // * Destroy the CCSPlayerController a kicked bot leaves behind
    // Returns true if the destroy was dispatched
    static bool DestroyControllerForClient(void *pClient)
    {
        if (!pClient)
            return false;
        if (!g_pfnUtilRemove)
        {
            META_CONPRINTF("[BOTHIDER] destroy ABORT: UTIL_Remove unresolved (signature scan failed at Load)\n");
            return false;
        }
        int entIdx = *reinterpret_cast<int *>(
            reinterpret_cast<unsigned char *>(pClient) + ssc::OFFSET_m_nEntityIndex);
        char cls[64];
        void *inst = ResolveEntityInstance(entIdx, cls, sizeof(cls));
        if (!inst)
        {
            // Resolution chain returned null
            META_CONPRINTF("[BOTHIDER] destroy ABORT: entity resolve failed "
                           "entIdx=%d cls='%s' grs=%p (check kEntSys_* offsets)\n",
                           entIdx, cls, g_pGameResourceService);
            return false;
        }
        // Only ever destroy a player controller — never collateral entities
        if (std::strcmp(cls, "cs_player_controller") != 0)
        {
            META_CONPRINTF("[BOTHIDER] destroy skipped entIdx=%d cls='%s' (not a controller)\n",
                           entIdx, cls);
            return false;
        }

        // ? One-time cross-check: prove our GameResourceService+0x58 chain resolves
        // the SAME CGameEntitySystem that UTIL_Remove uses
        static bool s_crossChecked = false;
        if (!s_crossChecked && g_ppEntSysGlobal)
        {
            void *entSysFromChain = nullptr;
            SehReadPtr(reinterpret_cast<unsigned char *>(g_pGameResourceService) +
                           targets::kEntSys_OffsetInGameResSvc,
                       &entSysFromChain);
            void *entSysFromRemove = nullptr;
            SehReadPtr(g_ppEntSysGlobal, &entSysFromRemove);
            META_CONPRINTF("[BOTHIDER] entSys cross-check: chain=%p remove=%p %s\n",
                           entSysFromChain, entSysFromRemove,
                           (entSysFromChain == entSysFromRemove) ? "MATCH" : "MISMATCH");
            s_crossChecked = true;
        }

        g_pfnUtilRemove(inst);
        META_CONPRINTF("[BOTHIDER] destroy dispatched entIdx=%d inst=%p cls='%s'\n",
                       entIdx, inst, cls);
        return true;
    }

    // Reset a disguised bot's idle timer
    static void ResetIdleTimerForClient(void *pClient)
    {
        if (!pClient)
            return;
        // controller offsets
        int pawnOff = schema::GetFieldOffset("CBasePlayerController", "m_hPawn");
        int idleOff = schema::GetFieldOffset("CCSPlayerPawnBase", "m_flIdleTimeSinceLastAction");
        if (pawnOff < 0 || idleOff < 0)
            return;

        int entIdx = *reinterpret_cast<int *>(
            reinterpret_cast<unsigned char *>(pClient) + ssc::OFFSET_m_nEntityIndex);
        char cls[64];
        void *controller = ResolveEntityInstance(entIdx, cls, sizeof(cls));
        if (!controller || std::strcmp(cls, "cs_player_controller") != 0)
            return;

        // m_hPawn is a CHandle; low 15 bits are the pawn entity index
        uint32_t hPawn = *reinterpret_cast<uint32_t *>(
            reinterpret_cast<unsigned char *>(controller) + pawnOff);
        if (hPawn == 0xFFFFFFFF)
            return;
        int pawnIdx = static_cast<int>(hPawn & 0x7FFF);
        void *pawn = ResolveEntityInstance(pawnIdx, nullptr, 0);
        if (!pawn)
            return;

        *reinterpret_cast<float *>(
            reinterpret_cast<unsigned char *>(pawn) + idleOff) = 0.0f;
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
    // For v0.2.x we leave this write disabled and rely on CServerSideClient::m_Name instead
    static void WriteControllerPlayerName(void * /*controller*/, const char * /*name*/)
    {
        // intentional no-op for v0.2.x
    }

    // Stamp synthetic SteamID64 into the controller
    static void WriteControllerSteamId(void * /*controller*/, uint64_t /*sid64*/)
    {
        // intentional no-op for v0.2.x
    }

    // Walk EntitySystem for the controller pointer
    // For v0.2.x we use pszName + slot bookkeeping only
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

        // Match incoming pszName
        bool managed = false;
        const BotEntry *cfg = nullptr;
        if (pszName && pszName[0])
        {
            auto it = g_ExpectedNames.find(pszName);
            if (it != g_ExpectedNames.end())
            {
                managed = true;
                cfg = it->second;
                Personas().MarkSlotManaged(idx, pszName);
                g_ExpectedNames.erase(it);
            }
            else if (Personas().IsSlotManaged(idx))
            {
                managed = true;
                cfg = g_SlotEntry[idx];
            }
        }
        if (!managed)
            RETURN_META(MRES_IGNORED);

        // The bot_info entry chosen at CreateFakeClient time is the identity source,
        // independent of whether the display name is the botprofile or bot_info name
        g_SlotEntry[idx] = cfg;
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
        if (m_bDisguiseEnabled && ssc::IsFakePlayerSet(pClient))
        {
            ssc::ClearFakePlayer(pClient);
        }

        // Write a SteamID64 into CServerSideClient.m_SteamID
        uint64_t sid = 0;
        if (cfgSid != 0)
        {
            sid = MakeUniqueSteamId(idx, cfgSid);
            *reinterpret_cast<uint64_t *>(raw + ssc::OFFSET_m_SteamID) = sid;
            Manager().SetSyntheticSid(idx, sid);
            Publisher().UpdateSyntheticSid(idx, sid);
        }

        META_CONPRINTF("[BOTHIDER] slot=%d steamid64=%llu name='%s'\n",
                       idx, (unsigned long long)sid, pszName ? pszName : "<null>");
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
        if (m_bDisguiseEnabled && ssc::IsFakePlayerSet(pClient))
        {
            ssc::ClearFakePlayer(pClient);
        }
        auto *raw = reinterpret_cast<unsigned char *>(pClient);
        auto *entry = g_SlotEntry[idx];
        if (entry && entry->SteamId64 != 0)
        {
            uint64_t sid = MakeUniqueSteamId(idx, entry->SteamId64);
            *reinterpret_cast<uint64_t *>(raw + ssc::OFFSET_m_SteamID) = sid;
            Manager().SetSyntheticSid(idx, sid);
            Publisher().UpdateSyntheticSid(idx, sid);
        }
        META_CONPRINTF("[BOTHIDER] CPiS safety-net slot=%d name='%s'\n", idx, pszName ? pszName : "<null>");
        RETURN_META(MRES_IGNORED);
    }

    // Clean teardown on disconnect
    // Restore the bot identity
    void HiderPlugin::Hook_ClientDisconnect_Pre(CPlayerSlot slot, ENetworkDisconnectionReason /*reason*/,
                                                const char * /*pszName*/, uint64 /*xuid*/,
                                                const char * /*pszNetworkID*/)
    {
        if (m_bSelfDisabled)
            RETURN_META(MRES_IGNORED);
        int idx = slot.Get();
        if (idx < 0 || idx >= PersonaPool::kMaxSlots)
            RETURN_META(MRES_IGNORED);
        if (!Personas().IsSlotManaged(idx))
            RETURN_META(MRES_IGNORED);

        // Capture the persona name
        std::string persona = Personas().GetSlotName(idx);

        // Restore engine-side bot identity
        void *pClient = ResolveClientBySlot(idx);
        if (pClient)
        {
            auto *raw = reinterpret_cast<unsigned char *>(pClient);
            ssc::SetFakePlayer(pClient);
            *reinterpret_cast<uint64_t *>(raw + ssc::OFFSET_m_SteamID) = 0;

            // * The engine drops the CServerSideClient on kick but leaves the CCSPlayerController
            // UTIL_IsNameTaken read its entity index and queue the controller for destruction
            DestroyControllerForClient(pClient);
        }

        // Free the bot_info assignment bound to this slot
        BotInfo().ReleaseAssignment(g_SlotEntry[idx]);
        g_SlotEntry[idx] = nullptr;

        // Drain manager + personas + shared memory for this slot
        Manager().ReleaseSlot(idx);
        g_ExpectedNames.erase(persona);

        META_CONPRINTF("[BOTHIDER] ClientDisconnect slot=%d name='%s' — slot released\n",
                       idx, persona.empty() ? "<null>" : persona.c_str());
        RETURN_META(MRES_IGNORED);
    }

    // True for console commands that disconnect a client
    static bool IsKickCommand(const char *name)
    {
        if (!name || !name[0])
            return false;
        return !std::strcmp(name, "kickid") ||
               !std::strcmp(name, "kick") ||
               !std::strcmp(name, "bot_kick") ||
               !std::strcmp(name, "banid");
    }

    // True for commands that force bots/humans onto teams
    static bool IsHumanTeamCommand(const char *name)
    {
        if (!name || !name[0])
            return false;
        return !std::strcmp(name, "mp_humanteam") ||
               !std::strcmp(name, "bot_join_team");
    }

    static int CaseCmp(const char *a, const char *b)
    {
#if defined(_WIN32)
        return _stricmp(a, b);
#else
        return strcasecmp(a, b);
#endif
    }

    // True if the value forces a specific team
    static bool IsTeamForceValue(const char *v)
    {
        if (!v || !v[0])
            return false;
        return std::strcmp(v, "0") != 0 && CaseCmp(v, "any") != 0;
    }

    // Poll mp_humanteam/bot_join_team values directly
    static bool IsTeamForceActive()
    {
        static const char *kNames[] = {"mp_humanteam", "bot_join_team"};
        for (const char *name : kNames)
        {
            ConVarRefAbstract ref(name);
            if (!ref.IsValidRef())
                continue;
            CUtlString val = ref.GetString();
            if (IsTeamForceValue(val.Get()))
                return true;
        }
        return false;
    }

    // Used when map_whitelist.json is absent/invalid
    static void LoadDefaultDisguiseWhitelist()
    {
        g_DisguiseWhitelist = {
            "ar_baggage",
            "ar_pool_day",
            "ar_shoots",
            "ar_shoots_night",
            "cs_alpine",
            "cs_italy",
            "cs_office",
            "de_ancient",
            "de_ancient_night",
            "de_anubis",
            "de_cache",
            "de_dust2",
            "de_inferno",
            "de_mirage",
            "de_nuke",
            "de_overpass",
            "de_poseidon",
            "de_sanctum",
            "de_stronghold",
            "de_train",
            "de_vertigo",
            "de_warden",
        };
    }

    // Load the map whitelist from a JSON
    static void LoadDisguiseWhitelist(const char *path)
    {
        g_DisguiseWhitelist.clear();
        std::ifstream ifs(path);
        if (ifs.is_open())
        {
            try
            {
                nlohmann::json root = nlohmann::json::parse(ifs);
                if (root.is_array())
                {
                    for (const auto &e : root)
                        if (e.is_string())
                            g_DisguiseWhitelist.push_back(e.get<std::string>());
                }
            }
            catch (...)
            {
                g_DisguiseWhitelist.clear();
            }
        }
        if (g_DisguiseWhitelist.empty())
            LoadDefaultDisguiseWhitelist();
    }

    // Official maps
    // Bots should stay disguised
    static bool IsDisguiseWhitelistMap(const char *mapName)
    {
        if (!mapName || !mapName[0])
            return false;
        // Strip any workshop/path prefix → bare map name
        const char *slash = std::strrchr(mapName, '/');
        const char *base = slash ? slash + 1 : mapName;
        for (const auto &m : g_DisguiseWhitelist)
            if (m == base)
                return true;
        return false;
    }

    // PRE ICvar::DispatchConCommand — restore fake-player identity on all managed slots
    void HiderPlugin::Hook_DispatchConCommand_Pre(ConCommandRef cmd, const CCommandContext &,
                                                  const CCommand &args)
    {
        if (m_bSelfDisabled)
            RETURN_META(MRES_IGNORED);
        if (!cmd.IsValidRef())
            RETURN_META(MRES_IGNORED);
        const char *cmdName = cmd.GetName();

        if (IsHumanTeamCommand(cmdName))
        {
            if (m_bDisguiseEnabled)
            {
                META_CONPRINTF("[BOTHIDER] '%s' detected — disabling disguise\n",
                               cmdName);
                SetDisguiseEnabled(false);
            }
            RETURN_META(MRES_IGNORED);
        }

        if (!IsKickCommand(cmdName))
            RETURN_META(MRES_IGNORED);

        // Match-end kickid
        if (m_bSuppressKickHooks || m_bRebuilding)
            RETURN_META(MRES_IGNORED);

        int restored = 0;
        for (int idx = 0; idx < PersonaPool::kMaxSlots; ++idx)
        {
            if (!Personas().IsSlotManaged(idx))
                continue;
            void *pClient = ResolveClientBySlot(idx);
            if (!pClient)
                continue;
            auto *raw = reinterpret_cast<unsigned char *>(pClient);
            ssc::SetFakePlayer(pClient);
            *reinterpret_cast<uint64_t *>(raw + ssc::OFFSET_m_SteamID) = 0;
            ++restored;
        }
        META_CONPRINTF("[BOTHIDER] kick PRE '%s' restored=%d\n", cmdName, restored);
        RETURN_META(MRES_IGNORED);
    } // end Hook_DispatchConCommand_Pre

    // POST ICvar::DispatchConCommand — the kick has run and released its target slot(s) via ClientDisconnect
    // Re-disguise every slot still managed so the surviving bots keep their forged identity
    void HiderPlugin::Hook_DispatchConCommand_Post(ConCommandRef cmd, const CCommandContext &,
                                                   const CCommand & /*args*/)
    {
        if (m_bSelfDisabled)
            RETURN_META(MRES_IGNORED);
        if (!cmd.IsValidRef())
            RETURN_META(MRES_IGNORED);
        const char *cmdName = cmd.GetName();

        if (!IsKickCommand(cmdName))
            RETURN_META(MRES_IGNORED);

        // Match-end kickid
        if (m_bSuppressKickHooks)
            RETURN_META(MRES_IGNORED);

        // Disguise-toggle rebuild: skip re-disguise + quota write, clear the flag
        if (m_bRebuilding)
        {
            m_bRebuilding = false;
            META_CONPRINTF("[BOTHIDER] disguise-off kick done\n");
            RETURN_META(MRES_IGNORED);
        }

        int redisguised = 0;
        for (int idx = 0; idx < PersonaPool::kMaxSlots; ++idx)
        {
            if (!Manager().IsManaged(idx))
                continue;
            void *pClient = ResolveClientBySlot(idx);
            if (!pClient)
                continue;
            auto *raw = reinterpret_cast<unsigned char *>(pClient);
            if (m_bDisguiseEnabled && ssc::IsFakePlayerSet(pClient))
                ssc::ClearFakePlayer(pClient);
            uint64_t sid = Manager().GetSyntheticSid(idx);
            if (sid != 0)
                *reinterpret_cast<uint64_t *>(raw + ssc::OFFSET_m_SteamID) = sid;
            ++redisguised;
        }
        // Set bot_quota
        if (engine)
        {
            char quotaCmd[48];
            std::snprintf(quotaCmd, sizeof(quotaCmd), "bot_quota %d\n", redisguised);
            engine->ServerCommand(quotaCmd);
        }
        META_CONPRINTF("[BOTHIDER] kick POST '%s' redisguised=%d quota=%d\n",
                       cmd.GetName(), redisguised, redisguised);
        RETURN_META(MRES_IGNORED);
    } // end Hook_DispatchConCommand_Post

    // Toggle disguise: off restores m_bFakePlayer=1 so the bot manager spawns bots again
    void HiderPlugin::SetDisguiseEnabled(bool enabled)
    {
        if (m_bDisguiseEnabled == enabled)
            return;
        m_bDisguiseEnabled = enabled;

        // Count managed bots
        int managed = 0;
        for (int idx = 0; idx < PersonaPool::kMaxSlots; ++idx)
            if (Manager().IsManaged(idx))
                ++managed;

        // Rebuild
        if (engine && managed > 0)
        {
            for (int idx = 0; idx < PersonaPool::kMaxSlots; ++idx)
            {
                if (!Manager().IsManaged(idx))
                    continue;
                void *pClient = ResolveClientBySlot(idx);
                if (pClient)
                    ssc::SetFakePlayer(pClient);
            }
            m_bRebuilding = true;
            // fill-mode quota = humans + bot
            int quota = CountHumanClients() + managed;
            char quotaCmd[48];
            std::snprintf(quotaCmd, sizeof(quotaCmd), "bot_quota %d\n", quota);
            engine->ServerCommand("bot_kick\n");
            engine->ServerCommand(quotaCmd);
            META_CONPRINTF("[BOTHIDER] disguise %s — rebuilding %d bot(s), quota=%d\n",
                           enabled ? "ON" : "OFF", managed, quota);
            return;
        }

        // Fallback
        for (int idx = 0; idx < PersonaPool::kMaxSlots; ++idx)
        {
            if (!Manager().IsManaged(idx))
                continue;
            void *pClient = ResolveClientBySlot(idx);
            if (!pClient)
                continue;
            auto *raw = reinterpret_cast<unsigned char *>(pClient);
            if (enabled)
            {
                ssc::ClearFakePlayer(pClient);
                uint64_t sid = Manager().GetSyntheticSid(idx);
                if (sid != 0)
                    *reinterpret_cast<uint64_t *>(raw + ssc::OFFSET_m_SteamID) = sid;
            }
            else
            {
                ssc::SetFakePlayer(pClient);
            }
        }
        META_CONPRINTF("[BOTHIDER] disguise %s (no rebuild)\n", enabled ? "ON" : "OFF");
    }

    // Clean-rebuild on rematch
    void HiderPlugin::RebuildBots()
    {
        if (m_bSelfDisabled || !m_bDisguiseEnabled || !engine || m_bRebuilding)
            return;

        // Restore m_bFakePlayer
        int managed = 0;
        for (int idx = 0; idx < PersonaPool::kMaxSlots; ++idx)
        {
            if (!Manager().IsManaged(idx))
                continue;
            void *pClient = ResolveClientBySlot(idx);
            if (pClient)
                ssc::SetFakePlayer(pClient);
            ++managed;
        }
        if (managed == 0)
            return;

        // Re-fill
        int quota = managed;
        ConVarRefAbstract botQuota("bot_quota");
        if (botQuota.IsValidRef())
            quota = botQuota.GetInt();

        m_bRebuilding = true;
        char quotaCmd[48];
        std::snprintf(quotaCmd, sizeof(quotaCmd), "bot_quota %d\n", quota);
        // Drop quota to 0 before kicking: otherwise the engine keeps bots alive to
        // satisfy the live quota mid-kick, and survivors skip CreateFakeClient (stay undisguised)
        engine->ServerCommand("bot_quota 0\n");
        engine->ServerCommand("bot_kick all\n");
        engine->ServerCommand(quotaCmd);
        META_CONPRINTF("[BOTHIDER] rematch rebuild — kicked %d bot(s), bot_quota->%d\n",
                       managed, quota);
    }

    // Match-end
    void HiderPlugin::KickAllBots()
    {
        if (m_bSelfDisabled || !engine)
            return;

        int quota = 0;
        ConVarRefAbstract botQuota("bot_quota");
        if (botQuota.IsValidRef())
            quota = botQuota.GetInt();

        m_bSuppressKickHooks = true;

        engine->ServerCommand("bot_quota 0\n");

        // kickid
        int kicked = 0;
        int managed = 0;
        for (int idx = 0; idx < PersonaPool::kMaxSlots; ++idx)
        {
            if (!Manager().IsManaged(idx))
                continue;
            ++managed;
            void *pClient = ResolveClientBySlot(idx);
            if (!pClient)
                continue;
            ssc::SetFakePlayer(pClient);
            int16_t userId = *reinterpret_cast<int16_t *>(
                reinterpret_cast<unsigned char *>(pClient) + ssc::OFFSET_m_UserID);
            char kickCmd[32];
            std::snprintf(kickCmd, sizeof(kickCmd), "kickid %d\n", static_cast<int>(userId));
            engine->ServerCommand(kickCmd);
            ++kicked;
        }

        m_SavedQuota = quota > 0 ? quota : managed;
        META_CONPRINTF("[BOTHIDER] match-end kick — kickid issued for %d/%d bot(s), "
                       "quota saved=%d, held at 0\n",
                       kicked, managed, m_SavedQuota);
    }

    // Match-begin
    void HiderPlugin::RefillBots()
    {
        if (m_bSelfDisabled || !engine)
            return;
        m_bSuppressKickHooks = false;
        int quota = m_SavedQuota > 0 ? m_SavedQuota : 0;
        if (quota == 0)
        {
            META_CONPRINTF("[BOTHIDER] match-begin refill skipped — no saved quota\n");
            return;
        }
        char quotaCmd[48];
        std::snprintf(quotaCmd, sizeof(quotaCmd), "bot_quota %d\n", quota);
        engine->ServerCommand(quotaCmd);
        META_CONPRINTF("[BOTHIDER] match-begin refill — bot_quota->%d\n", quota);
    }

    // Replace the name with the next name from bot_info.json
    CPlayerSlot HiderPlugin::Hook_CreateFakeClient_Pre(const char *netname)
    {
        if (m_bSelfDisabled)
        {
            RETURN_META_VALUE(MRES_IGNORED, CPlayerSlot(-1));
        }

        std::string persona;

        // Always pick a bot_info.json entry as the identity source (steamid/crosshair/ping)
        const BotEntry *entry = BotInfo().PickForBot(netname);

        // Display name: bot_info name when the toggle is on, else keep the botprofile name
        if (m_bUseBotInfoName)
        {
            if (entry)
                persona = entry->Name;
            else
                persona = Personas().PickFromRoster();
        }
        else
        {
            persona = (netname && netname[0]) ? netname : Personas().PickFromRoster();
        }
        if (persona.empty())
        {
            RETURN_META_VALUE(MRES_IGNORED, CPlayerSlot(-1));
        }

        g_ExpectedNames[persona] = entry;
        static thread_local std::string s_PersonaBuffer;
        s_PersonaBuffer = persona;

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

        // Reset bots' idle timers (1s)
        if ((++m_TickCounter & 63u) == 0u)
        {
            // Poll mp_humanteam/bot_join_team
            if (m_bDisguiseEnabled && IsTeamForceActive())
            {
                META_CONPRINTF("[BOTHIDER] team-force convar active — disabling disguise\n");
                SetDisguiseEnabled(false);
            }

            for (int idx = 0; idx < PersonaPool::kMaxSlots; ++idx)
            {
                if (!Manager().IsManaged(idx))
                    continue;
                void *pClient = ResolveClientBySlot(idx);
                if (pClient)
                    ResetIdleTimerForClient(pClient);
            }
        }

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
                uint64_t unique = MakeUniqueSteamId(slot, sid);
                *reinterpret_cast<uint64_t *>(
                    reinterpret_cast<unsigned char *>(pClient) + ssc::OFFSET_m_SteamID) = unique;
                Manager().SetSyntheticSid(slot, unique);
                Publisher().UpdateSyntheticSid(slot, unique);
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
            },
            // SET_DISGUISE: global toggle for the m_bFakePlayer disguise
            [this](bool enabled)
            {
                SetDisguiseEnabled(enabled);
            },
            // REBUILD
            [this]()
            {
                RebuildBots();
            },
            // KICK_ALL
            [this]()
            {
                KickAllBots();
            },
            // REFILL
            [this]()
            {
                RefillBots();
            },
            // SET_NAME_SOURCE: global toggle for the display-name source
            [this](bool useBotInfo)
            {
                SetUseBotInfoName(useBotInfo);
                META_CONPRINTF("[BOTHIDER] name source -> %s\n",
                               useBotInfo ? "bot_info" : "botprofile");
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

        // Whitelisted maps run disguised
        if (IsDisguiseWhitelistMap(pMapName) && !m_bDisguiseEnabled)
        {
            META_CONPRINTF("[BOTHIDER] whitelist map '%s' — enabling disguise\n", pMapName);
            SetDisguiseEnabled(true);
        }
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

        // GameResourceServiceServer — needed to resolve CCSPlayerController by slot
        // Served by engine2.dll
        g_pGameResourceService = ismm->GetEngineFactory(false)(
            targets::kIface_GameResourceServiceServer, nullptr);
        if (!g_pGameResourceService)
        {
            META_CONPRINTF("[BOTHIDER] warning: %s unresolved — controller mgmt disabled\n",
                           targets::kIface_GameResourceServiceServer);
        }
        else
        {
            META_CONPRINTF("[BOTHIDER] GameResourceService at %p\n", g_pGameResourceService);
        }

        // Resolve UTIL_Remove
        // Required to destroy controllers on kick
        {
            std::string gdPath = g_SMAPI->GetBaseDir();
            gdPath += "/addons/BotHider/gamedata.json";
            nlohmann::json gamedata;
            if (!sig::LoadGamedata(gdPath.c_str(), gamedata))
            {
                META_CONPRINTF("[BOTHIDER] warning: gamedata.json not loaded at '%s' — "
                               "controller cleanup disabled\n",
                               gdPath.c_str());
            }
            else
            {
                // Override member offsets from gamedata.json (fallback kept if absent)
                LoadMemberOffsets(gamedata);

                sig::ModuleInfo serverModule = sig::ModuleFromInterfacePtr(gameclients);
                if (!serverModule)
                    serverModule = sig::ModuleFromName(targets::kServerModuleName);
                ResolveUtilRemoveAndEntSys(gamedata, serverModule);

                // Install the team-0 kick-guard that keeps SourceTV alive (GOTV fix)
                InstallKickGuardHook(gamedata, serverModule);
            }
        }
        if (g_pfnUtilRemove)
        {
            META_CONPRINTF("[BOTHIDER] UTIL_Remove resolved at %p (entSysGlobal=%p)\n",
                           reinterpret_cast<void *>(g_pfnUtilRemove),
                           reinterpret_cast<void *>(g_ppEntSysGlobal));
        }
        else
        {
            META_CONPRINTF("[BOTHIDER] warning: UTIL_Remove signature unresolved — "
                           "controller cleanup disabled\n");
        }

        g_pCVar = icvar;
        g_SMAPI->AddListener(this, this);

        // Resolve schema offsets for idle-timer reset
        if (schema::Init())
        {
            int pawnOff = schema::GetFieldOffset("CBasePlayerController", "m_hPawn");
            int idleOff = schema::GetFieldOffset("CCSPlayerPawnBase", "m_flIdleTimeSinceLastAction");
            META_CONPRINTF("[BOTHIDER] schema resolved m_hPawn=%d m_flIdleTimeSinceLastAction=%d\n",
                           pawnOff, idleOff);
        }
        else
        {
            META_CONPRINTF("[BOTHIDER] warning: SchemaSystem unresolved — idle-kick fix disabled\n");
        }

        // Resolve CUtlString::Set from tier0
#if defined(_WIN32)
        HMODULE tier0 = GetModuleHandleA(targets::kTier0ModuleName);
        if (tier0)
        {
            m_pUtlStringSet = reinterpret_cast<CUtlStringSetFn>(
                GetProcAddress(tier0, targets::kSym_CUtlString_Set));
        }
#else
        void *tier0 = dlopen(targets::kTier0ModuleName, RTLD_NOLOAD | RTLD_NOW);
        if (tier0)
        {
            m_pUtlStringSet = reinterpret_cast<CUtlStringSetFn>(
                dlsym(tier0, targets::kSym_CUtlString_Set));
        }
#endif
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

        // Load the map whitelist
        std::string wlPath = g_SMAPI->GetBaseDir();
        wlPath += "/addons/BotHider/map_whitelist.json";
        LoadDisguiseWhitelist(wlPath.c_str());
        META_CONPRINTF("[BOTHIDER] disguise whitelist — %zu map(s) from '%s'\n",
                       g_DisguiseWhitelist.size(), wlPath.c_str());

        SH_ADD_HOOK(IServerGameClients, OnClientConnected, gameclients,
                    SH_MEMBER(this, &HiderPlugin::Hook_OnClientConnected_Post), true);
        SH_ADD_HOOK(IServerGameClients, ClientPutInServer, gameclients,
                    SH_MEMBER(this, &HiderPlugin::Hook_ClientPutInServer_Post), true);
        SH_ADD_HOOK(IServerGameClients, ClientDisconnect, gameclients,
                    SH_MEMBER(this, &HiderPlugin::Hook_ClientDisconnect_Pre), false);
        SH_ADD_HOOK(IVEngineServer, CreateFakeClient, engine,
                    SH_MEMBER(this, &HiderPlugin::Hook_CreateFakeClient_Pre), false);
        SH_ADD_HOOK(IServerGameDLL, GameFrame, server,
                    SH_MEMBER(this, &HiderPlugin::Hook_GameFrame_Post), true);
        SH_ADD_HOOK(ICvar, DispatchConCommand, icvar,
                    SH_MEMBER(this, &HiderPlugin::Hook_DispatchConCommand_Pre), false);
        SH_ADD_HOOK(ICvar, DispatchConCommand, icvar,
                    SH_MEMBER(this, &HiderPlugin::Hook_DispatchConCommand_Post), true);

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
        SH_REMOVE_HOOK(IServerGameClients, ClientDisconnect, gameclients,
                       SH_MEMBER(this, &HiderPlugin::Hook_ClientDisconnect_Pre), false);
        SH_REMOVE_HOOK(IVEngineServer, CreateFakeClient, engine,
                       SH_MEMBER(this, &HiderPlugin::Hook_CreateFakeClient_Pre), false);
        SH_REMOVE_HOOK(IServerGameDLL, GameFrame, server,
                       SH_MEMBER(this, &HiderPlugin::Hook_GameFrame_Post), true);
        SH_REMOVE_HOOK(ICvar, DispatchConCommand, icvar,
                       SH_MEMBER(this, &HiderPlugin::Hook_DispatchConCommand_Pre), false);
        SH_REMOVE_HOOK(ICvar, DispatchConCommand, icvar,
                       SH_MEMBER(this, &HiderPlugin::Hook_DispatchConCommand_Post), true);

        if (m_StartChangeLevelHookId != 0)
        {
            SH_REMOVE_HOOK_ID(m_StartChangeLevelHookId);
            m_StartChangeLevelHookId = 0;
        }
        m_pHookedGameServer = nullptr;
        // Tear down the kick-guard detour before the module unloads
        g_KickHook.Remove();
        g_pfnKickOneTramp = nullptr;
        Manager().ReleaseAll();
        Publisher().Shutdown();
        return true;
    }

} // namespace cs2bh
