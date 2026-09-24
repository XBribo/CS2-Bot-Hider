using BotHiderApi;

namespace BotHiderImpl;

// Compatibility façade. No independent writes or presentation state live here.
internal sealed class LegacyBotHiderApi(NativePresentationClient client, BotHiderPresentationService service) : IBotHiderApi
{
    public bool IsManagedBot(int slot) => service.IsManagedBot(slot);
    public int[] GetManagedSlots() => Enumerable.Range(0, 64).Where(IsManagedBot).ToArray();
    public ulong GetBotSteamId(int slot) => client.GetPublishedSteamId(slot);
    public ulong GetBaseBotSteamId(int slot) => client.TryGetSlot(slot, out var s) ? s.BaseSteamId : 0;
    public ulong GetSlotIncarnation(int slot) => client.TryGetSlot(slot, out var s) ? s.Incarnation : 0;
    public string GetPersonaName(int slot) => client.GetPublishedPersonaName(slot);
    public string GetBasePersonaName(int slot) => client.TryGetSlot(slot, out var s) ? s.ReadBaseName() : "";
    public int GetPing(int slot) => client.TryGetSlot(slot, out var s) ? s.Ping : 0;
    private BotHiderPresentationOverride? Override(int slot)
        => service.TryGetManagedSlot(slot, out var state) ? service.GetPresentationOverride(slot, state.Incarnation) : null;
    public string GetCrosshairCode(int slot)
        => Override(slot)?.CrosshairCode ?? (client.TryGetSlot(slot, out var s) ? s.ReadCrosshair() : "");
    public uint GetScoreboardFlair(int slot)
        => Override(slot)?.ScoreboardFlair ?? (client.TryGetSlot(slot, out var s) ? s.ScoreboardFlair : 0);
    public bool HasBotAvatar(int slot) => client.AvatarState(slot, out _) == 1;
    public int GetConfiguredAvatarSize(int slot) { client.AvatarState(slot, out var size); return size; }
    public (string Name, ulong Addr)[] GetSignatures() => client.GetSignatures();

    private bool Mutate(int slot, Action<BotHiderPresentationOverride> change)
    {
        if (!service.TryGetManagedSlot(slot, out var state) || service.IsLeased(slot) ||
            !client.TryGetSlot(slot, out var native)) return false;
        var request = new BotHiderPresentationOverride { Slot = slot, Incarnation = state.Incarnation };
        change(request);
        using var owner = new CancellationTokenSource();
        var lease = service.AcquirePresentationLease("bothider:legacy", [request], owner.Token);
        if (!lease.Ok) return false;
        try
        {
            // The same validation/readback/rollback as the lease API happens
            // before the legacy setter commits a new base persona.
            return client.SetBase(slot, native,
                request.SteamId ?? native.BaseSteamId,
                request.PlayerName?.Trim() ?? native.ReadBaseName(),
                request.CrosshairCode?.Trim() ?? native.ReadCrosshair(),
                request.ScoreboardFlair ?? native.ScoreboardFlair);
        }
        finally { service.ReleasePresentationLease(lease.LeaseToken); }
    }
    public bool SetBotSteamId(int slot, ulong steamId64) => Mutate(slot, o => o.SteamId = steamId64);
    public bool SetPersonaName(int slot, string name)
    {
        var normalized = PersonaName.Normalize(name);
        return normalized.Length > 0 && Mutate(slot, o => o.PlayerName = normalized);
    }
    public bool SetScoreboardFlair(int slot, uint itemDefIndex) => Mutate(slot, o => o.ScoreboardFlair = itemDefIndex);
    public bool SetCrosshairCode(int slot, string code)
        => Mutate(slot, o => o.CrosshairCode = code == "0" ? "" : code);
    public bool SetIdentityMode(BotIdentityMode mode)
        => !service.HasLeases && Enum.IsDefined(mode) && client.SetDisguise(mode == BotIdentityMode.Player);
    public bool SetNameSource(bool useBotInfo) => client.SetNameSource(useBotInfo);
    public bool SetBotAvatar(int slot, string pngPath) => TrySetBotAvatar(slot, pngPath, out _);
    public bool TrySetBotAvatar(int slot, string pngPath, out string error)
    {
        error = "";
        if (!IsManagedBot(slot) || service.IsLeased(slot)) { error = "slot_unavailable_or_leased"; return false; }
        if (client.ExternalAvatars) { error = "external_avatar_publisher"; return false; }
        try
        {
            byte[] png = [];
            if (pngPath != "0")
            {
                using var stream = File.OpenRead(pngPath);
                if (stream.Length is < 8 or > 16384) { error = "png_size"; return false; }
                png = new byte[(int)stream.Length]; stream.ReadExactly(png);
                if (!png.AsSpan(0, 8).SequenceEqual(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 }))
                { error = "png_signature"; return false; }
            }
            if (client.SetAvatar(slot, png)) return true;
            error = "native_rejected"; return false;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        { error = ex.Message; return false; }
    }
}
