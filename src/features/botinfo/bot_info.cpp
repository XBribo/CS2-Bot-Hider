#include "bot_info.h"
#include "clan_tag_config.h"
#include "bridge.h"
#include "core/log.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace cs2bh {

namespace {
BotInfoStore g_botInfo; // NOLINT(bugprone-throwing-static-initialization)

// Local xorshift64* generator.
uint64_t NextRand(uint64_t& s)
{
    uint64_t x = s ? s : 0x9E3779B97F4A7C15ULL;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

// Parse a 32-bit account ID from a JSON key.
bool ParseAccountId(const std::string& text, uint32_t& accountId)
{
    accountId = 0;
    if (text.empty() || text.front() == '0') return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto [position, error] = std::from_chars(begin, end, accountId);
    return error == static_cast<std::errc>(0) && position == end && accountId != 0;
}
} // namespace

// Access the global identity store.
BotInfoStore& BotInfo() { return g_botInfo; }

// Load enabled identities from players.
bool BotInfoStore::Load(const char* path)
{
    m_entries.clear();
    m_assigned.clear();
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

    size_t rejected = 0;
    for (const auto& [key, val] : root["players"].items())
    {
        if (!val.is_object())
        {
            ++rejected;
            continue;
        }
        BotEntry e;
        if (!ParseAccountId(key, e.accountId) || !val.contains("player_name") || !val["player_name"].is_string())
        {
            ++rejected;
            continue;
        }
        e.name = val["player_name"].get<std::string>();
        const bool hasControl = std::ranges::any_of(e.name, [](unsigned char ch) {
            return ch < 0x20 || ch == 0x7F;
        });
        if (e.name.size() >= bridge::kNameLen || hasControl || e.name.find_first_not_of(' ') == std::string::npos)
        {
            ++rejected;
            continue;
        }
        e.steamId64 = kSteamId64Base + static_cast<uint64_t>(e.accountId);
        if (val.contains("crosshair_code") && val["crosshair_code"].is_string()) e.crosshairCode = val["crosshair_code"].get<std::string>();
        if (e.crosshairCode.size() >= bridge::kCrosshairLen || e.crosshairCode.find('\0') != std::string::npos)
        {
            ++rejected;
            continue;
        }
        if (e.crosshairCode == "0") e.crosshairCode.clear();
        if (val.contains("scoreboard_flair") && val["scoreboard_flair"].is_number_integer())
        {
            const auto& flair = val["scoreboard_flair"];
            if (flair >= 0 && flair <= UINT16_MAX) e.scoreboardFlair = flair.get<uint32_t>();
        }
        e.clanTag = ParseClanTag(val);
        m_entries.push_back(std::move(e));
    }
    m_assigned.assign(m_entries.size(), false);
    if (rejected) BH_LOG_WARN("bot_info skipped %zu invalid identities", rejected);
    return !m_entries.empty();
}

// Pick an available identity, preferring an exact name match.
const BotEntry* BotInfoStore::PickForBot(const char* engineName, const std::unordered_set<uint64_t>& occupied)
{
    std::vector<size_t> available;
    std::vector<size_t> matches;
    available.reserve(m_entries.size());
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        const auto& entry = m_entries[i];
        if (m_assigned[i] || occupied.contains(entry.steamId64)) continue;
        available.push_back(i);
        if (engineName && entry.name == engineName) matches.push_back(i);
    }
    const auto& candidates = matches.empty() ? available : matches;
    if (candidates.empty()) return nullptr;
    const size_t pick = candidates[NextRand(m_rngState) % candidates.size()];
    m_assigned[pick] = true;
    return &m_entries[pick];
}

// Release the selected identity's assignment.
void BotInfoStore::ReleaseAssignment(const BotEntry* entry)
{
    if (!entry) return;
    m_assigned[entry - m_entries.data()] = false;
}

// Clear identity assignments.
void BotInfoStore::ResetAssignments() { std::fill(m_assigned.begin(), m_assigned.end(), false); }

} // namespace cs2bh
