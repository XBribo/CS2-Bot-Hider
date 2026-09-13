#pragma once

#include <string>

namespace cs2bh::platform {
// Returns the path of this plugin module, or empty on failure.
std::string SelfModulePath();
} // namespace cs2bh::platform
