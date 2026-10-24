// personas.cpp

#include "personas.h"

#include <mutex>
#include <string>

namespace cs2bh {

namespace {

PersonaPool g_personaPool; // NOLINT(bugprone-throwing-static-initialization)

} // namespace

PersonaPool& Personas() { return g_personaPool; }

// Stores a slot's display name.
void PersonaPool::SetSlotName(int slot, const char* name)
{
    if (slot < 0 || slot >= kMaxSlots) return;
    std::lock_guard<std::mutex> g(m_mutex);
    m_slotNames[slot].assign(name ? name : "");
}

void PersonaPool::ClearSlot(int slot)
{
    if (slot < 0 || slot >= kMaxSlots) return;
    std::lock_guard<std::mutex> g(m_mutex);
    m_slotNames[slot].clear();
}

std::string PersonaPool::GetSlotName(int slot) const
{
    if (slot < 0 || slot >= kMaxSlots) return {};
    std::lock_guard<std::mutex> g(m_mutex);
    return m_slotNames[slot];
}

} // namespace cs2bh
