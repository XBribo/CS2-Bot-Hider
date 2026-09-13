#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace cs2bh::modules {
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
// Parses hexadecimal bytes and wildcard markers.
bool ParseSigString(const std::string& signature, std::vector<uint8_t>& bytes, std::vector<bool>& wildcards);
// Finds the first pattern match across the module segments.
void* FindPatternIn(const ModuleInfo& module, const std::vector<uint8_t>& pattern, const std::vector<bool>& wildcards);
// Resolves a loaded module by its basename.
ModuleInfo ModuleFromName(const char* moduleName);
// Resolves a loaded module from an interface's vtable.
ModuleInfo ModuleFromInterfacePtr(void* interfacePtr);
// Resolves only executable code segments.
ModuleInfo ModuleCodeFromName(const char* moduleName);
// Finds all pattern matches across the module segments.
std::vector<void*> FindPatternMatchesIn(const ModuleInfo& module, const std::vector<uint8_t>& pattern, const std::vector<bool>& wildcards);
} // namespace cs2bh::modules
