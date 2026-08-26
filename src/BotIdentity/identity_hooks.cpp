#include "identity_hooks.h"

#include "entity_access.h"
#include "fake_client_manager.h"
#include "identity_runtime.h"
#include "personas.h"
#include "serversideclient_ref.h"
#include "version_targets.h"

#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <entity2/entityinstance.h>
#include <funchook.h>

#if defined(_WIN32)
#include <intrin.h>
#define CS2BH_FASTCALL __fastcall
#else
#define CS2BH_FASTCALL
#endif

namespace cs2bh::identity_hooks {

using MaintainQuotaFn = int64_t(CS2BH_FASTCALL*)(void*);
using HandleJoinTeamFn = int64_t(CS2BH_FASTCALL*)(void*, unsigned int, bool);
using ApplyHumanTeamRestrictionFn = int64_t(CS2BH_FASTCALL*)();
using PackEntitiesFn = void(CS2BH_FASTCALL*)(void*, void*, int, void*, void*);
using SameMapClientCollectorFn = void*(CS2BH_FASTCALL*)(void*, uintptr_t);

static funchook_t* g_pFunchook = nullptr;
static size_t g_PreparedFunchookCount = 0;
static bool g_FunchooksInstalled = false;

static MaintainQuotaFn g_pfnQuotaTramp = nullptr;
static void* g_pQuotaHookTarget = nullptr;
static HandleJoinTeamFn g_pfnHandleJoinTeamTramp = nullptr;
static void* g_pHandleJoinTeamHookTarget = nullptr;
static ApplyHumanTeamRestrictionFn g_pfnApplyHumanTeamRestrictionTramp = nullptr;
static void* g_pApplyHumanTeamRestrictionHookTarget = nullptr;
static PackEntitiesFn g_pfnPackEntitiesTramp = nullptr;
static void* g_pPackEntitiesHookTarget = nullptr;
static SameMapClientCollectorFn g_pfnSameMapCollectorTramp = nullptr;
static void* g_pSameMapTeardownHookTarget = nullptr;
static void* g_pSameMapTeardownReturnAddress = nullptr;
static std::recursive_mutex g_PackEntitiesMutex;
static thread_local uint32_t g_PackEntitiesDepth = 0;
static std::array<bool, 64> g_QuotaResolveWarned{};
static bool g_QuotaInvalidOffsetWarned = false;

struct NativeBotIdentitySnapshot
{
    int Slot = -1;
    void* Client = nullptr;
    void* Controller = nullptr;
    uint32_t Handle = 0xFFFFFFFF;
    uint16_t UserId = 0;
    uint8_t ConnectionFlags = 0;
    uint8_t FakePlayer = 0;
    uint32_t ControllerFlags = 0;
    bool HasController = false;
    bool Modified = false;
};

static bool IsValidController(void* controller, const char* className, uint32_t handle)
{
    return controller && className && std::strcmp(className, "cs_player_controller") == 0 &&
           !entity_access::IsEntityBeingDeleted(controller) &&
           static_cast<uint32_t>(reinterpret_cast<CEntityInstance*>(controller)->GetRefEHandle().ToInt()) == handle;
}

class ScopedNativeBotIdentityRestore
{
  public:
    ScopedNativeBotIdentityRestore() { Capture(); }
    ~ScopedNativeBotIdentityRestore() { Restore(); }

    int ModifiedCount() const { return m_ModifiedCount; }

  private:
    std::array<NativeBotIdentitySnapshot, 64> m_Snapshots{};
    int m_ModifiedCount = 0;

    void Capture()
    {
        if (ssc::OFFSET_m_UserID < 0 || ssc::OFFSET_m_nEntityIndex < 0 || ssc::OFFSET_m_NetChannel < 0 ||
            ssc::OFFSET_m_nConnectionTypeFlags < 0 || ssc::OFFSET_m_bFakePlayer < 0 ||
            targets::kController_FakeClientFlagsOffset < 0)
        {
            if (!g_QuotaInvalidOffsetWarned)
            {
                META_CONPRINTF("[BOTHIDER] warning: quota identity restore disabled: invalid client/controller offsets\n");
                g_QuotaInvalidOffsetWarned = true;
            }
            return;
        }

        for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
        {
            if (!Manager().IsManaged(slot)) continue;

            auto& snapshot = m_Snapshots[slot];
            snapshot.Slot = slot;
            snapshot.Client = entity_access::ResolveClientBySlot(slot);
            if (!snapshot.Client)
            {
                if (!g_QuotaResolveWarned[slot])
                {
                    META_CONPRINTF("[BOTHIDER] warning: quota identity restore client resolve failed slot=%d\n", slot);
                    g_QuotaResolveWarned[slot] = true;
                }
                continue;
            }

            auto* raw = reinterpret_cast<unsigned char*>(snapshot.Client);
            snapshot.UserId = *reinterpret_cast<uint16_t*>(raw + ssc::OFFSET_m_UserID);
            snapshot.ConnectionFlags = raw[ssc::OFFSET_m_nConnectionTypeFlags];
            snapshot.FakePlayer = raw[ssc::OFFSET_m_bFakePlayer];
            snapshot.Modified = true;
            ssc::SetFakePlayer(snapshot.Client);
            ++m_ModifiedCount;

            const int entityIndex = *reinterpret_cast<int*>(raw + ssc::OFFSET_m_nEntityIndex);
            char className[64];
            snapshot.Controller = entity_access::ResolveEntityInstance(entityIndex, className, sizeof(className));
            if (!snapshot.Controller)
            {
                if (!g_QuotaResolveWarned[slot])
                {
                    META_CONPRINTF("[BOTHIDER] warning: quota identity restore controller resolve failed slot=%d userid=%u entIdx=%d "
                                   "clientFake=%u conn=0x%02x net=%p\n",
                                   slot, static_cast<unsigned int>(snapshot.UserId), entityIndex,
                                   static_cast<unsigned int>(snapshot.FakePlayer), static_cast<unsigned int>(snapshot.ConnectionFlags),
                                   *reinterpret_cast<void**>(raw + ssc::OFFSET_m_NetChannel));
                    g_QuotaResolveWarned[slot] = true;
                }
                continue;
            }
            if (std::strcmp(className, "cs_player_controller") != 0 || entity_access::IsEntityBeingDeleted(snapshot.Controller))
            {
                META_CONPRINTF("[BOTHIDER] warning: quota identity restore invalid controller slot=%d userid=%u entIdx=%d cls='%s'\n",
                               slot, static_cast<unsigned int>(snapshot.UserId), entityIndex, className);
                g_QuotaResolveWarned[slot] = true;
                snapshot.Controller = nullptr;
                continue;
            }

            g_QuotaResolveWarned[slot] = false;

            auto* entity = reinterpret_cast<CEntityInstance*>(snapshot.Controller);
            snapshot.Handle = static_cast<uint32_t>(entity->GetRefEHandle().ToInt());
            auto* flags = reinterpret_cast<uint32_t*>(reinterpret_cast<unsigned char*>(snapshot.Controller) +
                                                      targets::kController_FakeClientFlagsOffset);
            snapshot.ControllerFlags = *flags;
            snapshot.HasController = true;
            const uint32_t before = *flags;
            *flags |= 0x100u;
            if (*flags != before)
            {
                entity_access::MarkEntityFieldChanged(snapshot.Controller,
                                                       static_cast<uint32_t>(targets::kController_FakeClientFlagsOffset));
            }

        }
    }

    void Restore()
    {
        for (const auto& snapshot : m_Snapshots)
        {
            if (!snapshot.Modified || !snapshot.Client) continue;
            if (!Manager().IsManaged(snapshot.Slot)) continue;

            void* currentClient = entity_access::ResolveClientBySlot(snapshot.Slot);
            if (currentClient != snapshot.Client)
            {
                META_CONPRINTF("[BOTHIDER] warning: quota identity restore skipped slot=%d: client rebound\n", snapshot.Slot);
                continue;
            }

            auto* raw = reinterpret_cast<unsigned char*>(currentClient);
            const uint16_t currentUserId = *reinterpret_cast<uint16_t*>(raw + ssc::OFFSET_m_UserID);
            if (currentUserId != snapshot.UserId)
            {
                META_CONPRINTF("[BOTHIDER] warning: quota identity restore skipped slot=%d: userid changed %u->%u\n", snapshot.Slot,
                               static_cast<unsigned int>(snapshot.UserId), static_cast<unsigned int>(currentUserId));
                continue;
            }
            raw[ssc::OFFSET_m_nConnectionTypeFlags] = snapshot.ConnectionFlags;
            raw[ssc::OFFSET_m_bFakePlayer] = snapshot.FakePlayer;

            if (!snapshot.HasController) continue;
            const int entityIndex = *reinterpret_cast<int*>(raw + ssc::OFFSET_m_nEntityIndex);
            char className[64];
            void* controller = entity_access::ResolveEntityInstance(entityIndex, className, sizeof(className));
            if (!IsValidController(controller, className, snapshot.Handle) || controller != snapshot.Controller)
            {
                META_CONPRINTF("[BOTHIDER] warning: quota identity restore skipped slot=%d userid=%u: controller rebound\n",
                               snapshot.Slot, static_cast<unsigned int>(snapshot.UserId));
                continue;
            }

            auto* flags = reinterpret_cast<uint32_t*>(reinterpret_cast<unsigned char*>(controller) +
                                                      targets::kController_FakeClientFlagsOffset);
            if (*flags != snapshot.ControllerFlags)
            {
                *flags = snapshot.ControllerFlags;
                entity_access::MarkEntityFieldChanged(controller,
                                                       static_cast<uint32_t>(targets::kController_FakeClientFlagsOffset));
            }
        }
    }
};

static unsigned int g_PopulationTransactionDepth = 0;
static bool g_PopulationTransactionRedisguise = false;
static std::unique_ptr<ScopedNativeBotIdentityRestore> g_PopulationIdentity;

void BeginPopulationTransaction(bool redisguise)
{
    if (g_PopulationTransactionDepth++ == 0)
    {
        g_PopulationTransactionRedisguise = redisguise;
        g_PopulationIdentity = std::make_unique<ScopedNativeBotIdentityRestore>();
    }
    else
    {
        g_PopulationTransactionRedisguise = g_PopulationTransactionRedisguise || redisguise;
    }
}

void EndPopulationTransaction(bool redisguise)
{
    if (g_PopulationTransactionDepth == 0)
    {
        META_CONPRINTF("[BOTHIDER] warning: population transaction end without begin\n");
        return;
    }

    g_PopulationTransactionRedisguise = g_PopulationTransactionRedisguise || redisguise;
    if (--g_PopulationTransactionDepth != 0) return;

    const bool applyDisguise = g_PopulationTransactionRedisguise;
    g_PopulationTransactionRedisguise = false;
    g_PopulationIdentity.reset();
    if (applyDisguise) identity_runtime::ApplyManagedDisguise(g_Plugin.IsDisguiseEnabled());
}

bool PopulationTransactionActive() { return g_PopulationTransactionDepth != 0; }

PopulationTransactionScope::PopulationTransactionScope(bool redisguise) : m_Redisguise(redisguise)
{
    BeginPopulationTransaction(redisguise);
}

PopulationTransactionScope::~PopulationTransactionScope() { EndPopulationTransaction(m_Redisguise); }

class PackEntitiesDepthGuard
{
  public:
    // Marks the current thread as executing the outer packing callback
    PackEntitiesDepthGuard() { ++g_PackEntitiesDepth; }

    // Clears the current thread packing depth
    ~PackEntitiesDepthGuard() { --g_PackEntitiesDepth; }
};

class ScopedBotFlagOverride
{
  public:
    // Clears FL_BOT and marks changed fields before entity packing
    ScopedBotFlagOverride() : m_ModifiedPawns(ApplyBotFlagOverride()) {}

    // Restores only FL_BOT after entity packing without marking changes
    ~ScopedBotFlagOverride() { RestoreBotFlagOverride(m_ModifiedPawns); }

  private:
    std::vector<BotPawnRef> m_ModifiedPawns;
};

// Passes entity packing through with a scoped FL_BOT override
static void CS2BH_FASTCALL Detour_PackEntities(void* serverObject, void* packContext, int clientCount, void* clients, void* snapshotContext)
{
    std::lock_guard<std::recursive_mutex> lock(g_PackEntitiesMutex);
    if (g_PackEntitiesDepth != 0)
    {
        g_pfnPackEntitiesTramp(serverObject, packContext, clientCount, clients, snapshotContext);
        return;
    }

    PackEntitiesDepthGuard depthGuard;
    ScopedBotFlagOverride flagOverride;
    g_pfnPackEntitiesTramp(serverObject, packContext, clientCount, clients, snapshotContext);
}

// Restores managed clients only at the same-map teardown call site
static void* CS2BH_FASTCALL Detour_SameMapClientCollector(void* collection, uintptr_t source)
{
#if defined(_MSC_VER)
    void* returnAddress = _ReturnAddress();
#else
    void* returnAddress = __builtin_extract_return_addr(__builtin_return_address(0));
#endif
    if (returnAddress == g_pSameMapTeardownReturnAddress)
    {
        const int restored = identity_runtime::RestoreManagedClientsForEngineTeardown();
        META_CONPRINTF("[BOTHIDER] same-map teardown PRE restored=%d\n", restored);
    }

    return g_pfnSameMapCollectorTramp ? g_pfnSameMapCollectorTramp(collection, source) : nullptr;
}

// Prepares one target and replaces its original pointer with the trampoline
template <typename Function> static bool PrepareFunchook(Function& original, void* target, void* detour, const char* name)
{
    if (!g_pFunchook)
    {
        g_pFunchook = funchook_create();
        if (!g_pFunchook)
        {
            META_CONPRINTF("[BOTHIDER] warning: funchook_create failed for %s\n", name);
            return false;
        }
    }

    void* trampoline = target;
    int result = funchook_prepare(g_pFunchook, &trampoline, detour);
    if (result != FUNCHOOK_ERROR_SUCCESS)
    {
        META_CONPRINTF("[BOTHIDER] warning: funchook_prepare failed for %s: %s (%d)\n", name, funchook_error_message(g_pFunchook), result);
        original = nullptr;
        return false;
    }

    original = reinterpret_cast<Function>(trampoline);
    ++g_PreparedFunchookCount;
    return true;
}

// Clears all published hook targets and trampoline pointers
static void ClearBindings()
{
    g_pfnQuotaTramp = nullptr;
    g_pfnPackEntitiesTramp = nullptr;
    g_pQuotaHookTarget = nullptr;
    g_pPackEntitiesHookTarget = nullptr;
    g_pfnHandleJoinTeamTramp = nullptr;
    g_pHandleJoinTeamHookTarget = nullptr;
    g_pfnApplyHumanTeamRestrictionTramp = nullptr;
    g_pApplyHumanTeamRestrictionHookTarget = nullptr;
    g_pfnSameMapCollectorTramp = nullptr;
    g_pSameMapTeardownHookTarget = nullptr;
    g_pSameMapTeardownReturnAddress = nullptr;
    g_PreparedFunchookCount = 0;
    g_FunchooksInstalled = false;
}

// Restores Bot identity while the engine counts bot quota
static int64_t CS2BH_FASTCALL Detour_MaintainBotQuota(void* manager)
{
    PopulationTransactionScope scope(g_Plugin.IsDisguiseEnabled());
    return g_pfnQuotaTramp ? g_pfnQuotaTramp(manager) : 0;
}

// Restores Bot identity while the engine applies mp_humanteam
static int64_t CS2BH_FASTCALL Detour_ApplyHumanTeamRestriction()
{
    PopulationTransactionScope scope(g_Plugin.IsDisguiseEnabled());
    return g_pfnApplyHumanTeamRestrictionTramp ? g_pfnApplyHumanTeamRestrictionTramp() : 0;
}

// Restores Bot identity only while the engine validates an initial team join
static int64_t CS2BH_FASTCALL Detour_HandleCommandJoinTeam(void* controller, unsigned int requestedTeam, bool unknownFlag)
{
    ManagedControllerTrace trace = TraceManagedController(controller);
    std::unique_ptr<PopulationTransactionScope> populationScope;
    if (trace.Managed && !trace.Hltv)
    {
        populationScope = std::make_unique<PopulationTransactionScope>(g_Plugin.IsDisguiseEnabled());
    }

    return g_pfnHandleJoinTeamTramp ? g_pfnHandleJoinTeamTramp(controller, requestedTeam, unknownFlag) : 0;
}

// Resolves and prepares the bot quota detour
static void PrepareQuotaHook(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule)
{
    if (!serverModule) return;
    std::string signature = sig::FindPlatformSig(gamedata, "CCSBotManager::MaintainBotQuota");
    std::vector<uint8_t> bytes;
    std::vector<bool> wildcards;
    if (signature.empty() || !sig::ParseSigString(signature, bytes, wildcards))
    {
        META_CONPRINTF("[BOTHIDER] warning: MaintainBotQuota sig missing — quota fix disabled\n");
        return;
    }
    void* target = sig::FindPatternIn(serverModule, bytes, wildcards);
    if (!target)
    {
        META_CONPRINTF("[BOTHIDER] warning: MaintainBotQuota sig not found — quota fix disabled\n");
        return;
    }
    if (PrepareFunchook(g_pfnQuotaTramp, target, reinterpret_cast<void*>(&Detour_MaintainBotQuota), "CCSBotManager::MaintainBotQuota"))
    {
        g_pQuotaHookTarget = target;
    }
}

// Resolves and prepares the team-join identity detour
static void PrepareHandleJoinTeamHook(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule)
{
    if (!serverModule) return;
    std::string signature = sig::FindPlatformSig(gamedata, "CCSPlayerController::HandleCommand_JoinTeam");
    std::vector<uint8_t> bytes;
    std::vector<bool> wildcards;
    if (signature.empty() || !sig::ParseSigString(signature, bytes, wildcards))
    {
        META_CONPRINTF("[BOTHIDER] warning: HandleCommand_JoinTeam signature missing or malformed\n");
        return;
    }

    std::vector<void*> matches = sig::FindPatternMatchesIn(serverModule, bytes, wildcards);
    if (matches.size() != 1)
    {
        META_CONPRINTF("[BOTHIDER] warning: HandleCommand_JoinTeam hook requires exactly one match\n");
        return;
    }

    void* target = matches.front();
    if (PrepareFunchook(g_pfnHandleJoinTeamTramp, target, reinterpret_cast<void*>(&Detour_HandleCommandJoinTeam),
                        "CCSPlayerController::HandleCommand_JoinTeam"))
    {
        g_pHandleJoinTeamHookTarget = target;
    }
}

// Resolves and prepares the human-team restriction detour
static void PrepareHumanTeamRestrictionHook(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule)
{
    if (!serverModule) return;
    std::string signature = sig::FindPlatformSig(gamedata, "MpHumanTeam_ApplyRestriction");
    std::vector<uint8_t> bytes;
    std::vector<bool> wildcards;
    if (signature.empty() || !sig::ParseSigString(signature, bytes, wildcards))
    {
        META_CONPRINTF("[BOTHIDER] warning: MpHumanTeam_ApplyRestriction signature missing or malformed\n");
        return;
    }

    std::vector<void*> matches = sig::FindPatternMatchesIn(serverModule, bytes, wildcards);
    if (matches.size() != 1)
    {
        META_CONPRINTF("[BOTHIDER] warning: MpHumanTeam_ApplyRestriction hook requires exactly one match\n");
        return;
    }

    void* target = matches.front();
    if (PrepareFunchook(g_pfnApplyHumanTeamRestrictionTramp, target, reinterpret_cast<void*>(&Detour_ApplyHumanTeamRestriction),
                        "MpHumanTeam_ApplyRestriction"))
    {
        g_pApplyHumanTeamRestrictionHookTarget = target;
    }
}

// Resolves and prepares the entity-packing detour
static void PreparePackEntitiesHook(const nlohmann::json& gamedata)
{
    std::string signature = sig::FindPlatformSig(gamedata, "CNetworkGameServer::PackEntities");
    std::vector<uint8_t> bytes;
    std::vector<bool> wildcards;
    if (signature.empty() || !sig::ParseSigString(signature, bytes, wildcards))
    {
        META_CONPRINTF("[BOTHIDER] warning: PackEntities signature missing or malformed\n");
        return;
    }

    sig::ModuleInfo codeModule = sig::ModuleCodeFromName(targets::kEngineModuleName);
    if (!codeModule)
    {
        META_CONPRINTF("[BOTHIDER] warning: %s code range unresolved - PackEntities hook disabled\n", targets::kEngineModuleName);
        return;
    }

    std::vector<void*> matches = sig::FindPatternMatchesIn(codeModule, bytes, wildcards);
    if (matches.size() != 1)
    {
        META_CONPRINTF("[BOTHIDER] warning: PackEntities hook requires exactly one match\n");
        return;
    }

    void* target = matches.front();
    if (PrepareFunchook(g_pfnPackEntitiesTramp, target, reinterpret_cast<void*>(&Detour_PackEntities), "CNetworkGameServer::PackEntities"))
    {
        g_pPackEntitiesHookTarget = target;
    }
}

// Resolves the helper called immediately before the same-map client teardown loop
static void PrepareSameMapTeardownHook(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule)
{
    if (!serverModule) return;
    constexpr const char* kTargetName = "CCSGameRules::SameMapTeardown";
    std::string signature = sig::FindPlatformSig(gamedata, kTargetName);
    std::vector<uint8_t> bytes;
    std::vector<bool> wildcards;
    if (signature.empty() || !sig::ParseSigString(signature, bytes, wildcards))
    {
        META_CONPRINTF("[BOTHIDER] warning: same-map teardown signature missing or malformed\n");
        return;
    }

    std::vector<void*> matches = sig::FindPatternMatchesIn(serverModule, bytes, wildcards);
    if (matches.size() != 1)
    {
        META_CONPRINTF("[BOTHIDER] warning: same-map teardown hook requires exactly one match\n");
        return;
    }

    constexpr int kMissingCallOffset = std::numeric_limits<int>::min();
    const int callOffset = sig::FindPlatformOffset(gamedata, kTargetName, kMissingCallOffset);
    if (callOffset == kMissingCallOffset)
    {
        META_CONPRINTF("[BOTHIDER] warning: same-map teardown helper call offset is missing\n");
        return;
    }

    auto* callSite = static_cast<unsigned char*>(matches.front()) + callOffset;
    const uintptr_t moduleBegin = reinterpret_cast<uintptr_t>(serverModule.Base);
    const uintptr_t moduleEnd = moduleBegin + serverModule.Size;
    const uintptr_t callAddress = reinterpret_cast<uintptr_t>(callSite);
    if (callAddress < moduleBegin || callAddress > moduleEnd - 5 || callSite[0] != 0xE8)
    {
        META_CONPRINTF("[BOTHIDER] warning: same-map teardown helper call is invalid\n");
        return;
    }

    int32_t displacement = 0;
    std::memcpy(&displacement, callSite + 1, sizeof(displacement));
    auto* target = callSite + 5 + displacement;
    const uintptr_t targetAddress = reinterpret_cast<uintptr_t>(target);
    if (targetAddress < moduleBegin || targetAddress >= moduleEnd)
    {
        META_CONPRINTF("[BOTHIDER] warning: same-map teardown helper target is outside server module\n");
        return;
    }

    if (PrepareFunchook(g_pfnSameMapCollectorTramp, target, reinterpret_cast<void*>(&Detour_SameMapClientCollector),
                        "CCSGameRules::SameMapTeardown helper"))
    {
        g_pSameMapTeardownHookTarget = target;
        g_pSameMapTeardownReturnAddress = callSite + 5;
    }
}

// Resolves and prepares every optional identity detour
void PrepareAll(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule)
{
    PrepareQuotaHook(gamedata, serverModule);
    PrepareHandleJoinTeamHook(gamedata, serverModule);
    PrepareHumanTeamRestrictionHook(gamedata, serverModule);
    PreparePackEntitiesHook(gamedata);
    PrepareSameMapTeardownHook(gamedata, serverModule);
}

// Installs every successfully prepared identity detour
void InstallPrepared()
{
    if (!g_pFunchook || g_PreparedFunchookCount == 0)
    {
        if (g_pFunchook) funchook_destroy(g_pFunchook);
        g_pFunchook = nullptr;
        ClearBindings();
        return;
    }

    int result = funchook_install(g_pFunchook, 0);
    if (result != FUNCHOOK_ERROR_SUCCESS)
    {
        META_CONPRINTF("[BOTHIDER] warning: funchook_install failed: %s (%d)\n", funchook_error_message(g_pFunchook), result);
        funchook_destroy(g_pFunchook);
        g_pFunchook = nullptr;
        ClearBindings();
        return;
    }

    g_FunchooksInstalled = true;
}

// Uninstalls all identity detours and releases their shared handle
bool Remove()
{
    if (g_PackEntitiesDepth != 0)
    {
        META_CONPRINTF("[BOTHIDER] error: refusing funchook removal during PackEntities\n");
        return false;
    }

    std::unique_lock<std::recursive_mutex> lock(g_PackEntitiesMutex);
    if (!g_pFunchook)
    {
        ClearBindings();
        return true;
    }

    if (g_FunchooksInstalled)
    {
        int result = funchook_uninstall(g_pFunchook, 0);
        if (result != FUNCHOOK_ERROR_SUCCESS)
        {
            std::string message = funchook_error_message(g_pFunchook);
            lock.unlock();
            META_CONPRINTF("[BOTHIDER] error: funchook_uninstall failed: %s (%d)\n", message.c_str(), result);
            return false;
        }
    }

    int result = funchook_destroy(g_pFunchook);
    std::string destroyMessage;
    if (result != FUNCHOOK_ERROR_SUCCESS) destroyMessage = funchook_error_message(g_pFunchook);
    g_pFunchook = nullptr;
    ClearBindings();
    lock.unlock();
    if (result != FUNCHOOK_ERROR_SUCCESS)
        META_CONPRINTF("[BOTHIDER] warning: funchook_destroy failed: %s (%d)\n", destroyMessage.c_str(), result);
    return true;
}

// Returns the resolved bot-quota hook target
void* MaintainQuotaTarget() { return g_pQuotaHookTarget; }

// Returns the resolved entity-packing hook target
void* PackEntitiesTarget() { return g_pPackEntitiesHookTarget; }

// Returns the resolved team-join hook target
void* HandleJoinTeamTarget() { return g_pHandleJoinTeamHookTarget; }

// Returns the resolved human-team restriction hook target
void* HumanTeamRestrictionTarget() { return g_pApplyHumanTeamRestrictionHookTarget; }

// Returns the resolved same-map teardown helper target
void* SameMapTeardownTarget() { return g_pSameMapTeardownHookTarget; }

} // namespace cs2bh::identity_hooks
