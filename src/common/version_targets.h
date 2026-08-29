// version_targets.h

#pragma once

#include <cstdint>

namespace cs2bh::targets {

// CNetworkGameServerBase::m_Clients — CUtlVector<CServerSideClient*>
inline int g_clientListOffset = 584;

// CServerSideClient::m_bFakePlayer
inline int g_fakePlayerOffset = 160;

// CServerSideClient::m_Name — CUtlString { char* m_pString } @ +0
inline int g_nameOffset = 64;

// IServerGameClients (VCSource2GameClients) vtable slots
inline constexpr int kVtSlotOnClientConnected = 11;
inline constexpr int kVtSlotClientPutInServer = 13;

// Current CServerSideClient::SetName vtable slot
inline int g_vtableSlotClientSetName = -1;

// INetworkGameServer::StartChangeLevel vtable slot
inline constexpr int kVtSlotStartChangeLevel = 39;

// Schema candidates
inline constexpr int kSchemaFallbackMIszPlayerName = 1300; // 0x514
inline constexpr int kSchemaFallbackMIPing = 2048; // 0x800

// * UTIL_Remove(CEntityInstance*) in the server module

inline constexpr const char* kIfaceGameResourceServiceServer = "GameResourceServiceServerV001";
inline int g_entitySystemOffsetInGameResourceService = 0x58; // GameResourceService → CGameEntitySystem*
inline int g_entitySystemIdentityChunksOffset = 0x10; // CEntitySystem → m_pIdentityChunks[]
inline int g_entityIdentitySize = 0x70; // sizeof(CEntityIdentity) = 112
inline int g_entityIdentityInstanceOffset = 0x00; // CEntityIdentity::m_pInstance
inline int g_entityIdentityClassNameOffset = 0x20; // CEntityIdentity::m_designerName
inline constexpr int kEntListChunkSize = 512; // entities per identity chunk

// CBasePlayerController::m_iszPlayerName
inline constexpr int kControllerPlayerNameOffset = 1780;

inline int g_controllerFakeClientFlagsOffset = 904; // 0x388
inline int g_controllerTeamOffset = 836;

// CBaseEntity::m_fFlags network field
inline constexpr int kBaseEntityFlagsOffset = 0x388;
inline constexpr uint32_t kEntityFlagBot = 0x10;

#if defined(_WIN32)
inline constexpr const char* kServerModuleName = "server.dll";
inline constexpr const char* kEngineModuleName = "engine2.dll";
inline constexpr const char* kSchemaSystemModuleName = "schemasystem.dll";
inline constexpr const char* kSchemaServerTypeScope = "server.dll";
#else
inline constexpr const char* kEngineModuleName = "libengine2.so";
inline constexpr const char* kServerModuleName = "libserver.so";
inline constexpr const char* kSchemaSystemModuleName = "libschemasystem.so";
inline constexpr const char* kSchemaServerTypeScope = "libserver.so";
#endif

// Interface version strings
inline constexpr const char* kIfaceServerGameClients = "Source2GameClients001";

} // namespace cs2bh::targets
