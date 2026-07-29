#include "identity_runtime.h"

#include "bot_info.h"
#include "entity_access.h"
#include "fake_client_manager.h"
#include "identity_hooks.h"
#include "identity_state.h"
#include "personas.h"
#include "plugin.h"
#include "serversideclient_ref.h"
#include "version_targets.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <entity2/entityinstance.h>
#include <iserver.h>
#include <tier1/utlvector.h>

extern INetworkServerService *g_pNetworkServerService;

namespace cs2bh
{

    // Returns whether a SteamID is used by another connected client
    static bool IsSteamIdInUseByOther(uint64_t steamId, int exceptSlot)
    {
        if (steamId == 0 || !g_pNetworkServerService)
            return false;
        auto *gameServer = g_pNetworkServerService->GetIGameServer();
        if (!gameServer)
            return false;
        auto *clients = reinterpret_cast<CUtlVector<void *> *>(
            reinterpret_cast<unsigned char *>(gameServer) +
            targets::kClientListOffset);
        const int count = clients->Count();
        if (count < 0 || count > 256)
            return false;

        for (int slot = 0; slot < count; ++slot)
        {
            if (slot == exceptSlot)
                continue;
            void *client = clients->Element(slot);
            if (!client)
                continue;
            const uint64_t otherSteamId = *reinterpret_cast<uint64_t *>(
                reinterpret_cast<unsigned char *>(client) +
                ssc::OFFSET_m_SteamID);
            if (otherSteamId == steamId)
                return true;
        }
        return false;
    }

    // Resolves the current pawn for one managed bot slot
    static BotPawnRef ResolveManagedBotPawn(int slot)
    {
        BotPawnRef result;
        const int pawnHandleOffset = entity_access::BotPawnHandleOffset();
        if (pawnHandleOffset < 0 || !Manager().IsManaged(slot))
            return result;

        void *client = entity_access::ResolveClientBySlot(slot);
        if (!client)
            return result;

        const int controllerIndex = *reinterpret_cast<int *>(
            reinterpret_cast<unsigned char *>(client) +
            ssc::OFFSET_m_nEntityIndex);
        char className[64];
        void *controller = entity_access::ResolveEntityInstance(
            controllerIndex, className, sizeof(className));
        if (!controller ||
            std::strcmp(className, "cs_player_controller") != 0 ||
            entity_access::IsEntityBeingDeleted(controller))
        {
            return result;
        }

        const uint32_t pawnHandle = *reinterpret_cast<uint32_t *>(
            reinterpret_cast<unsigned char *>(controller) +
            pawnHandleOffset);
        if (pawnHandle == 0xFFFFFFFF)
            return result;

        const int pawnIndex = static_cast<int>(pawnHandle & 0x7FFF);
        void *pawn =
            entity_access::ResolveEntityInstance(pawnIndex, nullptr, 0);
        if (!pawn || entity_access::IsEntityBeingDeleted(pawn))
            return result;

        auto *pawnEntity = reinterpret_cast<CEntityInstance *>(pawn);
        if (static_cast<uint32_t>(pawnEntity->GetRefEHandle().ToInt()) !=
            pawnHandle)
        {
            return result;
        }

        result.Instance = pawn;
        result.Handle = pawnHandle;
        return result;
    }

    // Collects every unique pawn currently owned by managed bots
    static std::vector<BotPawnRef> CollectManagedBotPawns()
    {
        std::vector<BotPawnRef> pawns;
        pawns.reserve(PersonaPool::kMaxSlots);
        for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
        {
            BotPawnRef pawn = ResolveManagedBotPawn(slot);
            if (!pawn.Instance)
                continue;
            auto duplicate = std::find_if(
                pawns.begin(),
                pawns.end(),
                [&pawn](const BotPawnRef &existing)
                {
                    return existing.Instance == pawn.Instance;
                });
            if (duplicate == pawns.end())
                pawns.push_back(pawn);
        }
        return pawns;
    }

    // Checks whether a current client still references one controller
    static bool IsControllerReferencedByClient(
        void *controller,
        uint16_t *userIdOut)
    {
        for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
        {
            void *client = entity_access::ResolveClientBySlot(slot);
            if (!client)
                continue;
            const int entityIndex = *reinterpret_cast<int *>(
                reinterpret_cast<unsigned char *>(client) +
                ssc::OFFSET_m_nEntityIndex);
            char className[64];
            void *current = entity_access::ResolveEntityInstance(
                entityIndex, className, sizeof(className));
            if (current != controller ||
                std::strcmp(className, "cs_player_controller") != 0)
            {
                continue;
            }

            if (userIdOut)
            {
                *userIdOut = *reinterpret_cast<uint16_t *>(
                    reinterpret_cast<unsigned char *>(client) +
                    ssc::OFFSET_m_UserID);
            }
            return true;
        }
        return false;
    }

    // Collects controller and client state for a managed team join
    ManagedControllerTrace TraceManagedController(void *controller)
    {
        ManagedControllerTrace trace;
        if (!controller ||
            targets::kController_FakeClientFlagsOffset < 0)
        {
            return trace;
        }

        for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
        {
            if (!Manager().IsManaged(slot))
                continue;
            void *client = entity_access::ResolveClientBySlot(slot);
            if (!client)
                continue;
            const int entityIndex = *reinterpret_cast<int *>(
                reinterpret_cast<unsigned char *>(client) +
                ssc::OFFSET_m_nEntityIndex);
            char className[64];
            void *resolved = entity_access::ResolveEntityInstance(
                entityIndex, className, sizeof(className));
            if (resolved != controller ||
                std::strcmp(className, "cs_player_controller") != 0 ||
                entity_access::IsEntityBeingDeleted(resolved))
            {
                continue;
            }

            trace.Slot = slot;
            trace.Handle = static_cast<uint32_t>(
                reinterpret_cast<CEntityInstance *>(resolved)
                    ->GetRefEHandle()
                    .ToInt());
            trace.Flags = *reinterpret_cast<uint32_t *>(
                reinterpret_cast<unsigned char *>(resolved) +
                targets::kController_FakeClientFlagsOffset);
            if (targets::kController_TeamOffset >= 0)
            {
                trace.CurrentTeam = *reinterpret_cast<unsigned char *>(
                    reinterpret_cast<unsigned char *>(resolved) +
                    targets::kController_TeamOffset);
            }
            trace.Managed = true;
            trace.Hltv = ssc::IsHltv(client);
            break;
        }
        return trace;
    }

    // Toggles the transient bit after validating the controller handle
    bool SetJoinTeamFakeClientFlag(
        void *controller,
        uint32_t handle,
        bool enabled)
    {
        if (!controller || handle == 0xFFFFFFFF ||
            targets::kController_FakeClientFlagsOffset < 0)
        {
            return false;
        }

        const int entityIndex = static_cast<int>(handle & 0x7FFF);
        char className[64];
        void *current = entity_access::ResolveEntityInstance(
            entityIndex, className, sizeof(className));
        if (current != controller ||
            std::strcmp(className, "cs_player_controller") != 0 ||
            entity_access::IsEntityBeingDeleted(current) ||
            static_cast<uint32_t>(
                reinterpret_cast<CEntityInstance *>(current)
                    ->GetRefEHandle()
                    .ToInt()) != handle)
        {
            return false;
        }

        auto *flags = reinterpret_cast<uint32_t *>(
            reinterpret_cast<unsigned char *>(current) +
            targets::kController_FakeClientFlagsOffset);
        if (enabled)
            *flags |= 0x100u;
        else
            *flags &= ~0x100u;
        return true;
    }

    // Clears FL_BOT for managed pawns during entity packing
    std::vector<BotPawnRef> ApplyBotFlagOverride()
    {
        std::vector<BotPawnRef> pawns = CollectManagedBotPawns();
        std::vector<BotPawnRef> modified;
        modified.reserve(pawns.size());
        for (const BotPawnRef &pawn : pawns)
        {
            auto *flags = reinterpret_cast<uint32_t *>(
                reinterpret_cast<unsigned char *>(pawn.Instance) +
                targets::kBaseEntity_FlagsOffset);
            if ((*flags & targets::kEntityFlagBot) == 0)
                continue;
            *flags &= ~targets::kEntityFlagBot;
            entity_access::MarkEntityFieldChanged(
                pawn.Instance,
                static_cast<uint32_t>(
                    targets::kBaseEntity_FlagsOffset));
            modified.push_back(pawn);
        }
        return modified;
    }

    // Restores FL_BOT on every still-current pawn
    void RestoreBotFlagOverride(const std::vector<BotPawnRef> &pawns)
    {
        for (const BotPawnRef &pawn : pawns)
        {
            const int pawnIndex =
                static_cast<int>(pawn.Handle & 0x7FFF);
            void *currentPawn = entity_access::ResolveEntityInstance(
                pawnIndex, nullptr, 0);
            if (currentPawn != pawn.Instance ||
                entity_access::IsEntityBeingDeleted(currentPawn))
            {
                continue;
            }

            auto *currentEntity =
                reinterpret_cast<CEntityInstance *>(currentPawn);
            if (static_cast<uint32_t>(
                    currentEntity->GetRefEHandle().ToInt()) != pawn.Handle)
            {
                continue;
            }

            auto *flags = reinterpret_cast<uint32_t *>(
                reinterpret_cast<unsigned char *>(currentPawn) +
                targets::kBaseEntity_FlagsOffset);
            *flags |= targets::kEntityFlagBot;
        }
    }

    // Flips managed controller identity around bot-sensitive engine passes
    int FlipManagedController904(
        bool restore,
        std::array<ManagedControllerFlagSnapshot, 64> *saved)
    {
        if (!saved ||
            targets::kController_FakeClientFlagsOffset < 0)
        {
            return 0;
        }

        const int controllerFlagsOffset =
            targets::kController_FakeClientFlagsOffset;
        constexpr uint32_t kFakeClientBit = 0x100;
        int touched = 0;
        for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
        {
            if (!restore)
                (*saved)[slot] = ManagedControllerFlagSnapshot{};
            if (!restore && !Manager().IsManaged(slot))
                continue;

            ManagedControllerFlagSnapshot &snapshot = (*saved)[slot];
            if (restore && !snapshot.Modified)
                continue;

            void *client = entity_access::ResolveClientBySlot(slot);
            if (!client)
                continue;
            const int entityIndex = *reinterpret_cast<int *>(
                reinterpret_cast<unsigned char *>(client) +
                ssc::OFFSET_m_nEntityIndex);
            char className[64];
            void *controller = entity_access::ResolveEntityInstance(
                entityIndex, className, sizeof(className));
            if (!controller ||
                std::strcmp(className, "cs_player_controller") != 0 ||
                entity_access::IsEntityBeingDeleted(controller))
            {
                continue;
            }

            auto *entity =
                reinterpret_cast<CEntityInstance *>(controller);
            const uint32_t handle =
                static_cast<uint32_t>(entity->GetRefEHandle().ToInt());
            auto *flags = reinterpret_cast<uint32_t *>(
                reinterpret_cast<unsigned char *>(controller) +
                controllerFlagsOffset);
            if (!restore)
            {
                if ((*flags & kFakeClientBit) == 0)
                {
                    snapshot.Client = client;
                    snapshot.Controller = controller;
                    snapshot.Handle = handle;
                    snapshot.UserId = *reinterpret_cast<uint16_t *>(
                        reinterpret_cast<unsigned char *>(client) +
                        ssc::OFFSET_m_UserID);
                    snapshot.Modified = true;
                    *flags |= kFakeClientBit;
                    ++touched;
                }
                continue;
            }

            const uint16_t userId = *reinterpret_cast<uint16_t *>(
                reinterpret_cast<unsigned char *>(client) +
                ssc::OFFSET_m_UserID);
            if (client != snapshot.Client ||
                controller != snapshot.Controller ||
                handle != snapshot.Handle ||
                userId != snapshot.UserId)
            {
                snapshot = ManagedControllerFlagSnapshot{};
                continue;
            }
            *flags &= ~kFakeClientBit;
            snapshot = ManagedControllerFlagSnapshot{};
            ++touched;
        }
        return touched;
    }

    namespace identity_runtime
    {

        // Counts connected human clients
        int CountHumanClients()
        {
            if (!g_pNetworkServerService)
                return 0;
            auto *gameServer =
                g_pNetworkServerService->GetIGameServer();
            if (!gameServer)
                return 0;
            auto *clients = reinterpret_cast<CUtlVector<void *> *>(
                reinterpret_cast<unsigned char *>(gameServer) +
                targets::kClientListOffset);
            const int count = clients->Count();
            if (count < 0 || count > 256)
                return 0;

            int humans = 0;
            for (int slot = 0; slot < count; ++slot)
            {
                void *client = clients->Element(slot);
                if (!client)
                    continue;
                void *networkChannel = *reinterpret_cast<void **>(
                    reinterpret_cast<unsigned char *>(client) +
                    ssc::OFFSET_m_NetChannel);
                if (networkChannel)
                    ++humans;
            }
            return humans;
        }

        // Selects a non-colliding SteamID for one managed slot
        uint64_t MakeUniqueSteamId(int slot, uint64_t desired)
        {
            if (desired != 0 &&
                !IsSteamIdInUseByOther(desired, slot))
            {
                return desired;
            }

            for (const BotEntry &entry : BotInfo().All())
            {
                if (entry.SteamId64 != 0 &&
                    !IsSteamIdInUseByOther(entry.SteamId64, slot))
                {
                    return entry.SteamId64;
                }
            }

            const uint64_t base =
                desired != 0
                    ? desired
                    : BotInfoStore::kSteamId64Base + 1;
            for (int bump = 1; bump <= 4096; ++bump)
            {
                const uint64_t candidate =
                    base + static_cast<uint64_t>(bump);
                if (!IsSteamIdInUseByOther(candidate, slot))
                    return candidate;
            }
            return desired;
        }

        // Synchronizes the controller fake-client bit for one slot
        bool SetControllerFakeClientFlag(int slot, bool fakeClient)
        {
            if (targets::kController_FakeClientFlagsOffset < 0)
                return false;
            void *client = entity_access::ResolveClientBySlot(slot);
            if (!client)
                return false;

            const int entityIndex = *reinterpret_cast<int *>(
                reinterpret_cast<unsigned char *>(client) +
                ssc::OFFSET_m_nEntityIndex);
            char className[64];
            void *controller = entity_access::ResolveEntityInstance(
                entityIndex, className, sizeof(className));
            if (!controller ||
                std::strcmp(className, "cs_player_controller") != 0)
            {
                return false;
            }

            constexpr uint32_t kFakeClientBit = 0x100;
            auto *flags = reinterpret_cast<uint32_t *>(
                reinterpret_cast<unsigned char *>(controller) +
                targets::kController_FakeClientFlagsOffset);
            const uint32_t before = *flags;
            if (fakeClient)
                *flags |= kFakeClientBit;
            else
                *flags &= ~kFakeClientBit;
            if (*flags != before)
            {
                entity_access::MarkEntityFieldChanged(
                    controller,
                    static_cast<uint32_t>(
                        targets::kController_FakeClientFlagsOffset));
            }
            return true;
        }

        // Queues one client controller for identity-checked removal
        bool QueueControllerRemovalForClient(void *client, int slot)
        {
            if (!client)
                return false;
            if (!entity_access::UtilRemoveTarget())
            {
                META_CONPRINTF(
                    "[BOTHIDER] deferred destroy unavailable: UTIL_Remove unresolved\n");
                return false;
            }

            const int entityIndex = *reinterpret_cast<int *>(
                reinterpret_cast<unsigned char *>(client) +
                ssc::OFFSET_m_nEntityIndex);
            char className[64];
            void *controller = entity_access::ResolveEntityInstance(
                entityIndex, className, sizeof(className));
            if (!controller)
            {
                META_CONPRINTF(
                    "[BOTHIDER] deferred destroy skipped: entity resolve failed "
                    "entIdx=%d cls='%s' grs=%p (check kEntSys_* offsets)\n",
                    entityIndex,
                    className,
                    entity_access::GameResourceService());
                return false;
            }
            if (std::strcmp(className, "cs_player_controller") != 0)
            {
                META_CONPRINTF(
                    "[BOTHIDER] deferred destroy skipped entIdx=%d cls='%s' "
                    "(not a controller)\n",
                    entityIndex,
                    className);
                return false;
            }
            if (entity_access::IsEntityBeingDeleted(controller))
                return false;

            const uint32_t handle = static_cast<uint32_t>(
                reinterpret_cast<CEntityInstance *>(controller)
                    ->GetRefEHandle()
                    .ToInt());
            auto &pending =
                identity_state::PendingControllerRemovals();
            auto duplicate = std::find_if(
                pending.begin(),
                pending.end(),
                [handle](
                    const identity_state::PendingControllerRemoval &item)
                {
                    return item.Handle == handle;
                });
            if (duplicate != pending.end())
                return true;

            const uint16_t userId = *reinterpret_cast<uint16_t *>(
                reinterpret_cast<unsigned char *>(client) +
                ssc::OFFSET_m_UserID);
            pending.push_back(
                {controller, handle, slot, userId, 0});
            META_CONPRINTF(
                "[BOTHIDER] deferred destroy queued slot=%d entIdx=%d "
                "handle=0x%08x\n",
                slot,
                entityIndex,
                handle);
            return true;
        }

        // Processes queued controller removals
        void DrainPendingControllerRemovals()
        {
            auto &pending =
                identity_state::PendingControllerRemovals();
            if (!entity_access::UtilRemoveTarget())
            {
                pending.clear();
                return;
            }

            for (auto item = pending.begin(); item != pending.end();)
            {
                const int entityIndex =
                    static_cast<int>(item->Handle & 0x7FFF);
                char className[64];
                void *current = entity_access::ResolveEntityInstance(
                    entityIndex, className, sizeof(className));
                if (current != item->Controller ||
                    std::strcmp(
                        className, "cs_player_controller") != 0 ||
                    entity_access::IsEntityBeingDeleted(current) ||
                    static_cast<uint32_t>(
                        reinterpret_cast<CEntityInstance *>(current)
                            ->GetRefEHandle()
                            .ToInt()) != item->Handle)
                {
                    item = pending.erase(item);
                    continue;
                }

                uint16_t currentUserId = 0;
                if (IsControllerReferencedByClient(
                        current, &currentUserId))
                {
                    if (currentUserId != item->UserId)
                    {
                        META_CONPRINTF(
                            "[BOTHIDER] deferred destroy abandoned slot=%d "
                            "handle=0x%08x: controller was rebound to "
                            "userid=%u\n",
                            item->Slot,
                            item->Handle,
                            static_cast<unsigned int>(currentUserId));
                        item = pending.erase(item);
                        continue;
                    }
                    if (item->ReferencedFrames++ == 0)
                    {
                        ++item;
                        continue;
                    }
                }

                entity_access::LogEntitySystemCrossCheck();
                entity_access::RemoveEntity(current);
                META_CONPRINTF(
                    "[BOTHIDER] deferred destroy dispatched slot=%d "
                    "entIdx=%d handle=0x%08x\n",
                    item->Slot,
                    entityIndex,
                    item->Handle);
                item = pending.erase(item);
            }
        }

        // Clears queued controller removals
        void ClearPendingControllerRemovals()
        {
            identity_state::PendingControllerRemovals().clear();
        }

        // Identifies SourceTV before its server-side flag is initialized
        bool IsHltvConnection(
            const char *name,
            const char *networkId)
        {
            return (name && std::strcmp(name, "SourceTV") == 0) ||
                   (networkId &&
                    std::strcmp(networkId, "HLTV") == 0);
        }

        // Releases one managed slot after it resolves to SourceTV
        bool ReleaseManagedHltvSlot(int slot, void *client)
        {
            if (slot < 0 || slot >= PersonaPool::kMaxSlots ||
                !client || !Manager().IsManaged(slot) ||
                !ssc::IsHltv(client))
            {
                return false;
            }

            ssc::WriteSteamId(client, 0);
            const std::string &originalName =
                identity_state::OriginalSlotName(slot);
            if (!originalName.empty())
            {
                entity_access::SetEngineName(
                    client, originalName.c_str());
            }
            entity_access::RefreshClientUserInfo(slot);

            BotInfo().ReleaseAssignment(
                identity_state::SlotEntry(slot));
            identity_state::ClearSlot(slot);
            Manager().ReleaseSlot(slot);

            META_CONPRINTF(
                "[BOTHIDER] slot=%d rejected: SourceTV/HLTV client\n",
                slot);
            return true;
        }

    } // namespace identity_runtime

} // namespace cs2bh
