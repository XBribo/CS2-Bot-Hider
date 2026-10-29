namespace BotHiderApi;

// Keep the supplier in the shared assembly across provider reloads.
public static class BotHiderProvider
{
    private static IBotHiderApi? _api;
    private static bool _registered;

    public static void Attach(IBotHiderApi api, Action<Func<IBotHiderApi>> register)
    {
        if (!_registered)
        {
            register(() => _api!);
            _registered = true;
        }
        _api = api;
    }

    public static void Detach(IBotHiderApi? api)
    {
        if (ReferenceEquals(_api, api)) _api = null;
    }
}
