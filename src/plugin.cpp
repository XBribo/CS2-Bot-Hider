#include "core/log.h"
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
#include "core/cs2_sdk/src/core/memory_module.h"
#include "core/cs2_sdk/src/core/schema.h"
#include "core/config.h"
#include "core/interfaces.h"
#include "core/gamedata.h"

#include <cstdio>
#include <cstring>
#include <string>

#define VERSION_STRING  "v" SEMVER " @ " GITHUB_SHA
#define BUILD_TIMESTAMP __DATE__ " " __TIME__

#include <engine2/inetworkgameserver.h>
#include <engine2/inetworkserverservice.h>
#include <engine2/networkgameserverbase.h>
#include <engine2/isource2server.h>
#include <engine2/ivengineserver2.h>
#include <networkstringtabledefs.h>
#include <tier0/utlvector.h>
#include <tier0/convar.h>

namespace cs2bh {

HiderPlugin g_plugin;

} // namespace cs2bh

PLUGIN_EXPOSE(cs2bh::HiderPlugin, cs2bh::g_plugin); // NOLINT(misc-use-anonymous-namespace,bugprone-throwing-static-initialization)

namespace cs2bh {

// Bind callbacks after hook construction.
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

// Install virtual hooks; roll back partial registration.
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

// Remove virtual hooks outside active callbacks.
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

// Queue unload outside the frame callback.
void HiderPlugin::ProcessPendingUnload()
{
    if (!m_unloadPending || m_dispatchConCommandHook.HasRegistrations()) return;
    char command[64];
    std::snprintf(command, sizeof(command), "meta unload %d\n", static_cast<int>(g_PLID));
    m_unloadPending = false;
    g_engine->ServerCommand(command);
}

// Attach level hooks and reset runtime state.
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
            BH_LOG_DEBUG("StartChangeLevel attached target=%p", static_cast<void*>(gameServer));
        }
        else
        {
            m_hookedGameServer = nullptr;
            BH_LOG_WARN("StartChangeLevel hook failed target=%p", static_cast<void*>(gameServer));
        }
    }
    BH_LOG_DEBUG("level init map=%s", mapName ? mapName : "?");
}

// Release level state.
void HiderPlugin::OnLevelShutdown()
{
    identity_state::ClearAll();
    Manager().ReleaseAll();
    avatar::ProcessOverrides();
    avatar::ResetRuntime();
    BotInfo().ResetAssignments();
    BH_LOG_DEBUG("level shutdown");
}

// Resolve interfaces and install plugin modules.
bool HiderPlugin::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool /*late*/)
{
    PLUGIN_SAVEVARS();

    // Reset state for plugin reloads.
    m_identityMode = IdentityMode::Bot;
    m_unloadPending = false;

    if (!interfaces::Init(ismm, error, maxlen)) return false;
    if (!log::Init(g_SMAPI->GetBaseDir(), "BotHider", error, maxlen)) return false;

    if (!KHook::__exported__khook)
    {
        std::snprintf(error, maxlen, "KHook export unavailable; virtual hooks disabled");
        BH_LOG_ERROR("%s", error);
        log::Close();
        return false;
    }

    if (!offsets::LoadFromSchema(error, maxlen))
    {
        cs2_sdk::schema::Reset();
        log::Close();
        return false;
    }

    const auto settings = config::Load(g_SMAPI->GetBaseDir());
    Manager().ConfigureFakePing(settings.fakePingEnabled, settings.fakePingMin, settings.fakePingMax);

    auto* networkStringTables =
        static_cast<INetworkStringTableContainer*>(ismm->GetEngineFactory()(INTERFACENAME_NETWORKSTRINGTABLESERVER, nullptr));
    avatar::SetStringTableContainer(networkStringTables);
    if (!networkStringTables)
    {
        BH_LOG_WARN("avatars disabled: string tables unavailable");
    }

    if (!gamedata::Prepare(error, maxlen))
    {
        avatar::SetStringTableContainer(nullptr);
        entity_access::Reset();
        cs2_sdk::schema::Reset();
        log::Close();
        return false;
    }

    g_pCVar = g_icvar;

    identity_hooks::InstallPrepared();

    if (!settings.botMode && !identity_hooks::CanEnableDisguise(error, maxlen))
    {
        BH_LOG_ERROR("%s", error);
        identity_hooks::Remove();
        avatar::SetStringTableContainer(nullptr);
        entity_access::Reset();
        cs2_sdk::schema::Reset();
        log::Close();
        return false;
    }
    m_identityMode = settings.botMode ? IdentityMode::Bot : IdentityMode::Player;

    Manager().Init();

    Publisher().Init();
    Publisher().PublishSignature("UTIL_Remove", entity_access::UtilRemoveTarget());
    Publisher().PublishSignature("MaintainBotQuota", identity_hooks::MaintainQuotaTarget());
    Publisher().PublishSignature("CountPotentialVoters", identity_hooks::CountPotentialVotersTarget());
    Publisher().PublishSignature("PackEntities", identity_hooks::PackEntitiesTarget());
    Publisher().PublishSignature("HandleJoinTeam", identity_hooks::HandleJoinTeamTarget());
    Publisher().PublishSignature("HumanTeamRestriction", identity_hooks::HumanTeamRestrictionTarget());
    Publisher().PublishSignature("SameMapTeardown", identity_hooks::SameMapTeardownTarget());
    Publisher().PublishSignature("ScriptIsBot", identity_hooks::ScriptIsBotTarget());

    // Load bot identities.
    std::string jsonPath = g_SMAPI->GetBaseDir();
    jsonPath += "/addons/BotHider/bot_info.json";
    if (!BotInfo().Load(jsonPath.c_str()))
    {
        BH_LOG_WARN("bot_info unavailable path='%s': using engine names", jsonPath.c_str());
    }

    if (!InstallVirtualHooks())
    {
        BH_LOG_ERROR("KHook virtual hooks failed");
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
    if (identity_hooks::ScriptIsBotTarget()) ++installedHooks;
    BH_LOG_INFO("Loaded %s", GetVersion());
    BH_LOG_DEBUG("mode=%s ping=%s range=%d-%d identities=%zu hooks=%d/7 cleanup=%s avatar=%s", IsBotMode() ? "bot" : "player",
                 settings.fakePingEnabled ? "on" : "off", settings.fakePingMin, settings.fakePingMax, BotInfo().Count(), installedHooks,
                 entity_access::UtilRemoveTarget() ? "ok" : "fail", networkStringTables ? "ok" : "fail");
    return true;
}

// Remove hooks and release plugin modules.
bool HiderPlugin::Unload(char* error, size_t maxlen)
{
    if (Publisher().HasProvider())
    {
        std::snprintf(error, maxlen, "unload BotHiderImpl before BotHider");
        return false;
    }
    Publisher().BeginShutdown();
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
        BH_LOG_ERROR("%s", error);
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
    cs2_sdk::schema::Reset();
    BH_LOG_INFO("Unloaded");
    log::Close();
    return true;
}

const char* HiderPlugin::GetAuthor() { return "XBribo(๑•.•๑)"; }
const char* HiderPlugin::GetName() { return "BotHider"; }
const char* HiderPlugin::GetDescription() { return "Bot persona/steamid/ping/crosshair/avatar hider"; }
const char* HiderPlugin::GetURL() { return "https://github.com/XBribo/CS2-Bot-Hider"; }
const char* HiderPlugin::GetLicense() { return "AGPL-3.0"; }
const char* HiderPlugin::GetVersion() { return VERSION_STRING; }
const char* HiderPlugin::GetDate() { return BUILD_TIMESTAMP; }
const char* HiderPlugin::GetLogTag() { return "BH"; }

} // namespace cs2bh
