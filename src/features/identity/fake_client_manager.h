// fake_client_manager.h

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
    uint32_t scoreboardFlair = 0;
    PingJitter jitter{ 50 }; // 50ms baseline
    PingDisplay display;
    bool steamIdWritten = false;
};

class FakeClientManager
{
  public:
    FakeClientManager();

    // Configures fake-ping generation before slots are adopted
    void ConfigureFakePing(bool enabled, int minimumMs, int maximumMs);

    void Init();

    bool AdoptSlot(int slot, const char* name, uint64_t steamId64, const char* crosshairCode, uint32_t scoreboardFlair);

    // Release a slot on disconnect / mapchange
    void ReleaseSlot(int slot);
    void ReleaseAll();

    void OnTick();

    // True if the slot has a managed bot bound
    bool IsManaged(int slot) const;

    uint64_t GetSyntheticSid(int slot) const;

    // Override the SteamID64
    void SetSyntheticSid(int slot, uint64_t sid);

    SteamIdProvider* SteamIds() { return m_steamIds.get(); }

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
