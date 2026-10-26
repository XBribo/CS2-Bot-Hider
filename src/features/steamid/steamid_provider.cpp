// SteamID pool from bot_info.json.

#include "steamid_provider.h"
#include <cstdint>

#include "bot_info.h"

namespace cs2bh {

// Hash the session ID with FNV-1a 64-bit.
SteamIdProvider::SteamIdProvider(const char* sessionId)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    if (sessionId)
    {
        for (const char* p = sessionId; *p; ++p)
        {
            h = (h ^ static_cast<unsigned char>(*p)) * 0x100000001B3ULL;
        }
    }
    m_sessionSeed = h;
}

// Select a pool entry by slot and session seed; stable within the session.
uint64_t SteamIdProvider::Generate(int botSlot) const noexcept
{
    const auto& entries = BotInfo().All();
    if (entries.empty()) return 0;
    uint64_t x = m_sessionSeed ^ (static_cast<uint64_t>(botSlot) * 0x9E3779B97F4A7C15ULL);
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ULL;
    x ^= x >> 33;
    return entries[x % entries.size()].steamId64;
}

} // namespace cs2bh
