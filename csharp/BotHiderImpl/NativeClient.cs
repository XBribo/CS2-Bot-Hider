using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using BotHiderApi;

namespace BotHiderImpl;

// The native plugin and this provider ship together and run on the game thread.
internal sealed unsafe class NativeClient : IBotHiderApi, IDisposable
{
    private const int MaxSlots = 64;
    private const int AvatarMaxBytes = 16 * 1024;
    private const int MaxSignatures = 8;

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct SlotSnapshot
    {
        public ulong SteamId, BaseSteamId, Incarnation, AvatarSteamId;
        public int Ping;
        public uint Managed, ScoreboardFlair, ClanGroupId, AvatarLength, AvatarApplied;
        public fixed byte Name[32];
        public fixed byte BaseName[32];
        public fixed byte Crosshair[64];
        public fixed byte ClanTag[128];
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
    private delegate* unmanaged[Cdecl]<int> _attach;
    private delegate* unmanaged[Cdecl]<void> _detach;
    private delegate* unmanaged[Cdecl]<int, SlotSnapshot*, int, int> _getSlot;
    private delegate* unmanaged[Cdecl]<Signature*, int, int> _getSignatures;
    private delegate* unmanaged[Cdecl]<int, ulong, ulong, int> _setSteamId;
    private delegate* unmanaged[Cdecl]<int, ulong, byte*, int, int> _setName;
    private delegate* unmanaged[Cdecl]<int, ulong, byte*, int, int> _setCrosshair;
    private delegate* unmanaged[Cdecl]<int, ulong, uint, int> _setFlair;
    private delegate* unmanaged[Cdecl]<int, ulong, byte*, int, int> _setAvatar;
    private delegate* unmanaged[Cdecl]<int, int> _setMode;
    private delegate* unmanaged[Cdecl]<int, int> _setNameSource;

    public NativeClient(string libraryPath,
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

    public bool TryConnect()
    {
        if (_disposed || Environment.CurrentManagedThreadId != _threadId) return false;
        if (_library != 0) return true;
        if (!NativeLibrary.TryLoad(_libraryPath, out nint library)) return false;
        try
        {
            _attach = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(library, "BH_Attach");
            _detach = (delegate* unmanaged[Cdecl]<void>)NativeLibrary.GetExport(library, "BH_Detach");
            _getSlot = (delegate* unmanaged[Cdecl]<int, SlotSnapshot*, int, int>)NativeLibrary.GetExport(library, "BH_GetSlot");
            _getSignatures = (delegate* unmanaged[Cdecl]<Signature*, int, int>)NativeLibrary.GetExport(library, "BH_GetSignatures");
            _setSteamId = (delegate* unmanaged[Cdecl]<int, ulong, ulong, int>)NativeLibrary.GetExport(library, "BH_SetSteamId");
            _setName = (delegate* unmanaged[Cdecl]<int, ulong, byte*, int, int>)NativeLibrary.GetExport(library, "BH_SetName");
            _setCrosshair = (delegate* unmanaged[Cdecl]<int, ulong, byte*, int, int>)NativeLibrary.GetExport(library, "BH_SetCrosshair");
            _setFlair = (delegate* unmanaged[Cdecl]<int, ulong, uint, int>)NativeLibrary.GetExport(library, "BH_SetFlair");
            _setAvatar = (delegate* unmanaged[Cdecl]<int, ulong, byte*, int, int>)NativeLibrary.GetExport(library, "BH_SetAvatar");
            _setMode = (delegate* unmanaged[Cdecl]<int, int>)NativeLibrary.GetExport(library, "BH_SetMode");
            _setNameSource = (delegate* unmanaged[Cdecl]<int, int>)NativeLibrary.GetExport(library, "BH_SetNameSource");
            if (_attach() == 0) return false;
            _library = library;
            return true;
        }
        catch (EntryPointNotFoundException) { return false; }
        finally
        {
            if (_library == 0) NativeLibrary.Free(library);
        }
    }

    public bool IsConnected() => _library != 0 && Environment.CurrentManagedThreadId == _threadId;

    private bool ReadSlot(int slot, out SlotSnapshot snapshot)
    {
        snapshot = default;
        if (!IsConnected()) return false;
        fixed (SlotSnapshot* output = &snapshot)
            return _getSlot(slot, output, sizeof(SlotSnapshot)) != 0;
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
    public string GetPersonaName(int slot) => ReadSlot(slot, out var s) ? ReadText(s.Name, 32) : string.Empty;
    public string GetBasePersonaName(int slot) => ReadSlot(slot, out var s) ? ReadText(s.BaseName, 32) : string.Empty;
    public string GetCrosshairCode(int slot) => ReadSlot(slot, out var s) ? ReadText(s.Crosshair, 64) : string.Empty;
    public uint GetScoreboardFlair(int slot) => ReadSlot(slot, out var s) ? s.ScoreboardFlair : 0;
    public int GetConfiguredAvatarSize(int slot) => ReadSlot(slot, out var s) ? (int)s.AvatarLength : 0;
    public (string Tag, uint GroupId) GetClan(int slot) => ReadSlot(slot, out var s)
        ? (ReadText(s.ClanTag, 128), s.ClanGroupId) : (string.Empty, 0);
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
        int count = _getSignatures(signatures, MaxSignatures);
        var result = new (string, ulong)[count];
        for (int index = 0; index < count; index++)
            result[index] = (ReadText(signatures[index].Name, 32), signatures[index].Address);
        return result;
    }

    public bool SetBotSteamId(int slot, ulong steamId64)
    {
        if (!ReadSlot(slot, out var s) || _setSteamId(slot, s.Incarnation, steamId64) == 0) return false;
        _onSteamId?.Invoke(slot, s.Incarnation);
        return true;
    }

    public bool SetPersonaName(int slot, string name)
    {
        string normalized = BuildBoundedVisiblePersonaName(name);
        if (normalized.Length == 0 || !ReadSlot(slot, out var s)) return false;
        byte[] bytes = Encoding.UTF8.GetBytes(normalized);
        fixed (byte* text = bytes)
            if (_setName(slot, s.Incarnation, text, bytes.Length) == 0) return false;
        _onVisibleName?.Invoke(slot, GetPersonaName(slot), s.Incarnation);
        return true;
    }

    public bool SetCrosshairCode(int slot, string code)
    {
        if (code == null || !ReadSlot(slot, out var s)) return false;
        byte[] terminated = Encoding.UTF8.GetBytes((code == "0" ? string.Empty : code) + '\0');
        fixed (byte* text = terminated)
            if (_setCrosshair(slot, s.Incarnation, text, terminated.Length - 1) == 0) return false;
        _onCrosshairCode?.Invoke(slot, GetCrosshairCode(slot), s.Incarnation);
        return true;
    }

    public bool SetScoreboardFlair(int slot, uint itemDefIndex)
    {
        if (!ReadSlot(slot, out var s) || _setFlair(slot, s.Incarnation, itemDefIndex) == 0) return false;
        _onScoreboardFlair?.Invoke(slot, itemDefIndex, s.Incarnation);
        return true;
    }

    public bool SetIdentityMode(BotIdentityMode mode) => IsConnected() && _setMode((int)mode) != 0;
    public bool SetNameSource(bool useBotInfo) => IsConnected() && _setNameSource(useBotInfo ? 1 : 0) != 0;

    public bool SetBotAvatar(int slot, string pngPath) => TrySetBotAvatar(slot, pngPath, out _);

    public bool TrySetBotAvatar(int slot, string pngPath, out string error)
    {
        error = string.Empty;
        if (!ReadSlot(slot, out var s))
        {
            error = "native bridge is unavailable";
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
            if (_setAvatar(slot, s.Incarnation, data, bytes.Length) != 0) return true;
        }
        error = "native avatar request rejected: check the managed slot and PNG data";
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
            throw new InvalidOperationException("Dispose the native client on the game thread.");
        _disposed = true;
        if (_library == 0) return;
        _detach();
        NativeLibrary.Free(_library);
        _library = 0;
    }
}
