#pragma once

#include <cstdint>

namespace cs2bh::bridge {

inline constexpr int kMaxSlots = 64;
inline constexpr int kNameLen = 32;
inline constexpr int kCrosshairLen = 64;
inline constexpr int kClanTagLen = 128;
inline constexpr int kMaxSigs = 8;
inline constexpr int kAvatarMaxBytes = 16 * 1024;

struct SlotSnapshot
{
    uint64_t steamId = 0;
    uint64_t baseSteamId = 0;
    uint64_t incarnation = 0;
    uint64_t avatarSteamId = 0;
    int32_t ping = 0;
    uint32_t managed = 0;
    uint32_t scoreboardFlair = 0;
    uint32_t clanGroupId = 0;
    uint32_t avatarLength = 0;
    uint32_t avatarApplied = 0;
    char name[kNameLen]{};
    char baseName[kNameLen]{};
    char crosshair[kCrosshairLen]{};
    char clanTag[kClanTagLen]{};
};

struct Signature
{
    char name[kNameLen]{};
    uint64_t address = 0;
};

static_assert(sizeof(SlotSnapshot) == 312);
static_assert(sizeof(Signature) == 40);

} // namespace cs2bh::bridge
