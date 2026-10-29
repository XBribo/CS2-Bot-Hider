#pragma once

#include "native_bridge.h"

#include <array>
#include <cstdint>
#include <vector>

namespace cs2bh {

// Internal slot access uses indices validated by engine callbacks or the C bridge.
class SlotPublisher
{
  public:
    struct AvatarRequest
    {
        uint32_t sequence = 0;
        uint32_t length = 0;
        uint64_t incarnation = 0;
        std::vector<unsigned char> data;
    };

    void Init();
    void BeginShutdown() { m_ready = false; }
    void Shutdown();
    bool IsReady() const;
    bool Attach();
    void Detach();
    bool HasProvider() const { return m_attached; }

    void PublishAdopt(int slot,
                      uint64_t syntheticSid,
                      const char* personaName,
                      const char* crosshairCode,
                      uint32_t scoreboardFlair,
                      const char* clanTag,
                      uint32_t clanGroupId);
    void PublishRelease(int slot);
    void UpdateSyntheticSid(int slot, uint64_t sid);
    void UpdatePersonaName(int slot, const char* name);
    void UpdatePing(int slot, int ping);
    uint64_t GetIncarnation(int slot) const;
    const bridge::SlotSnapshot& Slot(int slot) const { return m_slots[slot]; }
    void UpdateCrosshair(int slot, const char* code);
    void UpdateScoreboardFlair(int slot, uint32_t flair);
    void SetAvatarRequest(int slot, const unsigned char* data, uint32_t length);
    const AvatarRequest& GetAvatarRequest(int slot) const { return m_avatars[slot]; }
    void PublishAvatarState(int slot, bool applied, uint64_t steamId);

    void PublishSignature(const char* name, const void* addr);
    int SignatureCount() const { return m_signatureCount; }
    const bridge::Signature* Signatures() const { return m_signatures.data(); }

  private:
    std::array<bridge::SlotSnapshot, bridge::kMaxSlots> m_slots{};
    std::array<AvatarRequest, bridge::kMaxSlots> m_avatars{};
    std::array<bridge::Signature, bridge::kMaxSigs> m_signatures{};
    uint64_t m_nextIncarnation = 0;
    int m_signatureCount = 0;
    bool m_ready = false;
    bool m_attached = false;
};

SlotPublisher& Publisher();

} // namespace cs2bh
