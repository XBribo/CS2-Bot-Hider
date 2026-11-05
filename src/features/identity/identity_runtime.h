#pragma once

#include <cstdint>
#include <unordered_set>

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

// Collect live SteamIDs and managed reservations for this allocation.
bool CollectOccupiedSteamIds(int slot, std::unordered_set<uint64_t>& occupied);

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
