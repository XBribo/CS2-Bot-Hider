#pragma once

#include "bridge.h"
#include "ping_display.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace cs2bh {

// Slot mutations and snapshots run on the game thread.
class FakeClientManager
{
  public:
    struct AvatarRequest
    {
        uint32_t sequence = 0;
        uint32_t length = 0;
        uint64_t incarnation = 0;
        std::vector<unsigned char> data;
    };

    // Configure fake ping before slot adoption.
    void ConfigureFakePing(bool enabled, int minimumMs, int maximumMs);

    void Init();
    void BeginShutdown() { m_ready = false; }
    void Shutdown();
    bool IsReady() const;
    bool Attach();
    void Detach();
    bool HasProvider() const { return m_attached; }

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
    std::string GetSlotName(int slot) const;
    void SetSlotName(int slot, const char* name);

    uint64_t GetIncarnation(int slot) const;
    const bridge::SlotSnapshot& Slot(int slot) const { return m_slots[slot].snapshot; }
    void UpdateCrosshair(int slot, const char* code);
    void UpdateScoreboardFlair(int slot, uint32_t flair);
    void SetAvatarRequest(int slot, const unsigned char* data, uint32_t length);
    const AvatarRequest& GetAvatarRequest(int slot) const { return m_slots[slot].avatar; }
    void PublishAvatarState(int slot, bool applied, uint64_t steamId);

    void PublishSignature(const char* name, const void* addr);
    int SignatureCount() const { return m_signatureCount; }
    const bridge::Signature* Signatures() const { return m_signatures.data(); }

  private:
    struct ManagedSlot
    {
        bridge::SlotSnapshot snapshot;
        AvatarRequest avatar;
        PingJitter jitter{ 50 };
        PingDisplay display;
    };

    void ClearSlot(int slot);

    mutable std::mutex m_mutex;
    std::array<ManagedSlot, bridge::kMaxSlots> m_slots{};
    std::array<bridge::Signature, bridge::kMaxSigs> m_signatures{};
    uint64_t m_rngState = 0;
    uint64_t m_nextIncarnation = 0;
    int m_signatureCount = 0;
    bool m_ready = false;
    bool m_attached = false;
    bool m_fakePingEnabled = true;
    int m_fakePingMinimum = 20;
    int m_fakePingMaximum = 90;
};

FakeClientManager& Manager();

} // namespace cs2bh
