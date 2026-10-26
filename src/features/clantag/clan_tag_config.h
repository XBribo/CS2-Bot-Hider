// Clan fields from bot_info.json.

#pragma once

#include "clan_tag.h"

#include <nlohmann/json.hpp>

namespace cs2bh {

// Parse a clan pair; return empty for missing or invalid fields.
ClanTag ParseClanTag(const nlohmann::json& value);

} // namespace cs2bh
