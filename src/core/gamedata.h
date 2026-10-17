#pragma once

#include <string>

namespace cs2bh::gamedata {
// Computes <plugin>/gamedata.json from <plugin>/bin/<platform>/<binary>.
// Does not check whether the file exists.
std::string ComputePath();

// Resolves engine targets and prepares identity hooks without installing them.
void Prepare();
} // namespace cs2bh::gamedata
