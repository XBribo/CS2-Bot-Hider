#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cs2bh {

struct BotEntry;

namespace identity_state {

struct PendingControllerRemoval
{
    void* controller = nullptr;
    uint32_t handle = 0xFFFFFFFF;
    int slot = -1;
    uint16_t userId = 0;
    unsigned int referencedFrames = 0;
};

struct PendingBotKick
{
    int slot = -1;
    void* client = nullptr;
    uint16_t userId = 0;
    uint64_t incarnation = 0;
};

// Binds bot identity data and the original engine name to one slot
void BindSlot(int slot, const BotEntry* entry, const char* originalName);

// Returns the bot identity entry bound to one slot
const BotEntry* SlotEntry(int slot);

// Returns the original engine name captured for one slot
const std::string& OriginalSlotName(int slot);

// Clears the identity data bound to one slot
void ClearSlot(int slot);

// Returns the pending controller-removal queue
std::vector<PendingControllerRemoval>& PendingControllerRemovals();

// Returns the identity-checked bot commands waiting for a game frame.
std::vector<PendingBotKick>& PendingBotKicks();

// Clears all slot bindings, pending controller removals and bot commands.
void ClearAll();

} // namespace identity_state

} // namespace cs2bh
