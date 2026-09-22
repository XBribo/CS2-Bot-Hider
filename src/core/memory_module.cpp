// Module lookup and signature scanning.

#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include "core/memory_module.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifdef _M_AMD64
#ifndef _AMD64_
#define _AMD64_ // NOLINT(bugprone-reserved-identifier)
#endif
#endif
#include <libloaderapi.h>
#include <processthreadsapi.h>
#include <psapi.h>
#include <winnt.h>
#else
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <tier0/platform.h>
#include "metamod_oslink.h"

namespace cs2bh::modules {
namespace {
#ifdef _WIN32
// Resolves the complete mapped PE image for one loaded module.
ModuleInfo ModuleFromHandle(HINSTANCE handle)
{
    ModuleInfo out;
    if (!handle) return out;

    MODULEINFO moduleInfo{};
    if (!GetModuleInformation(GetCurrentProcess(), handle, &moduleInfo, sizeof(moduleInfo))) return out;

    out.base = static_cast<unsigned char*>(moduleInfo.lpBaseOfDll);
    out.size = static_cast<size_t>(moduleInfo.SizeOfImage);
    out.segments.push_back({ .base = out.base, .size = out.size });
    return out;
}

// Resolves the executable PE section used by code-only signature scans.
ModuleInfo ModuleCodeFromHandle(HINSTANCE handle)
{
    ModuleInfo out;
    if (!handle) return out;

    const ModuleInfo image = ModuleFromHandle(handle);
    if (!image) return out;

    auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(image.base);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0) return out;

    auto* ntHeader = reinterpret_cast<IMAGE_NT_HEADERS*>(image.base + dosHeader->e_lfanew);
    if (ntHeader->Signature != IMAGE_NT_SIGNATURE) return out;

    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(ntHeader);
    for (WORD i = 0; i < ntHeader->FileHeader.NumberOfSections; ++i, ++section)
    {
        char name[IMAGE_SIZEOF_SHORT_NAME + 1] = {};
        std::memcpy(name, section->Name, IMAGE_SIZEOF_SHORT_NAME);
        if (std::strcmp(name, ".text") != 0) continue;

        const size_t sectionSize = static_cast<size_t>(section->Misc.VirtualSize);
        const size_t sectionOffset = static_cast<size_t>(section->VirtualAddress);
        if (sectionSize == 0 || sectionOffset >= image.size || sectionSize > image.size - sectionOffset) return out;

        out.base = image.base;
        out.size = image.size;
        out.segments.push_back({ .base = image.base + sectionOffset, .size = sectionSize });
        return out;
    }
    return out;
}
#else
// Adds one mapped ELF segment and updates the module bounds.
void AddSegment(ModuleInfo& module, uintptr_t address, size_t size)
{
    if (size == 0) return;

    auto* base = reinterpret_cast<unsigned char*>(address);
    module.segments.push_back({ .base = base, .size = size });

    if (!module.base || address < reinterpret_cast<uintptr_t>(module.base)) module.base = base;

    const uintptr_t end = address + size;
    const uintptr_t currentEnd = reinterpret_cast<uintptr_t>(module.base) + module.size;
    if (end > currentEnd) module.size = static_cast<size_t>(end - reinterpret_cast<uintptr_t>(module.base));
}

// Resolves ELF load segments directly from the handle's link_map.
bool FillModuleFromHandle(HINSTANCE handle, ModuleInfo& image, ModuleInfo& code)
{
    if (!handle) return false;

    link_map* linkMap = nullptr;
    if (dlinfo(handle, RTLD_DI_LINKMAP, &linkMap) != 0 || !linkMap || !linkMap->l_name || !linkMap->l_name[0]) return false;

    const int fileDescriptor = open(linkMap->l_name, O_RDONLY);
    if (fileDescriptor == -1) return false;

    struct stat fileStatus{};
    if (fstat(fileDescriptor, &fileStatus) != 0 || fileStatus.st_size <= 0)
    {
        close(fileDescriptor);
        return false;
    }

    const size_t fileSize = static_cast<size_t>(fileStatus.st_size);
    void* mappedFile = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fileDescriptor, 0);
    if (mappedFile == MAP_FAILED)
    {
        close(fileDescriptor);
        return false;
    }

    if (fileSize < sizeof(ElfW(Ehdr)))
    {
        munmap(mappedFile, fileSize);
        close(fileDescriptor);
        return false;
    }

    auto* elfHeader = static_cast<ElfW(Ehdr)*>(mappedFile);
    const size_t programHeaderOffset = static_cast<size_t>(elfHeader->e_phoff);
    const size_t programHeaderSize = static_cast<size_t>(elfHeader->e_phnum) * elfHeader->e_phentsize;
    const bool validElf = std::memcmp(elfHeader->e_ident, ELFMAG, SELFMAG) == 0 &&
                          elfHeader->e_ident[EI_CLASS] == ELFCLASS64 &&
                          programHeaderOffset <= fileSize && programHeaderSize <= fileSize - programHeaderOffset;
    if (!validElf)
    {
        munmap(mappedFile, fileSize);
        close(fileDescriptor);
        return false;
    }

    auto* programHeaders = reinterpret_cast<ElfW(Phdr)*>(static_cast<unsigned char*>(mappedFile) + programHeaderOffset);
    for (int i = 0; i < elfHeader->e_phnum; ++i)
    {
        const ElfW(Phdr)& programHeader = programHeaders[i];
        if (programHeader.p_type != PT_LOAD || programHeader.p_memsz == 0) continue;

        const uintptr_t address = static_cast<uintptr_t>(linkMap->l_addr + programHeader.p_vaddr);
        const size_t size = static_cast<size_t>(programHeader.p_memsz);
        AddSegment(image, address, size);
        if ((programHeader.p_flags & PF_X) != 0) AddSegment(code, address, size);
    }

    munmap(mappedFile, fileSize);
    close(fileDescriptor);
    return static_cast<bool>(image);
}
#endif
} // namespace

CModule::CModule(const char* relativeDirectory, const char* moduleName)
{
    if (!relativeDirectory || !moduleName || !moduleName[0]) return;

    const char* gameDirectory = Plat_GetGameDirectory();
    if (!gameDirectory || !gameDirectory[0]) return;

    m_path = std::string(gameDirectory) + relativeDirectory + kModulePrefix + moduleName + kModuleExtension;
    m_hModule = dlmount(m_path.c_str());
    if (!m_hModule) return;

#ifdef _WIN32
    m_image = ModuleFromHandle(reinterpret_cast<HMODULE>(m_hModule));
    m_code = ModuleCodeFromHandle(reinterpret_cast<HMODULE>(m_hModule));
#else
    if (!FillModuleFromHandle(static_cast<HINSTANCE>(m_hModule), m_image, m_code))
    {
        dlclose(m_hModule);
        m_hModule = nullptr;
    }
#endif
}

CModule* engine = nullptr;
CModule* server = nullptr;

void Initialize()
{
    if (!engine) engine = new CModule(kRootBin, "engine2");
    if (!server) server = new CModule(kGameBin, "server");
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
        const uint64_t value = std::strtoull(p, &end, 16);
        if (end == p || end - p > 2 || value > 0xFF) return false;
        outBytes.push_back(static_cast<uint8_t>(value));
        outWild.push_back(false);
        p = end;
    }
    return !outBytes.empty();
}

void* FindPatternIn(const ModuleInfo& module, const std::vector<uint8_t>& pattern, const std::vector<bool>& wild)
{
    if (!module || pattern.empty() || pattern.size() != wild.size()) return nullptr;

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
            if (match) return segment.base + i;
        }
    }
    return nullptr;
}

// Finds every pattern match across the selected module segments.
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

} // namespace cs2bh::modules
