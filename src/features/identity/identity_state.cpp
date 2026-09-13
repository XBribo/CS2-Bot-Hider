#include "identity_state.h"

#include "personas.h"

#include <array>
#include <string>
#include <vector>

namespace cs2bh::identity_state {

namespace {

std::array<const BotEntry*, PersonaPool::kMaxSlots> g_slotEntries{};
std::array<std::string, PersonaPool::kMaxSlots> g_originalSlotNames{};
std::vector<PendingControllerRemoval> g_pendingControllerRemovals;
const std::string kGEmptyName;

} // namespace

// Binds bot identity data and the original engine name to one slot
void BindSlot(int slot, const BotEntry* entry, const char* originalName)
{
    if (slot < 0 || slot >= PersonaPool::kMaxSlots) return;
    g_slotEntries[slot] = entry;
    g_originalSlotNames[slot] = originalName ? originalName : "";
}

// Returns the bot identity entry bound to one slot
const BotEntry* SlotEntry(int slot)
{
    if (slot < 0 || slot >= PersonaPool::kMaxSlots) return nullptr;
    return g_slotEntries[slot];
}

// Returns the original engine name captured for one slot
const std::string& OriginalSlotName(int slot)
{
    if (slot < 0 || slot >= PersonaPool::kMaxSlots) return kGEmptyName;
    return g_originalSlotNames[slot];
}

// Clears the identity data bound to one slot
void ClearSlot(int slot)
{
    if (slot < 0 || slot >= PersonaPool::kMaxSlots) return;
    g_slotEntries[slot] = nullptr;
    g_originalSlotNames[slot].clear();
}

// Returns the pending controller-removal queue
std::vector<PendingControllerRemoval>& PendingControllerRemovals() { return g_pendingControllerRemovals; }

// Clears all slot bindings and pending controller removals
void ClearAll()
{
    g_slotEntries.fill(nullptr);
    for (std::string& name : g_originalSlotNames)
        name.clear();
    g_pendingControllerRemovals.clear();
}

} // namespace cs2bh::identity_state
