// bot_info.cpp

#include "bot_info.h"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <fstream>

#include <nlohmann/json.hpp>

namespace cs2bh {

namespace {
BotInfoStore g_botInfo;

// xorshift64* — local RNG.
uint64_t NextRand(uint64_t& s)
{
    uint64_t x = s ? s : 0x9E3779B97F4A7C15ULL;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

// Parses one 32-bit Steam account ID from a JSON object key
bool ParseAccountId(const std::string& text, uint32_t& accountId)
{
    accountId = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto [position, error] = std::from_chars(begin, end, accountId);
    return error == std::errc{} && position == end && accountId != 0;
}
} // namespace

// Returns the global bot identity store
BotInfoStore& BotInfo() { return g_botInfo; }

// Loads enabled identities from the players object
bool BotInfoStore::Load(const char* path)
{
    m_entries.clear();
    m_byName.clear();
    m_assignmentCounts.clear();
    m_rngState = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) | 1ULL;

    std::ifstream ifs(path);
    if (!ifs.is_open()) return false;

    nlohmann::json root;
    try
    {
        root = nlohmann::json::parse(ifs);
    }
    catch (...)
    {
        return false;
    }

    if (!root.is_object() || !root.contains("players") || !root["players"].is_object())
    {
        return false;
    }

    for (auto& [key, val] : root["players"].items())
    {
        if (!val.is_object()) continue;
        BotEntry e;
        if (!ParseAccountId(key, e.accountId) || !val.contains("player_name") || !val["player_name"].is_string())
        {
            continue;
        }
        e.name = val["player_name"].get<std::string>();
        if (e.name.empty()) continue;
        e.steamId64 = kSteamId64Base + static_cast<uint64_t>(e.accountId);
        if (val.contains("crosshair_code") && val["crosshair_code"].is_string()) e.crosshairCode = val["crosshair_code"].get<std::string>();
        if (val.contains("scoreboard_flair") && val["scoreboard_flair"].is_number_unsigned())
        {
            uint64_t flair = val["scoreboard_flair"].get<uint64_t>();
            e.scoreboardFlair = flair <= 0xFFFFu ? static_cast<uint32_t>(flair) : 0;
        }
        else if (val.contains("scoreboard_flair") && val["scoreboard_flair"].is_number_integer())
        {
            int64_t flair = val["scoreboard_flair"].get<int64_t>();
            e.scoreboardFlair = (flair >= 0 && flair <= 0xFFFF) ? static_cast<uint32_t>(flair) : 0;
        }
        m_byName[e.name].push_back(m_entries.size());
        m_entries.push_back(std::move(e));
    }
    m_assignmentCounts.assign(m_entries.size(), 0);
    return !m_entries.empty();
}

// Returns the first enabled identity with an exact display-name match
const BotEntry* BotInfoStore::FindByName(const char* name) const
{
    if (!name) return nullptr;
    auto it = m_byName.find(name);
    if (it == m_byName.end() || it->second.empty()) return nullptr;
    return &m_entries[it->second.front()];
}

// Selects an available identity, preferring an exact display-name match
const BotEntry* BotInfoStore::PickForBot(const char* engineName)
{
    if (m_entries.empty()) return nullptr;

    if (engineName && engineName[0])
    {
        auto it = m_byName.find(engineName);
        if (it != m_byName.end())
        {
            std::vector<size_t> freeMatches;
            freeMatches.reserve(it->second.size());
            for (size_t index : it->second)
            {
                if (m_assignmentCounts[index] == 0) freeMatches.push_back(index);
            }

            const std::vector<size_t>& candidates = freeMatches.empty() ? it->second : freeMatches;
            const size_t pick = candidates[NextRand(m_rngState) % candidates.size()];
            ++m_assignmentCounts[pick];
            return &m_entries[pick];
        }
    }

    std::vector<size_t> free;
    free.reserve(m_entries.size());
    for (size_t i = 0; i < m_entries.size(); ++i)
        if (m_assignmentCounts[i] == 0) free.push_back(i);

    if (free.empty())
    {
        const size_t index = NextRand(m_rngState) % m_entries.size();
        ++m_assignmentCounts[index];
        return &m_entries[index];
    }
    const size_t pick = free[NextRand(m_rngState) % free.size()];
    ++m_assignmentCounts[pick];
    return &m_entries[pick];
}

// Releases one exact identity assignment
void BotInfoStore::ReleaseAssignment(const BotEntry* entry)
{
    if (!entry) return;
    auto it = m_byName.find(entry->name);
    if (it != m_byName.end())
    {
        for (size_t index : it->second)
        {
            if (&m_entries[index] != entry) continue;
            if (m_assignmentCounts[index] > 0) --m_assignmentCounts[index];
            return;
        }
    }
}

// Clears all active identity assignment counts
void BotInfoStore::ResetAssignments() { m_assignmentCounts.assign(m_entries.size(), 0); }

} // namespace cs2bh
