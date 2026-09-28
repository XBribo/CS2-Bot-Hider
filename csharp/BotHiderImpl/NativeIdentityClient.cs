using System.Runtime.InteropServices;
using System.Text;

namespace BotHiderImpl;

internal static class NativeIdentityClient
{
    [DllImport("BotHider", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotHider_GetNativeAbi();

    [DllImport("BotHider", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotHider_PublishIdentity(
        int slot, ulong incarnation, ulong steamId, byte[] name);

    // Verifies that the matched native bridge is loaded.
    public static bool IsAvailable()
    {
        try { return BotHider_GetNativeAbi() == 1; }
        catch (DllNotFoundException) { return false; }
        catch (EntryPointNotFoundException) { return false; }
        catch (BadImageFormatException) { return false; }
    }

    // Publishes an exact managed identity, guarded by its native incarnation.
    public static bool PublishIdentity(int slot, ulong incarnation, ulong steamId, string name)
    {
        if (incarnation == 0 || string.IsNullOrEmpty(name) ||
            name.Contains('\0') || Encoding.UTF8.GetByteCount(name) > 31 || !IsAvailable())
            return false;

        var bytes = new byte[32];
        Encoding.UTF8.GetBytes(name, bytes);
        try { return BotHider_PublishIdentity(slot, incarnation, steamId, bytes) == 0; }
        catch (DllNotFoundException) { return false; }
        catch (EntryPointNotFoundException) { return false; }
        catch (BadImageFormatException) { return false; }
    }
}
