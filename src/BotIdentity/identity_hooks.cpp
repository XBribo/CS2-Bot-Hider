#include "identity_hooks.h"

#include "identity_runtime.h"
#include "version_targets.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
static std::atomic_bool g_PackEntitiesFirstCallLogged = false;
static std::recursive_mutex g_PackEntitiesMutex;
static thread_local uint32_t g_PackEntitiesDepth = 0;

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

class ScopedJoinTeamFakeClientFlag
{
  public:
    // Restores the controller bot bit only during team validation
    ScopedJoinTeamFakeClientFlag(void* controller, uint32_t handle, bool enable)
        : m_Controller(controller), m_Handle(handle), m_Applied(enable && SetJoinTeamFakeClientFlag(controller, handle, true))
    {
    }

    // Clears only the bit added by this scope
    ~ScopedJoinTeamFakeClientFlag()
    {
        if (m_Applied && !SetJoinTeamFakeClientFlag(m_Controller, m_Handle, false))
        {
            META_CONPRINTF("[BOTHIDER] warning: failed to restore JoinTeam fake-client scope\n");
        }
    }

    // Reports whether the temporary flag was applied
    bool Applied() const { return m_Applied; }

  private:
    void* m_Controller = nullptr;
    uint32_t m_Handle = 0xFFFFFFFF;
    bool m_Applied = false;
};

// Passes entity packing through with a scoped FL_BOT override
static void CS2BH_FASTCALL Detour_PackEntities(void* serverObject, void* packContext, int clientCount, void* clients, void* snapshotContext)
{
    if (!g_PackEntitiesFirstCallLogged.exchange(true, std::memory_order_relaxed))
    {
        size_t threadId = std::hash<std::thread::id>{}(std::this_thread::get_id());
        META_CONPRINTF("[BOTHIDER] CNetworkGameServer::PackEntities first entered on thread %zu\n", threadId);
    }

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
    std::array<ManagedControllerFlagSnapshot, 64> flipped{};
    FlipManagedController904(false, &flipped);
    int64_t result = g_pfnQuotaTramp ? g_pfnQuotaTramp(manager) : 0;
    FlipManagedController904(true, &flipped);
    return result;
}

// Restores Bot identity while the engine applies mp_humanteam
static int64_t CS2BH_FASTCALL Detour_ApplyHumanTeamRestriction()
{
    std::array<ManagedControllerFlagSnapshot, 64> flipped{};
    int scoped = FlipManagedController904(false, &flipped);
    int64_t result = g_pfnApplyHumanTeamRestrictionTramp ? g_pfnApplyHumanTeamRestrictionTramp() : 0;
    FlipManagedController904(true, &flipped);
    if (scoped > 0) META_CONPRINTF("[BOTHIDER] MpHumanTeam_ApplyRestriction bot scope=%d\n", scoped);
    return result;
}

// Restores Bot identity only while the engine validates an initial team join
static int64_t CS2BH_FASTCALL Detour_HandleCommandJoinTeam(void* controller, unsigned int requestedTeam, bool unknownFlag)
{
    ManagedControllerTrace trace = TraceManagedController(controller);
    const bool needsFakeClientScope = trace.Managed && !trace.Hltv && (trace.Flags & 0x100u) == 0;
    ScopedJoinTeamFakeClientFlag fakeClientScope(controller, trace.Handle, needsFakeClientScope);
    if (fakeClientScope.Applied())
    {
        META_CONPRINTF("[BOTHIDER] HandleCommand_JoinTeam bot scope ctrl=%p slot=%d current=%u requested=%u\n", controller, trace.Slot,
                       trace.CurrentTeam, requestedTeam);
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
    META_CONPRINTF("[BOTHIDER] CCSPlayerController::HandleCommand_JoinTeam signature matches=%zu\n", matches.size());
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
    META_CONPRINTF("[BOTHIDER] MpHumanTeam_ApplyRestriction signature matches=%zu\n", matches.size());
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
    META_CONPRINTF("[BOTHIDER] CNetworkGameServer::PackEntities signature matches=%zu\n", matches.size());
    if (matches.size() != 1)
    {
        META_CONPRINTF("[BOTHIDER] warning: PackEntities hook requires exactly one match\n");
        return;
    }

    void* target = matches.front();
    g_PackEntitiesFirstCallLogged.store(false, std::memory_order_relaxed);
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
    META_CONPRINTF("[BOTHIDER] CCSGameRules same-map teardown signature matches=%zu\n", matches.size());
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
    if (g_pQuotaHookTarget) META_CONPRINTF("[BOTHIDER] bot-quota fix installed at %p\n", g_pQuotaHookTarget);
    if (g_pPackEntitiesHookTarget)
        META_CONPRINTF("[BOTHIDER] CNetworkGameServer::PackEntities hook installed at %p\n", g_pPackEntitiesHookTarget);
    if (g_pHandleJoinTeamHookTarget)
        META_CONPRINTF("[BOTHIDER] CCSPlayerController::HandleCommand_JoinTeam hook installed at %p\n", g_pHandleJoinTeamHookTarget);
    if (g_pApplyHumanTeamRestrictionHookTarget)
        META_CONPRINTF("[BOTHIDER] MpHumanTeam_ApplyRestriction hook installed at %p\n", g_pApplyHumanTeamRestrictionHookTarget);
    if (g_pSameMapTeardownHookTarget)
        META_CONPRINTF("[BOTHIDER] CCSGameRules same-map teardown hook installed at %p caller=%p\n", g_pSameMapTeardownHookTarget,
                       g_pSameMapTeardownReturnAddress);
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
