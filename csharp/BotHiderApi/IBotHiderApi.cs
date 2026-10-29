namespace BotHiderApi;

public static class BotHiderContract
{
    public const int MaxPlayerNameUtf8Bytes = 31;
}

public enum BotIdentityMode
{
    Player = 0,
    Bot = 1,
}

// Calls run on the game thread; slot parameters use the engine player slot.
public interface IBotHiderApi
{
    bool IsManagedBot(int slot);

    ulong GetBotSteamId(int slot);

    int[] GetManagedSlots();

    string GetPersonaName(int slot);

    int GetPing(int slot);

    string GetCrosshairCode(int slot);

    // Check native avatar application.
    bool HasBotAvatar(int slot);

    // Read the flair item definition index.
    uint GetScoreboardFlair(int slot);

    // Hook addresses; 0=unresolved.
    (string Name, ulong Addr)[] GetSignatures();

    bool SetBotSteamId(int slot, ulong steamId64);

    // Set crosshair; empty or "0" clears it.
    bool SetCrosshairCode(int slot, string code);

    // Apply a PNG avatar; "0" clears it.
    bool SetBotAvatar(int slot, string pngPath);

    // Normalize to at most 31 UTF-8 bytes; empty names fail.
    bool SetPersonaName(int slot, string name);

    bool SetScoreboardFlair(int slot, uint itemDefIndex);

    // Set the global identity mode.
    bool SetIdentityMode(BotIdentityMode mode);

    // New-bot names: true=bot_info.json, false=botprofile.
    bool SetNameSource(bool useBotInfo);
}
