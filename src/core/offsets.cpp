#include "core/offsets.h"
#include "core/cs2_sdk/src/core/schema.h"
#include "core/interfaces.h"
#include "core/cs2_sdk/src/core/gameconfig.h"
#include "core/log.h"
#include "serversideclient_ref.h"

#include <cstdio>

namespace cs2bh::offsets {

// Resolves required flags and optional pawn handles.
bool LoadFromSchema(char* error, size_t maxlen)
{
    const bool schemaReady = cs2_sdk::schema::Init(g_schemaSystem, error, maxlen);
    const cs2_sdk::schema::FieldBinding fields[] = {
        { .target = &g_baseEntityFlagsOffset, .className = "CBaseEntity", .fieldName = "m_fFlags" },
    };
    const bool fieldsReady = cs2_sdk::schema::ResolveFields(fields);
    if (!fieldsReady)
    {
        std::snprintf(error, maxlen, "required CBaseEntity Schema offset unavailable: m_fFlags=%d; identity hooks disabled",
                      offsets::g_baseEntityFlagsOffset);
        BH_LOG_ERROR("%s\n", error);
        return false;
    }
    BH_LOG_DEBUG("Schema CBaseEntity: m_fFlags=0x%x\n", offsets::g_baseEntityFlagsOffset);

    if (schemaReady)
    {
        int pawnOff = cs2_sdk::schema::GetFieldOffset("CBasePlayerController", "m_hPawn");
        int playerPawnOff = cs2_sdk::schema::GetFieldOffset("CCSPlayerController", "m_hPlayerPawn");
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

void LoadFromGamedata(const nlohmann::json& gamedata)
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
        { &g_vtableSlotClientSetName, "CServerSideClient::SetName" },
        { &g_entitySystemIdentityChunksOffset, "CEntitySystem::m_EntityList" },
        { &g_entityIdentitySize, "CEntityIdentity::Size" },
        { &g_entityIdentityInstanceOffset, "CEntityIdentity::m_pInstance" },
        { &g_entityIdentityClassNameOffset, "CEntityIdentity::m_designerName" },
    };
    cs2_sdk::gameconfig::LoadOffsets(gamedata, offsets);
}

} // namespace cs2bh::offsets
