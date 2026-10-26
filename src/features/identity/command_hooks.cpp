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

// Check for a client-removal command.
bool IsKickCommand(const char* name)
{
    if (!name || !name[0]) return false;
    return !std::strcmp(name, "kickid") || !std::strcmp(name, "kick") || !std::strcmp(name, "bot_kick") || !std::strcmp(name, "banid");
}

// Check for a bot-add command.
bool IsBotAddCommand(const char* name)
{
    if (!name || !name[0]) return false;
    return !std::strcmp(name, "bot_add") || !std::strcmp(name, "bot_add_t") || !std::strcmp(name, "bot_add_ct");
}

// Find a managed slot by persona name.
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

// Find a managed slot by user ID.
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

// Capture the bot's client, user ID, and lifetime for deferred removal.
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

// Validate slot ownership and dispatch bot_kick with separate arguments.
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
        // Validate lifetime metadata when available.
        if (userId != kick.userId || (kick.incarnation != 0 && Publisher().GetIncarnation(kick.slot) != kick.incarnation)) continue;

        const std::string name = Personas().GetSlotName(kick.slot);
        if (name.empty()) continue;
        const char* argv[] = { "bot_kick", name.c_str() };
        const CCommand arguments(2, argv);
        if (arguments.ArgC() != 2)
        {
            BH_LOG_WARN("bot_kick rejected slot=%d: argument limit", kick.slot);
            continue;
        }
        const CCommandContext context(CT_NO_TARGET, CPlayerSlot(-1));
        g_icvar->DispatchConCommand(command, context, arguments);
    }
}

} // namespace

// Begin a population-command identity transaction.
KHook::Return<void>
HiderPlugin::HookDispatchConCommandPre(ICvar*, ConCommandRef command, const CCommandContext&, const CCommand& arguments) noexcept
{
    const hooks::CallbackScope callbackScope;
    hooks::BeginDispatch();
    g_populationCommandFrames.push_back(false);
    if (!command.IsValidRef()) return { KHook::Action::Ignore };
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

// End the transaction after nested quota work completes.
KHook::Return<void> HiderPlugin::HookDispatchConCommandPost(ICvar*, ConCommandRef, const CCommandContext&, const CCommand&) noexcept
{
    const hooks::CallbackScope callbackScope;
    const bool populationCommand = g_populationCommandFrames.back();
    g_populationCommandFrames.pop_back();
    if (populationCommand) identity_hooks::EndPopulationTransaction(IsDisguiseEnabled());
    hooks::EndDispatch();
    return { KHook::Action::Ignore };
}

// Set the global identity mode.
void HiderPlugin::SetIdentityMode(IdentityMode mode)
{
    if (m_identityMode == mode) return;
    m_identityMode = mode;
    identity_runtime::ApplyManagedDisguise(mode == IdentityMode::Player);
    BH_LOG_DEBUG("identity mode=%s", mode == IdentityMode::Bot ? "bot" : "player");
}

// Restore native identity and clear state before level change.
KHook::Return<CServerChangelevelState*>
HiderPlugin::HookStartChangeLevelPre(INetworkGameServer*,
                                     const char* mapName,
                                     const char* landmark,
                                     void* /*changelevelState*/) noexcept // NOLINT(readability-make-member-function-const)
{
    const hooks::CallbackScope callbackScope;
    const int restoredClients = identity_runtime::RestoreManagedClientsForEngineTeardown();
    identity_state::ClearAll();
    Manager().ReleaseAll();
    avatar::ProcessOverrides();
    BotInfo().ResetAssignments();
    BH_LOG_DEBUG("level change restored=%d map='%s' landmark='%s'", restoredClients, mapName ? mapName : "?", landmark ? landmark : "");
    return { KHook::Action::Ignore, nullptr };
}

// Process frame cleanup and bridge commands.
KHook::Return<void> HiderPlugin::HookGameFramePost(IServerGameDLL*, bool simulating, bool /*firstTick*/, bool /*lastTick*/) noexcept
{
    const hooks::CallbackScope callbackScope;
    if (!m_unloadPending) DrainPendingBotKicks();
    ProcessPendingUnload();
    if (!simulating) return { KHook::Action::Ignore };

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

    DrainBridgeCommands();
    avatar::ProcessOverrides();
    return { KHook::Action::Ignore };
}

// Route shared-memory commands.
void HiderPlugin::DrainBridgeCommands()
{
    Publisher().DrainCommands([this](int slot, uint64_t steamId) {
        ApplySteamIdCommand(slot, steamId);
    }, [this](int slot, const char* name) {
        ApplyPersonaNameCommand(slot, name);
    }, [this](bool botMode) {
        SetIdentityMode(botMode ? IdentityMode::Bot : IdentityMode::Player);
    }, [this](bool useBotInfo) {
        SetUseBotInfoName(useBotInfo);
        BH_LOG_DEBUG("name source=%s", useBotInfo ? "bot_info" : "botprofile");
    });
}

// Apply the resolved SteamID, then publish it.
void HiderPlugin::ApplySteamIdCommand(int slot, uint64_t steamId)
{
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
}

// Update engine and slot names, then publish.
void HiderPlugin::ApplyPersonaNameCommand(int slot, const char* name)
{
    if (!Manager().IsManaged(slot) || !name || !name[0]) return;
    void* client = entity_access::ResolveClientBySlot(slot);
    if (!client) return;
    entity_access::SetEngineName(client, name);
    Personas().SetSlotName(slot, name);
    Publisher().UpdatePersonaName(slot, name);
}

} // namespace cs2bh
