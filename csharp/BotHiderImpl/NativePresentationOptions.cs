using System.Runtime.InteropServices;

namespace BotHiderImpl;

public sealed unsafe partial class NativePresentationClient
{
    [DllImport("BotHider", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotHider_SetBase(int slot, ulong session, ulong incarnation,
        ulong sid, byte[] name, byte[] crosshair, uint flair);
    [DllImport("BotHider", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotHider_SetAvatar(int slot, ulong session, ulong incarnation, byte[] png, int length);
    [DllImport("BotHider", CallingConvention = CallingConvention.Cdecl)]
    private static extern int BotHider_GetAvatarState(int slot, ulong session, ulong incarnation, out int size);

    [DllImport("BotHider", CallingConvention = CallingConvention.Cdecl)]
    private static extern ulong BotHider_GetUserInfoPublications();
    internal ulong UserInfoPublications => Session != 0 ? BotHider_GetUserInfoPublications() : 0;

    internal bool SetBase(int slot, Slot previous, ulong sid, string name, string crosshair, uint flair)
        => TryEncodeFixedUtf8(name, 32, out var n) && TryEncodeFixedUtf8(crosshair, 64, out var c) &&
           BotHider_SetBase(slot, previous.Session, previous.Incarnation, sid, n, c, flair) == 0;
    internal bool SetAvatar(int slot, byte[] png)
        => TryGetSlot(slot, out var s) && s.Managed != 0 &&
           BotHider_SetAvatar(slot, s.Session, s.Incarnation, png, png.Length) == 0;
    internal int AvatarState(int slot, out int size)
    {
        size = 0;
        return TryGetSlot(slot, out var s) && s.Managed != 0
            ? BotHider_GetAvatarState(slot, s.Session, s.Incarnation, out size) : -1;
    }
}
