// personas.cpp

#include "personas.h"

#include "bot_info.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace cs2bh {

namespace {

PersonaPool g_personaPool; // NOLINT(bugprone-throwing-static-initialization)

} // namespace

PersonaPool& Personas() { return g_personaPool; }

PersonaPool::PersonaPool()
{
    m_rosterRngState = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0x9E3779B97F4A7C15ULL;
}

std::string PersonaPool::PickFromRoster()
{
    const auto& entries = BotInfo().All();
    if (entries.empty()) return {};
    std::lock_guard<std::mutex> g(m_mutex);
    uint64_t x = m_rosterRngState;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    m_rosterRngState = x;
    return entries[x % entries.size()].name;
}

void PersonaPool::MarkSlotManaged(int slot, const char* name)
{
    if (slot < 0 || slot >= kMaxSlots) return;
    std::lock_guard<std::mutex> g(m_mutex);
    m_slotManaged[slot] = true;
    m_slotNames[slot].assign(name ? name : "");
}

void PersonaPool::ClearSlot(int slot)
{
    if (slot < 0 || slot >= kMaxSlots) return;
    std::lock_guard<std::mutex> g(m_mutex);
    m_slotManaged[slot] = false;
    m_slotNames[slot].clear();
}

bool PersonaPool::IsSlotManaged(int slot) const
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    std::lock_guard<std::mutex> g(m_mutex);
    return m_slotManaged[slot];
}

std::string PersonaPool::GetSlotName(int slot) const
{
    if (slot < 0 || slot >= kMaxSlots) return {};
    std::lock_guard<std::mutex> g(m_mutex);
    return m_slotNames[slot];
}

} // namespace cs2bh
