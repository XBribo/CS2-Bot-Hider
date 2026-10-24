#include "core/log.h"
#include "ISmmPlugin.h"
#include "plugin.h"

#include "avatar_override.h"
#include "bot_info.h"
#include "core/interfaces.h"
#include "entity_access.h"
#include "fake_client_manager.h"
#include "identity_hooks.h"
#include "identity_runtime.h"
#include "identity_state.h"
#include "personas.h"
#include "serversideclient_ref.h"
#include "slot_publisher.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <icvar.h>
#include <tier0/convar.h>

namespace cs2bh {

namespace {

thread_local std::vector<bool> g_populationCommandFrames;

// Returns whether a console command disconnects a client
bool IsKickCommand(const char* name)
{
    if (!name || !name[0]) return false;
    return !std::strcmp(name, "kickid") || !std::strcmp(name, "kick") || !std::strcmp(name, "bot_kick") || !std::strcmp(name, "banid");
}

// Returns whether a command adds a bot
bool IsBotAddCommand(const char* name)
{
    if (!name || !name[0]) return false;
    return !std::strcmp(name, "bot_add") || !std::strcmp(name, "bot_add_t") || !std::strcmp(name, "bot_add_ct");
}

// Finds a managed slot from its current persona name
int FindManagedSlotByPersonaName(const char* name)
{
    if (!name || !name[0]) return -1;
    for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
    {
        if (!Manager().IsManaged(slot)) continue;
        if (Personas().GetSlotName(slot) == name) return slot;
    }
    return -1;
}

// Finds a managed slot from its current user ID
int FindManagedSlotByUserId(const char* value)
{
    if (!value || !value[0]) return -1;

    char* end = nullptr;
    const long userId = std::strtol(value, &end, 10);
    if (!end || end == value || end[0] != '\0' || userId < 0 || userId > UINT16_MAX) return -1;

    for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
    {
        if (!Manager().IsManaged(slot)) continue;
        void* client = entity_access::ResolveClientBySlot(slot);
        if (!client) continue;
        const auto* raw = reinterpret_cast<const unsigned char*>(client);
        if (*reinterpret_cast<const uint16_t*>(raw + ssc::g_userIdOffset) == static_cast<uint16_t>(userId)) return slot;
    }
    return -1;
}

// Captures the native target without placing its name in the engine text queue.
bool QueueManagedBotKick(int slot)
{
    if (!g_icvar || !Manager().IsManaged(slot) || !g_icvar->FindConCommand("bot_kick").IsValidRef()) return false;
    void* client = entity_access::ResolveClientBySlot(slot);
    if (!client || ssc::IsHltv(client) || Personas().GetSlotName(slot).empty()) return false;

    const uint16_t userId = *reinterpret_cast<const uint16_t*>(static_cast<const unsigned char*>(client) + ssc::g_userIdOffset);
    identity_state::PendingBotKicks().push_back(
        { .slot = slot, .client = client, .userId = userId, .incarnation = Publisher().GetIncarnation(slot) });
    return true;
}

// Dispatches argv directly after rechecking that each queued bot still owns its slot.
void DrainPendingBotKicks()
{
    auto& queued = identity_state::PendingBotKicks();
    if (queued.empty()) return;
    std::vector<identity_state::PendingBotKick> pending;
    pending.swap(queued);
    if (!g_icvar) return;
    const ConCommandRef command = g_icvar->FindConCommand("bot_kick");
    if (!command.IsValidRef()) return;

    for (const auto& kick : pending)
    {
        if (!Manager().IsManaged(kick.slot)) continue;
        void* client = entity_access::ResolveClientBySlot(kick.slot);
        if (!client || client != kick.client || ssc::IsHltv(client)) continue;
        const uint16_t userId = *reinterpret_cast<const uint16_t*>(static_cast<const unsigned char*>(client) + ssc::g_userIdOffset);
        // Native identity checks still work when the optional shared-memory bridge is unavailable.
        if (userId != kick.userId || (kick.incarnation != 0 && Publisher().GetIncarnation(kick.slot) != kick.incarnation)) continue;

        const std::string name = Personas().GetSlotName(kick.slot);
        if (name.empty()) continue;
        const char* argv[] = { "bot_kick", name.c_str() };
        const CCommand arguments(2, argv);
        if (arguments.ArgC() != 2)
        {
            BH_LOG_WARN("deferred bot_kick rejected slot=%d: command arguments exceed SDK limits\n", kick.slot);
            continue;
        }
        const CCommandContext context(CT_NO_TARGET, CPlayerSlot(-1));
        g_icvar->DispatchConCommand(command, context, arguments);
    }
}

} // namespace

// Opens one identity transaction for the complete engine population command.
KHook::Return<void>
HiderPlugin::HookDispatchConCommandPre(ICvar*, ConCommandRef command, const CCommandContext&, const CCommand& arguments) noexcept
{
    const hooks::CallbackScope callbackScope;
    hooks::BeginDispatch();
    g_populationCommandFrames.push_back(false);
    if (m_selfDisabled || !command.IsValidRef()) return { KHook::Action::Ignore };
    const char* commandName = command.GetName();

    if (IsBotAddCommand(commandName))
    {
        if (IsDisguiseEnabled())
        {
            identity_hooks::BeginPopulationTransaction(true);
            g_populationCommandFrames.back() = true;
        }
        return { KHook::Action::Ignore };
    }

    if (!std::strcmp(commandName, "kick"))
    {
        const char* target = arguments.ArgC() >= 2 ? arguments.Arg(1) : "";
        if (QueueManagedBotKick(FindManagedSlotByPersonaName(target)))
        {
            return { KHook::Action::Supersede };
        }
    }

    if (!std::strcmp(commandName, "kickid"))
    {
        const char* target = arguments.ArgC() >= 2 ? arguments.Arg(1) : "";
        if (QueueManagedBotKick(FindManagedSlotByUserId(target))) return { KHook::Action::Supersede };
    }

    if (!IsKickCommand(commandName))
    {
        return { KHook::Action::Ignore };
    }

    if (IsDisguiseEnabled())
    {
        identity_hooks::BeginPopulationTransaction(true);
        g_populationCommandFrames.back() = true;
    }
    return { KHook::Action::Ignore };
}

// Closes the transaction after the engine command and any nested quota pass complete.
KHook::Return<void> HiderPlugin::HookDispatchConCommandPost(ICvar*, ConCommandRef, const CCommandContext&, const CCommand&) noexcept
{
    const hooks::CallbackScope callbackScope;
    const bool populationCommand = g_populationCommandFrames.back();
    g_populationCommandFrames.pop_back();
    if (populationCommand) identity_hooks::EndPopulationTransaction(IsDisguiseEnabled());
    hooks::EndDispatch();
    return { KHook::Action::Ignore };
}

// Changes the global managed-bot identity mode
void HiderPlugin::SetIdentityMode(IdentityMode mode)
{
    if (m_identityMode == mode) return;
    m_identityMode = mode;
    identity_runtime::ApplyManagedDisguise(mode == IdentityMode::Player);
    BH_LOG_DEBUG("identity mode=%s\n", mode == IdentityMode::Bot ? "bot" : "player");
}

// Restores native bot identity and clears managed state before a level transition
KHook::Return<CServerChangelevelState*>
HiderPlugin::HookStartChangeLevelPre(INetworkGameServer*,
                                     const char* mapName,
                                     const char* landmark,
                                     void* /*changelevelState*/) noexcept // NOLINT(readability-make-member-function-const)
{
    const hooks::CallbackScope callbackScope;
    if (m_selfDisabled) return { KHook::Action::Ignore, nullptr };

    const int restoredClients = identity_runtime::RestoreManagedClientsForEngineTeardown();
    identity_state::ClearAll();
    Manager().ReleaseAll();
    avatar::ProcessOverrides();
    BotInfo().ResetAssignments();
    BH_LOG_DEBUG("StartChangeLevel PRE restored=%d map='%s' landmark='%s'\n", restoredClients, mapName ? mapName : "?",
                 landmark ? landmark : "");
    return { KHook::Action::Ignore, nullptr };
}

// Drives deferred cleanup and shared-memory commands each frame
KHook::Return<void> HiderPlugin::HookGameFramePost(IServerGameDLL*, bool simulating, bool /*firstTick*/, bool /*lastTick*/) noexcept
{
    const hooks::CallbackScope callbackScope;
    if (!m_selfDisabled && !m_unloadPending) DrainPendingBotKicks();
    ProcessPendingUnload();
    if (m_selfDisabled || !simulating) return { KHook::Action::Ignore };

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

    if ((++m_tickCounter & 63U) == 0U)
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
        BH_LOG_DEBUG("name source -> %s\n", useBotInfo ? "bot_info" : "botprofile");
    });
    avatar::ProcessOverrides();
    return { KHook::Action::Ignore };
}

} // namespace cs2bh
