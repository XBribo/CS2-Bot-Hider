#pragma once
#include "core/memory_module.h"
#include <nlohmann/json.hpp>
namespace cs2bh::gameconfig {
// Loads an object-valued JSON file.
bool LoadGamedata(const char* path, nlohmann::json& out);
// Returns the platform-specific signature for an entry.
std::string FindPlatformSig(const nlohmann::json& gamedata, const std::string& name);
// Returns the platform-specific offset or the supplied fallback.
int FindPlatformOffset(const nlohmann::json& gamedata, const std::string& name, int fallback);
// Returns the current platform's gamedata key.
const char* PlatformName();
// Resolves a named signature and reports errors to the caller.
void* ResolveSig(const nlohmann::json& gamedata, const modules::ModuleInfo& module, const char* name, char* errorOut, size_t errorOutLen);
} // namespace cs2bh::gameconfig
