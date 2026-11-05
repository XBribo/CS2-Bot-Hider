#pragma once

#include "core/cs2_sdk/src/core/schema.h"

namespace cs2bh::sdk {
// Resolve a typed entity field through Schema.
template <typename T> T* Field(void* entity, const char* className, const char* fieldName)
{
    const int offset = cs2_sdk::schema::GetFieldOffset(className, fieldName);
    if (!entity || offset < 0) return nullptr;
    return reinterpret_cast<T*>(static_cast<unsigned char*>(entity) + offset);
}

// Access the pawn's server-side idle timer.
inline float* IdleTime(void* pawn) { return Field<float>(pawn, "CCSPlayerPawnBase", "m_flIdleTimeSinceLastAction"); }
} // namespace cs2bh::sdk
