// version_targets.h
//
// Single source of truth for every reverse-engineered offset, vtable slot,
// and schema candidate referenced by the plugin

#pragma once

#include <cstdint>

namespace cs2bh::targets
{

    // CNetworkGameServerBase::m_Clients — CUtlVector<CServerSideClient*>
    // at +592 (m_nCount), +600 (m_pElements), +608 (m_nAllocCount), +612 (flags)
    inline constexpr int kClientListOffset = 592;

    // CServerSideClient::m_bFakePlayer
    inline constexpr int kFakePlayerOffset = 160;

    // CServerSideClient::m_Name — CUtlString { char* m_pString } @ +0
    inline constexpr int kNameOffset = 64;

    // IServerGameClients (VCSource2GameClients) vtable slots
    inline constexpr int kVTSlot_OnClientConnected = 11;
    inline constexpr int kVTSlot_ClientPutInServer = 13;

    // IVEngineServer::CreateFakeClient vtable slot
    inline constexpr int kVTSlot_CreateFakeClient = 53;

    // INetworkGameServer::StartChangeLevel vtable slot
    inline constexpr int kVTSlot_StartChangeLevel = 39;

    // Schema candidates
    inline constexpr int kSchemaFallback_m_iszPlayerName = 1300; // 0x514
    inline constexpr int kSchemaFallback_m_iPing = 2048;         // 0x800

    // * UTIL_Remove(CEntityInstance*) in server.dll
    // It null-checks the entity, then calls CGameEntitySystem::QueueDestroyEntity to mark it EF_MARKED_FOR_DELETE
    // Used to destroy the CCSPlayerController a kicked bot leaves behind

    inline constexpr const char *kIface_GameResourceServiceServer = "GameResourceServiceServerV001";
    inline constexpr int kEntSys_OffsetInGameResSvc = 0x58;   // GameResourceService → CGameEntitySystem*
    inline constexpr int kEntSys_IdentityChunksOffset = 0x10; // CEntitySystem → m_pIdentityChunks[]
    inline constexpr int kEntIdentity_Size = 0x70;            // sizeof(CEntityIdentity) = 112 (runtime-verified stride)
    inline constexpr int kEntIdentity_InstanceOffset = 0x00;  // CEntityIdentity::m_pInstance
    inline constexpr int kEntListChunkSize = 512;             // entities per identity chunk

    // CBasePlayerController::m_iszPlayerName
    inline constexpr int kController_PlayerNameOffset = 1780;

    // CUtlString::Set mangled name in tier0.dll
#if defined(_WIN32)
    inline constexpr const char *kSym_CUtlString_Set =
        "?Set@CUtlString@@QEAAXPEBD@Z";
#else
    inline constexpr const char *kSym_CUtlString_Set =
        "_ZN10CUtlString3SetEPKc";
#endif

#if defined(_WIN32)
    inline constexpr const char *kServerModuleName = "server.dll";
    inline constexpr const char *kTier0ModuleName = "tier0.dll";
    inline constexpr const char *kSchemaSystemModuleName = "schemasystem.dll";
    inline constexpr const char *kSchemaServerTypeScope = "server.dll";
#else
    inline constexpr const char *kServerModuleName = "libserver.so";
    inline constexpr const char *kTier0ModuleName = "libtier0.so";
    inline constexpr const char *kSchemaSystemModuleName = "libschemasystem.so";
    inline constexpr const char *kSchemaServerTypeScope = "libserver.so";
#endif

    // Interface version strings
    inline constexpr const char *kIface_ServerGameClients = "Source2GameClients001";

} // namespace cs2bh::targets
