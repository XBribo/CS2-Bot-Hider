#include "slot_publisher.h"

#include <chrono>
#include <cstring>
#include <utility>

namespace cs2bh {

namespace {
SlotPublisher g_publisher;
thread_local bool g_bridgeThread = false;

template <size_t N> void CopyText(char (&destination)[N], const char* source)
{
    std::memset(destination, 0, N);
    if (source) std::strncpy(destination, source, N - 1);
}
} // namespace

SlotPublisher& Publisher() { return g_publisher; }

void SlotPublisher::Init()
{
    g_bridgeThread = true;
    m_slots = {};
    m_avatars = {};
    m_signatures = {};
    m_signatureCount = 0;
    m_nextIncarnation = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    m_ready = true;
}

void SlotPublisher::Shutdown()
{
    m_ready = false;
    m_slots = {};
    m_avatars = {};
}

bool SlotPublisher::IsReady() const { return g_bridgeThread && m_ready; }

bool SlotPublisher::Attach()
{
    if (!IsReady() || m_attached) return false;
    m_attached = true;
    return true;
}

void SlotPublisher::Detach() { m_attached = false; }

void SlotPublisher::PublishAdopt(int slot,
                                 uint64_t syntheticSid,
                                 const char* personaName,
                                 const char* crosshairCode,
                                 uint32_t scoreboardFlair,
                                 const char* clanTag,
                                 uint32_t clanGroupId)
{
    PublishRelease(slot);
    auto& state = m_slots[slot];
    state.steamId = state.baseSteamId = syntheticSid;
    if (++m_nextIncarnation == 0) ++m_nextIncarnation;
    state.incarnation = m_nextIncarnation;
    state.managed = 1;
    state.scoreboardFlair = scoreboardFlair;
    state.clanGroupId = clanGroupId;
    CopyText(state.name, personaName);
    CopyText(state.baseName, personaName);
    CopyText(state.crosshair, crosshairCode);
    CopyText(state.clanTag, clanTag);
}

void SlotPublisher::PublishRelease(int slot)
{
    m_slots[slot] = {};
    auto sequence = m_avatars[slot].sequence + 1;
    m_avatars[slot] = {};
    m_avatars[slot].sequence = sequence;
}

void SlotPublisher::UpdatePing(int slot, int ping) { m_slots[slot].ping = ping; }

void SlotPublisher::UpdateSyntheticSid(int slot, uint64_t sid) { m_slots[slot].steamId = sid; }

void SlotPublisher::UpdatePersonaName(int slot, const char* name) { CopyText(m_slots[slot].name, name); }

uint64_t SlotPublisher::GetIncarnation(int slot) const { return m_slots[slot].incarnation; }

void SlotPublisher::UpdateCrosshair(int slot, const char* code) { CopyText(m_slots[slot].crosshair, code); }

void SlotPublisher::UpdateScoreboardFlair(int slot, uint32_t flair) { m_slots[slot].scoreboardFlair = flair; }

void SlotPublisher::SetAvatarRequest(int slot, const unsigned char* data, uint32_t length)
{
    std::vector<unsigned char> bytes;
    if (length) bytes.assign(data, data + length);
    auto& request = m_avatars[slot];
    request.data = std::move(bytes);
    request.length = length;
    request.incarnation = m_slots[slot].incarnation;
    ++request.sequence;
    m_slots[slot].avatarLength = length;
    PublishAvatarState(slot, false, 0);
}

void SlotPublisher::PublishAvatarState(int slot, bool applied, uint64_t steamId)
{
    m_slots[slot].avatarApplied = applied ? 1 : 0;
    m_slots[slot].avatarSteamId = applied ? steamId : 0;
}

void SlotPublisher::PublishSignature(const char* name, const void* addr)
{
    auto& entry = m_signatures[m_signatureCount++];
    CopyText(entry.name, name);
    entry.address = reinterpret_cast<uint64_t>(addr);
}

} // namespace cs2bh
