#include "plugin.h"

#include "avatar_override.h"
#include "bot_info.h"
#include "entity_access.h"
#include "fake_client_manager.h"
#include "identity_hooks.h"
#include "identity_runtime.h"
#include "identity_state.h"
#include "personas.h"
#include "serversideclient_ref.h"
#include "slot_publisher.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <eiface.h>
#include <iserver.h>
#include <tier1/convar.h>

extern IVEngineServer* engine;

namespace cs2bh {

// Returns whether a console command disconnects a client
static bool IsKickCommand(const char* name)
{
    if (!name || !name[0]) return false;
    return !std::strcmp(name, "kickid") || !std::strcmp(name, "kick") || !std::strcmp(name, "bot_kick") || !std::strcmp(name, "banid");
}

// Returns whether a command adds a bot
static bool IsBotAddCommand(const char* name)
{
    if (!name || !name[0]) return false;
    return !std::strcmp(name, "bot_add") || !std::strcmp(name, "bot_add_t") || !std::strcmp(name, "bot_add_ct");
}

// Returns whether a bot_kick target is a built-in group
static bool IsBotKickGroupTarget(const char* target)
{
    if (!target || !target[0]) return false;
    return !std::strcmp(target, "all") || !std::strcmp(target, "t") || !std::strcmp(target, "ct");
}

// Finds a managed slot from its current persona name
static int FindManagedSlotByPersonaName(const char* name)
{
    if (!name || !name[0]) return -1;
    for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
    {
        if (!Manager().IsManaged(slot)) continue;
        if (Personas().GetSlotName(slot) == name) return slot;
    }
    return -1;
}

// Restores bot identity before bot-sensitive console commands
void HiderPlugin::Hook_DispatchConCommand_Pre(ConCommandRef command, const CCommandContext&, const CCommand& arguments)
{
    if (m_bSelfDisabled || !command.IsValidRef()) RETURN_META(MRES_IGNORED);
    const char* commandName = command.GetName();

    if (IsBotAddCommand(commandName))
    {
        m_bBotAddInProgress = true;
        m_AddFlippedSlots.fill(ManagedControllerFlagSnapshot{});
        if (m_bDisguiseEnabled && !m_bRebuilding)
        {
            FlipManagedController904(false, &m_AddFlippedSlots);
        }
        RETURN_META(MRES_IGNORED);
    }

    if (!std::strcmp(commandName, "bot_kick"))
    {
        const char* target = arguments.ArgC() >= 2 ? arguments.Arg(1) : "";
        if (target[0] && !IsBotKickGroupTarget(target))
        {
            const int slot = FindManagedSlotByPersonaName(target);
            if (slot >= 0 && engine)
            {
                char kickCommand[640];
                std::snprintf(kickCommand, sizeof(kickCommand), "kick \"%s\"\n", target);
                engine->ServerCommand(kickCommand);
                META_CONPRINTF("[BOTHIDER] bot_kick '%s' redirected to kick "
                               "for managed slot=%d\n",
                               target, slot);
                RETURN_META(MRES_SUPERCEDE);
            }
        }
    }

    if (!IsKickCommand(commandName) || m_bRebuilding) RETURN_META(MRES_IGNORED);

    m_ManagedBeforeKick = 0;
    m_QuotaBeforeKick = -1;
    m_AdjustQuotaAfterKick = std::strcmp(commandName, "bot_kick") != 0;
    if (m_AdjustQuotaAfterKick)
    {
        for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
        {
            if (Manager().IsManaged(slot)) ++m_ManagedBeforeKick;
        }

        ConVarRefAbstract botQuota("bot_quota");
        if (botQuota.IsValidRef()) m_QuotaBeforeKick = botQuota.GetInt();
    }

    int restored = 0;
    for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
    {
        if (!Personas().IsSlotManaged(slot)) continue;
        void* client = entity_access::ResolveClientBySlot(slot);
        if (!client) continue;
        ssc::SetFakePlayer(client);
        identity_runtime::SetControllerFakeClientFlag(slot, true);
        ssc::WriteSteamId(client, 0);
        ++restored;
    }
    META_CONPRINTF("[BOTHIDER] kick PRE '%s' restored=%d\n", commandName, restored);
    RETURN_META(MRES_IGNORED);
}

// Restores disguise state after bot-sensitive console commands
void HiderPlugin::Hook_DispatchConCommand_Post(ConCommandRef command, const CCommandContext&, const CCommand& /*arguments*/)
{
    if (m_bSelfDisabled || !command.IsValidRef()) RETURN_META(MRES_IGNORED);
    const char* commandName = command.GetName();

    if (IsBotAddCommand(commandName))
    {
        if (m_bDisguiseEnabled && !m_bRebuilding)
        {
            FlipManagedController904(true, &m_AddFlippedSlots);
            m_bBotAddInProgress = false;
            for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
            {
                if (!Manager().IsManaged(slot)) continue;
                void* client = entity_access::ResolveClientBySlot(slot);
                if (!client) continue;
                ssc::ClearFakePlayer(client);
                identity_runtime::SetControllerFakeClientFlag(slot, false);
            }
        }
        m_bBotAddInProgress = false;
        m_AddFlippedSlots.fill(ManagedControllerFlagSnapshot{});
        RETURN_META(MRES_IGNORED);
    }

    if (!IsKickCommand(commandName)) RETURN_META(MRES_IGNORED);
    if (m_bRebuilding)
    {
        m_bRebuilding = false;
        META_CONPRINTF("[BOTHIDER] disguise-off kick done\n");
        RETURN_META(MRES_IGNORED);
    }

    int redisguised = 0;
    int managedAfterKick = 0;
    for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
    {
        if (!Manager().IsManaged(slot)) continue;
        ++managedAfterKick;
        void* client = entity_access::ResolveClientBySlot(slot);
        if (!client) continue;
        if (m_bDisguiseEnabled)
        {
            ssc::ClearFakePlayer(client);
            identity_runtime::SetControllerFakeClientFlag(slot, false);
        }
        const uint64_t steamId = Manager().GetSyntheticSid(slot);
        if (steamId != 0) ssc::WriteSteamId(client, steamId);
        entity_access::RefreshClientUserInfo(slot);
        ++redisguised;
    }

    if (m_AdjustQuotaAfterKick && m_QuotaBeforeKick >= 0)
    {
        const int removedManaged = m_ManagedBeforeKick - managedAfterKick;
        if (removedManaged > 0)
        {
            ConVarRefAbstract botQuota("bot_quota");
            if (botQuota.IsValidRef())
            {
                int desiredQuota = m_QuotaBeforeKick - removedManaged;
                if (desiredQuota < 0) desiredQuota = 0;
                if (botQuota.GetInt() != desiredQuota) botQuota.SetInt(desiredQuota);
            }
        }
    }

    m_ManagedBeforeKick = 0;
    m_QuotaBeforeKick = -1;
    m_AdjustQuotaAfterKick = false;
    META_CONPRINTF("[BOTHIDER] kick POST '%s' redisguised=%d quota=%d\n", commandName, redisguised, managedAfterKick);
    RETURN_META(MRES_IGNORED);
}

// Toggles the global disguise state
void HiderPlugin::SetDisguiseEnabled(bool enabled)
{
    if (m_bDisguiseEnabled == enabled) return;
    m_bDisguiseEnabled = enabled;

    int managed = 0;
    for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
    {
        if (Manager().IsManaged(slot)) ++managed;
    }

    if (engine && managed > 0)
    {
        for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
        {
            if (!Manager().IsManaged(slot)) continue;
            void* client = entity_access::ResolveClientBySlot(slot);
            if (!client) continue;
            ssc::SetFakePlayer(client);
            identity_runtime::SetControllerFakeClientFlag(slot, true);
        }

        m_bRebuilding = true;
        const int quota = identity_runtime::CountHumanClients() + managed;
        char quotaCommand[48];
        std::snprintf(quotaCommand, sizeof(quotaCommand), "bot_quota %d\n", quota);
        engine->ServerCommand("bot_kick\n");
        engine->ServerCommand(quotaCommand);
        META_CONPRINTF("[BOTHIDER] disguise %s rebuilding %d bot(s), "
                       "quota=%d\n",
                       enabled ? "ON" : "OFF", managed, quota);
        return;
    }

    for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
    {
        if (!Manager().IsManaged(slot)) continue;
        void* client = entity_access::ResolveClientBySlot(slot);
        if (!client) continue;
        if (enabled)
        {
            ssc::ClearFakePlayer(client);
            identity_runtime::SetControllerFakeClientFlag(slot, false);
            const uint64_t steamId = Manager().GetSyntheticSid(slot);
            if (steamId != 0) ssc::WriteSteamId(client, steamId);
        }
        else
        {
            ssc::SetFakePlayer(client);
            identity_runtime::SetControllerFakeClientFlag(slot, true);
            ssc::WriteSteamId(client, 0);
        }
        entity_access::RefreshClientUserInfo(slot);
    }
    META_CONPRINTF("[BOTHIDER] disguise %s (no rebuild)\n", enabled ? "ON" : "OFF");
}

// Rebuilds every managed bot while preserving the live quota
void HiderPlugin::RebuildBots()
{
    if (m_bSelfDisabled || !m_bDisguiseEnabled || !engine || m_bRebuilding)
    {
        return;
    }

    int managed = 0;
    for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
    {
        if (!Manager().IsManaged(slot)) continue;
        void* client = entity_access::ResolveClientBySlot(slot);
        if (client)
        {
            ssc::SetFakePlayer(client);
            identity_runtime::SetControllerFakeClientFlag(slot, true);
        }
        ++managed;
    }
    if (managed == 0) return;

    int quota = managed;
    ConVarRefAbstract botQuota("bot_quota");
    if (botQuota.IsValidRef()) quota = botQuota.GetInt();

    m_bRebuilding = true;
    char quotaCommand[48];
    std::snprintf(quotaCommand, sizeof(quotaCommand), "bot_quota %d\n", quota);
    engine->ServerCommand("bot_quota 0\n");
    engine->ServerCommand("bot_kick all\n");
    engine->ServerCommand(quotaCommand);
    META_CONPRINTF("[BOTHIDER] rematch rebuild kicked %d bot(s), "
                   "bot_quota->%d\n",
                   managed, quota);
}

// Clears managed state before a level transition
CUtlVector<INetworkGameClient*>*
HiderPlugin::Hook_StartChangeLevel_Pre(const char* mapName, const char* landmark, void* /*changelevelState*/)
{
    if (m_bSelfDisabled) RETURN_META_VALUE(MRES_IGNORED, nullptr);

    identity_state::ClearAll();
    Manager().ReleaseAll();
    avatar::ProcessOverrides();
    BotInfo().ResetAssignments();
    META_CONPRINTF("[BOTHIDER] StartChangeLevel PRE map='%s' landmark='%s'\n", mapName ? mapName : "?", landmark ? landmark : "");
    RETURN_META_VALUE(MRES_IGNORED, nullptr);
}

// Drives deferred cleanup and shared-memory commands each frame
void HiderPlugin::Hook_GameFrame_Post(bool simulating, bool /*bFirst*/, bool /*bLast*/)
{
    if (m_bSelfDisabled || !simulating) RETURN_META(MRES_IGNORED);

    identity_runtime::DrainPendingControllerRemovals();
    for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
    {
        if (!Manager().IsManaged(slot)) continue;
        void* client = entity_access::ResolveClientBySlot(slot);
        if (client)
        {
            identity_runtime::ReleaseManagedHltvSlot(slot, client);
        }
    }
    Manager().OnTick();

    if ((++m_TickCounter & 63u) == 0u)
    {
        for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
        {
            if (!Manager().IsManaged(slot)) continue;
            void* client = entity_access::ResolveClientBySlot(slot);
            if (client) entity_access::ResetIdleTimerForClient(client);
        }
    }

    Publisher().DrainCommands(
        // Updates both engine SteamID fields and userinfo
        [this](int slot, uint64_t steamId) {
        if (!Manager().IsManaged(slot)) return;
        void* client = entity_access::ResolveClientBySlot(slot);
        if (!client) return;
        const uint64_t uniqueSteamId = identity_runtime::MakeUniqueSteamId(slot, steamId);
        if (m_bDisguiseEnabled)
        {
            ssc::ClearFakePlayer(client);
            identity_runtime::SetControllerFakeClientFlag(slot, false);
        }
        ssc::WriteSteamId(client, uniqueSteamId);
        Manager().SetSyntheticSid(slot, uniqueSteamId);
        Publisher().UpdateSyntheticSid(slot, uniqueSteamId);
        entity_access::RefreshClientUserInfo(slot);
    },
        // Updates the engine and published persona name
        [this](int slot, const char* name) {
        if (!Manager().IsManaged(slot) || !name || !name[0])
        {
            return;
        }
        void* client = entity_access::ResolveClientBySlot(slot);
        if (!client) return;
        entity_access::SetEngineName(client, name);
        Personas().MarkSlotManaged(slot, name);
        Publisher().UpdatePersonaName(slot, name);
    },
        // Toggles the global disguise state
        [this](bool enabled) {
        SetDisguiseEnabled(enabled);
    },
        // Rebuilds managed bots
        [this]() {
        RebuildBots();
    },
        // Changes the display-name source
        [this](bool useBotInfo) {
        SetUseBotInfoName(useBotInfo);
        META_CONPRINTF("[BOTHIDER] name source -> %s\n", useBotInfo ? "bot_info" : "botprofile");
    });
    avatar::ProcessOverrides();
    RETURN_META(MRES_IGNORED);
}

} // namespace cs2bh
