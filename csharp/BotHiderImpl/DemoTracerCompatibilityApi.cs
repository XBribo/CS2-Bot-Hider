using Current = BotHiderApi;
using Legacy = DemoTracerBotHiderApi;

namespace BotHiderImpl;

// Binary-contract adapter only: the general service owns every lease and write.
internal sealed class DemoTracerCompatibilityApi(Current.IBotHiderPresentationApi service) : Legacy.IBotHiderApi
{
    public int ApiVersion => Legacy.DemoTracerBotHiderContract.ApiVersion;
    public bool IsManagedBot(int slot) => service.IsManagedBot(slot);
    public Legacy.BotHiderProviderInfo GetProviderInfo()
    {
        var s = service.GetProviderInfo();
        return new() { ApiVersion = ApiVersion, ProviderEpoch = s.ProviderEpoch, MapEpoch = s.MapEpoch, Connected = s.Connected, Draining = s.Draining };
    }
    public bool TryGetManagedSlot(int slot, out Legacy.BotHiderManagedSlot state)
    {
        bool ok = service.TryGetManagedSlot(slot, out var s);
        state = new()
        {
            Slot = s.Slot,
            Incarnation = s.Incarnation,
            BaseSteamId = s.BaseSteamId,
            PublishedSteamId = s.PublishedSteamId,
            BasePlayerName = s.BasePlayerName,
            BasePing = s.BasePing,
            BaseCrosshairCode = s.BaseCrosshairCode,
            BaseScoreboardFlair = s.BaseScoreboardFlair
        };
        return ok;
    }
    private static Current.BotHiderPresentationOverride[] Convert(Legacy.BotHiderPresentationOverride[] overrides)
        => overrides?.Select(s => s == null ? null! : new Current.BotHiderPresentationOverride
        {
            Slot = s.Slot,
            Incarnation = s.Incarnation,
            PlayerName = s.PlayerName,
            SteamId = s.SteamId,
            ScoreboardFlair = s.ScoreboardFlair,
            CrosshairCode = s.CrosshairCode
        }).ToArray()!;
    private static Legacy.BotHiderPresentationLeaseResult Convert(Current.BotHiderPresentationLeaseResult s)
        => new() { Ok = s.Ok, LeaseToken = s.LeaseToken, ProviderEpoch = s.ProviderEpoch, Reason = s.Reason, Slots = s.Slots };
    public Legacy.BotHiderPresentationLeaseResult AcquirePresentationLease(string owner, Legacy.BotHiderPresentationOverride[] overrides, CancellationToken ownerLifetime)
        => Convert(service.AcquirePresentationLease(owner, Convert(overrides), ownerLifetime));
    public Legacy.BotHiderPresentationLeaseResult ReplacePresentationLease(string leaseToken, Legacy.BotHiderPresentationOverride[] overrides)
        => Convert(service.ReplacePresentationLease(leaseToken, Convert(overrides)));
    public bool ReleasePresentationLease(string leaseToken) => service.ReleasePresentationLease(leaseToken);
    public int ReleasePresentationLeasesByOwner(string owner) => service.ReleasePresentationLeasesByOwner(owner);
    public Legacy.BotHiderDiagnostics GetDiagnostics()
    {
        var s = service.GetDiagnostics();
        return new()
        {
            Connected = s.Connected,
            ManagedSlots = s.ManagedSlots,
            ActiveLeases = s.ActiveLeases,
            LeasedSlots = s.LeasedSlots,
            RevokedLeases = s.RevokedLeases,
            PublishedWrites = s.PublishedWrites,
            ControllerRepairs = s.ControllerRepairs,
            Signatures = s.Signatures
        };
    }
}
