using BotHiderApi;
using CounterStrikeSharp.API.Core;
using HarmonyLib;

namespace BotHiderImpl;

// Override IsBot for managed slots.

[HarmonyPatch(typeof(CCSPlayerController), nameof(CCSPlayerController.IsBot), MethodType.Getter)]
public static class IsBotPatch
{
    // Bound for the plugin lifetime.
    internal static IBotHiderApi? Api;

    private static void Postfix(CCSPlayerController __instance, ref bool __result)
    {
        if (__result) return;

        var api = Api;
        if (api == null) return;

        try
        {
            int slot = __instance.Slot;
            if (slot >= 0 && api.IsManagedBot(slot))
                __result = true;
        }
        catch
        {
            // Preserve the original result.
        }
    }
}
