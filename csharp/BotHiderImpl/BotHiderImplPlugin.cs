using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Attributes.Registration;
using CounterStrikeSharp.API.Core.Capabilities;
using CounterStrikeSharp.API.Modules.Commands;
using BotHiderApi;
using HarmonyLib;

namespace BotHiderImpl;

public sealed partial class BotHiderImplPlugin : BasePlugin
{

    public override string ModuleName => "BotHiderImpl";
    public override string ModuleVersion => PluginBuildInfo.DisplayVersion;
    public override string ModuleAuthor => "XBribo contributors, unicbm";
    public override string ModuleDescription =>
        "Bot identity, presentation leases and avatars.";

    public static PluginCapability<IBotHiderApi> Capability { get; } =
        new(BotHiderContract.Capability);

    private BotHiderApiProvider? _api;
    private NativePresentationClient? _client;
    private BotHiderPresentationService? _presentation;
    private bool _applyPending;
    private bool _fullApplyPending;
    private ulong _pendingPingSlots;
    private bool _unloaded;
    private int _mapGeneration;
    private Harmony? _harmony;

    public override void Load(bool hotReload)
    {
        _unloaded = false;
        _client = new NativePresentationClient(OnNativePresentationChanged);
        _presentation = new BotHiderPresentationService(_client);
        _client.TryConnect();

        _api = new BotHiderApiProvider(_client, _presentation);
        Capabilities.RegisterPluginCapability(Capability, () => _api);
        IsBotPatch.Api = _api;
        _harmony = new Harmony("org.bothider.isbot");
        _harmony.PatchAll(typeof(BotHiderImplPlugin).Assembly);

        RegisterListener<Listeners.OnMapStart>(OnMapStart);
        RegisterListener<Listeners.OnMapEnd>(OnMapEnd);
        RegisterListener<Listeners.OnClientDisconnect>(OnClientDisconnect);
        ScheduleApply();
        Server.PrintToConsole(
            $"[BotHider] loaded api={BotHiderContract.ApiVersion} " +
            $"provider_epoch={_presentation.GetProviderInfo().ProviderEpoch} " +
            "crosshair_writer=networked_on_demand");
    }

    public override void Unload(bool hotReload)
    {
        _unloaded = true;
        _api = null;
        IsBotPatch.Api = null;
        _applyPending = false;
        _mapGeneration++;
        try
        {
            _harmony?.UnpatchAll(_harmony.Id);
        }
        finally
        {
            _harmony = null;
            try { _presentation?.Dispose(); }
            finally
            {
                _presentation = null;
                // Never leave a managed callback in native code after unload,
                // including when another cleanup step failed.
                try { _client?.ClearAvatarOverrides(); }
                finally
                {
                    try { _client?.Dispose(); }
                    finally
                    {
                        _client = null;
                        BotHiderContract.NotifyProviderChanged();
                    }
                }
            }
        }
    }

    public override void OnAllPluginsLoaded(bool hotReload)
    {
        _client?.TryConnect();
        ScheduleApply();
        BotHiderContract.NotifyProviderChanged();
    }

    [ConsoleCommand("bh_native_ready", "Rebind the native BotHider presentation lifecycle")]
    [CommandHelper(0, "", CommandUsage.SERVER_ONLY)]
    public void OnNativeReady(CCSPlayerController? player, CommandInfo command)
    {
        if (_client?.TryConnect() == true) OnNativePresentationChanged(1, -1);
    }

    private void OnNativePresentationChanged(uint reason, int slot)
    {
        if (_unloaded) return;
        if (reason == 1)
        {
            ScheduleApply();
            BotHiderContract.NotifyProviderChanged();
        }
        else if (reason == 2 && slot is >= 0 and < 64)
        {
            _pendingPingSlots |= 1UL << slot;
            SchedulePublication();
        }
    }

    private void OnMapStart(string mapName)
    {
        _applyPending = false;
        _fullApplyPending = false;
        _pendingPingSlots = 0;
        _mapGeneration++;
        _presentation?.ResetForMapBoundary();
        ScheduleApply();
    }

    private void OnMapEnd()
    {
        _applyPending = false;
        _fullApplyPending = false;
        _pendingPingSlots = 0;
        _mapGeneration++;
        _presentation?.ResetForMapBoundary();
    }

    private void OnClientDisconnect(int slot)
        => _presentation?.HandleClientDisconnect(slot);

    [GameEventHandler]
    public HookResult OnRoundStart(EventRoundStart @event, GameEventInfo info)
    {
        ScheduleApply();
        return HookResult.Continue;
    }

    [GameEventHandler]
    public HookResult OnPlayerConnectFull(EventPlayerConnectFull @event, GameEventInfo info)
    {
        if (@event.Userid is { IsValid: true } player)
            ScheduleApply();
        return HookResult.Continue;
    }

    [GameEventHandler]
    public HookResult OnPlayerSpawn(EventPlayerSpawn @event, GameEventInfo info)
    {
        if (@event.Userid is { IsValid: true } player)
            ScheduleApply();
        return HookResult.Continue;
    }

    [GameEventHandler]
    public HookResult OnPlayerDeath(EventPlayerDeath @event, GameEventInfo info)
    {
        if (@event.Userid is { IsValid: true } player)
            ScheduleApply();
        return HookResult.Continue;
    }

    private void ScheduleApply()
    {
        _fullApplyPending = true;
        SchedulePublication();
    }

    private void SchedulePublication()
    {
        if (_unloaded || _applyPending) return;
        _applyPending = true;
        var generation = _mapGeneration;
        Server.NextFrame(() =>
        {
            if (_unloaded || generation != _mapGeneration) return;
            _applyPending = false;
            var full = _fullApplyPending;
            var pingSlots = _pendingPingSlots;
            _fullApplyPending = false;
            _pendingPingSlots = 0;
            if (full) _presentation?.PublishManagedSlots();
            else
            {
                while (pingSlots != 0)
                {
                    var slot = System.Numerics.BitOperations.TrailingZeroCount(pingSlots);
                    pingSlots &= pingSlots - 1;
                    _presentation?.PublishPing(slot);
                }
            }
        });
    }

}
