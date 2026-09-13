#include "core/gameconfig.h"
#include <cstdio>
#include <fstream>
#include <vector>
namespace cs2bh::gameconfig {
namespace {
// Writes a configuration or signature-resolution error to the caller.
void SetError(char* out, size_t outLen, const char* fmt, const char* a, const char* b = nullptr)
{
    if (!out || outLen == 0) return;
    if (b) std::snprintf(out, outLen, fmt, a, b);
    else
        std::snprintf(out, outLen, fmt, a);
}
} // namespace
// Loads an object-valued JSON document and reports file or parse failures.
bool LoadGamedata(const char* path, nlohmann::json& out)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return false;
    try
    {
        out = nlohmann::json::parse(ifs);
    }
    catch (...)
    {
        return false;
    }
    return out.is_object();
}
// Returns the gamedata key for the current operating system.
const char* PlatformName()
{
#ifdef _WIN32
    return "windows";
#else
    return "linux";
#endif
}
// Returns a platform signature or an empty string when unavailable.
std::string FindPlatformSig(const nlohmann::json& gamedata, const std::string& name)
{
    auto it = gamedata.find(name);
    if (it == gamedata.end() || !it->is_object()) return "";
    auto sigIt = it->find("signatures");
    if (sigIt == it->end() || !sigIt->is_object()) return "";
    auto platformIt = sigIt->find(PlatformName());
    if (platformIt == sigIt->end() || !platformIt->is_string()) return "";
    return platformIt->get<std::string>();
}
// Returns a platform offset, retaining the caller's fallback for missing entries.
int FindPlatformOffset(const nlohmann::json& gamedata, const std::string& name, int fallback)
{
    auto it = gamedata.find(name);
    if (it == gamedata.end() || !it->is_object()) return fallback;
    auto offIt = it->find("offsets");
    if (offIt == it->end() || !offIt->is_object()) return fallback;
    auto platformIt = offIt->find(PlatformName());
    if (platformIt == offIt->end() || !platformIt->is_number_integer()) return fallback;
    return platformIt->get<int>();
}
// Resolves a named signature using the module scanner.
void* ResolveSig(const nlohmann::json& gamedata, const modules::ModuleInfo& module, const char* name, char* errorOut, size_t errorOutLen)
{
    std::string sig = FindPlatformSig(gamedata, name);
    if (sig.empty())
    {
        SetError(errorOut, errorOutLen, "gamedata missing '%s.signatures.%s'", name, PlatformName());
        return nullptr;
    }
    std::vector<uint8_t> bytes;
    std::vector<bool> wild;
    if (!modules::ParseSigString(sig, bytes, wild))
    {
        SetError(errorOut, errorOutLen, "failed to parse '%s' sig: '%s'", name, sig.c_str());
        return nullptr;
    }
    void* addr = modules::FindPatternIn(module, bytes, wild);
    if (!addr)
    {
        SetError(errorOut, errorOutLen, "sig '%s' not found in target module", name);
        return nullptr;
    }
    return addr;
}
} // namespace cs2bh::gameconfig
