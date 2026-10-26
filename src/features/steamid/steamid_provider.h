// SteamID64 pool for fake clients.

#pragma once

#include <cstdint>

namespace cs2bh {

class SteamIdProvider
{
  public:
    explicit SteamIdProvider(const char* sessionId);

    // Select a SteamID64 for the slot.
    uint64_t Generate(int botSlot) const noexcept;

  private:
    uint64_t m_sessionSeed = 0;
};

} // namespace cs2bh
