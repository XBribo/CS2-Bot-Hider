// schema_resolver.cpp
//
// Resolves networked field offsets from the live ISchemaSystem at runtime

#include "schema_resolver.h"

#include <schemasystem/schemasystem.h>

#include <Windows.h>

#include <cstring>
#include <string>
#include <unordered_map>

namespace cs2bh::schema
{
    // Local alias for the module CreateInterface export signature
    using CreateIfaceFn = void *(*)(const char *, int *);

    namespace
    {
        ISchemaSystem *g_pSchema = nullptr;
        // Cache "<class>::<field>" -> offset so repeated lookups skip the walk
        std::unordered_map<std::string, int> g_OffsetCache;
    } // namespace

    // Fetch ISchemaSystem via schemasystem.dll's CreateInterface export
    bool Init()
    {
        if (g_pSchema)
            return true;
        HMODULE mod = GetModuleHandleA("schemasystem.dll");
        if (!mod)
            return false;
        auto createIface = reinterpret_cast<CreateInterfaceFn>(
            GetProcAddress(mod, "CreateInterface"));
        if (!createIface)
            return false;
        g_pSchema = reinterpret_cast<ISchemaSystem *>(
            createIface(SCHEMASYSTEM_INTERFACE_VERSION, nullptr));
        return g_pSchema != nullptr;
    }

    // Find a class binding across the relevant type scopes
    static CSchemaClassInfo *FindClass(const char *className)
    {
        if (auto *scope = g_pSchema->FindTypeScopeForModule("server.dll", nullptr))
        {
            if (auto *info = scope->FindDeclaredClass(className).Get())
                return info;
        }
        // Fallback
        if (auto *scope = g_pSchema->GlobalTypeScope())
        {
            if (auto *info = scope->FindDeclaredClass(className).Get())
                return info;
        }
        return nullptr;
    }

    // Walk the class fields for fieldName and return its single-inheritance offset
    int GetFieldOffset(const char *className, const char *fieldName)
    {
        if (!className || !fieldName)
            return -1;
        std::string key = std::string(className) + "::" + fieldName;
        auto it = g_OffsetCache.find(key);
        if (it != g_OffsetCache.end())
            return it->second;
        if (!g_pSchema)
            return -1;

        CSchemaClassInfo *info = FindClass(className);
        if (!info)
            return -1;

        int offset = -1;
        for (uint16 i = 0; i < info->m_nFieldCount; ++i)
        {
            const SchemaClassFieldData_t &f = info->m_pFields[i];
            if (f.m_pszName && std::strcmp(f.m_pszName, fieldName) == 0)
            {
                offset = f.m_nSingleInheritanceOffset;
                break;
            }
        }
        g_OffsetCache[key] = offset;
        return offset;
    }

} // namespace cs2bh::schema
