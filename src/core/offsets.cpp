#include "core/offsets.h"
#include <cs2_sdk/schema.h>
#include "core/interfaces.h"
#include <cs2_sdk/gameconfig.h>
#include "core/log.h"
#include "serversideclient_ref.h"

#include <cstdio>

namespace cs2bh::offsets {

// Resolves required flags and optional pawn handles.
bool LoadFromSchema(char* error, size_t maxlen)
{
    if (!cs2_sdk::schema::Init(g_schemaSystem, error, maxlen)) return false;
    const cs2_sdk::schema::FieldBinding fields[] = {
        { .target = &g_baseEntityFlagsOffset, .className = "CBaseEntity", .fieldName = "m_fFlags" },
    };
    if (!cs2_sdk::schema::ResolveFields(fields, error, maxlen)) return false;
    BH_LOG_DEBUG("CBaseEntity::m_fFlags=0x%x", offsets::g_baseEntityFlagsOffset);

    g_botPawnHandleOffset = cs2_sdk::schema::GetFieldOffset("CCSPlayerController", "m_hPlayerPawn");
    if (g_botPawnHandleOffset < 0) g_botPawnHandleOffset = cs2_sdk::schema::GetFieldOffset("CBasePlayerController", "m_hPawn");
    if (g_botPawnHandleOffset < 0) BH_LOG_WARN("FL_BOT override disabled: pawn handle unresolved");
    return true;
}

bool LoadFromGamedata(const nlohmann::json& gamedata, char* error, size_t maxlen)
{
    const cs2_sdk::gameconfig::OffsetBinding offsets[] = {
        { &ssc::g_nameOffset, "CServerSideClient::m_Name" },
        { &ssc::g_entityIndexOffset, "CServerSideClient::m_nEntityIndex" },
        { &ssc::g_netChannelOffset, "CServerSideClient::m_NetChannel" },
        { &ssc::g_connectionTypeFlagsOffset, "CServerSideClient::m_nConnectionTypeFlags" },
        { &ssc::g_fakePlayerOffset, "CServerSideClient::m_bFakePlayer" },
        { &ssc::g_userIdOffset, "CServerSideClient::m_UserID" },
        { &ssc::g_steamIdOffset, "CServerSideClient::m_SteamID" },
        { &ssc::g_steamIdMirrorOffset, "CServerSideClient::m_SteamIDMirror" },
        { &ssc::g_isHltvOffset, "CServerSideClient::m_bIsHLTV" },
        { &g_clientListOffset, "CNetworkGameServerBase::m_Clients" },
        { &g_entitySystemIdentityChunksOffset, "CEntitySystem::m_EntityList" },
        { &g_entityIdentitySize, "CEntityIdentity::Size" },
        { &g_entityIdentityInstanceOffset, "CEntityIdentity::m_pInstance" },
        { &g_entityIdentityClassNameOffset, "CEntityIdentity::m_designerName" },
    };
    if (!cs2_sdk::gameconfig::LoadRequiredOffsets(gamedata, offsets, error, maxlen)) return false;
    if (g_entityIdentitySize == 0)
    {
        if (error && maxlen) std::snprintf(error, maxlen, "CEntityIdentity::Size must be positive");
        return false;
    }
    g_vtableSlotClientSetName = cs2_sdk::gameconfig::FindPlatformOffset(gamedata, "CServerSideClient::SetName", -1);
    return true;
}

} // namespace cs2bh::offsets
