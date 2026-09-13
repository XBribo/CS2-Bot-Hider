#include "utils/platform.h"

#ifdef _WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace cs2bh::platform {
// Resolves the on-disk path of the module containing this function.
std::string SelfModulePath()
{
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&SelfModulePath), &module))
        return "";
    char path[MAX_PATH] = { 0 };
    const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return "";
    return std::string(path, length);
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&SelfModulePath), &info) == 0 || !info.dli_fname) return "";
    return std::string(info.dli_fname);
#endif
}
} // namespace cs2bh::platform
