#include "identity_state.h"

#include "personas.h"

#include <array>

namespace cs2bh::identity_state
{

    static std::array<const BotEntry *, PersonaPool::kMaxSlots> g_SlotEntries{};
    static std::array<std::string, PersonaPool::kMaxSlots> g_OriginalSlotNames{};
    static std::vector<PendingControllerRemoval> g_PendingControllerRemovals;
    static const std::string g_EmptyName;

    // Binds bot identity data and the original engine name to one slot
    void BindSlot(int slot, const BotEntry *entry, const char *originalName)
    {
        if (slot < 0 || slot >= PersonaPool::kMaxSlots)
            return;
        g_SlotEntries[slot] = entry;
        g_OriginalSlotNames[slot] = originalName ? originalName : "";
    }

    // Returns the bot identity entry bound to one slot
    const BotEntry *SlotEntry(int slot)
    {
        if (slot < 0 || slot >= PersonaPool::kMaxSlots)
            return nullptr;
        return g_SlotEntries[slot];
    }

    // Returns the original engine name captured for one slot
    const std::string &OriginalSlotName(int slot)
    {
        if (slot < 0 || slot >= PersonaPool::kMaxSlots)
            return g_EmptyName;
        return g_OriginalSlotNames[slot];
    }

    // Clears the identity data bound to one slot
    void ClearSlot(int slot)
    {
        if (slot < 0 || slot >= PersonaPool::kMaxSlots)
            return;
        g_SlotEntries[slot] = nullptr;
        g_OriginalSlotNames[slot].clear();
    }

    // Returns the pending controller-removal queue
    std::vector<PendingControllerRemoval> &PendingControllerRemovals()
    {
        return g_PendingControllerRemovals;
    }

    // Clears all slot bindings and pending controller removals
    void ClearAll()
    {
        g_SlotEntries.fill(nullptr);
        for (std::string &name : g_OriginalSlotNames)
            name.clear();
        g_PendingControllerRemovals.clear();
    }

} // namespace cs2bh::identity_state
