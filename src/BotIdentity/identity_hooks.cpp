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

static funchook_t* g_funchook = nullptr;
static size_t g_preparedFunchookCount = 0;
static bool g_funchooksInstalled = false;

static MaintainQuotaFn g_quotaTrampoline = nullptr;
static void* g_quotaHookTarget = nullptr;
static HandleJoinTeamFn g_handleJoinTeamTrampoline = nullptr;
static void* g_handleJoinTeamHookTarget = nullptr;
static ApplyHumanTeamRestrictionFn g_applyHumanTeamRestrictionTrampoline = nullptr;
static void* g_applyHumanTeamRestrictionHookTarget = nullptr;
static PackEntitiesFn g_packEntitiesTrampoline = nullptr;
static void* g_packEntitiesHookTarget = nullptr;
static SameMapClientCollectorFn g_sameMapCollectorTrampoline = nullptr;
static void* g_sameMapTeardownHookTarget = nullptr;
static void* g_sameMapTeardownReturnAddress = nullptr;
static std::recursive_mutex g_packEntitiesMutex;
static thread_local uint32_t g_packEntitiesDepth = 0;
static std::array<bool, 64> g_quotaResolveWarned{};
static bool g_quotaInvalidOffsetWarned = false;

struct NativeBotIdentitySnapshot
{
    int slot = -1;
    void* client = nullptr;
    void* controller = nullptr;
    uint32_t handle = 0xFFFFFFFF;
    uint16_t userId = 0;
    uint8_t connectionFlags = 0;
    uint8_t fakePlayer = 0;
    uint32_t controllerFlags = 0;
    bool hasController = false;
    bool modified = false;
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

    int ModifiedCount() const { return m_modifiedCount; }

  private:
    std::array<NativeBotIdentitySnapshot, 64> m_snapshots{};
    int m_modifiedCount = 0;

    void Capture()
    {
        if (ssc::g_userIdOffset < 0 || ssc::g_entityIndexOffset < 0 || ssc::g_netChannelOffset < 0 ||
            ssc::g_connectionTypeFlagsOffset < 0 || ssc::g_fakePlayerOffset < 0 || targets::g_controllerFakeClientFlagsOffset < 0)
        {
            if (!g_quotaInvalidOffsetWarned)
            {
                META_CONPRINTF("[BOTHIDER] warning: quota identity restore disabled: invalid client/controller offsets\n");
                g_quotaInvalidOffsetWarned = true;
            }
            return;
        }

        for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
        {
            if (!Manager().IsManaged(slot)) continue;

            auto& snapshot = m_snapshots[slot];
            snapshot.slot = slot;
            snapshot.client = entity_access::ResolveClientBySlot(slot);
            if (!snapshot.client)
            {
                if (!g_quotaResolveWarned[slot])
                {
                    META_CONPRINTF("[BOTHIDER] warning: quota identity restore client resolve failed slot=%d\n", slot);
                    g_quotaResolveWarned[slot] = true;
                }
                continue;
            }

            auto* raw = reinterpret_cast<unsigned char*>(snapshot.client);
            snapshot.userId = *reinterpret_cast<uint16_t*>(raw + ssc::g_userIdOffset);
            snapshot.connectionFlags = raw[ssc::g_connectionTypeFlagsOffset];
            snapshot.fakePlayer = raw[ssc::g_fakePlayerOffset];
            snapshot.modified = true;
            ssc::SetFakePlayer(snapshot.client);
            ++m_modifiedCount;

            const int entityIndex = *reinterpret_cast<int*>(raw + ssc::g_entityIndexOffset);
            char className[64];
            snapshot.controller = entity_access::ResolveEntityInstance(entityIndex, className, sizeof(className));
            if (!snapshot.controller)
            {
                if (!g_quotaResolveWarned[slot])
                {
                    META_CONPRINTF("[BOTHIDER] warning: quota identity restore controller resolve failed slot=%d userid=%u entIdx=%d "
                                   "clientFake=%u conn=0x%02x net=%p\n",
                                   slot, static_cast<unsigned int>(snapshot.userId), entityIndex,
                                   static_cast<unsigned int>(snapshot.fakePlayer), static_cast<unsigned int>(snapshot.connectionFlags),
                                   *reinterpret_cast<void**>(raw + ssc::g_netChannelOffset));
                    g_quotaResolveWarned[slot] = true;
                }
                continue;
            }
            if (std::strcmp(className, "cs_player_controller") != 0 || entity_access::IsEntityBeingDeleted(snapshot.controller))
            {
                META_CONPRINTF("[BOTHIDER] warning: quota identity restore invalid controller slot=%d userid=%u entIdx=%d cls='%s'\n", slot,
                               static_cast<unsigned int>(snapshot.userId), entityIndex, className);
                g_quotaResolveWarned[slot] = true;
                snapshot.controller = nullptr;
                continue;
            }

            g_quotaResolveWarned[slot] = false;

            auto* entity = reinterpret_cast<CEntityInstance*>(snapshot.controller);
            snapshot.handle = static_cast<uint32_t>(entity->GetRefEHandle().ToInt());
            auto* flags = reinterpret_cast<uint32_t*>(reinterpret_cast<unsigned char*>(snapshot.controller) +
                                                      targets::g_controllerFakeClientFlagsOffset);
            snapshot.controllerFlags = *flags;
            snapshot.hasController = true;
            const uint32_t before = *flags;
            *flags |= 0x100u;
            if (*flags != before)
            {
                entity_access::MarkEntityFieldChanged(snapshot.controller,
                                                      static_cast<uint32_t>(targets::g_controllerFakeClientFlagsOffset));
            }
        }
    }

    void Restore()
    {
        for (const auto& snapshot : m_snapshots)
        {
            if (!snapshot.modified || !snapshot.client) continue;
            if (!Manager().IsManaged(snapshot.slot)) continue;

            void* currentClient = entity_access::ResolveClientBySlot(snapshot.slot);
            if (currentClient != snapshot.client)
            {
                META_CONPRINTF("[BOTHIDER] warning: quota identity restore skipped slot=%d: client rebound\n", snapshot.slot);
                continue;
            }

            auto* raw = reinterpret_cast<unsigned char*>(currentClient);
            const uint16_t currentUserId = *reinterpret_cast<uint16_t*>(raw + ssc::g_userIdOffset);
            if (currentUserId != snapshot.userId)
            {
                META_CONPRINTF("[BOTHIDER] warning: quota identity restore skipped slot=%d: userid changed %u->%u\n", snapshot.slot,
                               static_cast<unsigned int>(snapshot.userId), static_cast<unsigned int>(currentUserId));
                continue;
            }
            raw[ssc::g_connectionTypeFlagsOffset] = snapshot.connectionFlags;
            raw[ssc::g_fakePlayerOffset] = snapshot.fakePlayer;

            if (!snapshot.hasController) continue;
            const int entityIndex = *reinterpret_cast<int*>(raw + ssc::g_entityIndexOffset);
            char className[64];
            void* controller = entity_access::ResolveEntityInstance(entityIndex, className, sizeof(className));
            if (!IsValidController(controller, className, snapshot.handle) || controller != snapshot.controller)
            {
                META_CONPRINTF("[BOTHIDER] warning: quota identity restore skipped slot=%d userid=%u: controller rebound\n", snapshot.slot,
                               static_cast<unsigned int>(snapshot.userId));
                continue;
            }

            auto* flags =
                reinterpret_cast<uint32_t*>(reinterpret_cast<unsigned char*>(controller) + targets::g_controllerFakeClientFlagsOffset);
            if (*flags != snapshot.controllerFlags)
            {
                *flags = snapshot.controllerFlags;
                entity_access::MarkEntityFieldChanged(controller, static_cast<uint32_t>(targets::g_controllerFakeClientFlagsOffset));
            }
        }
    }
};

static unsigned int g_populationTransactionDepth = 0;
static bool g_populationTransactionRedisguise = false;
static std::unique_ptr<ScopedNativeBotIdentityRestore> g_populationIdentity;

void BeginPopulationTransaction(bool redisguise)
{
    if (g_populationTransactionDepth++ == 0)
    {
        g_populationTransactionRedisguise = redisguise;
        g_populationIdentity = std::make_unique<ScopedNativeBotIdentityRestore>();
    }
    else
    {
        g_populationTransactionRedisguise = g_populationTransactionRedisguise || redisguise;
    }
}

void EndPopulationTransaction(bool redisguise)
{
    if (g_populationTransactionDepth == 0)
    {
        META_CONPRINTF("[BOTHIDER] warning: population transaction end without begin\n");
        return;
    }

    g_populationTransactionRedisguise = g_populationTransactionRedisguise || redisguise;
    if (--g_populationTransactionDepth != 0) return;

    const bool applyDisguise = g_populationTransactionRedisguise;
    g_populationTransactionRedisguise = false;
    g_populationIdentity.reset();
    if (applyDisguise) identity_runtime::ApplyManagedDisguise(g_plugin.IsDisguiseEnabled());
}

bool PopulationTransactionActive() { return g_populationTransactionDepth != 0; }

PopulationTransactionScope::PopulationTransactionScope(bool redisguise) : m_redisguise(redisguise)
{
    BeginPopulationTransaction(redisguise);
}

PopulationTransactionScope::~PopulationTransactionScope() { EndPopulationTransaction(m_redisguise); }

class PackEntitiesDepthGuard
{
  public:
    // Marks the current thread as executing the outer packing callback
    PackEntitiesDepthGuard() { ++g_packEntitiesDepth; }

    // Clears the current thread packing depth
    ~PackEntitiesDepthGuard() { --g_packEntitiesDepth; }
};

class ScopedBotFlagOverride
{
  public:
    // Clears FL_BOT and marks changed fields before entity packing
    ScopedBotFlagOverride() : m_modifiedPawns(ApplyBotFlagOverride()) {}

    // Restores only FL_BOT after entity packing without marking changes
    ~ScopedBotFlagOverride() { RestoreBotFlagOverride(m_modifiedPawns); }

  private:
    std::vector<BotPawnRef> m_modifiedPawns;
};

// Passes entity packing through with a scoped FL_BOT override
static void CS2BH_FASTCALL DetourPackEntities(void* serverObject, void* packContext, int clientCount, void* clients, void* snapshotContext)
{
    std::lock_guard<std::recursive_mutex> lock(g_packEntitiesMutex);
    if (g_packEntitiesDepth != 0)
    {
        g_packEntitiesTrampoline(serverObject, packContext, clientCount, clients, snapshotContext);
        return;
    }

    PackEntitiesDepthGuard depthGuard;
    ScopedBotFlagOverride flagOverride;
    g_packEntitiesTrampoline(serverObject, packContext, clientCount, clients, snapshotContext);
}

// Restores managed clients only at the same-map teardown call site
static void* CS2BH_FASTCALL DetourSameMapClientCollector(void* collection, uintptr_t source)
{
#if defined(_MSC_VER)
    void* returnAddress = _ReturnAddress();
#else
    void* returnAddress = __builtin_extract_return_addr(__builtin_return_address(0));
#endif
    if (returnAddress == g_sameMapTeardownReturnAddress)
    {
        const int restored = identity_runtime::RestoreManagedClientsForEngineTeardown();
        META_CONPRINTF("[BOTHIDER] same-map teardown PRE restored=%d\n", restored);
    }

    return g_sameMapCollectorTrampoline ? g_sameMapCollectorTrampoline(collection, source) : nullptr;
}

// Prepares one target and replaces its original pointer with the trampoline
template <typename Function> static bool PrepareFunchook(Function& original, void* target, void* detour, const char* name)
{
    if (!g_funchook)
    {
        g_funchook = funchook_create();
        if (!g_funchook)
        {
            META_CONPRINTF("[BOTHIDER] warning: funchook_create failed for %s\n", name);
            return false;
        }
    }

    void* trampoline = target;
    int result = funchook_prepare(g_funchook, &trampoline, detour);
    if (result != FUNCHOOK_ERROR_SUCCESS)
    {
        META_CONPRINTF("[BOTHIDER] warning: funchook_prepare failed for %s: %s (%d)\n", name, funchook_error_message(g_funchook), result);
        original = nullptr;
        return false;
    }

    original = reinterpret_cast<Function>(trampoline);
    ++g_preparedFunchookCount;
    return true;
}

// Clears all published hook targets and trampoline pointers
static void ClearBindings()
{
    g_quotaTrampoline = nullptr;
    g_packEntitiesTrampoline = nullptr;
    g_quotaHookTarget = nullptr;
    g_packEntitiesHookTarget = nullptr;
    g_handleJoinTeamTrampoline = nullptr;
    g_handleJoinTeamHookTarget = nullptr;
    g_applyHumanTeamRestrictionTrampoline = nullptr;
    g_applyHumanTeamRestrictionHookTarget = nullptr;
    g_sameMapCollectorTrampoline = nullptr;
    g_sameMapTeardownHookTarget = nullptr;
    g_sameMapTeardownReturnAddress = nullptr;
    g_preparedFunchookCount = 0;
    g_funchooksInstalled = false;
}

// Restores Bot identity while the engine counts bot quota
static int64_t CS2BH_FASTCALL DetourMaintainBotQuota(void* manager)
{
    if (!g_plugin.IsDisguiseEnabled()) return g_quotaTrampoline ? g_quotaTrampoline(manager) : 0;
    PopulationTransactionScope scope(true);
    return g_quotaTrampoline ? g_quotaTrampoline(manager) : 0;
}

// Restores Bot identity while the engine applies mp_humanteam
static int64_t CS2BH_FASTCALL DetourApplyHumanTeamRestriction()
{
    if (!g_plugin.IsDisguiseEnabled()) return g_applyHumanTeamRestrictionTrampoline ? g_applyHumanTeamRestrictionTrampoline() : 0;
    PopulationTransactionScope scope(true);
    return g_applyHumanTeamRestrictionTrampoline ? g_applyHumanTeamRestrictionTrampoline() : 0;
}

// Restores Bot identity only while the engine validates an initial team join
static int64_t CS2BH_FASTCALL DetourHandleCommandJoinTeam(void* controller, unsigned int requestedTeam, bool unknownFlag)
{
    if (!g_plugin.IsDisguiseEnabled())
        return g_handleJoinTeamTrampoline ? g_handleJoinTeamTrampoline(controller, requestedTeam, unknownFlag) : 0;

    ManagedControllerTrace trace = TraceManagedController(controller);
    std::unique_ptr<PopulationTransactionScope> populationScope;
    if (trace.managed && !trace.hltv)
    {
        populationScope = std::make_unique<PopulationTransactionScope>(true);
    }

    return g_handleJoinTeamTrampoline ? g_handleJoinTeamTrampoline(controller, requestedTeam, unknownFlag) : 0;
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
    if (PrepareFunchook(g_quotaTrampoline, target, reinterpret_cast<void*>(&DetourMaintainBotQuota), "CCSBotManager::MaintainBotQuota"))
    {
        g_quotaHookTarget = target;
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
    if (PrepareFunchook(g_handleJoinTeamTrampoline, target, reinterpret_cast<void*>(&DetourHandleCommandJoinTeam),
                        "CCSPlayerController::HandleCommand_JoinTeam"))
    {
        g_handleJoinTeamHookTarget = target;
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
    if (PrepareFunchook(g_applyHumanTeamRestrictionTrampoline, target, reinterpret_cast<void*>(&DetourApplyHumanTeamRestriction),
                        "MpHumanTeam_ApplyRestriction"))
    {
        g_applyHumanTeamRestrictionHookTarget = target;
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
    if (PrepareFunchook(g_packEntitiesTrampoline, target, reinterpret_cast<void*>(&DetourPackEntities), "CNetworkGameServer::PackEntities"))
    {
        g_packEntitiesHookTarget = target;
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
    const uintptr_t moduleBegin = reinterpret_cast<uintptr_t>(serverModule.base);
    const uintptr_t moduleEnd = moduleBegin + serverModule.size;
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

    if (PrepareFunchook(g_sameMapCollectorTrampoline, target, reinterpret_cast<void*>(&DetourSameMapClientCollector),
                        "CCSGameRules::SameMapTeardown helper"))
    {
        g_sameMapTeardownHookTarget = target;
        g_sameMapTeardownReturnAddress = callSite + 5;
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
    if (!g_funchook || g_preparedFunchookCount == 0)
    {
        if (g_funchook) funchook_destroy(g_funchook);
        g_funchook = nullptr;
        ClearBindings();
        return;
    }

    int result = funchook_install(g_funchook, 0);
    if (result != FUNCHOOK_ERROR_SUCCESS)
    {
        META_CONPRINTF("[BOTHIDER] warning: funchook_install failed: %s (%d)\n", funchook_error_message(g_funchook), result);
        funchook_destroy(g_funchook);
        g_funchook = nullptr;
        ClearBindings();
        return;
    }

    g_funchooksInstalled = true;
}

// Uninstalls all identity detours and releases their shared handle
bool Remove()
{
    if (g_packEntitiesDepth != 0)
    {
        META_CONPRINTF("[BOTHIDER] error: refusing funchook removal during PackEntities\n");
        return false;
    }

    std::unique_lock<std::recursive_mutex> lock(g_packEntitiesMutex);
    if (!g_funchook)
    {
        ClearBindings();
        return true;
    }

    if (g_funchooksInstalled)
    {
        int result = funchook_uninstall(g_funchook, 0);
        if (result != FUNCHOOK_ERROR_SUCCESS)
        {
            std::string message = funchook_error_message(g_funchook);
            lock.unlock();
            META_CONPRINTF("[BOTHIDER] error: funchook_uninstall failed: %s (%d)\n", message.c_str(), result);
            return false;
        }
    }

    int result = funchook_destroy(g_funchook);
    std::string destroyMessage;
    if (result != FUNCHOOK_ERROR_SUCCESS) destroyMessage = funchook_error_message(g_funchook);
    g_funchook = nullptr;
    ClearBindings();
    lock.unlock();
    if (result != FUNCHOOK_ERROR_SUCCESS)
        META_CONPRINTF("[BOTHIDER] warning: funchook_destroy failed: %s (%d)\n", destroyMessage.c_str(), result);
    return true;
}

// Returns the resolved bot-quota hook target
void* MaintainQuotaTarget() { return g_quotaHookTarget; }

// Returns the resolved entity-packing hook target
void* PackEntitiesTarget() { return g_packEntitiesHookTarget; }

// Returns the resolved team-join hook target
void* HandleJoinTeamTarget() { return g_handleJoinTeamHookTarget; }

// Returns the resolved human-team restriction hook target
void* HumanTeamRestrictionTarget() { return g_applyHumanTeamRestrictionHookTarget; }

// Returns the resolved same-map teardown helper target
void* SameMapTeardownTarget() { return g_sameMapTeardownHookTarget; }

} // namespace cs2bh::identity_hooks
