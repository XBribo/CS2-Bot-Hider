#pragma once

#include <cstdint>

namespace cs2bh::identity_runtime {

// Apply the selected identity mode.
void ApplyManagedDisguise(bool disguised);

enum class SteamIdAvailability
{
    Available,
    InUse,
    CheckFailed,
};

// Checks SteamID availability.
SteamIdAvailability CheckSteamIdAvailability(int slot, uint64_t steamId);

// Selects an automatic identity.
uint64_t SelectSteamIdForBot(int slot, uint64_t desired);

// Sync the controller's fake-client bit.
bool SetControllerFakeClientFlag(int slot, bool fakeClient);

// Restore bot identity before teardown.
int RestoreManagedClientsForEngineTeardown();

// Queue identity-checked controller removal.
bool QueueControllerRemovalForClient(void* client, int slot);

// Process queued controller removals.
void DrainPendingControllerRemovals();

// Clear queued controller removals.
void ClearPendingControllerRemovals();

// Detect SourceTV during initialization.
bool IsHltvConnection(const char* name, const char* networkId);

// Release managed slots identified as SourceTV.
bool ReleaseManagedHltvSlot(int slot, void* client);

} // namespace cs2bh::identity_runtime
