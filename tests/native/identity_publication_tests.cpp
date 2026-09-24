#include "serversideclient_ref.h"
#include "slot_publisher.h"
#include <array>
#include <cstring>

int main()
{
    using namespace cs2bh;
    std::array<unsigned char, 512> client{};
    client[ssc::g_connectionTypeFlagsOffset] = 0x28; // Preserve unrelated flags.
    client[ssc::g_fakePlayerOffset] = 1;
    unsigned publications = ssc::ReconcileIdentity(client.data(), true, 123);
    if (publications != 1 || client[ssc::g_connectionTypeFlagsOffset] != 0x21) return 1;
    // Repeated quota/intro passes must not publish the same identity again.
    for (int frame = 0; frame < 1280; ++frame)
        publications += ssc::ReconcileIdentity(client.data(), true, 123);
    if (publications != 1) return 2;
    client[ssc::g_steamIdMirrorOffset] ^= 1;
    if (!ssc::ReconcileIdentity(client.data(), true, 123)) return 3;
    if (ssc::ReconcileIdentity(client.data(), true, 123)) return 4;
    if (!ssc::ReconcileIdentity(client.data(), false, 123)) return 5;
    if (ssc::ReconcileIdentity(client.data(), false, 123)) return 6;
    if (!ssc::ReconcileIdentity(client.data(), true, 456)) return 7;

    SlotPublisher state;
    state.Init();
    state.PublishAdopt(4, 123, "base", "crosshair", 9);
    PresentationSlot previous{}, current{};
    state.ReadSlot(4, previous);
    state.UpdateSyntheticSid(4, 456);
    state.UpdatePersonaName(4, "lease");
    state.ReadSlot(4, current);
    if (current.BaseSteamId != 123 || std::strcmp(current.BaseName, "base") || current.SteamId != 456) return 9;
    if (state.SetBase(4, previous.Session + 1, previous.Incarnation, 789, "new", "", 0)) return 10;
    if (!state.SetBase(4, previous.Session, previous.Incarnation, 789, "new", "", 0)) return 11;
    state.ReadSlot(4, current);
    if (current.BaseSteamId != 789 || current.SteamId != 456) return 12;
    const unsigned char png[] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (!state.SetAvatar(4, previous.Session, previous.Incarnation, png, sizeof(png))) return 13;
    SlotPublisher::AvatarRequest firstRequest{}, replacementRequest{};
    state.ReadAvatarRequest(4, firstRequest);
    state.PublishRelease(4);
    state.PublishAdopt(4, 999, "replacement", "", 0);
    if (state.SetAvatar(4, previous.Session, previous.Incarnation, png, sizeof(png)) || state.AvatarSize(4)) return 14;
    state.ReadSlot(4, current);
    if (!state.SetAvatar(4, current.Session, current.Incarnation, png, sizeof(png))) return 16;
    state.ReadAvatarRequest(4, replacementRequest);
    if (firstRequest.sequence == replacementRequest.sequence) return 17;
    state.Shutdown();
    state.Init();
    if (state.SetBase(4, previous.Session, previous.Incarnation, 789, "new", "", 0)) return 15;
}
