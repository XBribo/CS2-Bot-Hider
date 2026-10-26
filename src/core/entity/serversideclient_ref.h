
#pragma once

#include <cstdint>
#include <cstring>

namespace cs2bh::ssc {

// Fallback offsets; gamedata.json overrides them at load.
inline int g_nameOffset = 64; // CUtlString
inline int g_entityIndexOffset = 76; // CEntityIndex (int)
inline int g_netChannelOffset = 88; // INetChannel*
inline int g_connectionTypeFlagsOffset = 96; // byte, fake-client mask 0x08
inline int g_fakePlayerOffset = 160; // bool
inline int g_userIdOffset = 168; // short
inline int g_steamIdOffset = 171; // CSteamID
inline int g_steamIdMirrorOffset = 179; // mirrored CSteamID
inline int g_isHltvOffset = 322; // bool

// Read the CUtlString pointer at the name offset.
inline const char* ReadName(const void* client)
{
    if (!client) return nullptr;
    const auto* utl = reinterpret_cast<const char* const*>(reinterpret_cast<const unsigned char*>(client) + g_nameOffset);
    return *utl;
}

// Clear the fake-player flags.
inline void ClearFakePlayer(void* client)
{
    auto* raw = reinterpret_cast<unsigned char*>(client);
    auto& connectionFlags = raw[g_connectionTypeFlagsOffset];
    connectionFlags = static_cast<unsigned char>((connectionFlags & ~0x08U) | 0x01U);
    raw[g_fakePlayerOffset] = 0;
}

// Set the fake-player flags.
inline void SetFakePlayer(void* client)
{
    auto* raw = reinterpret_cast<unsigned char*>(client);
    auto& connectionFlags = raw[g_connectionTypeFlagsOffset];
    connectionFlags = static_cast<unsigned char>((connectionFlags & ~0x01U) | 0x08U);
    raw[g_fakePlayerOffset] = 1;
}

// Write both engine SteamID fields.
inline void WriteSteamId(void* client, uint64_t steamId)
{
    auto* raw = reinterpret_cast<unsigned char*>(client);
    std::memcpy(raw + g_steamIdOffset, &steamId, sizeof(steamId));
    std::memcpy(raw + g_steamIdMirrorOffset, &steamId, sizeof(steamId));
}

// Check the fake-player flag.
inline bool IsFakePlayerSet(const void* client)
{
    const auto* raw = reinterpret_cast<const unsigned char*>(client);
    return raw[g_fakePlayerOffset] == 0x01;
}

// Check SourceTV identity.
inline bool IsHltv(const void* client)
{
    if (!client) return false;
    const auto* raw = reinterpret_cast<const unsigned char*>(client);
    return raw[g_isHltvOffset] != 0;
}

} // namespace cs2bh::ssc
