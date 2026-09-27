// clan_tag.cpp

#include "clan_tag_config.h"

#include <cstdint>

namespace cs2bh {

// Validates the configured tag and its 32-bit Steam group ID as one pair.
ClanTag ParseClanTag(const nlohmann::json& value)
{
    if (!value.contains("clan_tag") || !value["clan_tag"].is_string() ||
        !value.contains("clan_group_id") || !value["clan_group_id"].is_number_unsigned())
        return {};

    const std::string tag = value["clan_tag"].get<std::string>();
    const uint64_t groupId = value["clan_group_id"].get<uint64_t>();
    if (tag.empty() || tag.size() >= 128 || tag.find('\0') != std::string::npos ||
        groupId == 0 || groupId > UINT32_MAX)
        return {};

    return { tag, static_cast<uint32_t>(groupId) };
}

} // namespace cs2bh
