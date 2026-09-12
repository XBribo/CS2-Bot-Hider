#include "identity_hooks.h"

#include "ISmmPlugin.h"
#include "entity_access.h"
#include "fake_client_manager.h"
#include "identity_runtime.h"
#include "nlohmann/json.hpp"
#include "personas.h"
#include "plugin.h"
#include "serversideclient_ref.h"
#include "schema_resolver.h"
#include "sig_scan.h"
#include "version_targets.h"

#include <cstdint>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <string>
#include <vector>

#include <entity2/entityinstance.h>
#include <khook.hpp>

namespace cs2bh::identity_hooks {

namespace {

void* g_quotaHookTarget = nullptr;
void* g_countPotentialVotersHookTarget = nullptr;
void* g_handleJoinTeamHookTarget = nullptr;
void* g_applyHumanTeamRestrictionHookTarget = nullptr;
void* g_packEntitiesHookTarget = nullptr;
void* g_sameMapTeardownHookTarget = nullptr;
int g_pickNewTeamsOnResetOffset = -1;
std::recursive_mutex g_packEntitiesMutex;
thread_local uint32_t g_packEntitiesDepth = 0;
thread_local uint32_t g_nativeHookDepth = 0;
std::array<bool, 64> g_identityResolveWarned{};
bool g_identityInvalidOffsetWarned = false;

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

bool IsValidController(void* controller, const char* className, uint32_t handle)
{
    return controller && className && std::strcmp(className, "cs_player_controller") == 0 &&
           !entity_access::IsEntityBeingDeleted(controller) &&
           std::cmp_equal(static_cast<uint32_t>(reinterpret_cast<CEntityInstance*>(controller)->GetRefEHandle().ToInt()), handle);
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
            ssc::g_connectionTypeFlagsOffset < 0 || ssc::g_fakePlayerOffset < 0 || targets::g_baseEntityFlagsOffset < 0)
        {
            if (!g_identityInvalidOffsetWarned)
            {
                META_CONPRINTF("[BOTHIDER] warning: native identity restore disabled: invalid client/controller offsets\n");
                g_identityInvalidOffsetWarned = true;
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
                if (!g_identityResolveWarned[slot])
                {
                    META_CONPRINTF("[BOTHIDER] warning: native identity restore client resolve failed slot=%d\n", slot);
                    g_identityResolveWarned[slot] = true;
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
                if (!g_identityResolveWarned[slot])
                {
                    META_CONPRINTF("[BOTHIDER] warning: native identity restore controller resolve failed slot=%d userid=%u entIdx=%d "
                                   "clientFake=%u conn=0x%02x net=%p\n",
                                   slot, static_cast<unsigned int>(snapshot.userId), entityIndex,
                                   static_cast<unsigned int>(snapshot.fakePlayer), static_cast<unsigned int>(snapshot.connectionFlags),
                                   *reinterpret_cast<void**>(raw + ssc::g_netChannelOffset));
                    g_identityResolveWarned[slot] = true;
                }
                continue;
            }
            if (std::strcmp(className, "cs_player_controller") != 0 || entity_access::IsEntityBeingDeleted(snapshot.controller))
            {
                META_CONPRINTF("[BOTHIDER] warning: native identity restore invalid controller slot=%d userid=%u entIdx=%d cls='%s'\n",
                               slot, static_cast<unsigned int>(snapshot.userId), entityIndex, className);
                g_identityResolveWarned[slot] = true;
                snapshot.controller = nullptr;
                continue;
            }

            g_identityResolveWarned[slot] = false;

            auto* entity = reinterpret_cast<CEntityInstance*>(snapshot.controller);
            snapshot.handle = static_cast<uint32_t>(entity->GetRefEHandle().ToInt());
            auto* flags =
                reinterpret_cast<uint32_t*>(reinterpret_cast<unsigned char*>(snapshot.controller) + targets::g_baseEntityFlagsOffset);
            snapshot.controllerFlags = *flags;
            snapshot.hasController = true;
            const uint32_t before = *flags;
            *flags |= 0x100U;
            if (*flags != before)
            {
                entity_access::MarkEntityFieldChanged(snapshot.controller, static_cast<uint32_t>(targets::g_baseEntityFlagsOffset));
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
                META_CONPRINTF("[BOTHIDER] warning: native identity restore skipped slot=%d: client rebound\n", snapshot.slot);
                continue;
            }

            auto* raw = reinterpret_cast<unsigned char*>(currentClient);
            const uint16_t currentUserId = *reinterpret_cast<uint16_t*>(raw + ssc::g_userIdOffset);
            if (currentUserId != snapshot.userId)
            {
                META_CONPRINTF("[BOTHIDER] warning: native identity restore skipped slot=%d: userid changed %u->%u\n", snapshot.slot,
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
                META_CONPRINTF("[BOTHIDER] warning: native identity restore skipped slot=%d userid=%u: controller rebound\n", snapshot.slot,
                               static_cast<unsigned int>(snapshot.userId));
                continue;
            }

            auto* flags = reinterpret_cast<uint32_t*>(reinterpret_cast<unsigned char*>(controller) + targets::g_baseEntityFlagsOffset);
            if (*flags != snapshot.controllerFlags)
            {
                *flags = snapshot.controllerFlags;
                entity_access::MarkEntityFieldChanged(controller, static_cast<uint32_t>(targets::g_baseEntityFlagsOffset));
            }
        }
    }
};

unsigned int g_populationTransactionDepth = 0;
bool g_populationTransactionRedisguise = false;
std::unique_ptr<ScopedNativeBotIdentityRestore> g_populationIdentity;

} // namespace

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

namespace {

thread_local std::vector<bool> g_quotaFrames;
thread_local std::vector<bool> g_humanTeamFrames;
thread_local std::vector<bool> g_joinTeamFrames;
thread_local std::vector<bool> g_teardownFrames;
thread_local std::vector<std::unique_ptr<ScopedNativeBotIdentityRestore>> g_voterFrames;
thread_local std::vector<BotPawnRef> g_packedPawns;

// Keeps each PRE paired with its POST even when callbacks nest or change identity mode.
void BeginPopulationFrame(std::vector<bool>& frames, bool enabled)
{
    frames.push_back(enabled);
    ++g_nativeHookDepth;
    if (enabled) BeginPopulationTransaction(true);
}

// Restores the transaction belonging to this invocation, not the current identity mode.
void EndPopulationFrame(std::vector<bool>& frames)
{
    const bool enabled = frames.back();
    frames.pop_back();
    if (enabled) EndPopulationTransaction(true);
    --g_nativeHookDepth;
}

// Restores bot identity before quota evaluation without bypassing other consumers.
KHook::Return<int64_t> QuotaPre(void*) noexcept
{
    BeginPopulationFrame(g_quotaFrames, g_plugin.IsDisguiseEnabled());
    return { KHook::Action::Ignore };
}

// Reapplies disguise after quota evaluation.
KHook::Return<int64_t> QuotaPost(void*) noexcept
{
    EndPopulationFrame(g_quotaFrames);
    return { KHook::Action::Ignore };
}

// Captures bot identity for each nested voter count.
KHook::Return<int> VotersPre(void*) noexcept
{
    ++g_nativeHookDepth;
    g_voterFrames.push_back(g_plugin.IsDisguiseEnabled() ? std::make_unique<ScopedNativeBotIdentityRestore>() : nullptr);
    return { KHook::Action::Ignore };
}

// Restores the matching voter-count snapshot.
KHook::Return<int> VotersPost(void*) noexcept
{
    g_voterFrames.pop_back();
    --g_nativeHookDepth;
    return { KHook::Action::Ignore };
}

// Restores native identity while applying mp_humanteam.
KHook::Return<int64_t> HumanTeamPre() noexcept
{
    BeginPopulationFrame(g_humanTeamFrames, g_plugin.IsDisguiseEnabled());
    return { KHook::Action::Ignore };
}

// Reapplies disguise after the human-team restriction.
KHook::Return<int64_t> HumanTeamPost() noexcept
{
    EndPopulationFrame(g_humanTeamFrames);
    return { KHook::Action::Ignore };
}

// Restores native identity only for managed non-HLTV team joins.
KHook::Return<int64_t> JoinTeamPre(void* controller, unsigned int, bool) noexcept
{
    bool enabled = false;
    if (g_plugin.IsDisguiseEnabled())
    {
        const auto trace = TraceManagedController(controller);
        enabled = trace.managed && !trace.hltv;
    }
    BeginPopulationFrame(g_joinTeamFrames, enabled);
    return { KHook::Action::Ignore };
}

// Ends the identity transaction for this team join.
KHook::Return<int64_t> JoinTeamPost(void*, unsigned int, bool) noexcept
{
    EndPopulationFrame(g_joinTeamFrames);
    return { KHook::Action::Ignore };
}

// Serializes packing and clears FL_BOT only for the outermost invocation.
KHook::Return<void> PackPre(void*, void*, int, void*, void*) noexcept
{
    g_packEntitiesMutex.lock();
    ++g_nativeHookDepth;
    if (g_packEntitiesDepth++ == 0) g_packedPawns = ApplyBotFlagOverride();
    return { KHook::Action::Ignore };
}

// Restores FL_BOT after all nested packing calls have finished.
KHook::Return<void> PackPost(void*, void*, int, void*, void*) noexcept
{
    if (--g_packEntitiesDepth == 0)
    {
        RestoreBotFlagOverride(g_packedPawns);
        g_packedPawns.clear();
    }
    --g_nativeHookDepth;
    g_packEntitiesMutex.unlock();
    return { KHook::Action::Ignore };
}

// Restores native identity only while the end-match state machine can reset teams.
KHook::Return<bool> EndMatchPre(void* gameRules) noexcept
{
    const bool enabled = g_plugin.IsDisguiseEnabled() && gameRules && g_pickNewTeamsOnResetOffset >= 0 &&
                         *(static_cast<const uint8_t*>(gameRules) + g_pickNewTeamsOnResetOffset) != 0;
    BeginPopulationFrame(g_teardownFrames, enabled);
    return { KHook::Action::Ignore };
}

// Redisguises surviving clients after the engine finishes its end-match work.
KHook::Return<bool> EndMatchPost(void*) noexcept
{
    EndPopulationFrame(g_teardownFrames);
    return { KHook::Action::Ignore };
}

// Exposes registration failure while retaining KHook's typed callback bridge.
template <typename Return, typename... Args> class NativeHook : public KHook::Function<Return, Args...>
{
  public:
    using KHook::Function<Return, Args...>::Function;

    // Registers the target and reports whether KHook accepted it.
    bool Install(void* target)
    {
        this->Configure(target);
        return this->_associated_hook_id != KHook::INVALID_HOOK;
    }
};

using QuotaHook = NativeHook<int64_t, void*>;
using VotersHook = NativeHook<int, void*>;
using HumanTeamHook = NativeHook<int64_t>;
using JoinTeamHook = NativeHook<int64_t, void*, unsigned int, bool>;
using PackHook = NativeHook<void, void*, void*, int, void*, void*>;
using EndMatchHook = NativeHook<bool, void*>;
std::unique_ptr<QuotaHook> g_quotaHook;
std::unique_ptr<VotersHook> g_votersHook;
std::unique_ptr<HumanTeamHook> g_humanTeamHook;
std::unique_ptr<JoinTeamHook> g_joinTeamHook;
std::unique_ptr<PackHook> g_packHook;
std::unique_ptr<EndMatchHook> g_endMatchHook;

// Installs one resolved optional hook and clears failed targets from diagnostics.
template <typename Hook, typename Pre, typename Post>
void InstallHook(std::unique_ptr<Hook>& hook, void*& target, Pre pre, Post post, const char* name)
{
    if (!target) return;
    hook = std::make_unique<Hook>(pre, post);
    if (!hook->Install(target))
    {
        META_CONPRINTF("[BOTHIDER] warning: KHook registration failed for %s\n", name);
        hook.reset();
        target = nullptr;
    }
}

// Clears the resolved targets after hook removal.
void ClearBindings()
{
    g_quotaHookTarget = nullptr;
    g_countPotentialVotersHookTarget = nullptr;
    g_packEntitiesHookTarget = nullptr;
    g_handleJoinTeamHookTarget = nullptr;
    g_applyHumanTeamRestrictionHookTarget = nullptr;
    g_sameMapTeardownHookTarget = nullptr;
    g_pickNewTeamsOnResetOffset = -1;
}

// Resolves and prepares the bot quota detour
void PrepareQuotaHook(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule)
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
    g_quotaHookTarget = target;
}

// Resolves and prepares the eligible-voter count detour
void PrepareCountPotentialVotersHook(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule)
{
    if (!serverModule) return;
    std::string signature = sig::FindPlatformSig(gamedata, "CBaseIssue::CountPotentialVoters");
    std::vector<uint8_t> bytes;
    std::vector<bool> wildcards;
    if (signature.empty() || !sig::ParseSigString(signature, bytes, wildcards))
    {
        META_CONPRINTF("[BOTHIDER] warning: CountPotentialVoters signature missing or malformed\n");
        return;
    }

    std::vector<void*> matches = sig::FindPatternMatchesIn(serverModule, bytes, wildcards);
    if (matches.size() != 1)
    {
        META_CONPRINTF("[BOTHIDER] warning: CountPotentialVoters hook requires exactly one match\n");
        return;
    }

    void* target = matches.front();
    g_countPotentialVotersHookTarget = target;
}

// Resolves and prepares the team-join identity detour
void PrepareHandleJoinTeamHook(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule)
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
    g_handleJoinTeamHookTarget = target;
}

// Resolves and prepares the human-team restriction detour
void PrepareHumanTeamRestrictionHook(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule)
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
    g_applyHumanTeamRestrictionHookTarget = target;
}

// Resolves and prepares the entity-packing detour
void PreparePackEntitiesHook(const nlohmann::json& gamedata)
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
    g_packEntitiesHookTarget = target;
}

// Resolves the state-machine entry and its Schema-controlled team-reset condition.
void PrepareSameMapTeardownHook(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule)
{
    if (!serverModule) return;
    constexpr const char* kTargetName = "CCSGameRules::EndMatchState";
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

    g_pickNewTeamsOnResetOffset = schema::GetFieldOffset("CCSGameRules", "m_bPickNewTeamsOnReset");
    if (g_pickNewTeamsOnResetOffset < 0)
    {
        META_CONPRINTF("[BOTHIDER] warning: CCSGameRules::m_bPickNewTeamsOnReset Schema field unavailable - teardown hook disabled\n");
        return;
    }
    g_sameMapTeardownHookTarget = matches.front();
    META_CONPRINTF("[BOTHIDER] Schema CCSGameRules::m_bPickNewTeamsOnReset=0x%x\n", g_pickNewTeamsOnResetOffset);
}

} // namespace

// Resolves and prepares every optional identity detour
void PrepareAll(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule)
{
    PrepareQuotaHook(gamedata, serverModule);
    PrepareCountPotentialVotersHook(gamedata, serverModule);
    PrepareHandleJoinTeamHook(gamedata, serverModule);
    PrepareHumanTeamRestrictionHook(gamedata, serverModule);
    PreparePackEntitiesHook(gamedata);
    PrepareSameMapTeardownHook(gamedata, serverModule);
}

// Installs every successfully prepared identity detour
void InstallPrepared()
{
    InstallHook(g_quotaHook, g_quotaHookTarget, QuotaPre, QuotaPost, "CCSBotManager::MaintainBotQuota");
    InstallHook(g_votersHook, g_countPotentialVotersHookTarget, VotersPre, VotersPost, "CBaseIssue::CountPotentialVoters");
    InstallHook(g_joinTeamHook, g_handleJoinTeamHookTarget, JoinTeamPre, JoinTeamPost, "CCSPlayerController::HandleCommand_JoinTeam");
    InstallHook(g_humanTeamHook, g_applyHumanTeamRestrictionHookTarget, HumanTeamPre, HumanTeamPost, "MpHumanTeam_ApplyRestriction");
    InstallHook(g_packHook, g_packEntitiesHookTarget, PackPre, PackPost, "CNetworkGameServer::PackEntities");
    InstallHook(g_endMatchHook, g_sameMapTeardownHookTarget, EndMatchPre, EndMatchPost, "CCSGameRules::EndMatchState");
}

// Uninstalls all identity detours and releases their shared handle
bool Remove()
{
    if (g_nativeHookDepth != 0)
    {
        META_CONPRINTF("[BOTHIDER] error: refusing KHook removal during an identity callback\n");
        return false;
    }
    // KHook waits for active calls; holding the packing mutex here would deadlock them.
    g_packHook.reset();
    g_endMatchHook.reset();
    g_joinTeamHook.reset();
    g_humanTeamHook.reset();
    g_votersHook.reset();
    g_quotaHook.reset();
    ClearBindings();
    return true;
}

// Returns the resolved bot-quota hook target
void* MaintainQuotaTarget() { return g_quotaHookTarget; }

// Returns the resolved eligible-voter count hook target
void* CountPotentialVotersTarget() { return g_countPotentialVotersHookTarget; }

// Returns the resolved entity-packing hook target
void* PackEntitiesTarget() { return g_packEntitiesHookTarget; }

// Returns the resolved team-join hook target
void* HandleJoinTeamTarget() { return g_handleJoinTeamHookTarget; }

// Returns the resolved human-team restriction hook target
void* HumanTeamRestrictionTarget() { return g_applyHumanTeamRestrictionHookTarget; }

// Returns the resolved same-map teardown helper target
void* SameMapTeardownTarget() { return g_sameMapTeardownHookTarget; }

} // namespace cs2bh::identity_hooks
