#include "core/offsets.h"
#include "core/cs2_sdk/schema.h"
#include "core/gameconfig.h"
#include "core/log.h"
#include "serversideclient_ref.h"

#include <cstdio>

namespace cs2bh::offsets {

// Resolves required identity fields before hooks can write to entities.
bool LoadFromSchema(char* error, size_t maxlen)
{
    // Require live entity offsets before any identity writes or hook preparation
    const bool schemaReady = schema::Init();
    offsets::g_baseEntityFlagsOffset = schemaReady ? schema::GetFieldOffset("CBaseEntity", "m_fFlags") : -1;
    offsets::g_controllerTeamOffset = schemaReady ? schema::GetFieldOffset("CBaseEntity", "m_iTeamNum") : -1;
    if (offsets::g_baseEntityFlagsOffset < 0 || offsets::g_controllerTeamOffset < 0)
    {
        std::snprintf(error, maxlen, "required CBaseEntity Schema offsets unavailable: m_fFlags=%d m_iTeamNum=%d; identity hooks disabled",
                      offsets::g_baseEntityFlagsOffset, offsets::g_controllerTeamOffset);
        BH_LOG_ERROR("%s\n", error);
        return false;
    }
    BH_LOG_DEBUG("Schema CBaseEntity: m_fFlags=0x%x m_iTeamNum=0x%x\n", offsets::g_baseEntityFlagsOffset, offsets::g_controllerTeamOffset);

    // Resolve controller pawn and idle-timer schema offsets
    if (schemaReady)
    {
        int pawnOff = schema::GetFieldOffset("CBasePlayerController", "m_hPawn");
        int playerPawnOff = schema::GetFieldOffset("CCSPlayerController", "m_hPlayerPawn");
        g_botPawnHandleOffset = playerPawnOff >= 0 ? playerPawnOff : pawnOff;
        if (g_botPawnHandleOffset < 0) BH_LOG_WARN("bot pawn handle unresolved - FL_BOT override disabled\n");
    }
    else
    {
        g_botPawnHandleOffset = -1;
        BH_LOG_WARN("SchemaSystem unresolved — idle-kick and FL_BOT overrides disabled\n");
    }
    return true;
}

// Overrides runtime member offsets from gamedata
void LoadFromGamedata(const nlohmann::json& gamedata)
{
    using gameconfig::FindPlatformOffset;

    ssc::g_userIdStringOffset = FindPlatformOffset(gamedata, "CServerSideClient::m_UserIDString", ssc::g_userIdStringOffset);
    ssc::g_nameOffset = FindPlatformOffset(gamedata, "CServerSideClient::m_Name", ssc::g_nameOffset);
    ssc::g_clientSlotOffset = FindPlatformOffset(gamedata, "CServerSideClient::m_nClientSlot", ssc::g_clientSlotOffset);
    ssc::g_entityIndexOffset = FindPlatformOffset(gamedata, "CServerSideClient::m_nEntityIndex", ssc::g_entityIndexOffset);
    ssc::g_serverOffset = FindPlatformOffset(gamedata, "CServerSideClient::m_Server", ssc::g_serverOffset);
    ssc::g_netChannelOffset = FindPlatformOffset(gamedata, "CServerSideClient::m_NetChannel", ssc::g_netChannelOffset);
    ssc::g_connectionTypeFlagsOffset =
        FindPlatformOffset(gamedata, "CServerSideClient::m_nConnectionTypeFlags", ssc::g_connectionTypeFlagsOffset);
    ssc::g_signonStateOffset = FindPlatformOffset(gamedata, "CServerSideClient::m_nSignonState", ssc::g_signonStateOffset);
    ssc::g_attachedToOffset = FindPlatformOffset(gamedata, "CServerSideClient::m_pAttachedTo", ssc::g_attachedToOffset);
    ssc::g_fakePlayerOffset = FindPlatformOffset(gamedata, "CServerSideClient::m_bFakePlayer", ssc::g_fakePlayerOffset);
    ssc::g_userIdOffset = FindPlatformOffset(gamedata, "CServerSideClient::m_UserID", ssc::g_userIdOffset);
    ssc::g_steamIdOffset = FindPlatformOffset(gamedata, "CServerSideClient::m_SteamID", ssc::g_steamIdOffset);
    ssc::g_steamIdMirrorOffset = FindPlatformOffset(gamedata, "CServerSideClient::m_SteamIDMirror", ssc::g_steamIdMirrorOffset);
    ssc::g_isHltvOffset = FindPlatformOffset(gamedata, "CServerSideClient::m_bIsHLTV", ssc::g_isHltvOffset);

    offsets::g_clientListOffset = FindPlatformOffset(gamedata, "CNetworkGameServerBase::m_Clients", offsets::g_clientListOffset);
    offsets::g_vtableSlotClientSetName = FindPlatformOffset(gamedata, "CServerSideClient::SetName", offsets::g_vtableSlotClientSetName);
    offsets::g_entitySystemOffsetInGameResourceService =
        FindPlatformOffset(gamedata, "GameResourceServiceServer::m_pEntitySystem", offsets::g_entitySystemOffsetInGameResourceService);
    offsets::g_entitySystemIdentityChunksOffset =
        FindPlatformOffset(gamedata, "CEntitySystem::m_EntityList", offsets::g_entitySystemIdentityChunksOffset);
    offsets::g_entityIdentitySize = FindPlatformOffset(gamedata, "CEntityIdentity::Size", offsets::g_entityIdentitySize);
    offsets::g_entityIdentityInstanceOffset =
        FindPlatformOffset(gamedata, "CEntityIdentity::m_pInstance", offsets::g_entityIdentityInstanceOffset);
    offsets::g_entityIdentityClassNameOffset =
        FindPlatformOffset(gamedata, "CEntityIdentity::m_designerName", offsets::g_entityIdentityClassNameOffset);
}

} // namespace cs2bh::offsets
