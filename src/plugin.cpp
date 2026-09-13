#include "core/log.h"
// Metamod plugin entry and lifecycle orchestration
// All constants (offsets, vtable slots, schema candidates) live in offsets.h

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
#include "steam/steamtypes.h"
#include "offsets.h"
#include "core/memory_module.h"
#include "core/cs2_sdk/schema.h"
#include "core/config.h"
#include "core/interfaces.h"
#include "core/gamedata.h"

#include <cstdio>
#include <cstring>
#include <string>

#define VERSION_STRING  "v" SEMVER " @ " GITHUB_SHA
#define BUILD_TIMESTAMP __DATE__ " " __TIME__

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

namespace cs2bh {

HiderPlugin g_plugin;

} // namespace cs2bh

PLUGIN_EXPOSE(cs2bh::HiderPlugin, cs2bh::g_plugin); // NOLINT(misc-use-anonymous-namespace,bugprone-throwing-static-initialization)

namespace cs2bh {

// Binds callbacks after all hook members have been constructed.
HiderPlugin::HiderPlugin()
    : m_onClientConnectedHook(&IServerGameClients::OnClientConnected), m_clientPutInServerHook(&IServerGameClients::ClientPutInServer),
      m_clientDisconnectHook(&IServerGameClients::ClientDisconnect), m_startChangeLevelHook(&INetworkGameServer::StartChangeLevel),
      m_gameFrameHook(&IServerGameDLL::GameFrame), m_dispatchConCommandHook(&ICvar::DispatchConCommand)
{
    m_onClientConnectedHook.AddContext(this, nullptr, &HiderPlugin::HookOnClientConnectedPost);
    m_clientPutInServerHook.AddContext(this, nullptr, &HiderPlugin::HookClientPutInServerPost);
    m_clientDisconnectHook.AddContext(this, &HiderPlugin::HookClientDisconnectPre, nullptr);
    m_startChangeLevelHook.AddContext(this, &HiderPlugin::HookStartChangeLevelPre, nullptr);
    m_gameFrameHook.AddContext(this, nullptr, &HiderPlugin::HookGameFramePost);
    m_dispatchConCommandHook.AddContext(this, &HiderPlugin::HookDispatchConCommandPre, &HiderPlugin::HookDispatchConCommandPost);
}

HiderPlugin::~HiderPlugin() = default;

// Installs all process-wide virtual hooks and rolls back partial registration.
bool HiderPlugin::InstallVirtualHooks()
{
    if (!m_onClientConnectedHook.AddChecked(g_gameclients) || !m_clientPutInServerHook.AddChecked(g_gameclients) ||
        !m_clientDisconnectHook.AddChecked(g_gameclients) || !m_gameFrameHook.AddChecked(g_server) ||
        !m_dispatchConCommandHook.AddChecked(g_icvar))
    {
        RemoveVirtualHooks();
        return false;
    }
    return true;
}

// Removes all virtual hooks synchronously when no callback is executing.
bool HiderPlugin::RemoveVirtualHooks()
{
    const bool onClientConnected = m_onClientConnectedHook.RemoveAll();
    const bool clientPutInServer = m_clientPutInServerHook.RemoveAll();
    const bool clientDisconnect = m_clientDisconnectHook.RemoveAll();
    const bool gameFrame = m_gameFrameHook.RemoveAll();
    const bool dispatchConCommand = m_dispatchConCommandHook.RemoveAll();
    const bool startChangeLevel = m_startChangeLevelHook.RemoveAll();
    return onClientConnected && clientPutInServer && clientDisconnect && gameFrame && dispatchConCommand && startChangeLevel;
}

// Defers execution to the engine command queue, outside the current frame callback.
void HiderPlugin::ProcessPendingUnload()
{
    if (!m_unloadPending || m_dispatchConCommandHook.HasRegistrations()) return;
    char command[64];
    std::snprintf(command, sizeof(command), "meta unload %d\n", static_cast<int>(g_PLID));
    m_unloadPending = false;
    g_engine->ServerCommand(command);
}

// Attaches level-scoped hooks and resets transient runtime state
void HiderPlugin::OnLevelInit(char const* mapName, char const*, char const*, char const*, bool, bool)
{
    identity_runtime::ClearPendingControllerRemovals();
    avatar::ResetRuntime();
    auto* gameServer = g_pNetworkServerService ? g_pNetworkServerService->GetIGameServer() : nullptr;
    if (gameServer && gameServer != m_hookedGameServer)
    {
        if (m_hookedGameServer) m_startChangeLevelHook.Remove(m_hookedGameServer);
        if (m_startChangeLevelHook.AddChecked(gameServer))
        {
            m_hookedGameServer = gameServer;
            BH_LOG_INFO("[BOTHIDER] StartChangeLevel hook installed for %p\n", static_cast<void*>(gameServer));
        }
        else
        {
            m_hookedGameServer = nullptr;
            BH_LOG_WARN("[BOTHIDER] warning: StartChangeLevel hook installation failed for %p\n", static_cast<void*>(gameServer));
        }
    }
    BH_LOG_INFO("[BOTHIDER] OnLevelInit map=%s\n", mapName ? mapName : "?");
}

// Releases all state owned by the current level
void HiderPlugin::OnLevelShutdown()
{
    identity_state::ClearAll();
    Manager().ReleaseAll();
    avatar::ProcessOverrides();
    avatar::ResetRuntime();
    BotInfo().ResetAssignments();
    BH_LOG_INFO("[BOTHIDER] OnLevelShutdown — state drained\n");
}

// Resolves interfaces and installs every plugin module
bool HiderPlugin::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool /*late*/)
{
    PLUGIN_SAVEVARS();

    // Load may run again on the same global plugin object
    m_identityMode = IdentityMode::Player;
    m_unloadPending = false;

    if (!interfaces::Init(ismm, error, maxlen)) return false;
    if (!log::Init(g_SMAPI->GetBaseDir(), error, maxlen)) return false;

    if (!KHook::__exported__khook)
    {
        std::snprintf(error, maxlen, "KHook export unavailable; virtual hooks disabled");
        BH_LOG_ERROR("[BOTHIDER] error: %s\n", error);
        log::Close();
        return false;
    }

    if (!offsets::LoadFromSchema(error, maxlen))
    {
        schema::Reset();
        log::Close();
        return false;
    }

    const auto settings = config::Load(g_SMAPI->GetBaseDir());
    m_identityMode = settings.botMode ? IdentityMode::Bot : IdentityMode::Player;
    Manager().ConfigureFakePing(settings.fakePingEnabled, settings.fakePingMin, settings.fakePingMax);

    auto* networkStringTables =
        static_cast<INetworkStringTableContainer*>(ismm->GetEngineFactory()(INTERFACENAME_NETWORKSTRINGTABLESERVER, nullptr));
    avatar::SetStringTableContainer(networkStringTables);
    if (!networkStringTables)
    {
        BH_LOG_WARN("[BOTHIDER] warning: network string table interface unavailable - "
                    "custom avatars disabled\n");
    }

    // GameResourceServiceServer — needed to resolve CCSPlayerController by slot
    // Served by engine2.dll
    void* gameResourceService = ismm->GetEngineFactory(false)(offsets::kIfaceGameResourceServiceServer, nullptr);
    entity_access::SetGameResourceService(gameResourceService);
    if (!gameResourceService)
    {
        BH_LOG_WARN("[BOTHIDER] warning: %s unresolved — controller mgmt disabled\n", offsets::kIfaceGameResourceServiceServer);
    }

    gamedata::Prepare();

    g_pCVar = g_icvar;

    identity_hooks::InstallPrepared();

    Manager().Init();

    // Open the shared-memory bridge
    const bool sharedMemoryReady = Publisher().Init();
    if (sharedMemoryReady)
    {
        // Publish resolved hook/sig addresses for bh_status (0 = unresolved)
        Publisher().PublishSignature("UTIL_Remove", entity_access::UtilRemoveTarget());
        Publisher().PublishSignature("MaintainBotQuota", identity_hooks::MaintainQuotaTarget());
        Publisher().PublishSignature("CountPotentialVoters", identity_hooks::CountPotentialVotersTarget());
        Publisher().PublishSignature("PackEntities", identity_hooks::PackEntitiesTarget());
        Publisher().PublishSignature("HandleJoinTeam", identity_hooks::HandleJoinTeamTarget());
        Publisher().PublishSignature("HumanTeamRestriction", identity_hooks::HumanTeamRestrictionTarget());
        Publisher().PublishSignature("SameMapTeardown", identity_hooks::SameMapTeardownTarget());
    }
    else
    {
        BH_LOG_WARN("[BOTHIDER] warning: shared memory init failed — CSS bridge disabled\n");
    }

    // Load bot identity data from JSON config
    std::string jsonPath = g_SMAPI->GetBaseDir();
    jsonPath += "/addons/BotHider/bot_info.json";
    if (!BotInfo().Load(jsonPath.c_str()))
    {
        BH_LOG_WARN("[BOTHIDER] warning: bot_info.json not found or parse error at '%s' — "
                    "bot identity will fall back to curated roster\n",
                    jsonPath.c_str());
    }

    if (!InstallVirtualHooks())
    {
        BH_LOG_ERROR("[BOTHIDER] error: failed to install KHook virtual hooks");
        char cleanupError[256]{};
        Unload(cleanupError, sizeof(cleanupError));
        std::snprintf(error, maxlen, "failed to install KHook virtual hooks");
        return false;
    }
    g_SMAPI->AddListener(this, this);

    int installedHooks = 0;
    if (identity_hooks::MaintainQuotaTarget()) ++installedHooks;
    if (identity_hooks::CountPotentialVotersTarget()) ++installedHooks;
    if (identity_hooks::PackEntitiesTarget()) ++installedHooks;
    if (identity_hooks::HandleJoinTeamTarget()) ++installedHooks;
    if (identity_hooks::HumanTeamRestrictionTarget()) ++installedHooks;
    if (identity_hooks::SameMapTeardownTarget()) ++installedHooks;
    BH_LOG_INFO("[BOTHIDER] config mode=%s fake_ping=%s range=%d-%d identities=%zu\n", IsBotMode() ? "bot" : "player",
                settings.fakePingEnabled ? "on" : "off", settings.fakePingMin, settings.fakePingMax, BotInfo().Count());
    BH_LOG_INFO("[BOTHIDER] loaded %s hooks=%d/6 util_remove=%s schema=%s shm=%s avatar=%s\n", GetVersion(), installedHooks,
                entity_access::UtilRemoveTarget() ? "ok" : "fail", "ok", sharedMemoryReady ? "ok" : "fail",
                networkStringTables ? "ok" : "fail");
    return true;
}

// Removes hooks and releases every plugin module
bool HiderPlugin::Unload(char* error, size_t maxlen)
{
    if (!hooks::CanRemoveHooks())
    {
        if (!m_unloadPending)
        {
            if (!m_dispatchConCommandHook.RemoveAll(true))
            {
                std::snprintf(error, maxlen, "KHook export unavailable while scheduling unload");
                return false;
            }
            m_unloadPending = true;
        }
        std::snprintf(error, maxlen, "unload deferred until the command hook detaches; automatic retry queued");
        return false;
    }
    if (!identity_hooks::Remove())
    {
        std::snprintf(error, maxlen, "failed to uninstall KHook detours");
        return false;
    }
    if (!RemoveVirtualHooks())
    {
        std::snprintf(error, maxlen, "KHook export unavailable while removing virtual hooks");
        BH_LOG_ERROR("[BOTHIDER] error: %s\n", error);
        return false;
    }
    m_hookedGameServer = nullptr;
    m_unloadPending = false;
    identity_runtime::RestoreManagedClientsForEngineTeardown();
    identity_state::ClearAll();
    Manager().ReleaseAll();
    avatar::ProcessOverrides();
    avatar::ResetRuntime();
    Publisher().Shutdown();
    avatar::SetStringTableContainer(nullptr);
    entity_access::Reset();
    schema::Reset();
    BH_LOG_INFO("Plugin unloaded");
    log::Close();
    return true;
}

// Returns plugin author metadata.
const char* HiderPlugin::GetAuthor() { return "XBribo(๑•.•๑)"; }
// Returns the plugin name.
const char* HiderPlugin::GetName() { return "CS2-Bot-Hider"; }
// Returns the plugin description.
const char* HiderPlugin::GetDescription() { return "Bot persona/steamid/ping/crosshair/avatar hider"; }
// Returns the plugin project URL.
const char* HiderPlugin::GetURL() { return "https://github.com/XBribo/CS2-Bot-Hider"; }
// Returns the plugin license.
const char* HiderPlugin::GetLicense() { return "AGPL-3.0"; }
// Returns the version supplied by the build.
const char* HiderPlugin::GetVersion() { return VERSION_STRING; }
// Returns the compilation date and time.
const char* HiderPlugin::GetDate() { return BUILD_TIMESTAMP; }
// Returns the plugin log tag.
const char* HiderPlugin::GetLogTag() { return "BH"; }

} // namespace cs2bh
