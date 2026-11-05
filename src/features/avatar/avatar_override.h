#pragma once

class INetworkStringTableContainer;

namespace cs2bh::avatar {

// Sets the network string-table interface used for avatar overrides
void SetStringTableContainer(INetworkStringTableContainer* container);

// Resets all per-slot avatar bookkeeping and published states
void ResetRuntime();

// Apply pending avatars on the game thread.
void ProcessOverrides();

} // namespace cs2bh::avatar
