using System.Runtime.InteropServices;
using System.Text;

namespace BotHiderImpl;

public sealed unsafe partial class NativePresentationClient
{
    [DllImport("BotHider", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotHider_PublishAvatarOverride(ulong session, ulong steamId, byte[] png, int length);
    [DllImport("BotHider", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotHider_ClearAvatarOverride(ulong session, ulong steamId);
    [DllImport("BotHider", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotHider_ClearAvatarOverrides(ulong session);
    [DllImport("BotHider", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotHider_ReadAvatarStatus(ulong session, [Out] byte[] buffer, int length);

    internal bool TryPublishAvatarOverride(ulong steamId, byte[] png, out string error)
    {
        var session = Session;
        if (session == 0) { error = "provider_unavailable"; return false; }
        if (steamId == 0 || png is not { Length: >= 8 and <= 16384 } ||
            !png.AsSpan(0, 8).SequenceEqual(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 }))
        { error = "invalid_avatar"; return false; }
        var result = BotHider_PublishAvatarOverride(session, steamId, png, png.Length);
        error = result >= 0 ? "" : $"avatar_publication_failed:{result}";
        return result >= 0;
    }
    internal bool TryClearAvatarOverride(ulong steamId, out string error)
    {
        var session = Session;
        if (session == 0) { error = "provider_unavailable"; return false; }
        var result = BotHider_ClearAvatarOverride(session, steamId);
        error = result >= 0 ? "" : $"avatar_restoration_failed:{result}";
        return result >= 0;
    }
    internal void ClearAvatarOverrides()
    {
        var session = Session;
        if (session != 0) BotHider_ClearAvatarOverrides(session);
    }
    internal string GetAvatarStatus()
    {
        var session = Session;
        if (session == 0) return "provider_unavailable";
        var buffer = new byte[512];
        if (BotHider_ReadAvatarStatus(session, buffer, buffer.Length) != 0) return "unavailable";
        int end = Array.IndexOf(buffer, (byte)0);
        return Encoding.UTF8.GetString(buffer, 0, end < 0 ? buffer.Length : end);
    }
}
