#include "identity_state.h"

#include "bridge.h"
#include "bot_info.h"
#include "fake_client_manager.h"

#include <array>
#include <string>
#include <vector>

namespace cs2bh::identity_state {

namespace {

std::array<const BotEntry*, bridge::kMaxSlots> g_slotEntries{};
std::array<std::string, bridge::kMaxSlots> g_originalSlotNames{};
std::vector<PendingControllerRemoval> g_pendingControllerRemovals;
std::vector<PendingBotKick> g_pendingBotKicks;

} // namespace

// Bind slot identity and its original name.
void BindSlot(int slot, const BotEntry* entry, const char* originalName)
{
    g_slotEntries[slot] = entry;
    g_originalSlotNames[slot] = originalName ? originalName : "";
}

// Read the slot's original engine name.
const std::string& OriginalSlotName(int slot) { return g_originalSlotNames[slot]; }

void ReleaseSlot(int slot)
{
    BotInfo().ReleaseAssignment(g_slotEntries[slot]);
    g_slotEntries[slot] = nullptr;
    g_originalSlotNames[slot].clear();
    Manager().ReleaseSlot(slot);
}

// Access the controller-removal queue.
std::vector<PendingControllerRemoval>& PendingControllerRemovals() { return g_pendingControllerRemovals; }

// Access the deferred bot-kick queue.
std::vector<PendingBotKick>& PendingBotKicks() { return g_pendingBotKicks; }

void ReleaseAll()
{
    Manager().ReleaseAll();
    BotInfo().ResetAssignments();
    g_slotEntries.fill(nullptr);
    for (std::string& name : g_originalSlotNames)
        name.clear();
    g_pendingControllerRemovals.clear();
    g_pendingBotKicks.clear();
}

} // namespace cs2bh::identity_state
