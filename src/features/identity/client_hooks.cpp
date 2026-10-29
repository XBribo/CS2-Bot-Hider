#include "network_connection.pb.h"
#include "core/log.h"
#include "playerslot.h"
#include "plugin.h"

#include "bot_info.h"
#include "entity_access.h"
#include "fake_client_manager.h"
#include "identity_hooks.h"
#include "identity_runtime.h"
#include "identity_state.h"
#include "serversideclient_ref.h"
#include "steam/steamtypes.h"

#include <cstdint>
#include <string>

namespace cs2bh {

namespace {

// Check for a retained controller after disconnect.
bool IsTargetedClientRemovalReason(ENetworkDisconnectionReason reason)
{
    switch (reason)
    {
        case NETWORK_DISCONNECT_KICKED:
        case NETWORK_DISCONNECT_BANADDED:
        case NETWORK_DISCONNECT_KICKBANADDED:
            return true;
        default:
            break;
    }

    const int value = static_cast<int>(reason);
    return value >= static_cast<int>(NETWORK_DISCONNECT_KICKED_TEAMKILLING) &&
           value <= static_cast<int>(NETWORK_DISCONNECT_KICKED_INSECURECLIENT);
}

} // namespace

// Adopt fake clients from the connected slot.
KHook::Return<void> HiderPlugin::HookOnClientConnectedPost( // NOLINT(readability-make-member-function-const)
    IServerGameClients*,
    CPlayerSlot slot,
    const char* name,
    uint64 /*xuid*/,
    const char* networkId,
    const char* /*address*/,
    bool fakePlayer) noexcept
{
    const hooks::CallbackScope callbackScope;
    if (!fakePlayer || identity_runtime::IsHltvConnection(name, networkId))
    {
        return { KHook::Action::Ignore };
    }

    const int index = slot.Get();
    if (index < 0 || index >= bridge::kMaxSlots || Manager().IsManaged(index))
    {
        return { KHook::Action::Ignore };
    }

    void* client = entity_access::ResolveClientBySlot(index);
    if (!client || ssc::IsHltv(client)) return { KHook::Action::Ignore };

    AdoptClient(index, client, name);
    return { KHook::Action::Ignore };
}

bool HiderPlugin::AdoptClient(int index, void* client, const char* name)
{
    std::unordered_set<uint64_t> occupied;
    if (!identity_runtime::CollectOccupiedSteamIds(index, occupied))
    {
        BH_LOG_WARN("bot identity skipped slot=%d: availability check failed", index);
        return false;
    }
    const BotEntry* entry = BotInfo().PickForBot(name, occupied);
    if (!entry)
    {
        BH_LOG_WARN("bot identity skipped slot=%d: configured pool exhausted", index);
        return false;
    }
    const char* displayName = m_useBotInfoName || !name || !name[0] ? entry->name.c_str() : name;
    const uint64_t steamId = entry->steamId64;
    Manager().AdoptSlot(index, displayName, steamId, entry->crosshairCode.c_str(), entry->scoreboardFlair, entry->clanTag.tag.c_str(),
                        entry->clanTag.groupId);
    identity_state::BindSlot(index, entry, name);
    ssc::WriteSteamId(client, steamId);

    if (IsDisguiseEnabled())
    {
        ssc::ClearFakePlayer(client);
        identity_runtime::SetControllerFakeClientFlag(index, identity_hooks::PopulationTransactionActive());
    }

    return true;
}

void HiderPlugin::AdoptExistingBots()
{
    for (int slot = 0; slot < bridge::kMaxSlots; ++slot)
    {
        void* client = entity_access::ResolveClientBySlot(slot);
        if (!client || !ssc::IsFakePlayerSet(client) || ssc::IsHltv(client)) continue;
        const char* name = ssc::ReadName(client);
        if (identity_runtime::IsHltvConnection(name, nullptr)) continue;
        if (AdoptClient(slot, client, name))
        {
            const std::string visibleName = Manager().GetSlotName(slot);
            entity_access::SetEngineName(client, visibleName.c_str());
        }
    }
}

// Reapply managed identity on server entry.
KHook::Return<void> HiderPlugin::HookClientPutInServerPost(IServerGameClients*,
                                                           CPlayerSlot slot,
                                                           char const* name,
                                                           int type,
                                                           uint64 /*xuid*/) noexcept // NOLINT(readability-make-member-function-const)
{
    const hooks::CallbackScope callbackScope;
    (void)type;
    const int index = slot.Get();
    if (index < 0 || index >= bridge::kMaxSlots || !Manager().IsManaged(index))
    {
        return { KHook::Action::Ignore };
    }

    void* client = entity_access::ResolveClientBySlot(index);
    if (!client) return { KHook::Action::Ignore };
    if (identity_runtime::ReleaseManagedHltvSlot(index, client))
    {
        return { KHook::Action::Ignore };
    }

    const uint64_t desiredSteamId = Manager().GetSyntheticSid(index);
    const auto availability = identity_runtime::CheckSteamIdAvailability(index, desiredSteamId);
    if (availability != identity_runtime::SteamIdAvailability::Available)
    {
        BH_LOG_WARN("bot identity released slot=%d: %s", index,
                    availability == identity_runtime::SteamIdAvailability::InUse ? "SteamID already in use" : "availability check failed");
        ssc::SetFakePlayer(client);
        identity_runtime::SetControllerFakeClientFlag(index, true);
        ssc::WriteSteamId(client, 0);
        entity_access::RefreshClientUserInfo(index);
        BotInfo().ReleaseAssignment(identity_state::SlotEntry(index));
        identity_state::ClearSlot(index);
        Manager().ReleaseSlot(index);
        return { KHook::Action::Ignore };
    }

    if (IsDisguiseEnabled())
    {
        ssc::ClearFakePlayer(client);
        identity_runtime::SetControllerFakeClientFlag(index, identity_hooks::PopulationTransactionActive());
    }
    ssc::WriteSteamId(client, desiredSteamId);

    std::string visibleName = Manager().GetSlotName(index);
    if (visibleName.empty() && name) visibleName = name;
    if (!visibleName.empty())
    {
        entity_access::SetEngineName(client, visibleName.c_str());
    }
    else
    {
        entity_access::RefreshClientUserInfo(index);
    }

    return { KHook::Action::Ignore };
}

// Restore and release identity before disconnect teardown.
KHook::Return<void> HiderPlugin::HookClientDisconnectPre( // NOLINT(readability-make-member-function-const)
    IServerGameClients*,
    CPlayerSlot slot,
    ENetworkDisconnectionReason reason,
    const char* /*name*/,
    uint64 /*xuid*/,
    const char* /*networkId*/) noexcept
{
    const hooks::CallbackScope callbackScope;
    const int index = slot.Get();
    if (index < 0 || index >= bridge::kMaxSlots || !Manager().IsManaged(index))
    {
        return { KHook::Action::Ignore };
    }

    void* client = entity_access::ResolveClientBySlot(index);
    if (client)
    {
        ssc::SetFakePlayer(client);
        identity_runtime::SetControllerFakeClientFlag(index, true);
        ssc::WriteSteamId(client, 0);
        if (IsTargetedClientRemovalReason(reason))
        {
            identity_runtime::QueueControllerRemovalForClient(client, index);
        }
    }

    BotInfo().ReleaseAssignment(identity_state::SlotEntry(index));
    identity_state::ClearSlot(index);
    Manager().ReleaseSlot(index);

    return { KHook::Action::Ignore };
}

} // namespace cs2bh
