// sig_scan.cpp
//
// Signature scanning + gamedata.json loader.

#include "sig_scan.h"

#if defined(_WIN32)
#include <Windows.h>
#include <psapi.h>
#else
#include <dlfcn.h>
#include <link.h>
#include <strings.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace cs2bh::sig {
namespace {
const char* BaseName(const char* path)
{
    if (!path) return "";
    const char* slash = std::strrchr(path, '/');
    const char* backslash = std::strrchr(path, '\\');
    const char* base = slash && backslash ? std::max(slash, backslash) : (slash ? slash : backslash);
    return base ? base + 1 : path;
}

void SetError(char* out, size_t outLen, const char* fmt, const char* a, const char* b = nullptr)
{
    if (!out || outLen == 0) return;
    if (b) std::snprintf(out, outLen, fmt, a, b);
    else
        std::snprintf(out, outLen, fmt, a);
}

#if defined(_WIN32)
ModuleInfo ModuleFromHandle(HMODULE handle)
{
    ModuleInfo out;
    if (!handle) return out;

    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), handle, &mi, sizeof(mi))) return out;

    out.base = static_cast<unsigned char*>(mi.lpBaseOfDll);
    out.size = static_cast<size_t>(mi.SizeOfImage);
    out.segments.push_back({ out.base, out.size });
    return out;
}

// Resolves one mapped PE section and retains the full image bounds
ModuleInfo ModuleSectionFromHandle(HMODULE handle, const char* sectionName)
{
    ModuleInfo out;
    if (!handle || !sectionName) return out;

    ModuleInfo image = ModuleFromHandle(handle);
    if (!image) return out;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return out;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image.base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return out;

    const size_t nameLength = std::strlen(sectionName);
    if (nameLength == 0 || nameLength > IMAGE_SIZEOF_SHORT_NAME) return out;

    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
    {
        char name[IMAGE_SIZEOF_SHORT_NAME + 1] = {};
        std::memcpy(name, section->Name, IMAGE_SIZEOF_SHORT_NAME);
        if (std::strcmp(name, sectionName) != 0) continue;

        size_t sectionSize = static_cast<size_t>(section->Misc.VirtualSize);
        size_t sectionOffset = static_cast<size_t>(section->VirtualAddress);
        if (sectionSize == 0 || sectionOffset >= image.size || sectionSize > image.size - sectionOffset) return out;

        out.base = image.base;
        out.size = image.size;
        out.segments.push_back({ image.base + sectionOffset, sectionSize });
        return out;
    }
    return out;
}
#else
bool NameMatches(const char* loadedPath, const char* moduleName)
{
    if (!loadedPath || !loadedPath[0] || !moduleName || !moduleName[0]) return false;
    const char* loadedBase = BaseName(loadedPath);
    const char* wantBase = BaseName(moduleName);
    return std::strcmp(loadedBase, wantBase) == 0;
}

void FillModuleFromPhdr(dl_phdr_info* info, ModuleInfo& out)
{
    uintptr_t minAddr = UINTPTR_MAX;
    uintptr_t maxAddr = 0;
    out.segments.clear();

    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr) & ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        auto* segBase = reinterpret_cast<unsigned char*>(info->dlpi_addr + ph.p_vaddr);
        size_t segSize = static_cast<size_t>(ph.p_memsz);
        out.segments.push_back({ segBase, segSize });

        uintptr_t start = reinterpret_cast<uintptr_t>(segBase);
        uintptr_t end = start + segSize;
        minAddr = std::min(minAddr, start);
        maxAddr = std::max(maxAddr, end);
    }

    if (minAddr != UINTPTR_MAX && maxAddr > minAddr)
    {
        out.base = reinterpret_cast<unsigned char*>(minAddr);
        out.size = static_cast<size_t>(maxAddr - minAddr);
    }
}

// Collects executable load segments from one ELF module
void FillCodeModuleFromPhdr(dl_phdr_info* info, ModuleInfo& out)
{
    FillModuleFromPhdr(info, out);
    out.segments.clear();
    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr) & ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0 || (ph.p_flags & PF_X) == 0) continue;

        auto* segmentBase = reinterpret_cast<unsigned char*>(info->dlpi_addr + ph.p_vaddr);
        out.segments.push_back({ segmentBase, static_cast<size_t>(ph.p_memsz) });
    }
    if (out.segments.empty()) out = {};
}

struct FindByNameCtx
{
    const char* name = nullptr;
    ModuleInfo Result;
};

int FindByNameCallback(dl_phdr_info* info, size_t, void* data)
{
    auto* ctx = static_cast<FindByNameCtx*>(data);
    if (!NameMatches(info->dlpi_name, ctx->name)) return 0;

    FillModuleFromPhdr(info, ctx->Result);
    return ctx->Result ? 1 : 0;
}

// Resolves executable segments for a module basename
int FindCodeByNameCallback(dl_phdr_info* info, size_t, void* data)
{
    auto* ctx = static_cast<FindByNameCtx*>(data);
    if (!NameMatches(info->dlpi_name, ctx->name)) return 0;

    FillCodeModuleFromPhdr(info, ctx->Result);
    return ctx->Result ? 1 : 0;
}

struct FindByAddressCtx
{
    uintptr_t Address = 0;
    ModuleInfo Result;
};

int FindByAddressCallback(dl_phdr_info* info, size_t, void* data)
{
    auto* ctx = static_cast<FindByAddressCtx*>(data);
    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr) & ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        uintptr_t start = info->dlpi_addr + ph.p_vaddr;
        uintptr_t end = start + ph.p_memsz;
        if (ctx->Address >= start && ctx->Address < end)
        {
            FillModuleFromPhdr(info, ctx->Result);
            return ctx->Result ? 1 : 0;
        }
    }
    return 0;
}
#endif
} // namespace

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

const char* PlatformName()
{
#if defined(_WIN32)
    return "windows";
#else
    return "linux";
#endif
}

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

// Read gamedata[name].offsets[platform]; fall back if entry missing/non-integer
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

bool ParseSigString(const std::string& sigStr, std::vector<uint8_t>& outBytes, std::vector<bool>& outWild)
{
    outBytes.clear();
    outWild.clear();
    const char* p = sigStr.c_str();
    while (*p)
    {
        if (*p == ' ')
        {
            ++p;
            continue;
        }
        if (*p == '?')
        {
            outBytes.push_back(0);
            outWild.push_back(true);
            ++p;
            if (*p == '?') ++p;
            continue;
        }
        char* end = nullptr;
        unsigned long v = std::strtoul(p, &end, 16);
        if (end == p || end - p > 2 || v > 0xFF) return false;
        outBytes.push_back(static_cast<uint8_t>(v));
        outWild.push_back(false);
        p = end;
    }
    return !outBytes.empty();
}

void* FindPatternIn(const ModuleInfo& module, const std::vector<uint8_t>& pattern, const std::vector<bool>& wild)
{
    if (!module || pattern.empty() || pattern.size() != wild.size()) return nullptr;

    const size_t plen = pattern.size();
    for (const ModuleSegment& segment : module.segments)
    {
        if (!segment.base || segment.size < plen) continue;

        for (size_t i = 0; i + plen <= segment.size; ++i)
        {
            bool match = true;
            for (size_t j = 0; j < plen; ++j)
            {
                if (!wild[j] && segment.base[i + j] != pattern[j])
                {
                    match = false;
                    break;
                }
            }
            if (match) return segment.base + i;
        }
    }
    return nullptr;
}

// Finds every pattern match in the selected module segments
std::vector<void*> FindPatternMatchesIn(const ModuleInfo& module, const std::vector<uint8_t>& pattern, const std::vector<bool>& wild)
{
    std::vector<void*> matches;
    if (!module || pattern.empty() || pattern.size() != wild.size()) return matches;

    const size_t patternLength = pattern.size();
    for (const ModuleSegment& segment : module.segments)
    {
        if (!segment.base || segment.size < patternLength) continue;

        for (size_t i = 0; i + patternLength <= segment.size; ++i)
        {
            bool match = true;
            for (size_t j = 0; j < patternLength; ++j)
            {
                if (!wild[j] && segment.base[i + j] != pattern[j])
                {
                    match = false;
                    break;
                }
            }
            if (match) matches.push_back(segment.base + i);
        }
    }
    return matches;
}

ModuleInfo ModuleFromName(const char* moduleName)
{
#if defined(_WIN32)
    return ModuleFromHandle(GetModuleHandleA(moduleName));
#else
    FindByNameCtx ctx{};
    ctx.name = moduleName;
    dl_iterate_phdr(FindByNameCallback, &ctx);
    return ctx.Result;
#endif
}

// Resolves executable code ranges from a loaded module
ModuleInfo ModuleCodeFromName(const char* moduleName)
{
#if defined(_WIN32)
    return ModuleSectionFromHandle(GetModuleHandleA(moduleName), ".text");
#else
    FindByNameCtx ctx{};
    ctx.name = moduleName;
    dl_iterate_phdr(FindCodeByNameCallback, &ctx);
    return ctx.Result;
#endif
}

ModuleInfo ModuleFromInterfacePtr(void* interfacePtr)
{
    if (!interfacePtr) return {};
    void* vtable = *reinterpret_cast<void**>(interfacePtr);
    if (!vtable) return {};

#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(vtable, &mbi, sizeof(mbi))) return {};
    if (mbi.Type != MEM_IMAGE) return {};
    return ModuleFromHandle(reinterpret_cast<HMODULE>(mbi.AllocationBase));
#else
    FindByAddressCtx ctx{};
    ctx.Address = reinterpret_cast<uintptr_t>(vtable);
    dl_iterate_phdr(FindByAddressCallback, &ctx);
    return ctx.Result;
#endif
}

void* ResolveSig(const nlohmann::json& gamedata, const ModuleInfo& module, const char* name, char* errorOut, size_t errorOutLen)
{
    std::string sig = FindPlatformSig(gamedata, name);
    if (sig.empty())
    {
        SetError(errorOut, errorOutLen, "gamedata missing '%s.signatures.%s'", name, PlatformName());
        return nullptr;
    }
    std::vector<uint8_t> bytes;
    std::vector<bool> wild;
    if (!ParseSigString(sig, bytes, wild))
    {
        SetError(errorOut, errorOutLen, "failed to parse '%s' sig: '%s'", name, sig.c_str());
        return nullptr;
    }
    void* addr = FindPatternIn(module, bytes, wild);
    if (!addr)
    {
        SetError(errorOut, errorOutLen, "sig '%s' not found in target module", name);
        return nullptr;
    }
    return addr;
}
} // namespace cs2bh::sig
