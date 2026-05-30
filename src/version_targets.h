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

    // CUtlString::Set mangled name in tier0.dll
    inline constexpr const char *kSym_CUtlString_Set =
        "?Set@CUtlString@@QEAAXPEBD@Z";

    // Interface version strings
    inline constexpr const char *kIface_ServerGameClients = "Source2GameClients001";

} // namespace cs2bh::targets
