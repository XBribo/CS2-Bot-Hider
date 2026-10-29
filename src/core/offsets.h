#pragma once

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace cs2bh::offsets {

// Resolve entity fields before hook preparation.
bool LoadFromSchema(char* error, size_t maxlen);

// Loads required platform offsets.
bool LoadFromGamedata(const nlohmann::json& gamedata, char* error, size_t maxlen);

// CNetworkGameServerBase::m_Clients — CUtlVector<CServerSideClient*>
inline int g_clientListOffset = -1;

// CServerSideClient::SetName vtable slot
inline int g_vtableSlotClientSetName = -1;

inline int g_entitySystemIdentityChunksOffset = -1; // CEntitySystem → m_pIdentityChunks[]
inline int g_entityIdentitySize = -1; // sizeof(CEntityIdentity)
inline int g_entityIdentityInstanceOffset = -1; // CEntityIdentity::m_pInstance
inline int g_entityIdentityClassNameOffset = -1; // CEntityIdentity::m_designerName
inline constexpr int kEntListChunkSize = 512; // entities per identity chunk

// CBaseEntity Schema fields resolved before hook installation
inline int g_baseEntityFlagsOffset = -1;
inline int g_botPawnHandleOffset = -1;
inline constexpr uint32_t kEntityFlagBot = 0x10;
inline constexpr uint32_t kEntityFlagFakeClient = 0x100;

} // namespace cs2bh::offsets
