#pragma once

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace cs2bh::offsets {

// Resolve entity fields before hook preparation.
bool LoadFromSchema(char* error, size_t maxlen);

// Load platform offsets with fallback values.
void LoadFromGamedata(const nlohmann::json& gamedata);

// CNetworkGameServerBase::m_Clients — CUtlVector<CServerSideClient*>
inline int g_clientListOffset = 616;

// IServerGameClients (VCSource2GameClients) vtable slots
inline constexpr int kVtSlotOnClientConnected = 11;
inline constexpr int kVtSlotClientPutInServer = 13;

// CServerSideClient::SetName vtable slot
inline int g_vtableSlotClientSetName = -1;

// INetworkGameServer::StartChangeLevel vtable slot
inline constexpr int kVtSlotStartChangeLevel = 39;

inline int g_entitySystemIdentityChunksOffset = 0x10; // CEntitySystem → m_pIdentityChunks[]
inline int g_entityIdentitySize = 0x70; // sizeof(CEntityIdentity) = 112
inline int g_entityIdentityInstanceOffset = 0x00; // CEntityIdentity::m_pInstance
inline int g_entityIdentityClassNameOffset = 0x20; // CEntityIdentity::m_designerName
inline constexpr int kEntListChunkSize = 512; // entities per identity chunk

// CBaseEntity Schema fields resolved before hook installation
inline int g_baseEntityFlagsOffset = -1;
inline int g_botPawnHandleOffset = -1;
inline constexpr uint32_t kEntityFlagBot = 0x10;

// Interface version strings
inline constexpr const char* kIfaceServerGameClients = "Source2GameClients001";

} // namespace cs2bh::offsets
