#include "fake_client_manager.h"

#include <chrono>
#include <cstring>
#include <utility>

namespace cs2bh {

namespace {

FakeClientManager g_manager; // NOLINT(bugprone-throwing-static-initialization)
thread_local bool g_bridgeThread = false;

uint64_t SimpleRand(uint64_t& state)
{
    uint64_t x = state ? state : 0x9E3779B97F4A7C15ULL;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

template <size_t N> void CopyText(char (&destination)[N], const char* source)
{
    std::memset(destination, 0, N);
    if (source) std::strncpy(destination, source, N - 1);
}

} // namespace

FakeClientManager& Manager() { return g_manager; }

void FakeClientManager::ConfigureFakePing(bool enabled, int minimumMs, int maximumMs)
{
    std::lock_guard<std::mutex> g(m_mutex);
    m_fakePingEnabled = enabled;
    m_fakePingMinimum = minimumMs;
    m_fakePingMaximum = maximumMs;
}

void FakeClientManager::Init()
{
    std::lock_guard<std::mutex> g(m_mutex);
    g_bridgeThread = true;
    m_slots = {};
    m_signatures = {};
    m_signatureCount = 0;
    m_rngState = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    m_nextIncarnation = m_rngState;
    m_attached = false;
    m_ready = true;
}

void FakeClientManager::Shutdown()
{
    std::lock_guard<std::mutex> g(m_mutex);
    m_ready = false;
    m_slots = {};
}

bool FakeClientManager::IsReady() const { return g_bridgeThread && m_ready; }

bool FakeClientManager::Attach()
{
    if (!IsReady() || m_attached) return false;
    m_attached = true;
    return true;
}

void FakeClientManager::Detach() { m_attached = false; }

void FakeClientManager::AdoptSlot(int slot,
                                  const char* name,
                                  uint64_t steamId64,
                                  const char* crosshairCode,
                                  uint32_t scoreboardFlair,
                                  const char* clanTag,
                                  uint32_t clanGroupId)
{
    std::lock_guard<std::mutex> g(m_mutex);
    ClearSlot(slot);
    auto& s = m_slots[slot];
    auto& state = s.snapshot;
    const int range = m_fakePingMaximum - m_fakePingMinimum + 1;
    const int baseline = m_fakePingMinimum + static_cast<int>(SimpleRand(m_rngState) % static_cast<uint64_t>(range));
    s.jitter = PingJitter(baseline, m_fakePingMinimum, m_fakePingMaximum);
    state.steamId = state.baseSteamId = steamId64;
    if (++m_nextIncarnation == 0) ++m_nextIncarnation;
    state.incarnation = m_nextIncarnation;
    state.managed = 1;
    state.ping = m_fakePingEnabled ? baseline : 0;
    state.scoreboardFlair = scoreboardFlair;
    state.clanGroupId = clanGroupId;
    CopyText(state.name, name);
    CopyText(state.baseName, name);
    CopyText(state.crosshair, crosshairCode);
    CopyText(state.clanTag, clanTag);
}

void FakeClientManager::ClearSlot(int slot)
{
    auto& s = m_slots[slot];
    s.snapshot = {};
    s.display.Reset();
    const auto sequence = s.avatar.sequence + 1;
    s.avatar = {};
    s.avatar.sequence = sequence;
}

void FakeClientManager::ReleaseSlot(int slot)
{
    std::lock_guard<std::mutex> g(m_mutex);
    ClearSlot(slot);
}

void FakeClientManager::ReleaseAll()
{
    std::lock_guard<std::mutex> g(m_mutex);
    for (int slot = 0; slot < bridge::kMaxSlots; ++slot)
        ClearSlot(slot);
}

void FakeClientManager::OnTick()
{
    std::lock_guard<std::mutex> g(m_mutex);
    for (auto& s : m_slots)
    {
        if (!s.snapshot.managed || !m_fakePingEnabled) continue;
        s.display.RecordSample(s.jitter.NextSample());
        const int produced = s.display.MaybeProduce();
        if (produced >= 0) s.snapshot.ping = produced;
    }
}

bool FakeClientManager::IsManaged(int slot) const
{
    std::lock_guard<std::mutex> g(m_mutex);
    return m_slots[slot].snapshot.managed != 0;
}

uint64_t FakeClientManager::GetSyntheticSid(int slot) const
{
    std::lock_guard<std::mutex> g(m_mutex);
    return m_slots[slot].snapshot.steamId;
}

void FakeClientManager::SetSyntheticSid(int slot, uint64_t sid)
{
    std::lock_guard<std::mutex> g(m_mutex);
    m_slots[slot].snapshot.steamId = sid;
}

std::string FakeClientManager::GetSlotName(int slot) const
{
    std::lock_guard<std::mutex> g(m_mutex);
    return m_slots[slot].snapshot.name;
}

void FakeClientManager::SetSlotName(int slot, const char* name)
{
    std::lock_guard<std::mutex> g(m_mutex);
    CopyText(m_slots[slot].snapshot.name, name);
}

uint64_t FakeClientManager::GetIncarnation(int slot) const { return m_slots[slot].snapshot.incarnation; }

void FakeClientManager::UpdateCrosshair(int slot, const char* code) { CopyText(m_slots[slot].snapshot.crosshair, code); }

void FakeClientManager::UpdateScoreboardFlair(int slot, uint32_t flair) { m_slots[slot].snapshot.scoreboardFlair = flair; }

void FakeClientManager::SetAvatarRequest(int slot, const unsigned char* data, uint32_t length)
{
    std::vector<unsigned char> bytes;
    if (length) bytes.assign(data, data + length);
    auto& s = m_slots[slot];
    s.avatar.data = std::move(bytes);
    s.avatar.length = length;
    s.avatar.incarnation = s.snapshot.incarnation;
    ++s.avatar.sequence;
    s.snapshot.avatarLength = length;
    PublishAvatarState(slot, false, 0);
}

void FakeClientManager::PublishAvatarState(int slot, bool applied, uint64_t steamId)
{
    auto& state = m_slots[slot].snapshot;
    state.avatarApplied = applied ? 1 : 0;
    state.avatarSteamId = applied ? steamId : 0;
}

void FakeClientManager::PublishSignature(const char* name, const void* addr)
{
    auto& entry = m_signatures[m_signatureCount++];
    CopyText(entry.name, name);
    entry.address = reinterpret_cast<uint64_t>(addr);
}

} // namespace cs2bh
