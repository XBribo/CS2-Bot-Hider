using System.Runtime.InteropServices;

namespace BotHiderImpl;

internal static class NativeAvatarClient
{
    [DllImport("BotHider", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotHider_PublishAvatarOverride(ulong steamId, byte[] png, int length);

    [DllImport("BotHider", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotHider_ClearAvatarOverride(ulong steamId);

    [DllImport("BotHider", CallingConvention = CallingConvention.Cdecl)]
    private static extern void BotHider_ClearAvatarOverrides();

    // Publishes one validated PNG for a SteamID, including real players.
    public static bool TryPublish(ulong steamId, byte[] png, out string error)
    {
        error = string.Empty;
        if (steamId == 0 || png == null) { error = "invalid avatar request"; return false; }
        try
        {
            int result = BotHider_PublishAvatarOverride(steamId, png, png.Length);
            if (result >= 0) return true;
            error = $"native avatar publication failed ({result})";
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
        {
            error = ex.Message;
        }
        return false;
    }

    // Restores the value that preceded this direct publication.
    public static bool TryClear(ulong steamId, out string error)
    {
        error = string.Empty;
        if (steamId == 0) { error = "invalid steam ID"; return false; }
        try
        {
            int result = BotHider_ClearAvatarOverride(steamId);
            if (result >= 0) return true;
            error = $"native avatar restoration failed ({result})";
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
        {
            error = ex.Message;
        }
        return false;
    }

    // Clears direct publications without touching per-slot BotHider avatars.
    public static void ClearAll()
    {
        try { BotHider_ClearAvatarOverrides(); }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
        {
            System.Console.Error.WriteLine($"[BotHider] avatar cleanup unavailable: {ex.Message}");
        }
    }
}
