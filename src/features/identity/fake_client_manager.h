#pragma once

#include "personas.h"
#include "ping_display.h"
#include "steamid_provider.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>

namespace cs2bh {

struct ManagedSlot
{
    bool active = false;
    uint64_t syntheticSid = 0;
    PingJitter jitter{ 50 }; // 50ms baseline
    PingDisplay display;
};

class FakeClientManager
{
  public:
    FakeClientManager();

    // Configure fake ping before slot adoption.
    void ConfigureFakePing(bool enabled, int minimumMs, int maximumMs);

    void Init();

    void AdoptSlot(int slot,
                   const char* name,
                   uint64_t steamId64,
                   const char* crosshairCode,
                   uint32_t scoreboardFlair,
                   const char* clanTag,
                   uint32_t clanGroupId);

    // Release slot state.
    void ReleaseSlot(int slot);
    void ReleaseAll();

    void OnTick();

    // Check slot ownership.
    bool IsManaged(int slot) const;

    uint64_t GetSyntheticSid(int slot) const;

    // Override the SteamID64
    void SetSyntheticSid(int slot, uint64_t sid);

  private:
    mutable std::mutex m_mutex;
    std::array<ManagedSlot, PersonaPool::kMaxSlots> m_slots;
    std::unique_ptr<SteamIdProvider> m_steamIds;
    bool m_fakePingEnabled = true;
    int m_fakePingMinimum = 20;
    int m_fakePingMaximum = 90;
};

FakeClientManager& Manager();

} // namespace cs2bh
