#include "plugin.h"

#include "bot_info.h"
#include "entity_access.h"
#include "fake_client_manager.h"
#include "identity_runtime.h"
#include "identity_state.h"
#include "personas.h"
#include "serversideclient_ref.h"
#include "slot_publisher.h"

#include <cstdint>
#include <string>

#include <iserver.h>

namespace cs2bh
{

    // Checks whether a disconnect may leave its controller behind
    static bool IsTargetedClientRemovalReason(
        ENetworkDisconnectionReason reason)
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
        return value >=
                   static_cast<int>(
                       NETWORK_DISCONNECT_KICKED_TEAMKILLING) &&
               value <=
                   static_cast<int>(
                       NETWORK_DISCONNECT_KICKED_INSECURECLIENT);
    }

    // Adopts fake clients from the authoritative connected slot
    void HiderPlugin::Hook_OnClientConnected_Post(
        CPlayerSlot slot,
        const char *pszName,
        uint64 /*xuid*/,
        const char *pszNetworkID,
        const char * /*pszAddress*/,
        bool bFakePlayer)
    {
        if (m_bSelfDisabled || !bFakePlayer ||
            identity_runtime::IsHltvConnection(
                pszName, pszNetworkID))
        {
            RETURN_META(MRES_IGNORED);
        }

        const int index = slot.Get();
        if (index < 0 || index >= PersonaPool::kMaxSlots ||
            Manager().IsManaged(index))
        {
            RETURN_META(MRES_IGNORED);
        }

        void *client = entity_access::ResolveClientBySlot(index);
        if (!client || ssc::IsHltv(client))
            RETURN_META(MRES_IGNORED);

        const BotEntry *entry = BotInfo().PickForBot(pszName);
        std::string displayName;
        if (m_bUseBotInfoName && entry)
            displayName = entry->Name;
        else if (pszName && pszName[0])
            displayName = pszName;
        else if (entry)
            displayName = entry->Name;
        else
            displayName = Personas().PickFromRoster();

        if (displayName.empty())
        {
            BotInfo().ReleaseAssignment(entry);
            RETURN_META(MRES_IGNORED);
        }

        const uint64_t configuredSteamId =
            entry && entry->SteamId64 != 0 ? entry->SteamId64 : 0;
        const char *crosshairCode =
            entry ? entry->CrosshairCode.c_str() : nullptr;
        const uint32_t scoreboardFlair =
            entry ? entry->ScoreboardFlair : 0;
        const uint64_t steamId =
            identity_runtime::MakeUniqueSteamId(
                index, configuredSteamId);
        if (!Manager().AdoptSlot(
                index,
                displayName.c_str(),
                steamId,
                crosshairCode,
                scoreboardFlair))
        {
            BotInfo().ReleaseAssignment(entry);
            RETURN_META(MRES_IGNORED);
        }

        identity_state::BindSlot(index, entry, pszName);
        if (m_bDisguiseEnabled)
        {
            ssc::ClearFakePlayer(client);
            if (!m_bBotAddInProgress)
            {
                identity_runtime::SetControllerFakeClientFlag(
                    index, false);
            }
        }

        if (steamId != 0)
        {
            ssc::WriteSteamId(client, steamId);
            Manager().SetSyntheticSid(index, steamId);
            Publisher().UpdateBaseSyntheticSid(index, steamId);
        }

        META_CONPRINTF(
            "[BOTHIDER] slot=%d adopted name='%s' steamid64=%llu\n",
            index,
            displayName.c_str(),
            static_cast<unsigned long long>(steamId));
        RETURN_META(MRES_IGNORED);
    }

    // Reapplies managed identity after a client enters the server
    void HiderPlugin::Hook_ClientPutInServer_Post(
        CPlayerSlot slot,
        char const *pszName,
        int type,
        uint64 /*xuid*/)
    {
        if (m_bSelfDisabled)
            RETURN_META(MRES_IGNORED);

        (void)type;
        const int index = slot.Get();
        if (index < 0 || index >= PersonaPool::kMaxSlots ||
            !Personas().IsSlotManaged(index))
        {
            RETURN_META(MRES_IGNORED);
        }

        void *client = entity_access::ResolveClientBySlot(index);
        if (!client)
            RETURN_META(MRES_IGNORED);
        if (identity_runtime::ReleaseManagedHltvSlot(
                index, client))
        {
            RETURN_META(MRES_IGNORED);
        }

        if (m_bDisguiseEnabled)
        {
            ssc::ClearFakePlayer(client);
            identity_runtime::SetControllerFakeClientFlag(
                index, m_bBotAddInProgress);
        }

        const uint64_t desiredSteamId =
            Manager().GetSyntheticSid(index);
        const uint64_t steamId =
            identity_runtime::MakeUniqueSteamId(
                index, desiredSteamId);
        if (steamId != 0)
        {
            ssc::WriteSteamId(client, steamId);
            Manager().SetSyntheticSid(index, steamId);
            Publisher().UpdateBaseSyntheticSid(index, steamId);
        }

        std::string visibleName = Personas().GetSlotName(index);
        if (visibleName.empty() && pszName)
            visibleName = pszName;
        if (!visibleName.empty())
        {
            entity_access::SetEngineName(
                client, visibleName.c_str());
        }
        else
        {
            entity_access::RefreshClientUserInfo(index);
        }

        META_CONPRINTF(
            "[BOTHIDER] CPiS safety-net slot=%d name='%s'\n",
            index,
            pszName ? pszName : "<null>");
        RETURN_META(MRES_IGNORED);
    }

    // Restores and releases managed identity before disconnect teardown
    void HiderPlugin::Hook_ClientDisconnect_Pre(
        CPlayerSlot slot,
        ENetworkDisconnectionReason reason,
        const char * /*pszName*/,
        uint64 /*xuid*/,
        const char * /*pszNetworkID*/)
    {
        if (m_bSelfDisabled)
            RETURN_META(MRES_IGNORED);

        const int index = slot.Get();
        if (index < 0 || index >= PersonaPool::kMaxSlots ||
            !Personas().IsSlotManaged(index))
        {
            RETURN_META(MRES_IGNORED);
        }

        const std::string persona = Personas().GetSlotName(index);
        void *client = entity_access::ResolveClientBySlot(index);
        if (client)
        {
            ssc::SetFakePlayer(client);
            identity_runtime::SetControllerFakeClientFlag(
                index, true);
            ssc::WriteSteamId(client, 0);
            if (IsTargetedClientRemovalReason(reason))
            {
                identity_runtime::QueueControllerRemovalForClient(
                    client, index);
            }
        }

        BotInfo().ReleaseAssignment(
            identity_state::SlotEntry(index));
        identity_state::ClearSlot(index);
        Manager().ReleaseSlot(index);

        META_CONPRINTF(
            "[BOTHIDER] ClientDisconnect slot=%d name='%s' "
            "slot released\n",
            index,
            persona.empty() ? "<null>" : persona.c_str());
        RETURN_META(MRES_IGNORED);
    }

} // namespace cs2bh
