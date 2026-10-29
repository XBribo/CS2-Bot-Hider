#pragma once

#include <cstddef>

namespace cs2bh::gamedata {
// Resolve engine targets and prepare identity hooks.
bool Prepare(char* error, size_t maxlen);
} // namespace cs2bh::gamedata
