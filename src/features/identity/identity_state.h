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

// Bind slot identity and its original name.
void BindSlot(int slot, const BotEntry* entry, const char* originalName);

// Read the slot's original engine name.
const std::string& OriginalSlotName(int slot);

// Release the slot's complete identity.
void ReleaseSlot(int slot);

// Access the controller-removal queue.
std::vector<PendingControllerRemoval>& PendingControllerRemovals();

// Access the deferred bot-kick queue.
std::vector<PendingBotKick>& PendingBotKicks();

// Release all identities and deferred work.
void ReleaseAll();

} // namespace identity_state

} // namespace cs2bh
