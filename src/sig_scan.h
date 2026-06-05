// sig_scan.h
//
// Reusable signature-scanning + gamedata.json loader for CS2 Metamod plugins.
// Adapted from CS2-Bot-Locker/src/sig_scan.*; parsing uses nlohmann/json so the
// gamedata schema stays consistent with the project's other JSON config.

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cs2bh::sig
{
    struct ModuleSegment
    {
        unsigned char *Base = nullptr;
        size_t Size = 0;
    };

    struct ModuleInfo
    {
        unsigned char *Base = nullptr;
        size_t Size = 0;
        std::vector<ModuleSegment> Segments;

        explicit operator bool() const { return Base != nullptr && Size != 0; }
    };

    // * Read + parse gamedata.json into `out`. Returns false on open/parse error
    bool LoadGamedata(const char *path, nlohmann::json &out);

    // * Look up "<name>.signatures.<current-platform>" in parsed gamedata. Empty if absent
    std::string FindPlatformSig(const nlohmann::json &gamedata, const std::string &name);

    const char *PlatformName();

    // * "48 85 ? AB" -> raw bytes + per-byte wildcard mask. False on malformed input
    bool ParseSigString(const std::string &sigStr,
                        std::vector<uint8_t> &outBytes,
                        std::vector<bool> &outWild);

    // * Wildcard scan over a module's loaded image. Returns first match or nullptr
    void *FindPatternIn(const ModuleInfo &module,
                        const std::vector<uint8_t> &pattern,
                        const std::vector<bool> &wild);

    // * Resolve a module by basename, e.g. server.dll or libserver.so
    ModuleInfo ModuleFromName(const char *moduleName);

    // * Resolve the real CS2 server module past Metamod's shim by reading the vtable
    //   of any server-side interface pointer (the vtable lives in the real module).
    ModuleInfo ModuleFromInterfacePtr(void *interfacePtr);

    // * One-shot: fetch sig by name from gamedata, parse it, scan `module`.
    //   Returns match address or nullptr; writes a reason into errorOut on failure
    void *ResolveSig(const nlohmann::json &gamedata, const ModuleInfo &module,
                     const char *name, char *errorOut, size_t errorOutLen);
}
