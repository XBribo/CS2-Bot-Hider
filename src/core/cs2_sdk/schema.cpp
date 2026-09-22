// Resolves registered field offsets from the live ISchemaSystem at runtime.

#include "core/cs2_sdk/schema.h"
#include "core/interfaces.h"
#include "schemasystem/schematypes.h"
#include "offsets.h"

#include <schemasystem/schemasystem.h>

#include <string>
#include <unordered_map>

namespace cs2bh::schema {
namespace {
ISchemaSystem* g_schema = nullptr;
using FieldMap = std::unordered_map<std::string, int>;
std::unordered_map<std::string, FieldMap> g_classCache; // NOLINT(bugprone-throwing-static-initialization)
} // namespace

bool Init()
{
    if (g_schema) return true;
    g_schema = g_schemaSystem;
    return g_schema != nullptr;
}

namespace {

CSchemaClassInfo* FindClass(const char* className)
{
    static const char* kScopes[] = {
        offsets::kSchemaServerTypeScope,
        "server.dll",
        "libserver.so",
    };

    for (const char* scopeName : kScopes)
    {
        if (auto* scope = g_schema->FindTypeScopeForModule(scopeName, nullptr))
        {
            if (auto* info = scope->FindDeclaredClass(className).Get()) return info;
        }
    }

    if (auto* scope = g_schema->GlobalTypeScope())
    {
        if (auto* info = scope->FindDeclaredClass(className).Get()) return info;
    }
    return nullptr;
}

} // namespace

// Caches all declared fields for a class without treating a miss as offset zero.
int GetFieldOffset(const char* className, const char* fieldName)
{
    if (!className || !fieldName || !g_schema) return -1;
    auto table = g_classCache.find(className);
    if (table == g_classCache.end())
    {
        CSchemaClassInfo* info = FindClass(className);
        if (!info) return -1;
        FieldMap fields;
        for (uint16 i = 0; i < info->m_nFieldCount; ++i)
        {
            const auto& field = info->m_pFields[i];
            if (field.m_pszName) fields.emplace(field.m_pszName, field.m_nSingleInheritanceOffset);
        }
        table = g_classCache.emplace(className, std::move(fields)).first;
    }
    const auto field = table->second.find(fieldName);
    return field == table->second.end() ? -1 : field->second;
}

// Invalidates module pointers and field caches across plugin reloads.
void Reset()
{
    g_classCache.clear();
    g_schema = nullptr;
}

} // namespace cs2bh::schema
