#include "core/gameconfig.h"
#include "core/log.h"
#include "entity_access.h"
#include "core/cs2_sdk/entity/player.h"

#include "playerslot.h"
#include "nlohmann/json.hpp"
#include "ISmmPlugin.h"
#include "entityidentity.h"
#include "plugin.h" // NOLINT(misc-include-cleaner)
#include "serversideclient_ref.h"
#include "core/memory_module.h"
#include "offsets.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <entity2/entityinstance.h>
#include <iserver.h>
#include <tier1/utlvector.h>

#ifdef _WIN32
#include <excpt.h>
#define CS2BH_FASTCALL __fastcall
#else
#define CS2BH_FASTCALL
#endif

extern INetworkServerService* g_pNetworkServerService;

namespace cs2bh::entity_access {

namespace {

using UtilRemoveFn = void(CS2BH_FASTCALL*)(void*);
using ClientSetNameFn = void(CS2BH_FASTCALL*)(void*, const char*);

void* g_gameResourceService = nullptr;
UtilRemoveFn g_utilRemove = nullptr;
void** g_entitySystemGlobal = nullptr;

} // namespace

// Stores the GameResourceService interface used for entity resolution
void SetGameResourceService(void* gameResourceService) { g_gameResourceService = gameResourceService; }

// Returns the current GameResourceService interface
void* GameResourceService() { return g_gameResourceService; }

// Resolves one server-side client from its slot
void* ResolveClientBySlot(int slot)
{
    if (!g_pNetworkServerService || offsets::g_clientListOffset < 0) return nullptr;
    auto* gameServer = g_pNetworkServerService->GetIGameServer();
    if (!gameServer) return nullptr;
    auto* clients = reinterpret_cast<CUtlVector<void*>*>(reinterpret_cast<unsigned char*>(gameServer) + offsets::g_clientListOffset);
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
void ResolveUtilRemoveAndEntSys(const nlohmann::json& gamedata, const modules::ModuleInfo& serverModule)
{
    g_utilRemove = nullptr;
    g_entitySystemGlobal = nullptr;
    if (!serverModule)
    {
        BH_LOG_WARN("[BOTHIDER] warning: %s module unresolved for signature scan\n", offsets::kServerModuleName);
        return;
    }

    std::string signature = gameconfig::FindPlatformSig(gamedata, "UTIL_Remove");
    std::vector<uint8_t> bytes;
    std::vector<bool> wildcards;
    if (signature.empty() || !modules::ParseSigString(signature, bytes, wildcards))
    {
        BH_LOG_WARN("[BOTHIDER] warning: UTIL_Remove %s sig missing/malformed in gamedata.json\n", gameconfig::PlatformName());
        return;
    }

    auto* hit = static_cast<unsigned char*>(modules::FindPatternIn(serverModule, bytes, wildcards));
    if (!hit) return;
    g_utilRemove = reinterpret_cast<UtilRemoveFn>(hit);

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
        g_entitySystemGlobal = reinterpret_cast<void**>(instructionEnd + displacement);
        break;
    }
}

namespace {

// Reads one pointer while isolating invalid memory access on Windows
bool SafeReadPointer(const void* address, void** output)
{
#ifdef _WIN32
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
bool SafeReadString(const void* address, char* output, size_t capacity)
{
#ifdef _WIN32
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

} // namespace

// Resolves one entity instance and optionally copies its class name
void* ResolveEntityInstance(int entityIndex, char* classnameOut, size_t classnameCap)
{
    if (classnameOut && classnameCap) classnameOut[0] = '\0';
    if (!g_gameResourceService || entityIndex <= 0 || entityIndex >= 0x8000 || offsets::g_entitySystemOffsetInGameResourceService < 0 ||
        offsets::g_entitySystemIdentityChunksOffset < 0 || offsets::g_entityIdentitySize <= 0 ||
        offsets::g_entityIdentityInstanceOffset < 0 || (classnameOut && classnameCap && offsets::g_entityIdentityClassNameOffset < 0))
    {
        return nullptr;
    }

    void* entitySystem = nullptr;
    if (!SafeReadPointer(reinterpret_cast<unsigned char*>(g_gameResourceService) + offsets::g_entitySystemOffsetInGameResourceService,
                         &entitySystem) ||
        !entitySystem)
    {
        return nullptr;
    }

    void* chunk = nullptr;
    const void* chunkSlot = reinterpret_cast<unsigned char*>(entitySystem) + offsets::g_entitySystemIdentityChunksOffset +
                            ((entityIndex / offsets::kEntListChunkSize) * sizeof(void*));
    if (!SafeReadPointer(chunkSlot, &chunk) || !chunk) return nullptr;

    unsigned char* identity =
        reinterpret_cast<unsigned char*>(chunk) + ((entityIndex % offsets::kEntListChunkSize) * offsets::g_entityIdentitySize);
    if (classnameOut && classnameCap)
    {
        SafeReadString(identity + offsets::g_entityIdentityClassNameOffset, classnameOut, classnameCap);
    }

    void* instance = nullptr;
    if (!SafeReadPointer(identity + offsets::g_entityIdentityInstanceOffset, &instance) || !instance)
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
    const auto flags = static_cast<uint32_t>(entity->m_pEntity->m_flags);
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
void* UtilRemoveTarget() { return reinterpret_cast<void*>(g_utilRemove); }

// Returns the resolved entity-system global address
void* EntitySystemGlobalAddress() { return reinterpret_cast<void*>(g_entitySystemGlobal); }

// Removes one entity through the resolved engine function
bool RemoveEntity(void* instance)
{
    if (!g_utilRemove || !instance) return false;
    g_utilRemove(instance);
    return true;
}

// Returns the resolved controller pawn-handle offset
int BotPawnHandleOffset() { return offsets::g_botPawnHandleOffset; }

// Resets the idle timer for the pawn owned by one client
void ResetIdleTimerForClient(void* client)
{
    if (!client) return;

    const int entityIndex = *reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(client) + ssc::g_entityIndexOffset);
    char className[64];
    void* controller = ResolveEntityInstance(entityIndex, className, sizeof(className));
    if (!controller || std::strcmp(className, "cs_player_controller") != 0)
    {
        return;
    }

    const auto* handleField = sdk::PawnHandle(controller);
    if (!handleField) return;
    const uint32_t pawnHandle = *handleField;
    if (pawnHandle == 0xFFFFFFFF) return;
    const int pawnIndex = static_cast<int>(pawnHandle & 0x7FFF);
    void* pawn = ResolveEntityInstance(pawnIndex, nullptr, 0);
    if (!pawn) return;

    if (auto* idleTime = sdk::IdleTime(pawn)) *idleTime = 0.0F;
}

// Updates the engine-side name for one client
const char* SetEngineName(void* client, const char* newName)
{
    if (!client || !newName || !newName[0] || offsets::g_vtableSlotClientSetName < 0)
    {
        return nullptr;
    }
#ifdef _WIN32
    __try
    {
        auto** vtable = *reinterpret_cast<void***>(client);
        if (!vtable) return nullptr;
        auto setName = reinterpret_cast<ClientSetNameFn>(vtable[offsets::g_vtableSlotClientSetName]);
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
    auto setName = reinterpret_cast<ClientSetNameFn>(vtable[offsets::g_vtableSlotClientSetName]);
    if (!setName) return nullptr;
    setName(client, newName);
    return ssc::ReadName(client);
#endif
}

// Clears resolved interfaces and runtime targets
void Reset()
{
    g_gameResourceService = nullptr;
    g_utilRemove = nullptr;
    g_entitySystemGlobal = nullptr;
    offsets::g_botPawnHandleOffset = -1;
}

} // namespace cs2bh::entity_access
