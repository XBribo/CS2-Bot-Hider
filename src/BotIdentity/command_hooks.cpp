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

// Opens one identity transaction for the complete engine population command.
void HiderPlugin::Hook_DispatchConCommand_Pre(ConCommandRef command, const CCommandContext&, const CCommand& arguments)
{
    if (m_bSelfDisabled || !command.IsValidRef()) RETURN_META(MRES_IGNORED);
    const char* commandName = command.GetName();

    if (IsBotAddCommand(commandName))
    {
        identity_hooks::BeginPopulationTransaction(IsDisguiseEnabled());
        ++m_PopulationCommandDepth;
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
                RETURN_META(MRES_SUPERCEDE);
            }
        }
    }

    if (!IsKickCommand(commandName)) RETURN_META(MRES_IGNORED);

    identity_hooks::BeginPopulationTransaction(IsDisguiseEnabled());
    ++m_PopulationCommandDepth;
    RETURN_META(MRES_IGNORED);
}

// Closes the transaction after the engine command and any nested quota pass complete.
void HiderPlugin::Hook_DispatchConCommand_Post(ConCommandRef command, const CCommandContext&, const CCommand& /*arguments*/)
{
    if (m_bSelfDisabled || !command.IsValidRef()) RETURN_META(MRES_IGNORED);
    const char* commandName = command.GetName();

    if (IsBotAddCommand(commandName))
    {
        if (m_PopulationCommandDepth != 0)
        {
            --m_PopulationCommandDepth;
            identity_hooks::EndPopulationTransaction(IsDisguiseEnabled());
        }
        RETURN_META(MRES_IGNORED);
    }

    if (!IsKickCommand(commandName)) RETURN_META(MRES_IGNORED);
    if (m_PopulationCommandDepth != 0)
    {
        --m_PopulationCommandDepth;
        identity_hooks::EndPopulationTransaction(IsDisguiseEnabled());
    }
    RETURN_META(MRES_IGNORED);
}

// Changes the global managed-bot identity mode
void HiderPlugin::SetIdentityMode(IdentityMode mode)
{
    if (m_IdentityMode == mode) return;
    m_IdentityMode = mode;
    identity_runtime::ApplyManagedDisguise(mode == IdentityMode::Player);
    META_CONPRINTF("[BOTHIDER] identity mode=%s\n", mode == IdentityMode::Bot ? "bot" : "player");
}

// Restores native bot identity and clears managed state before a level transition
CUtlVector<INetworkGameClient*>*
HiderPlugin::Hook_StartChangeLevel_Pre(const char* mapName, const char* landmark, void* /*changelevelState*/)
{
    if (m_bSelfDisabled) RETURN_META_VALUE(MRES_IGNORED, nullptr);

    const int restoredClients = identity_runtime::RestoreManagedClientsForEngineTeardown();
    identity_state::ClearAll();
    Manager().ReleaseAll();
    avatar::ProcessOverrides();
    BotInfo().ResetAssignments();
    META_CONPRINTF("[BOTHIDER] StartChangeLevel PRE restored=%d map='%s' landmark='%s'\n", restoredClients,
                   mapName ? mapName : "?", landmark ? landmark : "");
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
        if (IsDisguiseEnabled())
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
        // Changes the global identity mode
        [this](bool botMode) {
        SetIdentityMode(botMode ? IdentityMode::Bot : IdentityMode::Player);
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
