// Bot identities from addons/BotHider/bot_info.json.

#pragma once

#include "clan_tag.h"

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace cs2bh {

struct BotEntry
{
    std::string name;
    uint32_t accountId = 0;
    uint64_t steamId64 = 0;
    std::string crosshairCode;
    uint32_t scoreboardFlair = 0;
    ClanTag clanTag;
};

class BotInfoStore
{
  public:
    // Load identities; log failures and return false.
    bool Load(const char* path);

    // Pick an available identity, preferring an exact name match.
    const BotEntry* PickForBot(const char* engineName);

    // Release an identity assignment.
    void ReleaseAssignment(const BotEntry* entry);

    // Clear identity assignment counts.
    void ResetAssignments();

    // Count enabled identities.
    size_t Count() const { return m_entries.size(); }

    // Read enabled identities.
    const std::vector<BotEntry>& All() const { return m_entries; }

    // SteamID64 base for 32-bit account IDs.
    static constexpr uint64_t kSteamId64Base = 76561197960265728ULL;

  private:
    std::vector<BotEntry> m_entries;
    std::unordered_map<std::string, std::vector<size_t>> m_byName;
    std::vector<uint32_t> m_assignmentCounts;
    uint64_t m_rngState = 0;
};

// Access the global identity store.
BotInfoStore& BotInfo();

} // namespace cs2bh
