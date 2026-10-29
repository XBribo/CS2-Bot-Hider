#pragma once

#include "core/cs2_sdk/src/core/memory_module.h"

#include <cstddef>

#include <nlohmann/json.hpp>

namespace cs2bh::entity_access {

// Resolve UTIL_Remove and its entity-system reference.
void ResolveUtilRemoveAndEntSys(const nlohmann::json& gamedata, const cs2_sdk::modules::ModuleInfo& serverModule);

// Read the UTIL_Remove target.
void* UtilRemoveTarget();

// Check whether the entity-system reference was resolved.
bool HasEntitySystem();

// Remove an entity through the engine.
bool RemoveEntity(void* instance);

// Read the controller pawn-handle offset.
int BotPawnHandleOffset();

// Resolve a server-side client by slot.
void* ResolveClientBySlot(int slot);

// Publish changed client userinfo.
bool RefreshClientUserInfo(int slot);

// Resolve an entity and optionally copy its class name.
void* ResolveEntityInstance(int entityIndex, char* classnameOut, size_t classnameCap);

// Check entity deletion state.
bool IsEntityBeingDeleted(void* instance);

// Notify a flattened entity field change.
void MarkEntityFieldChanged(void* instance, unsigned int offset);

// Reset the client's pawn idle timer.
void ResetIdleTimerForClient(void* client);

// Update the engine-side client name.
const char* SetEngineName(void* client, const char* newName);

// Clear resolved interfaces and targets.
void Reset();

} // namespace cs2bh::entity_access
