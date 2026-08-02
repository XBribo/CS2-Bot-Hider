#include "entity_access.h"

#include "plugin.h"
#include "schema_resolver.h"
#include "serversideclient_ref.h"
#include "version_targets.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <eiface.h>
#include <entity2/entityinstance.h>
#include <iserver.h>
#include <tier1/utlvector.h>

#if defined(_WIN32)
#include <Windows.h>
#define CS2BH_FASTCALL __fastcall
#else
#define CS2BH_FASTCALL
#endif

extern INetworkServerService* g_pNetworkServerService;

namespace cs2bh::entity_access {

using UtilRemoveFn = void(CS2BH_FASTCALL*)(void*);
using ClientSetNameFn = void(CS2BH_FASTCALL*)(void*, const char*);

static void* g_pGameResourceService = nullptr;
static UtilRemoveFn g_pfnUtilRemove = nullptr;
static void** g_ppEntSysGlobal = nullptr;
static int g_BotPawnHandleOffset = -1;

// Stores the GameResourceService interface used for entity resolution
void SetGameResourceService(void* gameResourceService) { g_pGameResourceService = gameResourceService; }

// Returns the current GameResourceService interface
void* GameResourceService() { return g_pGameResourceService; }

// Resolves one server-side client from its slot
void* ResolveClientBySlot(int slot)
{
    if (!g_pNetworkServerService || targets::kClientListOffset < 0) return nullptr;
    auto* gameServer = g_pNetworkServerService->GetIGameServer();
    if (!gameServer) return nullptr;
    auto* clients = reinterpret_cast<CUtlVector<void*>*>(reinterpret_cast<unsigned char*>(gameServer) + targets::kClientListOffset);
    const int count = clients->Count();
    if (count < 0 || count > 256 || slot < 0 || slot >= count) return nullptr;
    return clients->Element(slot);
}

// Publishes changed userinfo for one client slot
bool RefreshClientUserInfo(int slot)
{
    if (!g_pNetworkServerService || slot < 0 || slot >= 64) return false;
    auto* gameServer = g_pNetworkServerService->GetIGameServer();
    if (!gameServer) return false;
    gameServer->UserInfoChanged(CPlayerSlot(slot));
    return true;
}

// Resolves UTIL_Remove and its entity-system reference
void ResolveUtilRemoveAndEntSys(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule)
{
    g_pfnUtilRemove = nullptr;
    g_ppEntSysGlobal = nullptr;
    if (!serverModule)
    {
        META_CONPRINTF("[BOTHIDER] warning: %s module unresolved for signature scan\n", targets::kServerModuleName);
        return;
    }

    std::string signature = sig::FindPlatformSig(gamedata, "UTIL_Remove");
    std::vector<uint8_t> bytes;
    std::vector<bool> wildcards;
    if (signature.empty() || !sig::ParseSigString(signature, bytes, wildcards))
    {
        META_CONPRINTF("[BOTHIDER] warning: UTIL_Remove %s sig missing/malformed in gamedata.json\n", sig::PlatformName());
        return;
    }

    auto* hit = static_cast<unsigned char*>(sig::FindPatternIn(serverModule, bytes, wildcards));
    if (!hit) return;
    g_pfnUtilRemove = reinterpret_cast<UtilRemoveFn>(hit);

    /*
     * Windows: mov rcx, [rip+disp32]
     * Linux: lea rax, [rip+disp32]; mov rdi, [rax]
     */
    for (size_t i = 0; i + 7 <= bytes.size(); ++i)
    {
        const bool isWindowsMov = bytes[i] == 0x48 && bytes[i + 1] == 0x8B && bytes[i + 2] == 0x0D;
        const bool isLinuxLea = bytes[i] == 0x48 && bytes[i + 1] == 0x8D && bytes[i + 2] == 0x05;
        if (!isWindowsMov && !isLinuxLea) continue;

        unsigned char* displacementAddress = hit + i + 3;
        const int32_t displacement = *reinterpret_cast<int32_t*>(displacementAddress);
        unsigned char* instructionEnd = displacementAddress + 4;
        g_ppEntSysGlobal = reinterpret_cast<void**>(instructionEnd + displacement);
        break;
    }
}

// Overrides runtime member offsets from gamedata
void LoadMemberOffsets(const nlohmann::json& gamedata)
{
    using sig::FindPlatformOffset;

    ssc::OFFSET_m_UserIDString = FindPlatformOffset(gamedata, "CServerSideClient::m_UserIDString", ssc::OFFSET_m_UserIDString);
    ssc::OFFSET_m_Name = FindPlatformOffset(gamedata, "CServerSideClient::m_Name", ssc::OFFSET_m_Name);
    ssc::OFFSET_m_nClientSlot = FindPlatformOffset(gamedata, "CServerSideClient::m_nClientSlot", ssc::OFFSET_m_nClientSlot);
    ssc::OFFSET_m_nEntityIndex = FindPlatformOffset(gamedata, "CServerSideClient::m_nEntityIndex", ssc::OFFSET_m_nEntityIndex);
    ssc::OFFSET_m_Server = FindPlatformOffset(gamedata, "CServerSideClient::m_Server", ssc::OFFSET_m_Server);
    ssc::OFFSET_m_NetChannel = FindPlatformOffset(gamedata, "CServerSideClient::m_NetChannel", ssc::OFFSET_m_NetChannel);
    ssc::OFFSET_m_nConnectionTypeFlags =
        FindPlatformOffset(gamedata, "CServerSideClient::m_nConnectionTypeFlags", ssc::OFFSET_m_nConnectionTypeFlags);
    ssc::OFFSET_m_nSignonState = FindPlatformOffset(gamedata, "CServerSideClient::m_nSignonState", ssc::OFFSET_m_nSignonState);
    ssc::OFFSET_m_pAttachedTo = FindPlatformOffset(gamedata, "CServerSideClient::m_pAttachedTo", ssc::OFFSET_m_pAttachedTo);
    ssc::OFFSET_m_bFakePlayer = FindPlatformOffset(gamedata, "CServerSideClient::m_bFakePlayer", ssc::OFFSET_m_bFakePlayer);
    ssc::OFFSET_m_UserID = FindPlatformOffset(gamedata, "CServerSideClient::m_UserID", ssc::OFFSET_m_UserID);
    ssc::OFFSET_m_SteamID = FindPlatformOffset(gamedata, "CServerSideClient::m_SteamID", ssc::OFFSET_m_SteamID);
    ssc::OFFSET_m_SteamIDMirror = FindPlatformOffset(gamedata, "CServerSideClient::m_SteamIDMirror", ssc::OFFSET_m_SteamIDMirror);
    ssc::OFFSET_m_bIsHLTV = FindPlatformOffset(gamedata, "CServerSideClient::m_bIsHLTV", ssc::OFFSET_m_bIsHLTV);

    targets::kClientListOffset = FindPlatformOffset(gamedata, "CNetworkGameServerBase::m_Clients", targets::kClientListOffset);
    targets::kController_FakeClientFlagsOffset =
        FindPlatformOffset(gamedata, "CBasePlayerController::FakeClientFlags", targets::kController_FakeClientFlagsOffset);
    targets::kController_TeamOffset = FindPlatformOffset(gamedata, "CBaseEntity::m_iTeamNum", targets::kController_TeamOffset);
    targets::kVTSlot_ClientSetName = FindPlatformOffset(gamedata, "CServerSideClient::SetName", targets::kVTSlot_ClientSetName);
    targets::kEntSys_OffsetInGameResSvc =
        FindPlatformOffset(gamedata, "GameResourceServiceServer::m_pEntitySystem", targets::kEntSys_OffsetInGameResSvc);
    targets::kEntSys_IdentityChunksOffset =
        FindPlatformOffset(gamedata, "CEntitySystem::m_EntityList", targets::kEntSys_IdentityChunksOffset);
    targets::kEntIdentity_Size = FindPlatformOffset(gamedata, "CEntityIdentity::Size", targets::kEntIdentity_Size);
    targets::kEntIdentity_InstanceOffset =
        FindPlatformOffset(gamedata, "CEntityIdentity::m_pInstance", targets::kEntIdentity_InstanceOffset);
    targets::kEntIdentity_ClassNameOffset =
        FindPlatformOffset(gamedata, "CEntityIdentity::m_designerName", targets::kEntIdentity_ClassNameOffset);
}

// Reads one pointer while isolating invalid memory access on Windows
static bool SafeReadPointer(const void* address, void** output)
{
#if defined(_WIN32)
    __try
    {
        *output = *reinterpret_cast<void* const*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *output = nullptr;
        return false;
    }
#else
    if (!address)
    {
        *output = nullptr;
        return false;
    }
    *output = *reinterpret_cast<void* const*>(address);
    return true;
#endif
}

// Copies one indirectly referenced string with guarded reads on Windows
static bool SafeReadString(const void* address, char* output, size_t capacity)
{
#if defined(_WIN32)
    __try
    {
        const char* source = *reinterpret_cast<const char* const*>(address);
        if (!source)
        {
            output[0] = '\0';
            return false;
        }
        size_t i = 0;
        for (; i + 1 < capacity && source[i]; ++i)
            output[i] = source[i];
        output[i] = '\0';
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        output[0] = '\0';
        return false;
    }
#else
    if (!address)
    {
        output[0] = '\0';
        return false;
    }
    const char* source = *reinterpret_cast<const char* const*>(address);
    if (!source)
    {
        output[0] = '\0';
        return false;
    }
    size_t i = 0;
    for (; i + 1 < capacity && source[i]; ++i)
        output[i] = source[i];
    output[i] = '\0';
    return true;
#endif
}

// Resolves one entity instance and optionally copies its class name
void* ResolveEntityInstance(int entityIndex, char* classnameOut, size_t classnameCap)
{
    if (classnameOut && classnameCap) classnameOut[0] = '\0';
    if (!g_pGameResourceService || entityIndex <= 0 || entityIndex >= 0x8000 || targets::kEntSys_OffsetInGameResSvc < 0 ||
        targets::kEntSys_IdentityChunksOffset < 0 || targets::kEntIdentity_Size <= 0 || targets::kEntIdentity_InstanceOffset < 0 ||
        (classnameOut && classnameCap && targets::kEntIdentity_ClassNameOffset < 0))
    {
        return nullptr;
    }

    void* entitySystem = nullptr;
    if (!SafeReadPointer(reinterpret_cast<unsigned char*>(g_pGameResourceService) + targets::kEntSys_OffsetInGameResSvc, &entitySystem) ||
        !entitySystem)
    {
        return nullptr;
    }

    void* chunk = nullptr;
    const void* chunkSlot = reinterpret_cast<unsigned char*>(entitySystem) + targets::kEntSys_IdentityChunksOffset +
                            (entityIndex / targets::kEntListChunkSize) * sizeof(void*);
    if (!SafeReadPointer(chunkSlot, &chunk) || !chunk) return nullptr;

    unsigned char* identity =
        reinterpret_cast<unsigned char*>(chunk) + (entityIndex % targets::kEntListChunkSize) * targets::kEntIdentity_Size;
    if (classnameOut && classnameCap)
    {
        SafeReadString(identity + targets::kEntIdentity_ClassNameOffset, classnameOut, classnameCap);
    }

    void* instance = nullptr;
    if (!SafeReadPointer(identity + targets::kEntIdentity_InstanceOffset, &instance) || !instance)
    {
        return nullptr;
    }
    return instance;
}

// Returns whether an entity is already entering deletion
bool IsEntityBeingDeleted(void* instance)
{
    if (!instance) return true;
    auto* entity = reinterpret_cast<CEntityInstance*>(instance);
    if (!entity->m_pEntity) return true;
    const uint32_t flags = static_cast<uint32_t>(entity->m_pEntity->m_flags);
    return (flags & (EF_DELETE_IN_PROGRESS | EF_MARKED_FOR_DELETE)) != 0;
}

// Marks one flattened entity field as changed
void MarkEntityFieldChanged(void* instance, unsigned int offset)
{
    if (!instance) return;
    NetworkStateChangedData changed(offset);
    reinterpret_cast<CEntityInstance*>(instance)->NetworkStateChanged(changed);
}

// Returns the resolved UTIL_Remove target
void* UtilRemoveTarget() { return reinterpret_cast<void*>(g_pfnUtilRemove); }

// Returns the resolved entity-system global address
void* EntitySystemGlobalAddress() { return reinterpret_cast<void*>(g_ppEntSysGlobal); }

// Removes one entity through the resolved engine function
bool RemoveEntity(void* instance)
{
    if (!g_pfnUtilRemove || !instance) return false;
    g_pfnUtilRemove(instance);
    return true;
}

// Stores the resolved controller pawn-handle offset
void SetBotPawnHandleOffset(int offset) { g_BotPawnHandleOffset = offset; }

// Returns the resolved controller pawn-handle offset
int BotPawnHandleOffset() { return g_BotPawnHandleOffset; }

// Resets the idle timer for the pawn owned by one client
void ResetIdleTimerForClient(void* client)
{
    if (!client) return;

    const int pawnOffset = schema::GetFieldOffset("CBasePlayerController", "m_hPawn");
    const int idleOffset = schema::GetFieldOffset("CCSPlayerPawnBase", "m_flIdleTimeSinceLastAction");
    if (pawnOffset < 0 || idleOffset < 0) return;

    const int entityIndex = *reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(client) + ssc::OFFSET_m_nEntityIndex);
    char className[64];
    void* controller = ResolveEntityInstance(entityIndex, className, sizeof(className));
    if (!controller || std::strcmp(className, "cs_player_controller") != 0)
    {
        return;
    }

    const uint32_t pawnHandle = *reinterpret_cast<uint32_t*>(reinterpret_cast<unsigned char*>(controller) + pawnOffset);
    if (pawnHandle == 0xFFFFFFFF) return;
    const int pawnIndex = static_cast<int>(pawnHandle & 0x7FFF);
    void* pawn = ResolveEntityInstance(pawnIndex, nullptr, 0);
    if (!pawn) return;

    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(pawn) + idleOffset) = 0.0f;
}

// Updates the engine-side name for one client
const char* SetEngineName(void* client, const char* newName)
{
    if (!client || !newName || !newName[0] || targets::kVTSlot_ClientSetName < 0)
    {
        return nullptr;
    }
#if defined(_WIN32)
    __try
    {
        auto** vtable = *reinterpret_cast<void***>(client);
        if (!vtable) return nullptr;
        auto setName = reinterpret_cast<ClientSetNameFn>(vtable[targets::kVTSlot_ClientSetName]);
        if (!setName) return nullptr;
        setName(client, newName);
        return ssc::ReadName(client);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
#else
    auto** vtable = *reinterpret_cast<void***>(client);
    if (!vtable) return nullptr;
    auto setName = reinterpret_cast<ClientSetNameFn>(vtable[targets::kVTSlot_ClientSetName]);
    if (!setName) return nullptr;
    setName(client, newName);
    return ssc::ReadName(client);
#endif
}

// Clears resolved interfaces and runtime targets
void Reset()
{
    g_pGameResourceService = nullptr;
    g_pfnUtilRemove = nullptr;
    g_ppEntSysGlobal = nullptr;
    g_BotPawnHandleOffset = -1;
}

} // namespace cs2bh::entity_access
