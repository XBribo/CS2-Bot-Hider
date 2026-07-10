// custom_crosshair.cpp

#include "custom_crosshair.h"

#include "sig_scan.h"

#include <Windows.h>
#include <entity2/entityinstance.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace cs2bh::crosshair
{
    namespace
    {
        constexpr int kMaxSlots = 64;
        constexpr char kBuildPaintConfigSigName[] =
            "CCSGO_HudReticle::BuildPaintConfig";
        constexpr std::array<unsigned char, 8> kExpectedPrologue = {
            0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57};
        constexpr size_t kBuildPaintConfigStealLength = 15;

        void WriteAbsoluteJump(uint8_t *address, const void *destination)
        {
            address[0] = 0xFF;
            address[1] = 0x25;
            *reinterpret_cast<uint32_t *>(address + 2) = 0;
            *reinterpret_cast<uint64_t *>(address + 6) =
                reinterpret_cast<uint64_t>(destination);
        }

        // Kept local to this Windows-only translation unit so the optional HUD
        // feature does not alter BotHider's shared server hook implementation.
        class PreparedInlineHook
        {
        public:
            bool Prepare(void *target, void *detour, size_t stealLength)
            {
                if (m_target || !target || !detour || stealLength < 14 ||
                    stealLength > m_original.size())
                {
                    return false;
                }

                m_trampoline = VirtualAlloc(
                    nullptr, stealLength + 14,
                    MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (!m_trampoline)
                    return false;

                std::memcpy(m_original.data(), target, stealLength);
                std::memcpy(m_trampoline, target, stealLength);
                WriteAbsoluteJump(
                    static_cast<uint8_t *>(m_trampoline) + stealLength,
                    static_cast<uint8_t *>(target) + stealLength);

                m_target = target;
                m_detour = detour;
                m_stealLength = stealLength;
                return true;
            }

            bool Enable()
            {
                if (m_active || !m_target || !m_detour || !m_trampoline)
                    return false;

                DWORD oldProtection = 0;
                if (!VirtualProtect(
                        m_target, m_stealLength, PAGE_EXECUTE_READWRITE,
                        &oldProtection))
                {
                    return false;
                }

                auto *target = static_cast<uint8_t *>(m_target);
                WriteAbsoluteJump(target, m_detour);
                for (size_t i = 14; i < m_stealLength; ++i)
                    target[i] = 0x90;

                VirtualProtect(
                    m_target, m_stealLength, oldProtection, &oldProtection);
                FlushInstructionCache(
                    GetCurrentProcess(), m_target, m_stealLength);
                m_active = true;
                return true;
            }

            void Remove()
            {
                if (!m_trampoline)
                    return;

                if (m_active)
                {
                    DWORD oldProtection = 0;
                    if (VirtualProtect(
                            m_target, m_stealLength, PAGE_EXECUTE_READWRITE,
                            &oldProtection))
                    {
                        std::memcpy(
                            m_target, m_original.data(), m_stealLength);
                        VirtualProtect(
                            m_target, m_stealLength, oldProtection,
                            &oldProtection);
                        FlushInstructionCache(
                            GetCurrentProcess(), m_target, m_stealLength);
                    }
                }

                VirtualFree(m_trampoline, 0, MEM_RELEASE);
                m_target = nullptr;
                m_detour = nullptr;
                m_trampoline = nullptr;
                m_stealLength = 0;
                m_active = false;
            }

            void *Trampoline() const { return m_trampoline; }
            bool Active() const { return m_active; }

        private:
            void *m_target = nullptr;
            void *m_detour = nullptr;
            void *m_trampoline = nullptr;
            size_t m_stealLength = 0;
            std::array<uint8_t, 32> m_original{};
            bool m_active = false;
        };

        struct SlotOverride
        {
            std::atomic<uint32_t> generation{0};
            std::atomic<int> enabled{0};
            std::atomic<int> pawnEntityIndex{-1};
            std::atomic<int> weaponEntityIndex{-1};
            std::atomic<int> style{4};
            std::atomic<int> color{1};
            std::atomic<int> drawOutline{0};
            std::atomic<int> dot{0};
            std::atomic<int> useAlpha{0};
            std::atomic<int> tStyle{0};
            std::atomic<int> gap100{0};
            std::atomic<int> size100{100};
            std::atomic<int> thickness100{50};
            std::atomic<int> outline100{100};
            std::atomic<int> alpha{255};
            std::atomic<int> red{50};
            std::atomic<int> green{250};
            std::atomic<int> blue{50};
        };

        struct PaintValues
        {
            int style = 4;
            int color = 1;
            int drawOutline = 0;
            int dot = 0;
            int useAlpha = 0;
            int tStyle = 0;
            float gap = 0.0f;
            float size = 1.0f;
            float thickness = 0.5f;
            float outlineThickness = 1.0f;
            int alpha = 255;
            int red = 50;
            int green = 250;
            int blue = 50;
        };

#pragma pack(push, 1)
        struct HudPaintConfig
        {
            std::byte unknown00[0x0C];
            float liveGap;
            float smoothGap;
            uint8_t color;
            std::byte unknown15[3];
            int32_t style;
            uint8_t drawOutline;
            uint8_t dot;
            uint8_t recoil;
            uint8_t gapUseWeapon;
            uint8_t useAlpha;
            uint8_t tStyle;
            std::byte unknown22[2];
            float gap;
            std::byte unknown28[4];
            float thickness;
            float size;
            float outlineThickness;
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            uint8_t alpha;
        };
#pragma pack(pop)

        static_assert(offsetof(HudPaintConfig, liveGap) == 0x0C);
        static_assert(offsetof(HudPaintConfig, color) == 0x14);
        static_assert(offsetof(HudPaintConfig, style) == 0x18);
        static_assert(offsetof(HudPaintConfig, gap) == 0x24);
        static_assert(offsetof(HudPaintConfig, thickness) == 0x2C);
        static_assert(offsetof(HudPaintConfig, red) == 0x38);
        static_assert(sizeof(HudPaintConfig) == 0x3C);

        using BuildPaintConfigFn = void(__fastcall *)(void *, void *, void *);

        std::mutex g_hookMutex;
        std::mutex g_slotWriteMutex;
        PreparedInlineHook g_hook;
        std::atomic<BuildPaintConfigFn> g_original{nullptr};
        std::atomic<int> g_available{0};
        std::array<SlotOverride, kMaxSlots> g_slots{};

        bool ValidSlot(int slot)
        {
            return slot >= 0 && slot < kMaxSlots;
        }

        int ClampByte(int value)
        {
            return std::clamp(value, 0, 255);
        }

        int EntityIndex(void *entity)
        {
            if (!entity)
                return -1;
            __try
            {
                return reinterpret_cast<CEntityInstance *>(entity)->GetEntityIndex().Get();
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return -1;
            }
        }

        bool ReadOverrideForEntity(int entityIndex, PaintValues &values)
        {
            if (entityIndex < 0)
                return false;

            for (const auto &entry : g_slots)
            {
                for (int attempt = 0; attempt < 3; ++attempt)
                {
                    const uint32_t before = entry.generation.load(std::memory_order_acquire);
                    if ((before & 1u) != 0)
                        continue;

                    const bool enabled = entry.enabled.load(std::memory_order_relaxed) != 0;
                    const int pawnIndex = entry.pawnEntityIndex.load(std::memory_order_relaxed);
                    const int weaponIndex = entry.weaponEntityIndex.load(std::memory_order_relaxed);
                    const bool matches = enabled &&
                                         (entityIndex == pawnIndex || entityIndex == weaponIndex);

                    PaintValues snapshot;
                    if (matches)
                    {
                        snapshot.style = entry.style.load(std::memory_order_relaxed);
                        snapshot.color = entry.color.load(std::memory_order_relaxed);
                        snapshot.drawOutline = entry.drawOutline.load(std::memory_order_relaxed);
                        snapshot.dot = entry.dot.load(std::memory_order_relaxed);
                        snapshot.useAlpha = entry.useAlpha.load(std::memory_order_relaxed);
                        snapshot.tStyle = entry.tStyle.load(std::memory_order_relaxed);
                        snapshot.gap = static_cast<float>(entry.gap100.load(std::memory_order_relaxed)) / 100.0f;
                        snapshot.size = static_cast<float>(entry.size100.load(std::memory_order_relaxed)) / 100.0f;
                        snapshot.thickness = static_cast<float>(entry.thickness100.load(std::memory_order_relaxed)) / 100.0f;
                        snapshot.outlineThickness = static_cast<float>(entry.outline100.load(std::memory_order_relaxed)) / 100.0f;
                        snapshot.alpha = entry.alpha.load(std::memory_order_relaxed);
                        snapshot.red = entry.red.load(std::memory_order_relaxed);
                        snapshot.green = entry.green.load(std::memory_order_relaxed);
                        snapshot.blue = entry.blue.load(std::memory_order_relaxed);
                    }

                    const uint32_t after = entry.generation.load(std::memory_order_acquire);
                    if (before != after || (after & 1u) != 0)
                        continue;
                    if (!matches)
                        break;

                    values = snapshot;
                    return true;
                }
            }
            return false;
        }

        void ClearEntry(SlotOverride &entry)
        {
            entry.generation.fetch_add(1, std::memory_order_acq_rel);
            entry.enabled.store(0, std::memory_order_relaxed);
            entry.pawnEntityIndex.store(-1, std::memory_order_relaxed);
            entry.weaponEntityIndex.store(-1, std::memory_order_relaxed);
            entry.generation.fetch_add(1, std::memory_order_release);
        }

        void PatchPaintConfig(void *config, const PaintValues &values)
        {
            if (!config)
                return;

            __try
            {
                auto *paint = reinterpret_cast<HudPaintConfig *>(config);
                paint->liveGap = 4.0f;
                paint->smoothGap = 4.0f;
                paint->color = static_cast<uint8_t>(ClampByte(values.color));
                paint->style = values.style;
                paint->drawOutline = static_cast<uint8_t>(values.drawOutline != 0);
                paint->dot = static_cast<uint8_t>(values.dot != 0);
                paint->recoil = 0;
                paint->gapUseWeapon = 0;
                paint->useAlpha = static_cast<uint8_t>(values.useAlpha != 0);
                paint->tStyle = static_cast<uint8_t>(values.tStyle != 0);
                paint->gap = values.gap;
                paint->thickness = values.thickness;
                paint->size = values.size;
                paint->outlineThickness = values.outlineThickness;
                paint->red = static_cast<uint8_t>(ClampByte(values.red));
                paint->green = static_cast<uint8_t>(ClampByte(values.green));
                paint->blue = static_cast<uint8_t>(ClampByte(values.blue));
                paint->alpha = static_cast<uint8_t>(ClampByte(values.alpha));
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // This client-side enhancement is optional and must not disturb
                // BotHider's server-side identity lifecycle.
            }
        }

        void __fastcall DetourBuildPaintConfig(void *player, void *weapon, void *config)
        {
            const auto original = g_original.load(std::memory_order_acquire);
            if (!original)
                return;

            original(player, weapon, config);
            const int playerIndex = EntityIndex(player);
            const int weaponIndex = EntityIndex(weapon);
            PaintValues values;
            if (ReadOverrideForEntity(playerIndex, values) ||
                ReadOverrideForEntity(weaponIndex, values))
            {
                PatchPaintConfig(config, values);
            }
        }

        bool PrologueMatches(void *target)
        {
            if (!target)
                return false;
            __try
            {
                return std::memcmp(target, kExpectedPrologue.data(),
                                   kExpectedPrologue.size()) == 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

    } // namespace

    bool Install(const nlohmann::json &gamedata)
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        if (g_hook.Active())
            return true;

        const std::string signature =
            sig::FindPlatformSig(gamedata, kBuildPaintConfigSigName);
        const auto clientModule = sig::ModuleFromName("client.dll");
        if (signature.empty() || !clientModule)
            return false;

        std::vector<uint8_t> pattern;
        std::vector<bool> wildcards;
        if (!sig::ParseSigString(signature, pattern, wildcards))
            return false;

        void *target = sig::FindPatternIn(clientModule, pattern, wildcards);
        if (!PrologueMatches(target))
            return false;

        if (!g_hook.Prepare(
                target,
                reinterpret_cast<void *>(&DetourBuildPaintConfig),
                kBuildPaintConfigStealLength))
        {
            return false;
        }

        g_original.store(
            reinterpret_cast<BuildPaintConfigFn>(g_hook.Trampoline()),
            std::memory_order_release);
        if (!g_hook.Enable())
        {
            g_original.store(nullptr, std::memory_order_release);
            g_hook.Remove();
            return false;
        }

        g_available.store(1, std::memory_order_release);
        return true;
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        g_available.store(0, std::memory_order_release);
        ClearOverrides();
        g_hook.Remove();
        g_original.store(nullptr, std::memory_order_release);
    }

    int SetOverride(int slot, int pawnEntityIndex, int weaponEntityIndex,
                    const PaintConfigOverride *config, int size)
    {
        if (!ValidSlot(slot) || !config ||
            size < static_cast<int>(sizeof(PaintConfigOverride)) ||
            config->size < static_cast<int32_t>(sizeof(PaintConfigOverride)) ||
            (pawnEntityIndex < 0 && weaponEntityIndex < 0))
        {
            return -1;
        }
        if (g_available.load(std::memory_order_acquire) == 0)
            return -2;

        std::lock_guard<std::mutex> lock(g_slotWriteMutex);
        if (g_available.load(std::memory_order_acquire) == 0)
            return -2;

        auto &entry = g_slots[static_cast<size_t>(slot)];
        entry.generation.fetch_add(1, std::memory_order_acq_rel);
        entry.enabled.store(0, std::memory_order_relaxed);

        entry.pawnEntityIndex.store(pawnEntityIndex, std::memory_order_relaxed);
        entry.weaponEntityIndex.store(weaponEntityIndex, std::memory_order_relaxed);
        entry.style.store(config->style, std::memory_order_relaxed);
        entry.color.store(config->color, std::memory_order_relaxed);
        entry.drawOutline.store(config->drawOutline, std::memory_order_relaxed);
        entry.dot.store(config->dot, std::memory_order_relaxed);
        entry.useAlpha.store(config->useAlpha, std::memory_order_relaxed);
        entry.tStyle.store(config->tStyle, std::memory_order_relaxed);
        entry.gap100.store(config->gap100, std::memory_order_relaxed);
        entry.size100.store(config->size100, std::memory_order_relaxed);
        entry.thickness100.store(config->thickness100, std::memory_order_relaxed);
        entry.outline100.store(config->outline100, std::memory_order_relaxed);
        entry.alpha.store(config->alpha, std::memory_order_relaxed);
        entry.red.store(config->red, std::memory_order_relaxed);
        entry.green.store(config->green, std::memory_order_relaxed);
        entry.blue.store(config->blue, std::memory_order_relaxed);

        entry.enabled.store(1, std::memory_order_relaxed);
        entry.generation.fetch_add(1, std::memory_order_release);
        return 0;
    }

    int ClearOverride(int slot)
    {
        if (!ValidSlot(slot))
            return -1;

        std::lock_guard<std::mutex> lock(g_slotWriteMutex);
        auto &entry = g_slots[static_cast<size_t>(slot)];
        ClearEntry(entry);
        return 0;
    }

    int ClearOverrides()
    {
        std::lock_guard<std::mutex> lock(g_slotWriteMutex);
        for (auto &entry : g_slots)
            ClearEntry(entry);
        return 0;
    }
} // namespace cs2bh::crosshair

extern "C" __declspec(dllexport) int BotHider_SetCustomCrosshair(
    int slot,
    int pawnEntityIndex,
    int weaponEntityIndex,
    const cs2bh::crosshair::PaintConfigOverride *config,
    int size)
{
    return cs2bh::crosshair::SetOverride(
        slot, pawnEntityIndex, weaponEntityIndex, config, size);
}

extern "C" __declspec(dllexport) int BotHider_ClearCustomCrosshair(int slot)
{
    return cs2bh::crosshair::ClearOverride(slot);
}

extern "C" __declspec(dllexport) int BotHider_ClearCustomCrosshairs()
{
    return cs2bh::crosshair::ClearOverrides();
}
