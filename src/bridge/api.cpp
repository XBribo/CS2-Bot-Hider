#include "slot_publisher.h"
#include "avatar_publisher.h"
#include "entity_access.h"
#include "fake_client_manager.h"
#include "identity_runtime.h"
#include "personas.h"
#include "serversideclient_ref.h"

#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
#define BH_EXPORT extern "C" __declspec(dllexport)
#else
#define BH_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

// Confirms that a managed slot still identifies the same fake client.
void* LiveClient(int slot, uint64_t incarnation)
{
    if (slot < 0 || slot >= cs2bh::PersonaPool::kMaxSlots || incarnation == 0 ||
        !cs2bh::Publisher().IsOwnerThread() ||
        cs2bh::Publisher().GetIncarnation(slot) != incarnation ||
        !cs2bh::Manager().IsManaged(slot)) return nullptr;

    void* client = cs2bh::entity_access::ResolveClientBySlot(slot);
    if (!client || cs2bh::ssc::IsHltv(client)) return nullptr;
    void* channel = nullptr;
    std::memcpy(&channel, static_cast<unsigned char*>(client) + cs2bh::ssc::g_netChannelOffset, sizeof(channel));
    return channel == nullptr ? client : nullptr;
}

// Reads both native SteamID fields to verify exact publication.
bool SidMatches(void* client, uint64_t steamId)
{
    uint64_t primary = 0, mirror = 0;
    auto* bytes = static_cast<unsigned char*>(client);
    std::memcpy(&primary, bytes + cs2bh::ssc::g_steamIdOffset, sizeof(primary));
    std::memcpy(&mirror, bytes + cs2bh::ssc::g_steamIdMirrorOffset, sizeof(mirror));
    return primary == steamId && mirror == steamId;
}

} // namespace

// Returns the version of the native bridge used by the managed BotHider API.
BH_EXPORT int BotHider_GetNativeAbi() { return 1; }

// Publishes an exact identity or rejects it without substituting another SteamID.
BH_EXPORT int BotHider_PublishIdentity(int slot, uint64_t incarnation, uint64_t steamId, const char* name)
{
    if (!name || !*name || !std::memchr(name, '\0', 32)) return -1;
    void* client = LiveClient(slot, incarnation);
    if (!client || (steamId == 0
            ? cs2bh::Publisher().GetBaseSyntheticSid(slot) != 0
            : !cs2bh::identity_runtime::CanUseExactSteamId(slot, steamId))) return -1;

    const char* oldName = cs2bh::ssc::ReadName(client);
    const std::string previousName = oldName ? oldName : "";
    uint64_t previousSteamId = 0;
    std::memcpy(&previousSteamId, static_cast<unsigned char*>(client) + cs2bh::ssc::g_steamIdOffset,
                sizeof(previousSteamId));
    const bool nameChanged = previousName != name;
    const bool sidChanged = !SidMatches(client, steamId);

    if (sidChanged) cs2bh::ssc::WriteSteamId(client, steamId);
    bool published = SidMatches(client, steamId);
    if (published && nameChanged)
    {
        const char* applied = cs2bh::entity_access::SetEngineName(client, name);
        published = applied && std::strcmp(applied, name) == 0;
    }
    else if (published && sidChanged)
    {
        published = cs2bh::entity_access::RefreshClientUserInfo(slot);
    }

    if (!published)
    {
        if (sidChanged) cs2bh::ssc::WriteSteamId(client, previousSteamId);
        if (nameChanged && !previousName.empty())
            cs2bh::entity_access::SetEngineName(client, previousName.c_str());
        return -1;
    }

    cs2bh::Manager().SetSyntheticSid(slot, steamId);
    cs2bh::Personas().MarkSlotManaged(slot, name);
    cs2bh::Publisher().UpdateSyntheticSid(slot, steamId);
    cs2bh::Publisher().UpdatePersonaName(slot, name);
    return 0;
}

// Publishes one direct SteamID avatar without taking ownership of slot avatars.
BH_EXPORT int BotHider_PublishAvatarOverride(uint64_t steamId, const unsigned char* png, int length)
{
    if (!cs2bh::Publisher().IsOwnerThread()) return -1;
    try { return cs2bh::avatar::Publish(steamId, png, length); }
    catch (...) { return -1; }
}

// Restores only an avatar published by the direct SteamID API.
BH_EXPORT int BotHider_ClearAvatarOverride(uint64_t steamId)
{
    if (!cs2bh::Publisher().IsOwnerThread()) return -1;
    try { return cs2bh::avatar::Clear(steamId); }
    catch (...) { return -1; }
}

// Releases all direct SteamID avatars at consumer teardown.
BH_EXPORT void BotHider_ClearAvatarOverrides()
{
    if (cs2bh::Publisher().IsOwnerThread()) cs2bh::avatar::ClearAll();
}
