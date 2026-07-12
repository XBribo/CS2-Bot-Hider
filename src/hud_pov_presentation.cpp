#include "hud_pov_presentation.h"

#include "inline_hook.h"
#include "sig_scan.h"
#include "slot_publisher.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <Windows.h>
#define BH_FASTCALL __fastcall
#else
#define BH_FASTCALL
#endif

namespace cs2bh::HudPovPresentation
{
    namespace
    {
        constexpr const char *kTakeoverUpdateSigName = "CS2Hud::UpdateBotTakeoverHint";
        constexpr const char *kSpecUpdateSigName = "CSGOHudHealthAmmoCenter::UpdateSpecPlayer";
        constexpr const char *kMakeSymbolSigName = "Panorama::MakeSymbol";

        constexpr unsigned char kTakeoverUpdatePrologue[] = {
            0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C,
            0x24, 0x20, 0x57, 0x48, 0x81, 0xEC, 0x50, 0x01,
            0x00, 0x00};
        constexpr unsigned char kSpecUpdatePrologue[] = {
            0x40, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
            0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30};
        constexpr unsigned char kMakeSymbolPrologue[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
            0xEC, 0x20, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0xDA};

        // The stolen prologues contain only position-independent instructions.
        constexpr size_t kTakeoverStealLen = sizeof(kTakeoverUpdatePrologue);
        constexpr size_t kSpecStealLen = sizeof(kSpecUpdatePrologue);

        // Recovered from client.dll and hudhealthammocenter.vxml/vcss on
        // 2026-07-10. JoinTextBot is independent from the native team emblem.
        constexpr size_t kJoinTextBotOffset = 0xC0;
        constexpr size_t kPanelInterfaceOffset = 0x08;
        constexpr size_t kSetVisibleVtableOffset = 0x108;
        constexpr size_t kSetHasClassVtableOffset = 0x500;

        // CCSGO_HudHealthAmmoCenter Panorama handles.
        constexpr size_t kSpecBackgroundHandleOffset = 0x90;
        constexpr size_t kSpecAvatarHandleOffset = 0x98;
        constexpr size_t kSpecLegendHandleOffset = 0xA0;
        constexpr size_t kHudRootPanelOffset = 0x08;

        // UpdateSpecPlayer loads the Panorama panel registry through
        // `mov rcx, [rip+disp32]` at +0x129 in the verified client build.
        constexpr size_t kPanelRegistryLoadOffset = 0x129;
        constexpr unsigned char kPanelRegistryLoadOpcode[] = {0x48, 0x8B, 0x0D};
        constexpr size_t kResolveHandleVtableOffset = 0x110;
        constexpr size_t kGetPanelWrapperVtableOffset = 0x40;

        using UpdateFn = void(BH_FASTCALL *)(void *);
        using SetVisibleFn = void(BH_FASTCALL *)(void *, bool);
        using SetHasClassFn = void(BH_FASTCALL *)(void *, std::uint16_t, bool);
        using MakeSymbolFn = void *(BH_FASTCALL *)(std::uint16_t *, const char *);
        using ResolveHandleFn = void *(BH_FASTCALL *)(void *, const void *);
        using GetPanelWrapperFn = void *(BH_FASTCALL *)(void *);

        hook::InlineHook g_takeoverHook;
        hook::InlineHook g_specHook;
        UpdateFn g_takeoverOriginal = nullptr;
        UpdateFn g_specOriginal = nullptr;
        void **g_panelRegistryGlobal = nullptr;
        std::uint16_t g_specVisibleClass = 0;
        std::uint16_t g_spectatingTargetClass = 0;
        std::string g_status = "not installed";

        void SetPanelClass(void *panel, std::uint16_t symbol, bool enabled)
        {
            if (!panel)
                return;
            auto **vtable = *reinterpret_cast<void ***>(panel);
            if (!vtable)
                return;
            auto setHasClass = reinterpret_cast<SetHasClassFn>(
                vtable[kSetHasClassVtableOffset / sizeof(void *)]);
            if (setHasClass)
                setHasClass(panel, symbol, enabled);
        }

        void *ResolvePanelHandle(void *handle)
        {
            if (!handle || !g_panelRegistryGlobal || !*g_panelRegistryGlobal)
                return nullptr;

            auto *registry = *g_panelRegistryGlobal;
            auto **registryVtable = *reinterpret_cast<void ***>(registry);
            if (!registryVtable)
                return nullptr;
            auto resolve = reinterpret_cast<ResolveHandleFn>(
                registryVtable[kResolveHandleVtableOffset / sizeof(void *)]);
            if (!resolve)
                return nullptr;

            void *entry = resolve(registry, handle);
            if (!entry)
                return nullptr;
            auto **entryVtable = *reinterpret_cast<void ***>(entry);
            if (!entryVtable)
                return nullptr;
            auto getWrapper = reinterpret_cast<GetPanelWrapperFn>(
                entryVtable[kGetPanelWrapperVtableOffset / sizeof(void *)]);
            void *wrapper = getWrapper ? getWrapper(entry) : nullptr;
            return wrapper ? *reinterpret_cast<void **>(
                                 reinterpret_cast<unsigned char *>(wrapper) +
                                 kPanelInterfaceOffset)
                           : nullptr;
        }

        void HideTakeoverText(void *hud)
        {
#if defined(_WIN32)
            __try
            {
                if (!hud)
                    return;
                auto *wrapper = *reinterpret_cast<void **>(
                    reinterpret_cast<unsigned char *>(hud) + kJoinTextBotOffset);
                if (!wrapper)
                    return;
                auto *panel = *reinterpret_cast<void **>(
                    reinterpret_cast<unsigned char *>(wrapper) + kPanelInterfaceOffset);
                if (!panel)
                    return;
                auto **vtable = *reinterpret_cast<void ***>(panel);
                if (!vtable)
                    return;
                auto setVisible = reinterpret_cast<SetVisibleFn>(
                    vtable[kSetVisibleVtableOffset / sizeof(void *)]);
                if (setVisible)
                    setVisible(panel, false);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // A stale Panorama layout must not take down the listen server.
            }
#else
            (void)hud;
#endif
        }

        void ApplyObserverPovPresentation(void *specHud)
        {
#if defined(_WIN32)
            if (!Publisher().ObserverPovActive())
                return;

            __try
            {
                auto *bytes = reinterpret_cast<unsigned char *>(specHud);
                SetPanelClass(ResolvePanelHandle(bytes + kSpecBackgroundHandleOffset),
                              g_specVisibleClass, false);
                SetPanelClass(ResolvePanelHandle(bytes + kSpecAvatarHandleOffset),
                              g_specVisibleClass, false);
                SetPanelClass(ResolvePanelHandle(bytes + kSpecLegendHandleOffset),
                              g_specVisibleClass, false);

                // The native POV center remains under the spectator card.
                // Spectating only collapses its strokes; remove that class to
                // reveal the original T/CT emblem bar without redrawing it.
                auto *rootPanel = *reinterpret_cast<void **>(
                    bytes + kHudRootPanelOffset);
                SetPanelClass(rootPanel, g_spectatingTargetClass, false);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // A stale Panorama layout must not take down the listen server.
            }
#else
            (void)specHud;
#endif
        }

        void BH_FASTCALL TakeoverDetour(void *hud)
        {
            if (g_takeoverOriginal)
                g_takeoverOriginal(hud);
            if (Publisher().TakePovActive())
                HideTakeoverText(hud);
        }

        void BH_FASTCALL SpecDetour(void *specHud)
        {
            if (g_specOriginal)
                g_specOriginal(specHud);
            ApplyObserverPovPresentation(specHud);
        }
    } // namespace

    bool Install(const nlohmann::json &gamedata, char *errorOut, size_t errorOutLen)
    {
        if (g_takeoverHook.Active() && g_specHook.Active())
            return true;

#if !defined(_WIN32)
        g_status = "unavailable: Windows client hook only";
        return false;
#else
        const auto clientModule = sig::ModuleFromName("client.dll");
        if (!clientModule)
        {
            g_status = "unavailable: client.dll not loaded (dedicated server)";
            return false;
        }

        void *takeoverTarget = sig::ResolveSig(
            gamedata, clientModule, kTakeoverUpdateSigName, errorOut, errorOutLen);
        void *specTarget = sig::ResolveSig(
            gamedata, clientModule, kSpecUpdateSigName, errorOut, errorOutLen);
        void *makeSymbolTarget = sig::ResolveSig(
            gamedata, clientModule, kMakeSymbolSigName, errorOut, errorOutLen);
        if (!takeoverTarget || !specTarget || !makeSymbolTarget ||
            std::memcmp(takeoverTarget, kTakeoverUpdatePrologue,
                        sizeof(kTakeoverUpdatePrologue)) != 0 ||
            std::memcmp(specTarget, kSpecUpdatePrologue,
                        sizeof(kSpecUpdatePrologue)) != 0 ||
            std::memcmp(makeSymbolTarget, kMakeSymbolPrologue,
                        sizeof(kMakeSymbolPrologue)) != 0)
        {
            g_status = "unavailable: client signature/prologue mismatch";
            return false;
        }

        auto *registryLoad = reinterpret_cast<unsigned char *>(specTarget) +
                             kPanelRegistryLoadOffset;
        if (std::memcmp(registryLoad, kPanelRegistryLoadOpcode,
                        sizeof(kPanelRegistryLoadOpcode)) != 0)
        {
            g_status = "unavailable: Panorama registry load mismatch";
            return false;
        }
        std::int32_t registryDisp = 0;
        std::memcpy(&registryDisp, registryLoad + 3, sizeof(registryDisp));
        g_panelRegistryGlobal = reinterpret_cast<void **>(
            registryLoad + 7 + registryDisp);

        auto makeSymbol = reinterpret_cast<MakeSymbolFn>(makeSymbolTarget);
        makeSymbol(&g_specVisibleClass, "HudSpecplayerRoot--visible");
        makeSymbol(&g_spectatingTargetClass, "HUD--spectating-target");

        if (!g_takeoverHook.Install(takeoverTarget,
                                    reinterpret_cast<void *>(&TakeoverDetour),
                                    kTakeoverStealLen))
        {
            g_status = "failed: takeover hook install";
            std::snprintf(errorOut, errorOutLen, "%s", g_status.c_str());
            return false;
        }
        g_takeoverOriginal = reinterpret_cast<UpdateFn>(g_takeoverHook.Trampoline());

        if (!g_specHook.Install(specTarget,
                               reinterpret_cast<void *>(&SpecDetour),
                               kSpecStealLen))
        {
            g_takeoverHook.Remove();
            g_takeoverOriginal = nullptr;
            g_status = "failed: spectator hook install";
            std::snprintf(errorOut, errorOutLen, "%s", g_status.c_str());
            return false;
        }
        g_specOriginal = reinterpret_cast<UpdateFn>(g_specHook.Trampoline());

        g_status = "installed: managed bot POV HUD presentation";
        return true;
#endif
    }

    void Remove()
    {
        g_specHook.Remove();
        g_takeoverHook.Remove();
        g_takeoverOriginal = nullptr;
        g_specOriginal = nullptr;
        g_panelRegistryGlobal = nullptr;
        g_specVisibleClass = 0;
        g_spectatingTargetClass = 0;
        g_status = "not installed";
    }

    const char *Status()
    {
        return g_status.c_str();
    }
} // namespace cs2bh::HudPovPresentation

#undef BH_FASTCALL
