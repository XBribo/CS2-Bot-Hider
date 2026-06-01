// sig_scan.cpp
//
// Signature scanning + gamedata.json loader. Module-resolution logic mirrors
// CS2-Bot-Locker/src/sig_scan.cpp (ModuleFromInterfacePtr walks past Metamod's
// shim to the real server.dll); JSON parsing uses nlohmann/json.

#include "sig_scan.h"

#include <psapi.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace cs2bh::sig
{
    // Read + parse gamedata.json. False on open or parse failure
    bool LoadGamedata(const char *path, nlohmann::json &out)
    {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open())
            return false;
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

    // Pull "<name>.signatures.windows" out of parsed gamedata
    std::string FindWindowsSig(const nlohmann::json &gamedata, const std::string &name)
    {
        auto it = gamedata.find(name);
        if (it == gamedata.end() || !it->is_object())
            return "";
        auto sigIt = it->find("signatures");
        if (sigIt == it->end() || !sigIt->is_object())
            return "";
        auto winIt = sigIt->find("windows");
        if (winIt == sigIt->end() || !winIt->is_string())
            return "";
        return winIt->get<std::string>();
    }

    // "48 85 ? AB" -> bytes + wildcard mask. False on malformed token
    bool ParseSigString(const std::string &sigStr,
                        std::vector<uint8_t> &outBytes,
                        std::vector<bool> &outWild)
    {
        outBytes.clear();
        outWild.clear();
        const char *p = sigStr.c_str();
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
                if (*p == '?') // accept "??"
                    ++p;
                continue;
            }
            char *end = nullptr;
            unsigned long v = std::strtoul(p, &end, 16);
            // A valid hex byte consumes 1-2 chars and stays within 0xFF
            if (end == p || end - p > 2 || v > 0xFF)
                return false;
            outBytes.push_back(static_cast<uint8_t>(v));
            outWild.push_back(false);
            p = end;
        }
        return !outBytes.empty();
    }

    // Linear wildcard scan over the module's mapped image
    void *FindPatternIn(HMODULE module,
                        const std::vector<uint8_t> &pattern,
                        const std::vector<bool> &wild)
    {
        if (!module || pattern.empty())
            return nullptr;
        MODULEINFO mi{};
        if (!GetModuleInformation(GetCurrentProcess(), module, &mi, sizeof(mi)))
            return nullptr;
        auto base = static_cast<unsigned char *>(mi.lpBaseOfDll);
        const size_t size = mi.SizeOfImage;
        const size_t plen = pattern.size();
        if (size < plen)
            return nullptr;
        for (size_t i = 0; i + plen <= size; ++i)
        {
            bool match = true;
            for (size_t j = 0; j < plen; ++j)
            {
                if (!wild[j] && base[i + j] != pattern[j])
                {
                    match = false;
                    break;
                }
            }
            if (match)
                return base + i;
        }
        return nullptr;
    }

    // ? Resolve the real server.dll: a server-side interface's vtable is mapped
    //   inside the genuine module, so VirtualQuery on it yields that module's base
    HMODULE ModuleFromInterfacePtr(void *interfacePtr)
    {
        if (!interfacePtr)
            return nullptr;
        void *vtable = *reinterpret_cast<void **>(interfacePtr);
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(vtable, &mbi, sizeof(mbi)))
            return nullptr;
        if (mbi.Type != MEM_IMAGE)
            return nullptr;
        return reinterpret_cast<HMODULE>(mbi.AllocationBase);
    }

    // Fetch sig by name -> parse -> scan. nullptr + reason on any failure
    void *ResolveSig(const nlohmann::json &gamedata, HMODULE module,
                     const char *name, char *errorOut, size_t errorOutLen)
    {
        std::string sig = FindWindowsSig(gamedata, name);
        if (sig.empty())
        {
            std::snprintf(errorOut, errorOutLen,
                          "gamedata missing '%s.signatures.windows'", name);
            return nullptr;
        }
        std::vector<uint8_t> bytes;
        std::vector<bool> wild;
        if (!ParseSigString(sig, bytes, wild))
        {
            std::snprintf(errorOut, errorOutLen,
                          "failed to parse '%s' sig: '%s'", name, sig.c_str());
            return nullptr;
        }
        void *addr = FindPatternIn(module, bytes, wild);
        if (!addr)
        {
            std::snprintf(errorOut, errorOutLen,
                          "sig '%s' not found in server.dll", name);
            return nullptr;
        }
        return addr;
    }
}
