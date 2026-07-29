#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cs2bh
{

    struct BotEntry;

    namespace identity_state
    {

        struct PendingControllerRemoval
        {
            void *Controller = nullptr;
            uint32_t Handle = 0xFFFFFFFF;
            int Slot = -1;
            uint16_t UserId = 0;
            unsigned int ReferencedFrames = 0;
        };

        // Binds bot identity data and the original engine name to one slot
        void BindSlot(int slot, const BotEntry *entry, const char *originalName);

        // Returns the bot identity entry bound to one slot
        const BotEntry *SlotEntry(int slot);

        // Returns the original engine name captured for one slot
        const std::string &OriginalSlotName(int slot);

        // Clears the identity data bound to one slot
        void ClearSlot(int slot);

        // Returns the pending controller-removal queue
        std::vector<PendingControllerRemoval> &PendingControllerRemovals();

        // Clears all slot bindings and pending controller removals
        void ClearAll();

    } // namespace identity_state

} // namespace cs2bh
