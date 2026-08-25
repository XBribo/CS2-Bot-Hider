#pragma once

#include "plugin.h"
#include "sig_scan.h"

#include <array>
#include <cstdint>
#include <vector>

#include <nlohmann/json.hpp>

namespace cs2bh {

struct BotPawnRef
{
    void* Instance = nullptr;
    uint32_t Handle = 0xFFFFFFFF;
};

struct ManagedControllerTrace
{
    int Slot = -1;
    uint32_t Handle = 0xFFFFFFFF;
    uint32_t Flags = 0;
    unsigned int CurrentTeam = 0;
    bool Managed = false;
    bool Hltv = false;
};

// Clears FL_BOT for managed pawns during entity packing
std::vector<BotPawnRef> ApplyBotFlagOverride();

// Restores FL_BOT for pawns changed by the packing scope
void RestoreBotFlagOverride(const std::vector<BotPawnRef>& pawns);

// Collects identity state for one managed controller
ManagedControllerTrace TraceManagedController(void* controller);

namespace identity_hooks {

// Enters/leaves a nested transaction covering one Valve bot population command
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
    bool m_Redisguise;
};

// Resolves and prepares every optional identity detour
void PrepareAll(const nlohmann::json& gamedata, const sig::ModuleInfo& serverModule);

// Installs every successfully prepared identity detour
void InstallPrepared();

// Uninstalls all identity detours and releases their shared handle
bool Remove();

// Returns the resolved bot-quota hook target
void* MaintainQuotaTarget();

// Returns the resolved entity-packing hook target
void* PackEntitiesTarget();

// Returns the resolved team-join hook target
void* HandleJoinTeamTarget();

// Returns the resolved human-team restriction hook target
void* HumanTeamRestrictionTarget();

// Returns the resolved same-map teardown helper target
void* SameMapTeardownTarget();

} // namespace identity_hooks

} // namespace cs2bh
