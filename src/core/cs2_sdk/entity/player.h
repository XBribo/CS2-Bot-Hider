#pragma once

#include "core/cs2_sdk/schema.h"
#include <cstdint>

namespace cs2bh::sdk {
// Resolves a typed field only when both the entity and its Schema entry exist.
template <typename T> T* Field(void* entity, const char* className, const char* fieldName)
{
    const int offset = schema::GetFieldOffset(className, fieldName);
    if (!entity || offset < 0) return nullptr;
    return reinterpret_cast<T*>(static_cast<unsigned char*>(entity) + offset);
}

// Returns the controller's native pawn handle without assuming a class layout.
inline const uint32_t* PawnHandle(void* controller) { return Field<uint32_t>(controller, "CBasePlayerController", "m_hPawn"); }

// Returns the pawn's idle timer without marking this server-only write networked.
inline float* IdleTime(void* pawn) { return Field<float>(pawn, "CCSPlayerPawnBase", "m_flIdleTimeSinceLastAction"); }
} // namespace cs2bh::sdk
