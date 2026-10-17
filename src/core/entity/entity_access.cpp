#include "core/cs2_sdk/src/core/gameconfig.h"
#include "core/cs2_sdk/src/utils/memory.h"
#include "core/cs2_sdk/src/utils/platform.h"
#include "core/log.h"
#include "entity_access.h"
#include "core/entity/player.h"

#include "playerslot.h"
#include "nlohmann/json.hpp"
#include "ISmmPlugin.h"
#include "entityidentity.h"
#include "plugin.h" // NOLINT(misc-include-cleaner)
#include "serversideclient_ref.h"
#include "core/cs2_sdk/src/core/memory_module.h"
#include "offsets.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <entity2/entityinstance.h>
#include <entityhandle.h>
#include <engine2/inetworkgameserver.h>
#include <engine2/inetworkserverservice.h>
#include <engine2/networkgameserverbase.h>
#include <tier0/utlvector.h>

#ifdef _WIN32
#include <excpt.h>
#endif

extern INetworkServerService* g_pNetworkServerService;

namespace cs2bh::entity_access {

namespace {

using UtilRemoveFn = void(CS2_SDK_FASTCALL*)(void*);
using ClientSetNameFn = void(CS2_SDK_FASTCALL*)(void*, const char*);

UtilRemoveFn g_utilRemove = nullptr;
void** g_entitySystemGlobal = nullptr;

} // namespace

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

bool RefreshClientUserInfo(int slot)
{
    if (!g_pNetworkServerService || slot < 0 || slot >= 64) return false;
    auto* gameServer = g_pNetworkServerService->GetIGameServer();
    if (!gameServer) return false;
    gameServer->UserInfoChanged(CPlayerSlot(slot));
    return true;
}

void ResolveUtilRemoveAndEntSys(const nlohmann::json& gamedata, const cs2_sdk::modules::ModuleInfo& serverModule)
{
    g_utilRemove = nullptr;
    g_entitySystemGlobal = nullptr;
    if (!serverModule)
    {
        BH_LOG_WARN("%s module unresolved for signature scan\n", cs2_sdk::modules::kServerModuleName);
        return;
    }

    std::string signature = cs2_sdk::gameconfig::FindPlatformSig(gamedata, "UTIL_Remove");
    std::vector<uint8_t> bytes;
    std::vector<bool> wildcards;
    if (signature.empty() || !cs2_sdk::modules::ParseSigString(signature, bytes, wildcards))
    {
        BH_LOG_WARN("UTIL_Remove %s sig missing/malformed in gamedata.json\n", cs2_sdk::gameconfig::PlatformName());
        return;
    }

    auto* hit = static_cast<unsigned char*>(cs2_sdk::modules::FindPatternIn(serverModule, bytes, wildcards));
    if (!hit)
    {
        BH_LOG_WARN("UTIL_Remove signature missing or ambiguous\n");
        return;
    }
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

        g_entitySystemGlobal = static_cast<void**>(cs2_sdk::memory::ResolveRelativeAddress(hit + i, 3, 7));
        break;
    }
}

namespace {

// Clears the output even when the protected read fails.
bool SafeReadPointer(const void* address, void** output)
{
    *output = nullptr;
    return cs2_sdk::memory::Read(address, 0, *output);
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

void* ResolveEntityInstance(int entityIndex, char* classnameOut, size_t classnameCap)
{
    if (classnameOut && classnameCap) classnameOut[0] = '\0';
    if (!g_entitySystemGlobal || entityIndex <= 0 || entityIndex >= 0x8000 || offsets::g_entitySystemIdentityChunksOffset < 0 ||
        offsets::g_entityIdentitySize <= 0 || offsets::g_entityIdentityInstanceOffset < 0 ||
        (classnameOut && classnameCap && offsets::g_entityIdentityClassNameOffset < 0))
    {
        return nullptr;
    }

    void* entitySystem = nullptr;
    if (!SafeReadPointer(g_entitySystemGlobal, &entitySystem) || !entitySystem)
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

bool IsEntityBeingDeleted(void* instance)
{
    if (!instance) return true;
    auto* entity = reinterpret_cast<CEntityInstance*>(instance);
    if (!entity->m_pEntity) return true;
    const auto flags = static_cast<uint32_t>(entity->m_pEntity->m_flags);
    return (flags & (EF_DELETE_IN_PROGRESS | EF_MARKED_FOR_DELETE)) != 0;
}

void MarkEntityFieldChanged(void* instance, unsigned int offset)
{
    if (!instance) return;
    NetworkStateChangedData changed(offset);
    reinterpret_cast<CEntityInstance*>(instance)->NetworkStateChanged(changed);
}

void* UtilRemoveTarget() { return reinterpret_cast<void*>(g_utilRemove); }

bool RemoveEntity(void* instance)
{
    if (!g_utilRemove || !instance) return false;
    g_utilRemove(instance);
    return true;
}

int BotPawnHandleOffset() { return offsets::g_botPawnHandleOffset; }

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
    const int pawnIndex = CEntityHandle(pawnHandle).GetEntryIndex();
    void* pawn = ResolveEntityInstance(pawnIndex, nullptr, 0);
    if (!pawn) return;

    if (auto* idleTime = sdk::IdleTime(pawn)) *idleTime = 0.0F;
}

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

void Reset()
{
    g_utilRemove = nullptr;
    g_entitySystemGlobal = nullptr;
    offsets::g_botPawnHandleOffset = -1;
}

} // namespace cs2bh::entity_access
