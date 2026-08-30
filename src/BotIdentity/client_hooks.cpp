#include "plugin.h"

#include "bot_info.h"
#include "entity_access.h"
#include "fake_client_manager.h"
#include "identity_hooks.h"
#include "identity_runtime.h"
#include "identity_state.h"
#include "personas.h"
#include "serversideclient_ref.h"
#include "slot_publisher.h"

#include <cstdint>
#include <string>

#include <iserver.h>

namespace cs2bh {

// Checks whether a disconnect may leave its controller behind
static bool IsTargetedClientRemovalReason(ENetworkDisconnectionReason reason)
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

// Adopts fake clients from the authoritative connected slot
void HiderPlugin::HookOnClientConnectedPost(
    CPlayerSlot slot, const char* pszName, uint64 /*xuid*/, const char* pszNetworkID, const char* /*pszAddress*/, bool bFakePlayer)
{
    if (m_selfDisabled || !IsLifecycleActive() || !bFakePlayer || identity_runtime::IsHltvConnection(pszName, pszNetworkID))
    {
        RETURN_META(MRES_IGNORED);
    }

    const int index = slot.Get();
    if (index < 0 || index >= PersonaPool::kMaxSlots || Manager().IsManaged(index))
    {
        RETURN_META(MRES_IGNORED);
    }

    void* client = entity_access::ResolveClientBySlot(index);
    if (!client || ssc::IsHltv(client)) RETURN_META(MRES_IGNORED);

    const BotEntry* entry = BotInfo().PickForBot(pszName);
    std::string displayName;
    if (m_useBotInfoName && entry) displayName = entry->name;
    else if (pszName && pszName[0])
        displayName = pszName;
    else if (entry)
        displayName = entry->name;
    else
        displayName = Personas().PickFromRoster();

    if (displayName.empty())
    {
        BotInfo().ReleaseAssignment(entry);
        RETURN_META(MRES_IGNORED);
    }

    const uint64_t configuredSteamId = entry && entry->steamId64 != 0 ? entry->steamId64 : 0;
    const char* crosshairCode = entry ? entry->crosshairCode.c_str() : nullptr;
    const uint32_t scoreboardFlair = entry ? entry->scoreboardFlair : 0;
    const uint64_t steamId = identity_runtime::MakeUniqueSteamId(index, configuredSteamId);
    if (!Manager().AdoptSlot(index, displayName.c_str(), steamId, crosshairCode, scoreboardFlair))
    {
        BotInfo().ReleaseAssignment(entry);
        RETURN_META(MRES_IGNORED);
    }

    identity_state::BindSlot(index, entry, pszName);

    if (steamId != 0)
    {
        ssc::WriteSteamId(client, steamId);
        Manager().SetSyntheticSid(index, steamId);
        Publisher().UpdateBaseSyntheticSid(index, steamId);
    }

    if (IsDisguiseEnabled())
    {
        ssc::ClearFakePlayer(client);
        identity_runtime::SetControllerFakeClientFlag(index, identity_hooks::PopulationTransactionActive());
    }

    RETURN_META(MRES_IGNORED);
}

// Reapplies managed identity after a client enters the server
void HiderPlugin::HookClientPutInServerPost(CPlayerSlot slot, char const* pszName, int type, uint64 /*xuid*/)
{
    if (m_selfDisabled || !IsLifecycleActive()) RETURN_META(MRES_IGNORED);

    (void)type;
    const int index = slot.Get();
    if (index < 0 || index >= PersonaPool::kMaxSlots || !Personas().IsSlotManaged(index))
    {
        RETURN_META(MRES_IGNORED);
    }

    void* client = entity_access::ResolveClientBySlot(index);
    if (!client) RETURN_META(MRES_IGNORED);
    if (identity_runtime::ReleaseManagedHltvSlot(index, client))
    {
        RETURN_META(MRES_IGNORED);
    }

    if (IsDisguiseEnabled())
    {
        ssc::ClearFakePlayer(client);
        identity_runtime::SetControllerFakeClientFlag(index, identity_hooks::PopulationTransactionActive());
    }

    const uint64_t desiredSteamId = Manager().GetSyntheticSid(index);
    const uint64_t steamId = identity_runtime::MakeUniqueSteamId(index, desiredSteamId);
    if (steamId != 0)
    {
        ssc::WriteSteamId(client, steamId);
        Manager().SetSyntheticSid(index, steamId);
        Publisher().UpdateBaseSyntheticSid(index, steamId);
    }

    std::string visibleName = Personas().GetSlotName(index);
    if (visibleName.empty() && pszName) visibleName = pszName;
    if (!visibleName.empty())
    {
        entity_access::SetEngineName(client, visibleName.c_str());
    }
    else
    {
        entity_access::RefreshClientUserInfo(index);
    }

    RETURN_META(MRES_IGNORED);
}

// Restores and releases managed identity before disconnect teardown
void HiderPlugin::HookClientDisconnectPre(
    CPlayerSlot slot, ENetworkDisconnectionReason reason, const char* /*pszName*/, uint64 /*xuid*/, const char* /*pszNetworkID*/)
{
    if (m_selfDisabled) RETURN_META(MRES_IGNORED);

    const int index = slot.Get();
    if (!IsLifecycleActive())
    {
        // During teardown the client pointer may already be invalid. Release
        // only BotHider-owned containers; level shutdown drains the rest.
        if (index >= 0 && index < PersonaPool::kMaxSlots && Personas().IsSlotManaged(index))
        {
            BotInfo().ReleaseAssignment(identity_state::SlotEntry(index));
            identity_state::ClearSlot(index);
            Manager().ReleaseSlot(index);
        }
        RETURN_META(MRES_IGNORED);
    }

    if (index < 0 || index >= PersonaPool::kMaxSlots || !Personas().IsSlotManaged(index))
    {
        RETURN_META(MRES_IGNORED);
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

    RETURN_META(MRES_IGNORED);
}

} // namespace cs2bh
