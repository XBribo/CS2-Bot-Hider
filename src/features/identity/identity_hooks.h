#pragma once

#include "plugin.h"
#include "entity_access.h"
#include "core/cs2_sdk/src/core/memory_module.h"

#include <array>
#include <vector>

#include <nlohmann/json.hpp>

namespace cs2bh {

// Clear FL_BOT for managed pawns during packing.
void ApplyBotFlagOverride(std::vector<entity_access::PawnRef>& modified);

// Restore FL_BOT for the packing scope's pawns.
void RestoreBotFlagOverride(const std::vector<entity_access::PawnRef>& pawns);

// Check managed, non-HLTV controller ownership.
bool IsManagedBotController(void* controller);

namespace identity_hooks {

// Manages nested population-command identity transactions.
void BeginPopulationTransaction(bool redisguise);
void EndPopulationTransaction(bool redisguise);
bool PopulationTransactionActive();

class PopulationTransactionScope
{
  public:
    explicit PopulationTransactionScope(bool redisguise);
    ~PopulationTransactionScope();
    PopulationTransactionScope(const PopulationTransactionScope&) = delete;
    PopulationTransactionScope& operator=(const PopulationTransactionScope&) = delete;

  private:
    bool m_redisguise;
};

// Prepare identity detours.
void PrepareAll(const nlohmann::json& gamedata, const cs2_sdk::modules::ModuleInfo& serverModule);

// Install prepared identity detours.
void InstallPrepared();

// Validate the installed dependencies for player mode.
bool CanEnableDisguise(char* error, size_t maxlen);

// Remove identity hooks before releasing plugin state.
bool Remove();

// Read the bot-quota hook target.
void* MaintainQuotaTarget();

// Read the voter-count hook target.
void* CountPotentialVotersTarget();

// Read the entity-packing hook target.
void* PackEntitiesTarget();

// Read the team-join hook target.
void* HandleJoinTeamTarget();

// Read the human-team restriction hook target.
void* HumanTeamRestrictionTarget();

// Read the end-match hook target.
void* SameMapTeardownTarget();

// Read the script IsBot target.
void* ScriptIsBotTarget();

} // namespace identity_hooks

} // namespace cs2bh
