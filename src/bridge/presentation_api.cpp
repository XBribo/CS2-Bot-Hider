// In-process presentation ABI. All entry points are server-thread only.
// Ported from the DemoTracer maintained BotHider derivative, AGPL-3.0-only.
#include "slot_publisher.h"
#include "plugin.h"
#include "entity_access.h"
#include "serversideclient_ref.h"
#include "fake_client_manager.h"
#include "identity_runtime.h"
#include "personas.h"
#include "core/config.h"
#include "core/cs2_sdk/schema.h"
#include <entity2/entityinstance.h>
#include <algorithm>
#include <cstring>

#ifdef _WIN32
#define BH_EXPORT extern "C" __declspec(dllexport)
#else
#define BH_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {
using namespace cs2bh;
void* LiveClient(int slot, uint64_t session, uint64_t incarnation)
{
    if (!Publisher().Matches(slot, session, incarnation) || !Manager().IsManaged(slot)) return nullptr;
    auto* client = entity_access::ResolveClientBySlot(slot);
    if (!client || ssc::IsHltv(client)) return nullptr;
    // The manager can lag a slot replacement: never publish into a human client.
    void* channel = nullptr;
    std::memcpy(&channel, static_cast<unsigned char*>(client) + ssc::g_netChannelOffset, sizeof(channel));
    return channel ? nullptr : client;
}
bool SidMatches(void* client, uint64_t expected)
{
    uint64_t primary = 0, mirror = 0;
    std::memcpy(&primary, static_cast<unsigned char*>(client) + ssc::g_steamIdOffset, 8);
    std::memcpy(&mirror, static_cast<unsigned char*>(client) + ssc::g_steamIdMirrorOffset, 8);
    return primary == expected && mirror == expected;
}
void* LiveController(int slot, uint64_t session, uint64_t incarnation, uint32_t handle)
{
    auto* client = LiveClient(slot, session, incarnation);
    if (!client) return nullptr;
    int index = 0;
    std::memcpy(&index, static_cast<unsigned char*>(client) + ssc::g_entityIndexOffset, 4);
    char name[64]{};
    auto* entity = entity_access::ResolveEntityInstance(index, name, sizeof(name));
    if (!entity || std::strcmp(name, "cs_player_controller") || entity_access::IsEntityBeingDeleted(entity)) return nullptr;
    return static_cast<uint32_t>(static_cast<CEntityInstance*>(entity)->GetRefEHandle().ToInt()) == handle ? entity : nullptr;
}
} // namespace

BH_EXPORT int BotHider_GetNativeAbi() { return cs2bh::kNativePresentationAbi; }
BH_EXPORT uint64_t BotHider_GetSession() { return cs2bh::Publisher().Session(); }
BH_EXPORT int BotHider_Listen(uint64_t session, cs2bh::PresentationChanged listener)
{
    return cs2bh::Publisher().Listen(session, listener) ? 0 : -1;
}
BH_EXPORT int BotHider_ReadSlot(int slot, cs2bh::PresentationSlot* out, int size)
{
    return out && size == sizeof(*out) && cs2bh::Publisher().ReadSlot(slot, *out) ? 0 : -1;
}
BH_EXPORT int BotHider_ReadSignature(int index, cs2bh::PresentationSignature* out, int size)
{
    return out && size == sizeof(*out) && cs2bh::Publisher().ReadSignature(index, *out) ? 0 : -1;
}
BH_EXPORT int BotHider_PublishIdentity(int slot, uint64_t session, uint64_t incarnation, uint64_t sid, const char* name)
{
    using namespace cs2bh;
    if (!sid || !name || !*name || !std::memchr(name, 0, 32)) return -1;
    auto* client = LiveClient(slot, session, incarnation);
    if (!client) return -1;
    const char* previousName = ssc::ReadName(client);
    const bool nameChanged = !previousName || std::strcmp(previousName, name) != 0;
    const bool sidChanged = !SidMatches(client, sid);
    if (g_plugin.IsDisguiseEnabled())
    {
        ssc::ClearFakePlayer(client);
        identity_runtime::SetControllerFakeClientFlag(slot, false);
    }
    if (sidChanged) ssc::WriteSteamId(client, sid);
    if (!SidMatches(client, sid)) return -1;
    if (nameChanged)
    {
        const char* applied = entity_access::SetEngineName(client, name);
        if (!applied || std::strcmp(applied, name) != 0) return -1;
        // SetName publishes the combined final identity on both supported platforms.
    }
    else if (sidChanged && !entity_access::RefreshClientUserInfo(slot))
        return -1;
    if (!Publisher().Matches(slot, session, incarnation)) return -1;
    Manager().SetSyntheticSid(slot, sid);
    Personas().MarkSlotManaged(slot, name);
    Publisher().UpdateSyntheticSid(slot, sid);
    Publisher().UpdatePersonaName(slot, name);
    return 0;
}
BH_EXPORT int BotHider_PublishCrosshair(int slot, uint64_t session, uint64_t incarnation, uint32_t handle)
{
    auto* controller = LiveController(slot, session, incarnation, handle);
    const int offset = cs2bh::schema::GetFieldOffset("CCSPlayerController", "m_szCrosshairCodes");
    if (!controller || offset < 0) return -1;
    cs2bh::entity_access::MarkEntityFieldChanged(controller, offset);
    return 0;
}
BH_EXPORT int BotHider_PublishPing(int slot, uint64_t session, uint64_t incarnation, uint32_t handle)
{
    auto* controller = LiveController(slot, session, incarnation, handle);
    const int offset = cs2bh::schema::GetFieldOffset("CCSPlayerController", "m_iPing");
    cs2bh::PresentationSlot state;
    if (!controller || offset < 0 || !cs2bh::Publisher().ReadSlot(slot, state)) return -1;
    const int ping = std::max(0, state.Ping);
    std::memcpy(static_cast<char*>(controller) + offset, &ping, sizeof(ping));
    return 0;
}
BH_EXPORT int
BotHider_SetBase(int slot, uint64_t session, uint64_t incarnation, uint64_t sid, const char* name, const char* crosshair, uint32_t flair)
{
    return LiveClient(slot, session, incarnation) && cs2bh::Publisher().SetBase(slot, session, incarnation, sid, name, crosshair, flair)
               ? 0
               : -1;
}
BH_EXPORT int BotHider_SetOption(uint64_t session, int option, int value)
{
    using namespace cs2bh;
    if (!session || Publisher().Session() != session) return -1;
    if (option == 1) g_plugin.SetIdentityMode(value ? IdentityMode::Player : IdentityMode::Bot);
    else if (option == 2)
        g_plugin.SetUseBotInfoName(value != 0);
    else
        return -1;
    return 0;
}
BH_EXPORT int BotHider_GetOptions(uint64_t session)
{
    if (!session || cs2bh::Publisher().Session() != session) return -1;
    return (cs2bh::config::Current.autoRespawn ? 1 : 0) | (cs2bh::config::Current.externalAvatars ? 2 : 0);
}
BH_EXPORT int BotHider_SetAvatar(int slot, uint64_t session, uint64_t incarnation, const unsigned char* data, int length)
{
    if (cs2bh::config::Current.externalAvatars) return -1;
    try
    {
        return LiveClient(slot, session, incarnation) && cs2bh::Publisher().SetAvatar(slot, session, incarnation, data, length) ? 0 : -1;
    }
    catch (...)
    {
        return -1;
    }
}
BH_EXPORT int BotHider_GetAvatarState(int slot, uint64_t session, uint64_t incarnation, int* size)
{
    if (!size || !cs2bh::Publisher().Matches(slot, session, incarnation)) return -1;
    *size = cs2bh::Publisher().AvatarSize(slot);
    return cs2bh::Publisher().HasAvatar(slot) ? 1 : 0;
}

BH_EXPORT uint64_t BotHider_GetUserInfoPublications()
{
    return cs2bh::Publisher().Active() ? cs2bh::entity_access::UserInfoPublications() : 0;
}
