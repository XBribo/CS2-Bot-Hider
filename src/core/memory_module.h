#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cs2bh::modules {
#ifdef _WIN32
inline constexpr const char* kRootBin = "/bin/win64/";
inline constexpr const char* kGameBin = "/csgo/bin/win64/";
inline constexpr const char* kModulePrefix = "";
inline constexpr const char* kModuleExtension = ".dll";
#else
inline constexpr const char* kRootBin = "/bin/linuxsteamrt64/";
inline constexpr const char* kGameBin = "/csgo/bin/linuxsteamrt64/";
inline constexpr const char* kModulePrefix = "lib";
inline constexpr const char* kModuleExtension = ".so";
#endif

struct ModuleSegment
{
    unsigned char* base = nullptr;
    size_t size = 0;
};
struct ModuleInfo
{
    unsigned char* base = nullptr;
    size_t size = 0;
    std::vector<ModuleSegment> segments;
    // Reports whether the module has usable image bounds.
    explicit operator bool() const { return base != nullptr && size != 0; }
};

class CModule
{
  public:
    // Loads one game module from its explicit game-relative directory.
    CModule(const char* relativeDirectory, const char* moduleName);

    // Reports whether the module handle and mapped image are usable.
    explicit operator bool() const { return static_cast<bool>(m_image); }

    // Returns all mapped load segments used by general signature scans.
    const ModuleInfo& Image() const { return m_image; }

    // Returns only executable load segments used by code-only scans.
    const ModuleInfo& Code() const { return m_code; }

    // Returns the exact path passed to the platform loader.
    const char* Path() const { return m_path.c_str(); }

  private:
    std::string m_path;
    void* m_hModule = nullptr;
    ModuleInfo m_image;
    ModuleInfo m_code;
};

extern CModule* engine;
extern CModule* server;

// Loads the engine and server modules once for signature resolution.
void Initialize();

// Parses hexadecimal bytes and wildcard markers.
bool ParseSigString(const std::string& signature, std::vector<uint8_t>& bytes, std::vector<bool>& wildcards);
// Finds the first pattern match across the module segments.
void* FindPatternIn(const ModuleInfo& module, const std::vector<uint8_t>& pattern, const std::vector<bool>& wildcards);
// Finds all pattern matches across the module segments.
std::vector<void*> FindPatternMatchesIn(const ModuleInfo& module, const std::vector<uint8_t>& pattern, const std::vector<bool>& wildcards);
} // namespace cs2bh::modules
