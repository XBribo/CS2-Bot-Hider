#include "core/cs2_sdk/src/core/gameconfig.h"
#include "core/log.h"
#include "identity_hooks.h"

#include "ISmmPlugin.h"
#include "entity_access.h"
#include "fake_client_manager.h"
#include "identity_runtime.h"
#include "nlohmann/json.hpp"
#include "plugin.h"
#include "serversideclient_ref.h"
#include "core/cs2_sdk/src/core/schema.h"
#include "core/cs2_sdk/src/core/memory_module.h"
#include "core/cs2_sdk/src/utils/hooks.h"
#include "offsets.h"

#include <cstdint>
#include <array>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
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
void* g_scriptIsBotHookTarget = nullptr;
int g_pickNewTeamsOnResetOffset = -1;
std::recursive_mutex g_packEntitiesMutex;
thread_local uint32_t g_packEntitiesDepth = 0;
thread_local uint32_t g_hookDepth = 0;
std::array<bool, 64> g_identityResolveWarned{};

struct BotIdentitySnapshot
{
    int slot = -1;
    void* client = nullptr;
    void* controller = nullptr;
    uint32_t handle = 0xFFFFFFFF;
    uint16_t userId = 0;
    uint8_t connectionFlags = 0;
    uint8_t connectionFlagsMask = 0;
    uint8_t fakePlayer = 0;
    uint32_t controllerFlags = 0;
    uint32_t controllerFlagsMask = 0;
};

bool IsValidController(void* controller, const char* className, uint32_t handle)
{
    return std::strcmp(className, "cs_player_controller") == 0 && !entity_access::IsEntityBeingDeleted(controller) &&
           std::cmp_equal(static_cast<uint32_t>(reinterpret_cast<CEntityInstance*>(controller)->GetRefEHandle().ToInt()), handle);
}

class ScopedBotIdentityRestore
{
  public:
    // Control network notifications during temporary identity changes.
    explicit ScopedBotIdentityRestore(bool markNetworkChanges = true) : m_markNetworkChanges(markNetworkChanges) { Capture(); }
    ~ScopedBotIdentityRestore() { Restore(); }

  private:
    std::array<BotIdentitySnapshot, 64> m_snapshots{};
    bool m_markNetworkChanges = true;

    void Capture()
    {
        for (int slot = 0; slot < bridge::kMaxSlots; ++slot)
        {
            if (!Manager().IsManaged(slot)) continue;

            auto& snapshot = m_snapshots[slot];
            snapshot.slot = slot;
            snapshot.client = entity_access::ResolveClientBySlot(slot);
            if (!snapshot.client)
            {
                if (!g_identityResolveWarned[slot])
                {
                    BH_LOG_WARN("identity restore slot=%d: client unresolved", slot);
                    g_identityResolveWarned[slot] = true;
                }
                continue;
            }

            auto* raw = reinterpret_cast<unsigned char*>(snapshot.client);
            snapshot.userId = *reinterpret_cast<uint16_t*>(raw + ssc::g_userIdOffset);
            snapshot.connectionFlags = raw[ssc::g_connectionTypeFlagsOffset];
            snapshot.fakePlayer = raw[ssc::g_fakePlayerOffset];
            ssc::SetFakePlayer(snapshot.client);
            snapshot.connectionFlagsMask = snapshot.connectionFlags ^ raw[ssc::g_connectionTypeFlagsOffset];

            const int entityIndex = *reinterpret_cast<int*>(raw + ssc::g_entityIndexOffset);
            char className[64];
            snapshot.controller = entity_access::ResolveEntityInstance(entityIndex, className, sizeof(className));
            if (!snapshot.controller)
            {
                if (!g_identityResolveWarned[slot])
                {
                    BH_LOG_WARN("identity restore slot=%d userid=%u ent=%d: controller unresolved", slot,
                                static_cast<unsigned int>(snapshot.userId), entityIndex);
                    g_identityResolveWarned[slot] = true;
                }
                continue;
            }
            if (std::strcmp(className, "cs_player_controller") != 0 || entity_access::IsEntityBeingDeleted(snapshot.controller))
            {
                BH_LOG_WARN("identity restore slot=%d userid=%u ent=%d: invalid controller class='%s'", slot,
                            static_cast<unsigned int>(snapshot.userId), entityIndex, className);
                g_identityResolveWarned[slot] = true;
                snapshot.controller = nullptr;
                continue;
            }

            g_identityResolveWarned[slot] = false;

            auto* entity = reinterpret_cast<CEntityInstance*>(snapshot.controller);
            snapshot.handle = static_cast<uint32_t>(entity->GetRefEHandle().ToInt());
            auto* flags =
                reinterpret_cast<uint32_t*>(reinterpret_cast<unsigned char*>(snapshot.controller) + offsets::g_baseEntityFlagsOffset);
            snapshot.controllerFlags = *flags;
            const uint32_t before = *flags;
            *flags |= offsets::kEntityFlagFakeClient;
            snapshot.controllerFlagsMask = before ^ *flags;
            if (*flags != before && m_markNetworkChanges)
            {
                entity_access::MarkEntityFieldChanged(snapshot.controller, static_cast<uint32_t>(offsets::g_baseEntityFlagsOffset));
            }
        }
    }

    void Restore()
    {
        for (const auto& snapshot : m_snapshots)
        {
            if (!snapshot.client) continue;
            if (!Manager().IsManaged(snapshot.slot)) continue;

            void* currentClient = entity_access::ResolveClientBySlot(snapshot.slot);
            if (currentClient != snapshot.client)
            {
                BH_LOG_WARN("identity restore skipped slot=%d: client rebound", snapshot.slot);
                continue;
            }

            auto* raw = reinterpret_cast<unsigned char*>(currentClient);
            const uint16_t currentUserId = *reinterpret_cast<uint16_t*>(raw + ssc::g_userIdOffset);
            if (currentUserId != snapshot.userId)
            {
                BH_LOG_WARN("identity restore skipped slot=%d: userid=%u->%u", snapshot.slot, static_cast<unsigned int>(snapshot.userId),
                            static_cast<unsigned int>(currentUserId));
                continue;
            }
            auto& connectionFlags = raw[ssc::g_connectionTypeFlagsOffset];
            connectionFlags = static_cast<uint8_t>((connectionFlags & ~snapshot.connectionFlagsMask) |
                                                   (snapshot.connectionFlags & snapshot.connectionFlagsMask));
            if (snapshot.fakePlayer != 1) raw[ssc::g_fakePlayerOffset] = snapshot.fakePlayer;

            if (!snapshot.controller) continue;
            const int entityIndex = *reinterpret_cast<int*>(raw + ssc::g_entityIndexOffset);
            char className[64];
            void* controller = entity_access::ResolveEntityInstance(entityIndex, className, sizeof(className));
            if (controller != snapshot.controller || !IsValidController(controller, className, snapshot.handle))
            {
                BH_LOG_WARN("identity restore skipped slot=%d userid=%u: controller rebound", snapshot.slot,
                            static_cast<unsigned int>(snapshot.userId));
                continue;
            }

            auto* flags = reinterpret_cast<uint32_t*>(reinterpret_cast<unsigned char*>(controller) + offsets::g_baseEntityFlagsOffset);
            const uint32_t restored = (*flags & ~snapshot.controllerFlagsMask) | (snapshot.controllerFlags & snapshot.controllerFlagsMask);
            if (*flags != restored)
            {
                *flags = restored;
                if (m_markNetworkChanges)
                    entity_access::MarkEntityFieldChanged(controller, static_cast<uint32_t>(offsets::g_baseEntityFlagsOffset));
            }
        }
    }
};

unsigned int g_populationTransactionDepth = 0;
bool g_populationTransactionRedisguise = false;
std::unique_ptr<ScopedBotIdentityRestore> g_populationIdentity;

} // namespace

void BeginPopulationTransaction(bool redisguise)
{
    if (g_populationTransactionDepth++ == 0)
    {
        g_populationTransactionRedisguise = redisguise;
        g_populationIdentity = std::make_unique<ScopedBotIdentityRestore>();
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
        BH_LOG_WARN("population transaction: unmatched end");
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
thread_local std::vector<std::unique_ptr<ScopedBotIdentityRestore>> g_voterFrames;
thread_local std::vector<std::unique_ptr<ScopedBotIdentityRestore>> g_scriptIsBotFrames;
thread_local std::vector<entity_access::PawnRef> g_packedPawns;

// Pair PRE/POST state across nesting and mode changes.
void BeginPopulationFrame(std::vector<bool>& frames, bool enabled)
{
    frames.push_back(enabled);
    ++g_hookDepth;
    if (enabled) BeginPopulationTransaction(true);
}

// Restore this invocation's captured transaction state.
void EndPopulationFrame(std::vector<bool>& frames)
{
    const bool enabled = frames.back();
    frames.pop_back();
    if (enabled) EndPopulationTransaction(true);
    --g_hookDepth;
}

// Restore bot identity for quota evaluation; preserve hook chaining.
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

// Capture identity per nested voter count.
KHook::Return<int> VotersPre(void*) noexcept
{
    ++g_hookDepth;
    g_voterFrames.push_back(g_plugin.IsDisguiseEnabled() ? std::make_unique<ScopedBotIdentityRestore>() : nullptr);
    return { KHook::Action::Ignore };
}

// Restore the matching voter-count snapshot.
KHook::Return<int> VotersPost(void*) noexcept
{
    g_voterFrames.pop_back();
    --g_hookDepth;
    return { KHook::Action::Ignore };
}

// Restores bot identity during script checks.
KHook::Return<void> ScriptIsBotPre(void*) noexcept
{
    g_packEntitiesMutex.lock();
    ++g_hookDepth;
    g_scriptIsBotFrames.push_back(g_plugin.IsDisguiseEnabled() ? std::make_unique<ScopedBotIdentityRestore>(false) : nullptr);
    return { KHook::Action::Ignore };
}

// Restores disguise before returning to JavaScript.
KHook::Return<void> ScriptIsBotPost(void*) noexcept
{
    g_scriptIsBotFrames.pop_back();
    --g_hookDepth;
    g_packEntitiesMutex.unlock();
    return { KHook::Action::Ignore };
}

// Restore bot identity for mp_humanteam.
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

// Restore bot identity during managed team joins.
KHook::Return<bool> JoinTeamPre(void* controller, unsigned int, uint8_t) noexcept
{
    const bool enabled = g_plugin.IsDisguiseEnabled() && IsManagedBotController(controller);
    BeginPopulationFrame(g_joinTeamFrames, enabled);
    return { KHook::Action::Ignore };
}

// Ends the identity transaction for this team join.
KHook::Return<bool> JoinTeamPost(void*, unsigned int, uint8_t) noexcept
{
    EndPopulationFrame(g_joinTeamFrames);
    return { KHook::Action::Ignore };
}

// Serialize packing; clear FL_BOT at the outermost call.
KHook::Return<void> PackPre(void*, void*, int, void*, void*) noexcept
{
    g_packEntitiesMutex.lock();
    ++g_hookDepth;
    if (g_packEntitiesDepth++ == 0 && g_plugin.IsDisguiseEnabled()) ApplyBotFlagOverride(g_packedPawns);
    return { KHook::Action::Ignore };
}

// Restore FL_BOT after the outermost packing call.
KHook::Return<void> PackPost(void*, void*, int, void*, void*) noexcept
{
    if (--g_packEntitiesDepth == 0)
    {
        RestoreBotFlagOverride(g_packedPawns);
        g_packedPawns.clear();
    }
    --g_hookDepth;
    g_packEntitiesMutex.unlock();
    return { KHook::Action::Ignore };
}

// Restore bot identity during end-match team resets.
KHook::Return<bool> EndMatchPre(void* gameRules) noexcept
{
    const bool enabled = g_plugin.IsDisguiseEnabled() && gameRules && g_pickNewTeamsOnResetOffset >= 0 &&
                         *(static_cast<const uint8_t*>(gameRules) + g_pickNewTeamsOnResetOffset) != 0;
    BeginPopulationFrame(g_teardownFrames, enabled);
    return { KHook::Action::Ignore };
}

// Reapply disguise after end-match processing.
KHook::Return<bool> EndMatchPost(void*) noexcept
{
    EndPopulationFrame(g_teardownFrames);
    return { KHook::Action::Ignore };
}

using cs2_sdk::hooks::NativeHook;

using QuotaHook = NativeHook<int64_t, void*>;
using VotersHook = NativeHook<int, void*>;
using HumanTeamHook = NativeHook<int64_t>;
using JoinTeamHook = NativeHook<bool, void*, unsigned int, uint8_t>;
using PackHook = NativeHook<void, void*, void*, int, void*, void*>;
using EndMatchHook = NativeHook<bool, void*>;
using ScriptIsBotHook = NativeHook<void, void*>;
QuotaHook g_quotaHook;
VotersHook g_votersHook;
HumanTeamHook g_humanTeamHook;
JoinTeamHook g_joinTeamHook;
PackHook g_packHook;
EndMatchHook g_endMatchHook;
ScriptIsBotHook g_scriptIsBotHook;

// Clear failed diagnostic targets.
template <typename Hook, typename Pre, typename Post> void InstallHook(Hook& hook, void*& target, Pre pre, Post post, const char* name)
{
    if (!target) return;
    hook.Remove();
    if (!hook.Install(target, pre, post))
    {
        BH_LOG_WARN("hook install failed: %s", name);
        target = nullptr;
    }
}

// Clear targets after hook removal.
void ClearBindings()
{
    g_quotaHookTarget = nullptr;
    g_countPotentialVotersHookTarget = nullptr;
    g_packEntitiesHookTarget = nullptr;
    g_handleJoinTeamHookTarget = nullptr;
    g_applyHumanTeamRestrictionHookTarget = nullptr;
    g_sameMapTeardownHookTarget = nullptr;
    g_scriptIsBotHookTarget = nullptr;
    g_pickNewTeamsOnResetOffset = -1;
}

// Prepare the bot-quota detour.
void PrepareQuotaHook(const nlohmann::json& gamedata, const cs2_sdk::modules::ModuleInfo& serverModule)
{
    char error[256]{};
    g_quotaHookTarget = cs2_sdk::gameconfig::ResolveSig(gamedata, serverModule, "CCSBotManager::MaintainBotQuota", error, sizeof(error));
    if (!g_quotaHookTarget) BH_LOG_WARN("%s", error);
}

// Prepare the voter-count detour.
void PrepareCountPotentialVotersHook(const nlohmann::json& gamedata, const cs2_sdk::modules::ModuleInfo& serverModule)
{
    char error[256]{};
    g_countPotentialVotersHookTarget =
        cs2_sdk::gameconfig::ResolveSig(gamedata, serverModule, "CBaseIssue::CountPotentialVoters", error, sizeof(error));
    if (!g_countPotentialVotersHookTarget) BH_LOG_WARN("%s", error);
}

// Prepare the team-join detour.
void PrepareHandleJoinTeamHook(const nlohmann::json& gamedata, const cs2_sdk::modules::ModuleInfo& serverModule)
{
    char error[256]{};
    g_handleJoinTeamHookTarget =
        cs2_sdk::gameconfig::ResolveSig(gamedata, serverModule, "CCSPlayerController::HandleCommand_JoinTeam", error, sizeof(error));
    if (!g_handleJoinTeamHookTarget) BH_LOG_WARN("%s", error);
}

// Prepare the human-team restriction detour.
void PrepareHumanTeamRestrictionHook(const nlohmann::json& gamedata, const cs2_sdk::modules::ModuleInfo& serverModule)
{
    char error[256]{};
    g_applyHumanTeamRestrictionHookTarget =
        cs2_sdk::gameconfig::ResolveSig(gamedata, serverModule, "MpHumanTeam_ApplyRestriction", error, sizeof(error));
    if (!g_applyHumanTeamRestrictionHookTarget) BH_LOG_WARN("%s", error);
}

// Prepare the entity-packing detour.
void PreparePackEntitiesHook(const nlohmann::json& gamedata)
{
    cs2_sdk::modules::Initialize();
    if (!cs2_sdk::modules::engine || !cs2_sdk::modules::engine->Code())
    {
        BH_LOG_WARN("PackEntities disabled: %s code unavailable", cs2_sdk::modules::kEngineModuleName);
        return;
    }

    char error[256]{};
    g_packEntitiesHookTarget = cs2_sdk::gameconfig::ResolveSig(gamedata, cs2_sdk::modules::engine->Code(),
                                                               "CNetworkGameServer::PackEntities", error, sizeof(error));
    if (!g_packEntitiesHookTarget) BH_LOG_WARN("%s", error);
}

// Resolve the end-match entry and Schema team-reset flag.
void PrepareSameMapTeardownHook(const nlohmann::json& gamedata, const cs2_sdk::modules::ModuleInfo& serverModule)
{
    char error[256]{};
    void* target = cs2_sdk::gameconfig::ResolveSig(gamedata, serverModule, "CCSGameRules::EndMatchState", error, sizeof(error));
    if (!target)
    {
        BH_LOG_WARN("%s", error);
        return;
    }

    g_pickNewTeamsOnResetOffset = cs2_sdk::schema::GetFieldOffset("CCSGameRules", "m_bPickNewTeamsOnReset");
    if (g_pickNewTeamsOnResetOffset < 0)
    {
        BH_LOG_WARN("SameMapTeardown disabled: m_bPickNewTeamsOnReset unresolved");
        return;
    }
    g_sameMapTeardownHookTarget = target;
    BH_LOG_DEBUG("CCSGameRules::m_bPickNewTeamsOnReset=0x%x", g_pickNewTeamsOnResetOffset);
}

// Resolve the script IsBot callback.
void PrepareScriptIsBotHook(const nlohmann::json& gamedata, const cs2_sdk::modules::ModuleInfo& serverModule)
{
    g_scriptIsBotHookTarget = nullptr;
    char error[256]{};
    g_scriptIsBotHookTarget =
        cs2_sdk::gameconfig::ResolveSig(gamedata, serverModule, "CSPlayerController::ScriptIsBot", error, sizeof(error));
    if (!g_scriptIsBotHookTarget) BH_LOG_WARN("%s", error);
}

} // namespace

// Prepare identity detours.
void PrepareAll(const nlohmann::json& gamedata, const cs2_sdk::modules::ModuleInfo& serverModule)
{
    PrepareQuotaHook(gamedata, serverModule);
    PrepareCountPotentialVotersHook(gamedata, serverModule);
    PrepareHandleJoinTeamHook(gamedata, serverModule);
    PrepareHumanTeamRestrictionHook(gamedata, serverModule);
    PreparePackEntitiesHook(gamedata);
    PrepareSameMapTeardownHook(gamedata, serverModule);
    PrepareScriptIsBotHook(gamedata, serverModule);
}

// Install prepared identity detours.
void InstallPrepared()
{
    InstallHook(g_quotaHook, g_quotaHookTarget, QuotaPre, QuotaPost, "CCSBotManager::MaintainBotQuota");
    InstallHook(g_votersHook, g_countPotentialVotersHookTarget, VotersPre, VotersPost, "CBaseIssue::CountPotentialVoters");
    InstallHook(g_joinTeamHook, g_handleJoinTeamHookTarget, JoinTeamPre, JoinTeamPost, "CCSPlayerController::HandleCommand_JoinTeam");
    InstallHook(g_humanTeamHook, g_applyHumanTeamRestrictionHookTarget, HumanTeamPre, HumanTeamPost, "MpHumanTeam_ApplyRestriction");
    InstallHook(g_packHook, g_packEntitiesHookTarget, PackPre, PackPost, "CNetworkGameServer::PackEntities");
    InstallHook(g_endMatchHook, g_sameMapTeardownHookTarget, EndMatchPre, EndMatchPost, "CCSGameRules::EndMatchState");
    if (g_packHook.Active() && entity_access::UtilRemoveTarget())
    {
        InstallHook(g_scriptIsBotHook, g_scriptIsBotHookTarget, ScriptIsBotPre, ScriptIsBotPost, "CSPlayerController::ScriptIsBot");
    }
    else
    {
        g_scriptIsBotHookTarget = nullptr;
        BH_LOG_WARN("ScriptIsBot disabled: PackEntities or entity resolution unavailable");
    }
}

bool CanEnableDisguise(char* error, size_t maxlen)
{
    const struct
    {
        bool ready;
        const char* name;
    } dependencies[] = {
        { g_quotaHook.Active(), "CCSBotManager::MaintainBotQuota" },
        { g_votersHook.Active(), "CBaseIssue::CountPotentialVoters" },
        { g_joinTeamHook.Active(), "CCSPlayerController::HandleCommand_JoinTeam" },
        { g_humanTeamHook.Active(), "MpHumanTeam_ApplyRestriction" },
        { g_packHook.Active(), "CNetworkGameServer::PackEntities" },
        { g_endMatchHook.Active(), "CCSGameRules::EndMatchState" },
        { entity_access::UtilRemoveTarget() != nullptr, "UTIL_Remove" },
        { entity_access::HasEntitySystem(), "entity-system reference" },
        { offsets::g_botPawnHandleOffset >= 0, "controller pawn handle" },
    };
    std::string missing;
    for (const auto& dependency : dependencies)
    {
        if (dependency.ready) continue;
        if (!missing.empty()) missing += ", ";
        missing += dependency.name;
    }
    if (!missing.empty())
    {
        std::snprintf(error, maxlen, "player mode unavailable: %s", missing.c_str());
        return false;
    }
    return true;
}

// Remove identity detours outside callbacks.
bool Remove()
{
    if (g_hookDepth != 0)
    {
        BH_LOG_ERROR("hook removal rejected: identity callback active");
        return false;
    }
    // KHook waits for active calls with the packing mutex unlocked.
    g_scriptIsBotHook.Remove();
    g_packHook.Remove();
    g_endMatchHook.Remove();
    g_joinTeamHook.Remove();
    g_humanTeamHook.Remove();
    g_votersHook.Remove();
    g_quotaHook.Remove();
    ClearBindings();
    return true;
}

// Read the bot-quota hook target.
void* MaintainQuotaTarget() { return g_quotaHookTarget; }

// Read the voter-count hook target.
void* CountPotentialVotersTarget() { return g_countPotentialVotersHookTarget; }

// Read the entity-packing hook target.
void* PackEntitiesTarget() { return g_packEntitiesHookTarget; }

// Read the team-join hook target.
void* HandleJoinTeamTarget() { return g_handleJoinTeamHookTarget; }

// Read the human-team restriction hook target.
void* HumanTeamRestrictionTarget() { return g_applyHumanTeamRestrictionHookTarget; }

// Read the end-match hook target.
void* SameMapTeardownTarget() { return g_sameMapTeardownHookTarget; }

// Read the script IsBot target.
void* ScriptIsBotTarget() { return g_scriptIsBotHookTarget; }

} // namespace cs2bh::identity_hooks
