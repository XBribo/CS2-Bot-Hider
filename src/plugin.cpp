// Metamod plugin entry and lifecycle orchestration
// All constants (offsets, vtable slots, schema candidates) live in version_targets.h

#include "plugin.h"
#include "ISmmPlugin.h"
#include "ISmmPluginExt.h"
#include "ISmmAPI.h"
#include "entity_access.h"
#include "icvar.h"
#include "identity_runtime.h"
#include "identity_state.h"
#include "avatar_override.h"
#include "bot_info.h"
#include "interfaces/interfaces.h"
#include "fake_client_manager.h"
#include "identity_hooks.h"
#include "playerslot.h"
#include "slot_publisher.h"
#include "sourcehook.h"
#include "steam/steamtypes.h"
#include "version_targets.h"
#include "sig_scan.h"
#include "schema_resolver.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#define CS2BH_FASTCALL __fastcall
#else
#define CS2BH_FASTCALL
#endif

#include <iserver.h>
#include <eiface.h>
#include <networkstringtabledefs.h>
#include <tier1/utlvector.h>
#include <tier1/convar.h>

SH_DECL_HOOK6_void(IServerGameClients, OnClientConnected, SH_NOATTRIB, 0, CPlayerSlot, const char*, uint64, const char*, const char*, bool);
SH_DECL_HOOK4_void(IServerGameClients, ClientPutInServer, SH_NOATTRIB, 0, CPlayerSlot, char const*, int, uint64);
SH_DECL_HOOK5_void(
    IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, CPlayerSlot, ENetworkDisconnectionReason, const char*, uint64, const char*);
SH_DECL_HOOK3(INetworkGameServer, StartChangeLevel, SH_NOATTRIB, 0, CUtlVector<INetworkGameClient*>*, const char*, const char*, void*);
SH_DECL_HOOK3_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool, bool, bool);
SH_DECL_HOOK3_void(ICvar, DispatchConCommand, SH_NOATTRIB, 0, ConCommandRef, const CCommandContext&, const CCommand&);

namespace cs2bh {

HiderPlugin g_plugin;

void HiderPlugin::SetLifecycleActive()
{
    std::unique_lock<std::mutex> lock(m_LifecycleMutex);
    m_LifecycleCv.wait(lock, [this] {
        return !m_TeardownRestoreActive.load(std::memory_order_acquire);
    });

    // A new level starts with no valid transaction snapshot from the previous
    // level. Discard it before allowing hooks to access current entities.
    identity_hooks::ResetPopulationTransaction();
    m_populationCommandDepth = 0;
    m_tickCounter = 0;
    m_TeardownRestoreActive.store(false, std::memory_order_release);
    m_LifecycleState.store(LifecycleState::Active, std::memory_order_release);
    lock.unlock();
    META_CONPRINTF("[BOTHIDER] lifecycle state=active\n");
}

bool HiderPlugin::BeginLifecycleTeardown()
{
    std::unique_lock<std::mutex> lock(m_LifecycleMutex);
    if (m_LifecycleState.load(std::memory_order_acquire) != LifecycleState::Active)
    {
        m_LifecycleCv.wait(lock, [this] {
            return !m_TeardownRestoreActive.load(std::memory_order_acquire);
        });
        return false;
    }

    // Publish teardown before restoring anything. Ordinary hooks use
    // IsLifecycleActive() and stop touching level-owned objects immediately.
    m_LifecycleState.store(LifecycleState::Teardown, std::memory_order_release);
    m_TeardownRestoreActive.store(true, std::memory_order_release);
    return true;
}

void HiderPlugin::EndLifecycleTeardown()
{
    m_populationCommandDepth = 0;
    identity_runtime::ClearPendingControllerRemovals();
    identity_hooks::ResetPopulationTransaction();
    {
        std::lock_guard<std::mutex> lock(m_LifecycleMutex);
        m_TeardownRestoreActive.store(false, std::memory_order_release);
    }
    m_LifecycleCv.notify_all();
}

} // namespace cs2bh

PLUGIN_EXPOSE(cs2bh::HiderPlugin, cs2bh::g_plugin);

// Interface globals

IVEngineServer* engine = nullptr;
ICvar* icvar = nullptr;
IServerGameClients* gameclients = nullptr;
IServerGameDLL* server = nullptr;
extern INetworkServerService* g_pNetworkServerService;

namespace cs2bh {

// Attaches level-scoped hooks and resets transient runtime state
void HiderPlugin::OnLevelInit(char const* pMapName, char const*, char const*, char const*, bool, bool)
{
    SetLifecycleActive();
    identity_runtime::ClearPendingControllerRemovals();
    avatar::ResetRuntime();
    auto* gameServer = g_pNetworkServerService ? g_pNetworkServerService->GetIGameServer() : nullptr;
    if (gameServer && gameServer != m_hookedGameServer)
    {
        if (m_startChangeLevelHookId != 0)
        {
            SH_REMOVE_HOOK_ID(m_startChangeLevelHookId);
            m_startChangeLevelHookId = 0;
        }
        m_startChangeLevelHookId = SH_ADD_HOOK_MEMFUNC(INetworkGameServer, StartChangeLevel, gameServer, this,
                                                       &HiderPlugin::HookStartChangeLevelPre, false /* PRE */);
        m_hookedGameServer = static_cast<void*>(gameServer);
        META_CONPRINTF("[BOTHIDER] StartChangeLevel hook attached to %p (id %d)\n", static_cast<void*>(gameServer),
                       m_startChangeLevelHookId);
    }
    META_CONPRINTF("[BOTHIDER] OnLevelInit map=%s\n", pMapName ? pMapName : "?");
}

// Releases all state owned by the current level
void HiderPlugin::OnLevelShutdown()
{
    const bool restoreOwner = BeginLifecycleTeardown();
    int restoredClients = 0;
    if (restoreOwner)
    {
        // Restore only through the dedicated teardown window. Ordinary hooks
        // are already blocked by the state transition above.
        restoredClients = identity_runtime::RestoreManagedClientsForEngineTeardown();
        EndLifecycleTeardown();
    }
    identity_state::ClearAll();
    Manager().ReleaseAll();
    identity_runtime::ClearPendingControllerRemovals();
    avatar::ResetRuntime();
    BotInfo().ResetAssignments();
    if (m_startChangeLevelHookId != 0)
    {
        SH_REMOVE_HOOK_ID(m_startChangeLevelHookId);
        m_startChangeLevelHookId = 0;
    }
    m_hookedGameServer = nullptr;
    META_CONPRINTF("[BOTHIDER] OnLevelShutdown restored=%d — state drained\n", restoredClients);
}

// Resolves interfaces and installs every plugin module
bool HiderPlugin::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool /*late*/)
{
    PLUGIN_SAVEVARS();

    // Load may run again on the same global plugin object
    m_identityMode = IdentityMode::Player;
    m_fakePingEnabled = true;
    m_fakePingMin = 20;
    m_fakePingMax = 90;
    m_LifecycleState.store(LifecycleState::Inactive, std::memory_order_release);
    m_populationCommandDepth = 0;
    identity_hooks::ResetPopulationTransaction();

    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
    GET_V_IFACE_CURRENT(GetEngineFactory, icvar, ICvar, CVAR_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetServerFactory, gameclients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS);
    GET_V_IFACE_ANY(GetServerFactory, server, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);
    GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);

    // Reads startup identity and fake-ping settings
    {
        std::string configPath = g_SMAPI->GetBaseDir();
        configPath += "/addons/BotHider/config.json";
        std::ifstream configFile(configPath, std::ios::binary);
        if (configFile.is_open())
        {
            const std::string configText((std::istreambuf_iterator<char>(configFile)), std::istreambuf_iterator<char>());
            const nlohmann::json config = nlohmann::json::parse(configText, nullptr, false);
            if (config.is_discarded())
            {
                META_CONPRINTF("[BOTHIDER] warning: config.json parse error; using defaults\n");
            }
            else if (config.is_object())
            {
                if (config.contains("identity_mode") && config["identity_mode"].is_string())
                {
                    const std::string mode = config["identity_mode"].get<std::string>();
                    if (mode == "bot") m_identityMode = IdentityMode::Bot;
                    else if (mode != "player")
                        META_CONPRINTF("[BOTHIDER] warning: unsupported identity_mode='%s'; using player\n", mode.c_str());
                }

                if (config.contains("fake_ping") && config["fake_ping"].is_object())
                {
                    const auto& fakePing = config["fake_ping"];
                    if (fakePing.contains("enabled") && fakePing["enabled"].is_boolean())
                        m_fakePingEnabled = fakePing["enabled"].get<bool>();

                    int minimum = m_fakePingMin;
                    int maximum = m_fakePingMax;
                    if (fakePing.contains("min") && fakePing["min"].is_number_integer()) minimum = fakePing["min"].get<int>();
                    if (fakePing.contains("max") && fakePing["max"].is_number_integer()) maximum = fakePing["max"].get<int>();
                    if (minimum >= 1 && maximum <= 999 && minimum <= maximum)
                    {
                        m_fakePingMin = minimum;
                        m_fakePingMax = maximum;
                    }
                    else
                    {
                        META_CONPRINTF("[BOTHIDER] warning: invalid fake_ping range %d-%d; using 20-90\n", minimum, maximum);
                    }
                }
            }
        }
        else
        {
            // Creates the documented defaults on first install
            std::ofstream defaultConfig(configPath, std::ios::trunc);
            if (defaultConfig.is_open())
            {
                defaultConfig << "{\n"
                                 "    \"identity_mode\": \"player\",\n"
                                 "    \"fake_ping\": {\n"
                                 "        \"enabled\": true,\n"
                                 "        \"min\": 20,\n"
                                 "        \"max\": 90\n"
                                 "    }\n"
                                 "}\n";
            }
            else
            {
                META_CONPRINTF("[BOTHIDER] warning: config.json missing and could not be created; using defaults\n");
            }
        }
    }
    Manager().ConfigureFakePing(m_fakePingEnabled, m_fakePingMin, m_fakePingMax);

    auto* networkStringTables =
        static_cast<INetworkStringTableContainer*>(ismm->GetEngineFactory()(INTERFACENAME_NETWORKSTRINGTABLESERVER, nullptr));
    avatar::SetStringTableContainer(networkStringTables);
    if (!networkStringTables)
    {
        META_CONPRINTF("[BOTHIDER] warning: network string table interface unavailable - "
                       "custom avatars disabled\n");
    }

    // GameResourceServiceServer — needed to resolve CCSPlayerController by slot
    // Served by engine2.dll
    void* gameResourceService = ismm->GetEngineFactory(false)(targets::kIfaceGameResourceServiceServer, nullptr);
    entity_access::SetGameResourceService(gameResourceService);
    if (!gameResourceService)
    {
        META_CONPRINTF("[BOTHIDER] warning: %s unresolved — controller mgmt disabled\n", targets::kIfaceGameResourceServiceServer);
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
            entity_access::LoadMemberOffsets(gamedata);
            if (targets::g_vtableSlotClientSetName < 0)
            {
                META_CONPRINTF("[BOTHIDER] warning: CServerSideClient::SetName vtable slot missing - "
                               "name overwrite disabled\n");
            }

            sig::ModuleInfo serverModule = sig::ModuleFromInterfacePtr(gameclients);
            if (!serverModule) serverModule = sig::ModuleFromName(targets::kServerModuleName);
            entity_access::ResolveUtilRemoveAndEntSys(gamedata, serverModule);

            identity_hooks::PrepareAll(gamedata, serverModule);
        }
    }
    if (!entity_access::UtilRemoveTarget())
    {
        META_CONPRINTF("[BOTHIDER] warning: UTIL_Remove signature unresolved — "
                       "controller cleanup disabled\n");
    }

    g_pCVar = icvar;
    g_SMAPI->AddListener(this, this);

    // Resolve controller pawn and idle-timer schema offsets
    const bool schemaReady = schema::Init();
    if (schemaReady)
    {
        int pawnOff = schema::GetFieldOffset("CBasePlayerController", "m_hPawn");
        int playerPawnOff = schema::GetFieldOffset("CCSPlayerController", "m_hPlayerPawn");
        int idleOff = schema::GetFieldOffset("CCSPlayerPawnBase", "m_flIdleTimeSinceLastAction");
        entity_access::SetBotPawnHandleOffset(playerPawnOff >= 0 ? playerPawnOff : pawnOff);
        if (entity_access::BotPawnHandleOffset() < 0)
            META_CONPRINTF("[BOTHIDER] warning: bot pawn handle unresolved - FL_BOT override disabled\n");
    }
    else
    {
        entity_access::SetBotPawnHandleOffset(-1);
        META_CONPRINTF("[BOTHIDER] warning: SchemaSystem unresolved — idle-kick and FL_BOT overrides disabled\n");
    }

    identity_hooks::InstallPrepared();

    Manager().Init();

    // Open the shared-memory bridge
    const bool sharedMemoryReady = Publisher().Init();
    if (sharedMemoryReady)
    {
        // Publish resolved hook/sig addresses for bh_status (0 = unresolved)
        Publisher().PublishSignature("UTIL_Remove", entity_access::UtilRemoveTarget());
        Publisher().PublishSignature("MaintainBotQuota", identity_hooks::MaintainQuotaTarget());
        Publisher().PublishSignature("PackEntities", identity_hooks::PackEntitiesTarget());
        Publisher().PublishSignature("HandleJoinTeam", identity_hooks::HandleJoinTeamTarget());
        Publisher().PublishSignature("HumanTeamRestriction", identity_hooks::HumanTeamRestrictionTarget());
        Publisher().PublishSignature("SameMapTeardown", identity_hooks::SameMapTeardownTarget());
    }
    else
    {
        META_CONPRINTF("[BOTHIDER] warning: shared memory init failed — CSS bridge disabled\n");
    }

    // Load bot identity data from JSON config
    std::string jsonPath = g_SMAPI->GetBaseDir();
    jsonPath += "/addons/BotHider/bot_info.json";
    if (!BotInfo().Load(jsonPath.c_str()))
    {
        META_CONPRINTF("[BOTHIDER] warning: bot_info.json not found or parse error at '%s' — "
                       "bot identity will fall back to curated roster\n",
                       jsonPath.c_str());
    }

    SH_ADD_HOOK(IServerGameClients, OnClientConnected, gameclients, SH_MEMBER(this, &HiderPlugin::HookOnClientConnectedPost), true);
    SH_ADD_HOOK(IServerGameClients, ClientPutInServer, gameclients, SH_MEMBER(this, &HiderPlugin::HookClientPutInServerPost), true);
    SH_ADD_HOOK(IServerGameClients, ClientDisconnect, gameclients, SH_MEMBER(this, &HiderPlugin::HookClientDisconnectPre), false);
    SH_ADD_HOOK(IServerGameDLL, GameFrame, server, SH_MEMBER(this, &HiderPlugin::HookGameFramePost), true);
    SH_ADD_HOOK(ICvar, DispatchConCommand, icvar, SH_MEMBER(this, &HiderPlugin::HookDispatchConCommandPre), false);
    SH_ADD_HOOK(ICvar, DispatchConCommand, icvar, SH_MEMBER(this, &HiderPlugin::HookDispatchConCommandPost), true);

    int installedHooks = 0;
    if (identity_hooks::MaintainQuotaTarget()) ++installedHooks;
    if (identity_hooks::PackEntitiesTarget()) ++installedHooks;
    if (identity_hooks::HandleJoinTeamTarget()) ++installedHooks;
    if (identity_hooks::HumanTeamRestrictionTarget()) ++installedHooks;
    if (identity_hooks::SameMapTeardownTarget()) ++installedHooks;
    META_CONPRINTF("[BOTHIDER] config mode=%s fake_ping=%s range=%d-%d identities=%zu\n",
                   IsBotMode() ? "bot" : "player", m_fakePingEnabled ? "on" : "off", m_fakePingMin, m_fakePingMax,
                   BotInfo().Count());
    META_CONPRINTF("[BOTHIDER] loaded v%s hooks=%d/5 util_remove=%s schema=%s shm=%s avatar=%s\n", GetVersion(), installedHooks,
                   entity_access::UtilRemoveTarget() ? "ok" : "fail", schemaReady ? "ok" : "fail",
                   sharedMemoryReady ? "ok" : "fail", networkStringTables ? "ok" : "fail");
    SetLifecycleActive();
    return true;
}

// Removes hooks and releases every plugin module
bool HiderPlugin::Unload(char* error, size_t maxlen)
{
    const bool restoreOwner = BeginLifecycleTeardown();
    int restoredClients = 0;
    if (restoreOwner)
    {
        restoredClients = identity_runtime::RestoreManagedClientsForEngineTeardown();
        EndLifecycleTeardown();
    }
    SH_REMOVE_HOOK(IServerGameClients, OnClientConnected, gameclients, SH_MEMBER(this, &HiderPlugin::HookOnClientConnectedPost), true);
    SH_REMOVE_HOOK(IServerGameClients, ClientPutInServer, gameclients, SH_MEMBER(this, &HiderPlugin::HookClientPutInServerPost), true);
    SH_REMOVE_HOOK(IServerGameClients, ClientDisconnect, gameclients, SH_MEMBER(this, &HiderPlugin::HookClientDisconnectPre), false);
    SH_REMOVE_HOOK(IServerGameDLL, GameFrame, server, SH_MEMBER(this, &HiderPlugin::HookGameFramePost), true);
    SH_REMOVE_HOOK(ICvar, DispatchConCommand, icvar, SH_MEMBER(this, &HiderPlugin::HookDispatchConCommandPre), false);
    SH_REMOVE_HOOK(ICvar, DispatchConCommand, icvar, SH_MEMBER(this, &HiderPlugin::HookDispatchConCommandPost), true);

    if (m_startChangeLevelHookId != 0)
    {
        SH_REMOVE_HOOK_ID(m_startChangeLevelHookId);
        m_startChangeLevelHookId = 0;
    }

    // SourceHook callbacks are quiesced before removing native trampolines.
    // Native detours remain installed until identity_hooks::Remove() waits for
    // any engine thread already inside a detour.
    if (!identity_hooks::Remove())
    {
        std::snprintf(error, maxlen, "failed to uninstall funchook detours");
        return false;
    }

    m_hookedGameServer = nullptr;
    META_CONPRINTF("[BOTHIDER] Unload restored=%d\n", restoredClients);
    identity_state::ClearAll();
    Manager().ReleaseAll();
    identity_runtime::ClearPendingControllerRemovals();
    avatar::ResetRuntime();
    Publisher().Shutdown();
    avatar::SetStringTableContainer(nullptr);
    entity_access::Reset();
    return true;
}

} // namespace cs2bh
