using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using BotHiderApi;

namespace BotHiderImpl;

internal sealed unsafe class BridgeClient : IBotHiderApi, IDisposable
{
    private const int MaxSlots = 64;
    private const int AvatarMaxBytes = 16 * 1024;
    private const int MaxSignatures = 8;

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    internal struct SlotSnapshot
    {
        public ulong SteamId, BaseSteamId, Incarnation, AvatarSteamId;
        public int Ping;
        public uint Managed, ScoreboardFlair, ClanGroupId, AvatarLength, AvatarApplied;
        public fixed byte Name[32];
        public fixed byte BaseName[32];
        public fixed byte Crosshair[64];
        public fixed byte ClanTag[128];

        public string PersonaName
        {
            get { fixed (byte* text = Name) return ReadText(text, 32); }
        }

        public string CrosshairCode
        {
            get { fixed (byte* text = Crosshair) return ReadText(text, 64); }
        }

        public (string Tag, uint GroupId) Clan
        {
            get { fixed (byte* text = ClanTag) return (ReadText(text, 128), ClanGroupId); }
        }
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct Signature
    {
        public fixed byte Name[32];
        public ulong Address;
    }

    private readonly int _threadId = Environment.CurrentManagedThreadId;
    private readonly string _libraryPath;
    private readonly Action<int, string, ulong>? _onVisibleName;
    private readonly Action<int, ulong>? _onSteamId;
    private readonly Action<int, uint, ulong>? _onScoreboardFlair;
    private readonly Action<int, string, ulong>? _onCrosshairCode;
    private nint _library;
    private bool _disposed;

    private static class Imports
    {
        private const string LibraryName = "BotHider";
        internal static nint Library;

        static Imports()
        {
            NativeLibrary.SetDllImportResolver(typeof(BridgeClient).Assembly,
                (name, _, _) => name == LibraryName ? Library : 0);
        }

        [DllImport(LibraryName, EntryPoint = "BH_Attach", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Attach();
        [DllImport(LibraryName, EntryPoint = "BH_Detach", CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Detach();
        [DllImport(LibraryName, EntryPoint = "BH_GetSlot", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int GetSlot(int slot, SlotSnapshot* output, int size);
        [DllImport(LibraryName, EntryPoint = "BH_GetSignatures", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int GetSignatures(Signature* output, int capacity);
        [DllImport(LibraryName, EntryPoint = "BH_SetSteamId", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int SetSteamId(int slot, ulong incarnation, ulong steamId);
        [DllImport(LibraryName, EntryPoint = "BH_SetName", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int SetName(int slot, ulong incarnation, byte* name, int length);
        [DllImport(LibraryName, EntryPoint = "BH_SetCrosshair", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int SetCrosshair(int slot, ulong incarnation, byte* code, int length);
        [DllImport(LibraryName, EntryPoint = "BH_SetFlair", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int SetFlair(int slot, ulong incarnation, uint flair);
        [DllImport(LibraryName, EntryPoint = "BH_SetAvatar", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int SetAvatar(int slot, ulong incarnation, byte* data, int length);
        [DllImport(LibraryName, EntryPoint = "BH_SetMode", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int SetMode(int mode);
        [DllImport(LibraryName, EntryPoint = "BH_SetNameSource", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int SetNameSource(int useBotInfo);
    }

    public BridgeClient(string libraryPath,
                        Action<int, string, ulong>? onVisibleName = null,
                        Action<int, ulong>? onSteamId = null,
                        Action<int, uint, ulong>? onScoreboardFlair = null,
                        Action<int, string, ulong>? onCrosshairCode = null)
    {
        _libraryPath = libraryPath;
        _onVisibleName = onVisibleName;
        _onSteamId = onSteamId;
        _onScoreboardFlair = onScoreboardFlair;
        _onCrosshairCode = onCrosshairCode;
    }

    public bool TryConnect() => TryConnect(out _);

    public bool TryConnect(out string error)
    {
        error = string.Empty;
        if (_disposed || Environment.CurrentManagedThreadId != _threadId)
        {
            error = "client is disposed or called from another thread";
            return false;
        }
        if (_library != 0) return true;
        nint library;
        try { library = NativeLibrary.Load(_libraryPath); }
        catch (Exception e) when (e is DllNotFoundException or BadImageFormatException)
        {
            error = e.Message;
            return false;
        }
        try
        {
            Imports.Library = library;
            if (Imports.Attach() == 0)
            {
                error = "BH_Attach rejected the connection (plugin unavailable, wrong thread, or provider already attached)";
                return false;
            }
            _library = library;
            return true;
        }
        catch (EntryPointNotFoundException e)
        {
            error = e.Message;
            return false;
        }
        finally
        {
            if (_library == 0) NativeLibrary.Free(library);
        }
    }

    public bool IsConnected() => _library != 0 && Environment.CurrentManagedThreadId == _threadId;

    internal bool ReadSlot(int slot, out SlotSnapshot snapshot)
    {
        snapshot = default;
        if (!IsConnected()) return false;
        fixed (SlotSnapshot* output = &snapshot)
            return Imports.GetSlot(slot, output, sizeof(SlotSnapshot)) != 0;
    }

    private static string ReadText(byte* text, int capacity)
    {
        var bytes = new ReadOnlySpan<byte>(text, capacity);
        int end = bytes.IndexOf((byte)0);
        return Encoding.UTF8.GetString(end < 0 ? bytes : bytes[..end]);
    }

    public bool IsManagedBot(int slot) => ReadSlot(slot, out var s) && s.Managed != 0;
    public ulong GetBotSteamId(int slot) => ReadSlot(slot, out var s) ? s.SteamId : 0;
    public ulong GetBaseBotSteamId(int slot) => ReadSlot(slot, out var s) ? s.BaseSteamId : 0;
    public ulong GetSlotIncarnation(int slot) => ReadSlot(slot, out var s) ? s.Incarnation : 0;
    public int GetPing(int slot) => ReadSlot(slot, out var s) ? s.Ping : 0;
    public string GetPersonaName(int slot) => ReadSlot(slot, out var s) ? s.PersonaName : string.Empty;
    public string GetBasePersonaName(int slot) => ReadSlot(slot, out var s) ? ReadText(s.BaseName, 32) : string.Empty;
    public string GetCrosshairCode(int slot) => ReadSlot(slot, out var s) ? s.CrosshairCode : string.Empty;
    public uint GetScoreboardFlair(int slot) => ReadSlot(slot, out var s) ? s.ScoreboardFlair : 0;
    public int GetConfiguredAvatarSize(int slot) => ReadSlot(slot, out var s) ? (int)s.AvatarLength : 0;
    public (string Tag, uint GroupId) GetClan(int slot) => ReadSlot(slot, out var s) ? s.Clan : (string.Empty, 0);
    public bool HasBotAvatar(int slot) => ReadSlot(slot, out var s) && s.Managed != 0 &&
        s.AvatarApplied != 0 && s.AvatarSteamId == s.SteamId;

    public int[] GetManagedSlots()
    {
        var slots = new List<int>();
        for (int slot = 0; slot < MaxSlots; slot++)
            if (IsManagedBot(slot)) slots.Add(slot);
        return slots.ToArray();
    }

    public (string Name, ulong Addr)[] GetSignatures()
    {
        if (!IsConnected()) return [];
        Signature* signatures = stackalloc Signature[MaxSignatures];
        int count = Imports.GetSignatures(signatures, MaxSignatures);
        var result = new (string, ulong)[count];
        for (int index = 0; index < count; index++)
            result[index] = (ReadText(signatures[index].Name, 32), signatures[index].Address);
        return result;
    }

    public bool SetBotSteamId(int slot, ulong steamId64)
    {
        if (!ReadSlot(slot, out var s) || Imports.SetSteamId(slot, s.Incarnation, steamId64) == 0) return false;
        _onSteamId?.Invoke(slot, s.Incarnation);
        return true;
    }

    public bool SetPersonaName(int slot, string name)
    {
        string normalized = BuildBoundedVisiblePersonaName(name);
        if (normalized.Length == 0 || !ReadSlot(slot, out var s)) return false;
        byte[] bytes = Encoding.UTF8.GetBytes(normalized);
        fixed (byte* text = bytes)
            if (Imports.SetName(slot, s.Incarnation, text, bytes.Length) == 0) return false;
        _onVisibleName?.Invoke(slot, GetPersonaName(slot), s.Incarnation);
        return true;
    }

    public bool SetCrosshairCode(int slot, string code)
    {
        if (code == null || !ReadSlot(slot, out var s)) return false;
        byte[] terminated = Encoding.UTF8.GetBytes((code == "0" ? string.Empty : code) + '\0');
        fixed (byte* text = terminated)
            if (Imports.SetCrosshair(slot, s.Incarnation, text, terminated.Length - 1) == 0) return false;
        _onCrosshairCode?.Invoke(slot, GetCrosshairCode(slot), s.Incarnation);
        return true;
    }

    public bool SetScoreboardFlair(int slot, uint itemDefIndex)
    {
        if (!ReadSlot(slot, out var s) || Imports.SetFlair(slot, s.Incarnation, itemDefIndex) == 0) return false;
        _onScoreboardFlair?.Invoke(slot, itemDefIndex, s.Incarnation);
        return true;
    }

    public bool SetIdentityMode(BotIdentityMode mode) => IsConnected() && Imports.SetMode((int)mode) != 0;
    public bool SetNameSource(bool useBotInfo) => IsConnected() && Imports.SetNameSource(useBotInfo ? 1 : 0) != 0;

    public bool SetBotAvatar(int slot, string pngPath) => TrySetBotAvatar(slot, pngPath, out _);

    public bool TrySetBotAvatar(int slot, string pngPath, out string error)
    {
        error = string.Empty;
        if (!ReadSlot(slot, out var s))
        {
            error = "bridge is unavailable";
            return false;
        }
        if (s.Managed == 0)
        {
            error = "slot is not a BotHider-managed bot";
            return false;
        }
        byte[] bytes;
        try
        {
            if (pngPath == "0") bytes = [];
            else
            {
                using var file = File.OpenRead(pngPath);
                long length = file.Length;
                if (length == 0 || length > AvatarMaxBytes)
                {
                    error = "avatar PNG must contain 1 to 16384 bytes";
                    return false;
                }
                bytes = new byte[(int)length];
                file.ReadExactly(bytes);
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            error = $"failed to read avatar PNG: {ex.Message}";
            return false;
        }
        fixed (byte* data = bytes)
        {
            if (Imports.SetAvatar(slot, s.Incarnation, data, bytes.Length) != 0) return true;
        }
        error = "avatar request rejected: check the managed slot and PNG data";
        return false;
    }

    // Normalize a visible name to the UTF-8 field limit.
    private static string BuildBoundedVisiblePersonaName(string? source)
    {
        if (string.IsNullOrWhiteSpace(source))
            return string.Empty;

        var visibleElements = new List<string>();
        var elements = StringInfo.GetTextElementEnumerator(source);
        while (elements.MoveNext())
        {
            string element = elements.GetTextElement();
            if (!IsInvisiblePersonaElement(element))
                visibleElements.Add(element);
        }

        int first = 0;
        while (first < visibleElements.Count &&
               IsWhitespacePersonaElement(visibleElements[first]))
        {
            first++;
        }

        int last = visibleElements.Count;
        while (last > first && IsWhitespacePersonaElement(visibleElements[last - 1]))
            last--;

        var boundedElements = new List<string>();
        int utf8Bytes = 0;
        for (int index = first; index < last; index++)
        {
            string element = visibleElements[index];
            int elementBytes = Encoding.UTF8.GetByteCount(element);
            if (utf8Bytes + elementBytes > BotHiderContract.MaxPlayerNameUtf8Bytes)
                break;
            boundedElements.Add(element);
            utf8Bytes += elementBytes;
        }

        while (boundedElements.Count > 0 &&
               IsWhitespacePersonaElement(boundedElements[^1]))
        {
            boundedElements.RemoveAt(boundedElements.Count - 1);
        }
        return string.Concat(boundedElements);
    }

    private static bool IsInvisiblePersonaElement(string element)
    {
        foreach (Rune rune in element.EnumerateRunes())
        {
            if (!IsInvisiblePersonaRune(rune))
                return false;
        }
        return true;
    }

    private static bool IsWhitespacePersonaElement(string element)
    {
        bool hasWhitespace = false;
        foreach (Rune rune in element.EnumerateRunes())
        {
            if (Rune.IsWhiteSpace(rune))
            {
                hasWhitespace = true;
                continue;
            }
            if (!IsInvisiblePersonaRune(rune))
                return false;
        }
        return hasWhitespace;
    }

    private static bool IsInvisiblePersonaRune(Rune rune)
    {
        return Rune.GetUnicodeCategory(rune) is
            UnicodeCategory.Control or
            UnicodeCategory.Format or
            UnicodeCategory.LineSeparator or
            UnicodeCategory.ParagraphSeparator or
            UnicodeCategory.Surrogate or
            UnicodeCategory.OtherNotAssigned or
            UnicodeCategory.NonSpacingMark or
            UnicodeCategory.SpacingCombiningMark or
            UnicodeCategory.EnclosingMark;
    }

    public void Dispose()
    {
        if (_disposed) return;
        if (Environment.CurrentManagedThreadId != _threadId)
            throw new InvalidOperationException("Dispose on the game thread.");
        _disposed = true;
        if (_library == 0) return;
        Imports.Detach();
        NativeLibrary.Free(_library);
        _library = 0;
    }
}
