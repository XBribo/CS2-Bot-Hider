// custom_crosshair.h
// Low-level Windows HUD crosshair hook and target-map API.

#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

namespace cs2bh::crosshair
{
    struct PaintConfigOverride
    {
        int32_t size;
        int32_t style;
        int32_t color;
        int32_t drawOutline;
        int32_t dot;
        int32_t gapUseWeaponValue;
        int32_t useAlpha;
        int32_t tStyle;
        int32_t gap100;
        int32_t size100;
        int32_t thickness100;
        int32_t outline100;
        int32_t alpha;
        int32_t red;
        int32_t green;
        int32_t blue;
    };

    static_assert(sizeof(PaintConfigOverride) == 64);

    bool Install(const nlohmann::json &gamedata);
    void Shutdown();

    int SetOverride(int slot, int pawnEntityIndex, int weaponEntityIndex,
                    const PaintConfigOverride *config, int size);
    int ClearOverride(int slot);
    int ClearOverrides();
} // namespace cs2bh::crosshair

#if defined(_WIN32)
extern "C" __declspec(dllexport) int BotHider_SetCustomCrosshair(
    int slot,
    int pawnEntityIndex,
    int weaponEntityIndex,
    const cs2bh::crosshair::PaintConfigOverride *config,
    int size);

extern "C" __declspec(dllexport) int BotHider_ClearCustomCrosshair(int slot);
extern "C" __declspec(dllexport) int BotHider_ClearCustomCrosshairs();
#endif
