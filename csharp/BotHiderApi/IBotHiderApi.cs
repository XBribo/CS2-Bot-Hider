namespace BotHiderApi;
// Slot is the engine player slot (CCSPlayerController.Slot.Value)
public interface IBotHiderApi
{
    bool IsManagedBot(int slot);

    ulong GetSyntheticSteamId(int slot);

    int[] GetManagedSlots();

    string GetPersonaName(int slot);

    int GetPing(int slot);

    string GetCrosshairCode(int slot);

    // returns false if the slot is out of range.
    bool SetBotSteamId(int slot, ulong steamId64);

    // returns false if the slot/name is invalid.
    bool SetPersonaName(int slot, string name);
}
