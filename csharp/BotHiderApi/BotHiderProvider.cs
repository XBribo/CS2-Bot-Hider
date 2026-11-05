namespace BotHiderApi;

// Keep cached callbacks in the shared assembly.
public static class BotHiderProvider
{
    private static IBotHiderApi? _api;
    private static Func<object, int>? _getSlot;
    private static bool _registered;

    public static void Attach(IBotHiderApi api, Action<Func<IBotHiderApi>> register)
        => Attach(api, register, null);

    public static void Attach(IBotHiderApi api, Action<Func<IBotHiderApi>> register,
        Func<object, int>? getSlot)
    {
        if (!_registered)
        {
            register(() => _api!);
            _registered = true;
        }
        _api = api;
        _getSlot = getSlot;
    }

    public static void Detach(IBotHiderApi? api)
    {
        if (!ReferenceEquals(_api, api)) return;
        _api = null;
        _getSlot = null;
    }

    public static void ApplyIsBot(object __instance, ref bool __result)
    {
        if (__result || _api == null || _getSlot == null) return;
        try
        {
            __result = _api.IsManagedBot(_getSlot(__instance));
        }
        catch
        {
        }
    }
}
