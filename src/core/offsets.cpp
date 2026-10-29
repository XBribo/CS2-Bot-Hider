#include "core/offsets.h"
#include "core/cs2_sdk/src/core/schema.h"
#include "core/interfaces.h"
#include "core/cs2_sdk/src/core/gameconfig.h"
#include "core/log.h"
#include "serversideclient_ref.h"

#include <cstdio>
#include <string>

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
        BH_LOG_ERROR("%s", error);
        return false;
    }
    BH_LOG_DEBUG("CBaseEntity::m_fFlags=0x%x", offsets::g_baseEntityFlagsOffset);

    if (schemaReady)
    {
        int pawnOff = cs2_sdk::schema::GetFieldOffset("CBasePlayerController", "m_hPawn");
        int playerPawnOff = cs2_sdk::schema::GetFieldOffset("CCSPlayerController", "m_hPlayerPawn");
        g_botPawnHandleOffset = playerPawnOff >= 0 ? playerPawnOff : pawnOff;
        if (g_botPawnHandleOffset < 0) BH_LOG_WARN("FL_BOT override disabled: pawn handle unresolved");
    }
    else
    {
        g_botPawnHandleOffset = -1;
        BH_LOG_WARN("idle-kick and FL_BOT overrides disabled: Schema unavailable");
    }
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
    std::string invalid;
    for (const auto& binding : offsets)
    {
        *binding.target = cs2_sdk::gameconfig::FindPlatformOffset(gamedata, binding.name, -1);
        if (*binding.target < 0 || (binding.target == &g_entityIdentitySize && *binding.target == 0))
        {
            if (!invalid.empty()) invalid += ", ";
            invalid += binding.name;
        }
    }
    g_vtableSlotClientSetName = cs2_sdk::gameconfig::FindPlatformOffset(gamedata, "CServerSideClient::SetName", -1);
    if (!invalid.empty())
    {
        std::snprintf(error, maxlen, "required gamedata offsets missing or invalid (%s): %s", cs2_sdk::gameconfig::PlatformName(),
                      invalid.c_str());
        return false;
    }
    return true;
}

} // namespace cs2bh::offsets
