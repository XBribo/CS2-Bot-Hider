#pragma once

#include <cstdint>

namespace cs2bh::identity_runtime {

// Counts connected human clients
int CountHumanClients();

// Applies the requested native or disguised identity to all managed clients
void ApplyManagedDisguise(bool disguised);


// Selects a non-colliding SteamID for one managed slot
uint64_t MakeUniqueSteamId(int slot, uint64_t desired);

// Synchronizes the controller fake-client bit for one slot
bool SetControllerFakeClientFlag(int slot, bool fakeClient);

// Restores native identity for managed bots before engine-owned teardown
int RestoreManagedClientsForEngineTeardown();

// Queues one client controller for identity-checked removal
bool QueueControllerRemovalForClient(void* client, int slot);

// Processes queued controller removals
void DrainPendingControllerRemovals();

// Clears queued controller removals
void ClearPendingControllerRemovals();

// Identifies SourceTV before its server-side flag is initialized
bool IsHltvConnection(const char* name, const char* networkId);

// Releases one managed slot after it resolves to SourceTV
bool ReleaseManagedHltvSlot(int slot, void* client);

} // namespace cs2bh::identity_runtime
