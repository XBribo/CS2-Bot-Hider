#pragma once

#include <cstdint>

namespace cs2bh::identity_runtime {

// Apply native or disguised identity to managed clients.
void ApplyManagedDisguise(bool disguised);

// Select a collision-free SteamID.
uint64_t MakeUniqueSteamId(int slot, uint64_t desired);

// Sync the controller's fake-client bit.
bool SetControllerFakeClientFlag(int slot, bool fakeClient);

// Restore native bot identity before engine teardown.
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
