// clan_tag_config.h
//
// Parses Clan Tag fields from bot_info.json entries.

#pragma once

#include "clan_tag.h"

#include <nlohmann/json.hpp>

namespace cs2bh {

// Returns an empty pair when either configured clan field is missing or invalid.
ClanTag ParseClanTag(const nlohmann::json& value);

} // namespace cs2bh
