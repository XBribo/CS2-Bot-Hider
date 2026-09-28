#include "avatar_override.h"
#include "avatar_publisher.h"
#include "core/log.h"
#include "fake_client_manager.h"
#include "slot_publisher.h"
#include <array>
#include <networkstringtabledefs.h>

namespace cs2bh::avatar {
namespace {
struct AvatarRuntimeState
{
    uint32_t sequence = 0;
    uint64_t steamId = 0, incarnation = 0;
    bool applied = false;
};
INetworkStringTableContainer* g_tables = nullptr;
INetworkStringTable* g_lastTable = nullptr;
std::array<AvatarRuntimeState, 64> g_slots{};
} // namespace
void SetStringTableContainer(INetworkStringTableContainer* container) { g_tables = container; }
void ResetRuntime()
{
    g_slots = {};
    g_lastTable = nullptr;
    for (int slot = 0; slot < 64; ++slot)
        Publisher().PublishAvatarState(slot, false, 0);
}
void ProcessOverrides()
{
    auto* table = g_tables ? g_tables->FindTable("ServerAvatarOverrides") : nullptr;
    if (!table) return;
    if (g_lastTable != table)
    {
        ResetRuntime();
        g_lastTable = table;
    }
    for (int slot = 0; slot < 64; ++slot)
    {
        uint32_t sequence = 0, length = 0;
        uint64_t incarnation = 0;
        if (!Publisher().ReadAvatarMetadata(slot, sequence, length, incarnation)) continue;
        auto& prior = g_slots[slot];
        const bool current = Manager().IsManaged(slot) && length > 0 && incarnation != 0 && incarnation == Publisher().GetIncarnation(slot);
        const uint64_t sid = current ? Manager().GetSyntheticSid(slot) : 0;
        if (prior.applied && (sid != prior.steamId || incarnation != prior.incarnation))
        {
            if (Clear(prior.steamId, prior.incarnation) < 0) continue;
            prior = {};
            Publisher().PublishAvatarState(slot, false, 0);
        }
        if (!current || !sid) continue;
        if (prior.applied && prior.sequence == sequence && prior.steamId == sid && prior.incarnation == incarnation) continue;
        SlotPublisher::AvatarRequest request;
        if (!Publisher().ReadAvatarRequest(slot, request) || request.sequence != sequence || request.incarnation != incarnation) continue;
        // Both slot avatars and direct SteamID overrides use this publisher.
        // Different sources cannot overwrite or clear each other's ownership.
        if (Publish(sid, request.data.data(), static_cast<int>(request.data.size()), incarnation) < 0) continue;
        prior = { sequence, sid, incarnation, true };
        Publisher().PublishAvatarState(slot, true, sid);
    }
}
} // namespace cs2bh::avatar
