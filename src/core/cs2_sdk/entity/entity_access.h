#pragma once

#include "core/memory_module.h"

#include <cstddef>

#include <nlohmann/json.hpp>

namespace cs2bh::entity_access {

// Resolves UTIL_Remove and its entity-system reference
void ResolveUtilRemoveAndEntSys(const nlohmann::json& gamedata, const modules::ModuleInfo& serverModule);

// Returns the resolved UTIL_Remove target
void* UtilRemoveTarget();

// Removes one entity through the resolved engine function
bool RemoveEntity(void* instance);

// Returns the resolved controller pawn-handle offset
int BotPawnHandleOffset();

// Resolves one server-side client from its slot
void* ResolveClientBySlot(int slot);

// Publishes changed userinfo for one client slot
bool RefreshClientUserInfo(int slot);

// Resolves one entity instance and optionally copies its class name
void* ResolveEntityInstance(int entityIndex, char* classnameOut, size_t classnameCap);

// Returns whether an entity is already entering deletion
bool IsEntityBeingDeleted(void* instance);

// Marks one flattened entity field as changed
void MarkEntityFieldChanged(void* instance, unsigned int offset);

// Resets the idle timer for the pawn owned by one client
void ResetIdleTimerForClient(void* client);

// Updates the engine-side name for one client
const char* SetEngineName(void* client, const char* newName);

// Clears resolved interfaces and runtime targets
void Reset();

} // namespace cs2bh::entity_access
