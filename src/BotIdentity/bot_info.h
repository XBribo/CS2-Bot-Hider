// bot_info.h
//
// Loads bot identity data from addons/BotHider/bot_info.json

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace cs2bh {

struct BotEntry
{
    std::string Name;
    uint32_t AccountId = 0;
    uint64_t SteamId64 = 0;
    std::string CrosshairCode;
    uint32_t ScoreboardFlair = 0;
};

class BotInfoStore
{
  public:
    // Load from disk. Returns false + logs on failure
    bool Load(const char* path);

    // Looks up the first entry with a matching player_name
    const BotEntry* FindByName(const char* name) const;

    // Selects an available identity, preferring an exact display-name match
    const BotEntry* PickForBot(const char* engineName);

    // Release an entry's assignment (slot freed / mapchange).
    void ReleaseAssignment(const BotEntry* entry);

    // Clears all active identity assignment counts
    void ResetAssignments();

    // Returns the number of enabled identity entries
    size_t Count() const { return m_Entries.size(); }

    // Returns every enabled identity entry
    const std::vector<BotEntry>& All() const { return m_Entries; }

    // Converts a 32-bit account ID to SteamID64
    static constexpr uint64_t kSteamId64Base = 76561197960265728ULL;

  private:
    std::vector<BotEntry> m_Entries;
    std::unordered_map<std::string, std::vector<size_t>> m_ByName;
    std::vector<uint32_t> m_AssignmentCounts;
    uint64_t m_RngState = 0;
};

// Returns the global bot identity store
BotInfoStore& BotInfo();

} // namespace cs2bh
