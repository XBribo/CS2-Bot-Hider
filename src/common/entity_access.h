#pragma once

#include "sig_scan.h"

#include <cstddef>

#include <nlohmann/json.hpp>

namespace cs2bh::entity_access {

// Stores the GameResourceService interface used for entity resolution
void SetGameResourceService(void* gameResourceService);

// Returns the current GameResourceService interface
void* GameResourceService();

// Overrides runtime member offsets from gamedata
void LoadMemberOffsets(const nlohmann::json& gamedata);

// Resolves UTIL_Remove and its entity-system reference
void ResolveUtilRemoveAndEntSys(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule);

// Returns the resolved UTIL_Remove target
void* UtilRemoveTarget();

// Returns the resolved entity-system global address
void* EntitySystemGlobalAddress();

// Removes one entity through the resolved engine function
bool RemoveEntity(void* instance);

// Logs a one-time comparison of both resolved entity-system paths
void LogEntitySystemCrossCheck();

// Stores the resolved controller pawn-handle offset
void SetBotPawnHandleOffset(int offset);

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
