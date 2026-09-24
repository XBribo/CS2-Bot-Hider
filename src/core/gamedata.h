#pragma once
#include <nlohmann/json.hpp>
namespace cs2bh::gamedata {
// Loads native targets and prepares identity hooks without installing them.
void Prepare();
const nlohmann::json& Data();
} // namespace cs2bh::gamedata
