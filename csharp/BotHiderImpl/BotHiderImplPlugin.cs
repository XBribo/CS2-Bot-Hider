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

    public static PluginCapability<IBotHiderPresentationApi> Capability { get; } =
        new(BotHiderPresentationContract.Capability);

    private LegacyBotHiderApi? _legacy;
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
        WarnIfLegacyBotHiderPluginIsPresent();
        _client = new NativePresentationClient(OnNativePresentationChanged);
        _presentation = new BotHiderPresentationService(_client);
        _client.TryConnect();
        Capabilities.RegisterPluginCapability(Capability, () => _presentation);

        _legacy = new LegacyBotHiderApi(_client, _presentation);
        Capabilities.RegisterPluginCapability(new PluginCapability<BotHiderApi.IBotHiderApi>("bothider:api"), () => _legacy);
        var compatibility = new DemoTracerCompatibilityApi(_presentation);
        Capabilities.RegisterPluginCapability(new PluginCapability<DemoTracerBotHiderApi.IBotHiderApi>(DemoTracerBotHiderApi.DemoTracerBotHiderContract.Capability), () => compatibility);
        BotHiderPresentationContract.ProviderChanged += DemoTracerBotHiderApi.DemoTracerBotHiderContract.NotifyProviderChanged;
        IsBotPatch.Api = _legacy;
        _harmony = new Harmony("org.bothider.isbot");
        _harmony.PatchAll(typeof(BotHiderImplPlugin).Assembly);

        RegisterListener<Listeners.OnMapStart>(OnMapStart);
        RegisterListener<Listeners.OnMapEnd>(OnMapEnd);
        RegisterListener<Listeners.OnClientDisconnect>(OnClientDisconnect);
        ScheduleApply();
        Server.PrintToConsole(
            $"[BotHider] loaded api={BotHiderPresentationContract.ApiVersion} " +
            $"provider_epoch={_presentation.GetProviderInfo().ProviderEpoch} " +
            "crosshair_writer=networked_on_demand");
    }

    public override void Unload(bool hotReload)
    {
        _unloaded = true;
        _legacy = null;
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
                _client?.Dispose();
                _client = null;
                BotHiderPresentationContract.NotifyProviderChanged();
                BotHiderPresentationContract.ProviderChanged -= DemoTracerBotHiderApi.DemoTracerBotHiderContract.NotifyProviderChanged;
            }
        }
    }

    public override void OnAllPluginsLoaded(bool hotReload)
    {
        _client?.TryConnect();
        ScheduleApply();
        BotHiderPresentationContract.NotifyProviderChanged();
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
            BotHiderPresentationContract.NotifyProviderChanged();
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

    private void WarnIfLegacyBotHiderPluginIsPresent()
    {
        try
        {
            var pluginsDirectory = Directory.GetParent(ModuleDirectory)?.FullName;
            if (string.IsNullOrWhiteSpace(pluginsDirectory))
                return;

            foreach (var legacyDirectoryName in new[] { "BotHiderImpl", "BotHider", "DemoTracerBotHider" })
            {
                var legacyDirectory = Path.Combine(pluginsDirectory, legacyDirectoryName);
                if (string.Equals(Path.GetFullPath(legacyDirectory).TrimEnd(Path.DirectorySeparatorChar),
                    Path.GetFullPath(ModuleDirectory).TrimEnd(Path.DirectorySeparatorChar), StringComparison.OrdinalIgnoreCase))
                    continue;
                if (!Directory.Exists(legacyDirectory) ||
                    !Directory.EnumerateFiles(legacyDirectory, "*.dll").Any())
                    continue;
                Server.PrintToConsole(
                    "[BotHider] ERROR: another BotHider CSS plugin directory is present: " +
                    $"{legacyDirectoryName}. Remove it before runtime testing; multiple presentation writers are unsupported.");
            }
        }
        catch (Exception ex)
        {
            Server.PrintToConsole(
                $"[BotHider] legacy plugin check failed: {ex.Message}");
        }
    }

    [GameEventHandler]
    public HookResult OnRoundStart(EventRoundStart @event, GameEventInfo info)
    {
        ScheduleApply();
        if (_client?.AutoRespawn == true)
        {
            var generation = _mapGeneration;
            AddTimer(0.3f, () => { if (!_unloaded && generation == _mapGeneration) RespawnDeadManagedBots(); }, CounterStrikeSharp.API.Modules.Timers.TimerFlags.STOP_ON_MAPCHANGE);
        }
        return HookResult.Continue;
    }

    [GameEventHandler]
    public HookResult OnPlayerConnectFull(EventPlayerConnectFull @event, GameEventInfo info)
    {
        if (@event.Userid is { IsValid: true } player)
            SchedulePresentationReconcile(player.Slot);
        return HookResult.Continue;
    }

    [GameEventHandler]
    public HookResult OnPlayerSpawn(EventPlayerSpawn @event, GameEventInfo info)
    {
        if (@event.Userid is { IsValid: true } player)
            SchedulePresentationReconcile(player.Slot);
        return HookResult.Continue;
    }

    [GameEventHandler]
    public HookResult OnPlayerDeath(EventPlayerDeath @event, GameEventInfo info)
    {
        if (@event.Userid is { IsValid: true } player)
            SchedulePresentationReconcile(player.Slot);
        return HookResult.Continue;
    }

    private void SchedulePresentationReconcile(int slot) => ScheduleApply();

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
