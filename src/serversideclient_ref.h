// serversideclient_ref.h

#pragma once

#include <cstdint>

namespace cs2bh::ssc
{

    // offsets
    inline constexpr int OFFSET_m_UserIDString = 56;  // CUtlString
    inline constexpr int OFFSET_m_Name = 64;          // CUtlString  ← we write this
    inline constexpr int OFFSET_m_nClientSlot = 72;   // CPlayerSlot (int)
    inline constexpr int OFFSET_m_nEntityIndex = 76;  // CEntityIndex (int)
    inline constexpr int OFFSET_m_Server = 80;        // CNetworkGameServerBase*
    inline constexpr int OFFSET_m_NetChannel = 88;    // INetChannel*
    inline constexpr int OFFSET_m_nSignonState = 100; // SignonState_t
    inline constexpr int OFFSET_m_pAttachedTo = 144;
    inline constexpr int OFFSET_m_bFakePlayer = 160; // bool  ← we flip this to 0
    inline constexpr int OFFSET_m_UserID = 168;      // short
    inline constexpr int OFFSET_m_SteamID = 171;     // CSteamID

    // Read CUtlString { char* m_pString } at member offset
    inline const char *ReadName(const void *client)
    {
        if (!client)
            return nullptr;
        auto *utl = reinterpret_cast<const char *const *>(
            reinterpret_cast<const unsigned char *>(client) + OFFSET_m_Name);
        return *utl;
    }

    // sets m_bFakePlayer = 0
    inline void ClearFakePlayer(void *client)
    {
        auto *raw = reinterpret_cast<unsigned char *>(client);
        raw[OFFSET_m_bFakePlayer] = 0;
    }

    // sets m_bFakePlayer = 1 — restore bot identity before disconnect teardown
    inline void SetFakePlayer(void *client)
    {
        auto *raw = reinterpret_cast<unsigned char *>(client);
        raw[OFFSET_m_bFakePlayer] = 1;
    }

    inline bool IsFakePlayerSet(const void *client)
    {
        auto *raw = reinterpret_cast<const unsigned char *>(client);
        return raw[OFFSET_m_bFakePlayer] == 0x01;
    }

} // namespace cs2bh::ssc
