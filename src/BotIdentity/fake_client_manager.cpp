// fake_client_manager.cpp

#include "fake_client_manager.h"
#include "personas.h"
#include "slot_publisher.h"
#include "steamid_provider.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <cstdio>
#include <memory>

namespace cs2bh {

namespace {

FakeClientManager g_manager;

// Rand
uint64_t SimpleRand(uint64_t& state)
{
    uint64_t x = state ? state : 0x9E3779B97F4A7C15ULL;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

} // namespace

FakeClientManager& Manager() { return g_manager; }

FakeClientManager::FakeClientManager() = default;

// Configures the displayed fake-ping bounds
void FakeClientManager::ConfigureFakePing(bool enabled, int minimumMs, int maximumMs)
{
    std::lock_guard<std::mutex> g(m_mutex);
    m_fakePingEnabled = enabled;
    m_fakePingMinimum = minimumMs;
    m_fakePingMaximum = maximumMs;
}

void FakeClientManager::Init()
{
    if (m_steamIds) return;

    char sessionId[32];
    auto nowNs = std::chrono::system_clock::now().time_since_epoch().count();
    std::snprintf(sessionId, sizeof(sessionId), "%lld", static_cast<long long>(nowNs));

    m_steamIds = std::make_unique<SteamIdProvider>(sessionId);
}

bool FakeClientManager::AdoptSlot(int slot, const char* name, uint64_t steamId64, const char* crosshairCode, uint32_t scoreboardFlair)
{
    if (slot < 0 || slot >= PersonaPool::kMaxSlots) return false;
    if (!m_steamIds) return false;

    std::lock_guard<std::mutex> g(m_mutex);
    auto& s = m_slots[slot];

    // Selects one per-bot baseline inside the configured display range
    uint64_t state = static_cast<uint64_t>(slot) ^ m_steamIds->Generate(slot);
    const int range = m_fakePingMaximum - m_fakePingMinimum + 1;
    const int baseline = m_fakePingMinimum + static_cast<int>(SimpleRand(state) % static_cast<uint64_t>(range));

    s.active = true;
    // Uses the already validated SteamID selected for this slot
    s.syntheticSid = steamId64;
    s.scoreboardFlair = scoreboardFlair;
    s.jitter = PingJitter(baseline, m_fakePingMinimum, m_fakePingMaximum);
    s.display = PingDisplay{};
    s.steamIdWritten = false;

    Personas().MarkSlotManaged(slot, name);
    Publisher().PublishAdopt(slot, s.syntheticSid, name, crosshairCode, s.scoreboardFlair);
    Publisher().UpdatePing(slot, m_fakePingEnabled ? baseline : 0);
    return true;
}

void FakeClientManager::ReleaseSlot(int slot)
{
    if (slot < 0 || slot >= PersonaPool::kMaxSlots) return;
    std::lock_guard<std::mutex> g(m_mutex);
    m_slots[slot].active = false;
    m_slots[slot].steamIdWritten = false;
    m_slots[slot].scoreboardFlair = 0;
    m_slots[slot].display.Reset();
    Personas().ClearSlot(slot);
    Publisher().PublishRelease(slot);
}

void FakeClientManager::ReleaseAll()
{
    std::lock_guard<std::mutex> g(m_mutex);
    for (int i = 0; i < PersonaPool::kMaxSlots; ++i)
    {
        m_slots[i].active = false;
        m_slots[i].steamIdWritten = false;
        m_slots[i].scoreboardFlair = 0;
        m_slots[i].display.Reset();
        Personas().ClearSlot(i);
        Publisher().PublishRelease(i);
    }
}

void FakeClientManager::OnTick()
{
    struct Pending
    {
        int slot;
        int ping;
    };
    Pending pending[PersonaPool::kMaxSlots];
    int n = 0;
    {
        std::lock_guard<std::mutex> g(m_mutex);
        for (int i = 0; i < PersonaPool::kMaxSlots; ++i)
        {
            auto& s = m_slots[i];
            if (!s.active || !m_fakePingEnabled) continue;
            s.display.RecordSample(s.jitter.NextSample());
            int produced = s.display.MaybeProduce();
            if (produced >= 0) pending[n++] = { .slot = i, .ping = produced };
        }
    }
    for (int i = 0; i < n; ++i)
        Publisher().UpdatePing(pending[i].slot, pending[i].ping);
}

bool FakeClientManager::IsManaged(int slot) const
{
    if (slot < 0 || slot >= PersonaPool::kMaxSlots) return false;
    std::lock_guard<std::mutex> g(m_mutex);
    return m_slots[slot].active;
}

uint64_t FakeClientManager::GetSyntheticSid(int slot) const
{
    if (slot < 0 || slot >= PersonaPool::kMaxSlots) return 0;
    std::lock_guard<std::mutex> g(m_mutex);
    return m_slots[slot].syntheticSid;
}

void FakeClientManager::SetSyntheticSid(int slot, uint64_t sid)
{
    if (slot < 0 || slot >= PersonaPool::kMaxSlots) return;
    std::lock_guard<std::mutex> g(m_mutex);
    m_slots[slot].syntheticSid = sid;
}

} // namespace cs2bh
