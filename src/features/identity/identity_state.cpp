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
std::vector<PendingBotKick> g_pendingBotKicks;
const std::string kGEmptyName;

} // namespace

// Bind slot identity and its original name.
void BindSlot(int slot, const BotEntry* entry, const char* originalName)
{
    if (slot < 0 || slot >= PersonaPool::kMaxSlots) return;
    g_slotEntries[slot] = entry;
    g_originalSlotNames[slot] = originalName ? originalName : "";
}

// Read the slot's identity entry.
const BotEntry* SlotEntry(int slot)
{
    if (slot < 0 || slot >= PersonaPool::kMaxSlots) return nullptr;
    return g_slotEntries[slot];
}

// Read the slot's original engine name.
const std::string& OriginalSlotName(int slot)
{
    if (slot < 0 || slot >= PersonaPool::kMaxSlots) return kGEmptyName;
    return g_originalSlotNames[slot];
}

// Clear slot identity.
void ClearSlot(int slot)
{
    if (slot < 0 || slot >= PersonaPool::kMaxSlots) return;
    g_slotEntries[slot] = nullptr;
    g_originalSlotNames[slot].clear();
}

// Access the controller-removal queue.
std::vector<PendingControllerRemoval>& PendingControllerRemovals() { return g_pendingControllerRemovals; }

// Access the deferred bot-kick queue.
std::vector<PendingBotKick>& PendingBotKicks() { return g_pendingBotKicks; }

// Clear level bindings and deferred work.
void ClearAll()
{
    g_slotEntries.fill(nullptr);
    for (std::string& name : g_originalSlotNames)
        name.clear();
    g_pendingControllerRemovals.clear();
    g_pendingBotKicks.clear();
}

} // namespace cs2bh::identity_state
