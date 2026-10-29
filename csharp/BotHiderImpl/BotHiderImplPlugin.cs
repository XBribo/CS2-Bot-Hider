using BotHiderApi;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Attributes.Registration;
using CounterStrikeSharp.API.Core.Capabilities;
using CounterStrikeSharp.API.Modules.Memory;
using CounterStrikeSharp.API.Modules.Timers;
using HarmonyLib;

namespace BotHiderImpl;

public partial class BotHiderImplPlugin : BasePlugin
{
    public override string ModuleName => "BotHiderImpl";
    public override string ModuleVersion => PluginBuildInfo.DisplayVersion;
    public override string ModuleAuthor => "XBribo";
    public override string ModuleDescription =>
        "BotHider CSS Plugin";

    public static PluginCapability<IBotHiderApi> Capability { get; } =
        new("bothider:api");

    private BridgeClient? _client;
    private IBotHiderApi? _api;
    private readonly string[] _appliedCrosshair = new string[64];
    private readonly uint[] _appliedScoreboardFlair = new uint[64];
    private readonly ulong[] _observedIncarnations = new ulong[64];
    private readonly bool[] _pendingName = new bool[64];
    private readonly bool[] _pendingSid = new bool[64];
    private readonly bool[] _pendingScoreboardFlair = new bool[64];
    private readonly ClanTagPresenter _clanTags = new();
    private Harmony? _harmony;
    private int _presentationGeneration;

    public override void Load(bool hotReload)
    {
        Server.PrintToConsole($"[BotHider] {PluginBuildInfo.DisplayVersion}, built {PluginBuildInfo.BuildTime}");
        LoadCommandPermissions();
        // Bind scoreboard updates.
        _client = new BridgeClient(
            Path.Combine(Server.GameDirectory, "addons", "BotHider", "bin",
                OperatingSystem.IsWindows() ? "win64" : "linuxsteamrt64",
                OperatingSystem.IsWindows() ? "BotHider.dll" : "BotHider.so"),
            ApplyVisibleName,
            ApplyVisibleSid,
            ApplyVisibleScoreboardFlair,
            ApplyVisibleCrosshair);
        if (!_client.TryConnect())
        {
            _client.Dispose();
            _client = null;
            throw new InvalidOperationException("Load the matching BotHider plugin before BotHiderImpl.");
        }
        try
        {
            _api = new BotHiderCapabilityApi(_client);
            BotHiderProvider.Attach(_api, supplier => Capabilities.RegisterPluginCapability(Capability, supplier));
            IsBotPatch.Api = _client;
            _harmony = new Harmony("net.linyz.bothider.isbot");
            _harmony.PatchAll(typeof(BotHiderImplPlugin).Assembly);

            RegisterListener<Listeners.OnMapStart>(OnMapStart);
            RegisterListener<Listeners.OnMapEnd>(OnMapEnd);
            RegisterListener<Listeners.OnClientDisconnect>(OnClientDisconnect);
            AddTimer(2.0f, ApplyManagedSlots, TimerFlags.REPEAT);
            ApplyManagedSlots();
        }
        catch
        {
            Unload(hotReload);
            throw;
        }
    }

    public override void Unload(bool hotReload)
    {
        try
        {
            ResetAppliedState();
            _harmony?.UnpatchAll(_harmony.Id);
        }
        finally
        {
            _harmony = null;
            IsBotPatch.Api = null;
            BotHiderProvider.Detach(_api);
            _api = null;
            _client?.Dispose();
            _client = null;
        }
    }

    // Reset presentation on map start.
    private void OnMapStart(string mapName)
    {
        ResetAppliedState();
        ApplyManagedSlots();
    }

    // Clear presentation on map end.
    private void OnMapEnd()
    {
        ResetAppliedState();
    }

    // Clear the disconnected slot's presentation.
    private void OnClientDisconnect(int slot)
    {
        ResetAppliedSlot(slot, 0UL);
    }

    // Reapply bot presentation at round start.
    [GameEventHandler]
    public HookResult OnRoundStart(EventRoundStart @event, GameEventInfo info)
    {
        ApplyManagedSlots();
        return HookResult.Continue;
    }

    // Apply presentation on full connect.
    [GameEventHandler]
    public HookResult OnPlayerConnectFull(EventPlayerConnectFull @event, GameEventInfo info)
    {
        var player = @event.Userid;
        if (player != null && player.IsValid) ApplyManagedSlot(player.Slot);
        return HookResult.Continue;
    }

    // Restores presentation after spawn.
    [GameEventHandler]
    public HookResult OnPlayerSpawn(EventPlayerSpawn @event, GameEventInfo info)
    {
        var player = @event.Userid;
        if (player != null && player.IsValid) ApplyManagedSlot(player.Slot);
        return HookResult.Continue;
    }

    // Reapply presentation after death.
    [GameEventHandler]
    public HookResult OnPlayerDeath(EventPlayerDeath @event, GameEventInfo info)
    {
        var player = @event.Userid;
        if (player != null && player.IsValid) ApplyManagedSlot(player.Slot);
        return HookResult.Continue;
    }

    // Bind queued writes to the bot's lifetime.
    private void QueueVisibleWrite(int slot, ulong incarnation, string field,
                                   Action<CCSPlayerController, BridgeClient.SlotSnapshot> write)
    {
        var client = _client;
        int generation = Volatile.Read(ref _presentationGeneration);
        if (client == null || incarnation == 0) return;
        Server.NextFrame(() =>
        {
            try
            {
                if (!ReferenceEquals(_client, client) || Volatile.Read(ref _presentationGeneration) != generation ||
                    !client.ReadSlot(slot, out var snapshot) || snapshot.Managed == 0 || snapshot.Incarnation != incarnation) return;
                if (_observedIncarnations[slot] != incarnation)
                    ResetAppliedSlot(slot, incarnation);
                var player = Utilities.GetPlayerFromSlot(slot);
                if (player == null || !player.IsValid) return;
                write(player, snapshot);
            }
            catch (Exception e)
            {
                Server.PrintToConsole($"[BotHider] {field} write failed slot={slot}: {e.Message}");
            }
        });
    }

    // Queues the visible name.
    private void ApplyVisibleName(int slot, string name, ulong incarnation)
        => QueueVisibleWrite(slot, incarnation, "name", (player, _) => WriteVisibleName(slot, player, name));

    // Read SteamID after lifetime validation.
    private void ApplyVisibleSid(int slot, ulong incarnation)
    {
        QueueVisibleWrite(slot, incarnation, "SteamID", (player, snapshot) =>
        {
            ulong sid = snapshot.SteamId;
            if (_pendingSid[slot] || player.SteamID != sid) WriteVisibleSid(slot, player, sid);
        });
    }

    // Queues the visible crosshair.
    private void ApplyVisibleCrosshair(int slot, string code, ulong incarnation)
    {
        QueueVisibleWrite(slot, incarnation, "crosshair", (player, _) =>
        {
            WriteVisibleCrosshair(player, code);
            _appliedCrosshair[slot] = code;
        });
    }

    // Queues the visible scoreboard flair.
    private void ApplyVisibleScoreboardFlair(int slot, uint itemDefIndex, ulong incarnation)
    {
        QueueVisibleWrite(slot, incarnation, "scoreboard flair", (player, _) =>
        {
            if (TryApplyScoreboardFlair(slot, player, itemDefIndex))
                _appliedScoreboardFlair[slot] = itemDefIndex;
        });
    }

    // Writes and publishes the name.
    private void WriteVisibleName(int slot, CCSPlayerController player, string name)
    {
        _pendingName[slot] = true;
        player.PlayerName = name;
        Utilities.SetStateChanged(player, "CBasePlayerController", "m_iszPlayerName");
        _pendingName[slot] = false;
    }

    // Writes and publishes the SteamID.
    private void WriteVisibleSid(int slot, CCSPlayerController player, ulong sid)
    {
        _pendingSid[slot] = true;
        Schema.SetSchemaValue(player.Handle, "CBasePlayerController", "m_steamID", sid);
        Utilities.SetStateChanged(player, "CBasePlayerController", "m_steamID");
        _pendingSid[slot] = false;
    }

    // Writes and publishes the crosshair.
    private static void WriteVisibleCrosshair(CCSPlayerController player, string code)
    {
        player.CrosshairCodes = code;
        Utilities.SetStateChanged(player, "CCSPlayerController", "m_szCrosshairCodes");
    }

    // Invalidate queued writes and clear caches.
    private void ResetAppliedState()
    {
        Interlocked.Increment(ref _presentationGeneration);
        for (int slot = 0; slot < _observedIncarnations.Length; slot++)
            ResetAppliedSlot(slot, 0UL);
    }

    // Reset presentation for a slot lifetime.
    private void ResetAppliedSlot(int slot, ulong incarnation)
    {
        if (slot < 0 || slot >= _observedIncarnations.Length) return;
        try
        {
            _clanTags.Restore(slot);
        }
        catch (Exception e)
        {
            Server.PrintToConsole($"[BotHider] presentation reset failed slot={slot}: {e.Message}");
        }
        _observedIncarnations[slot] = incarnation;
        _pendingName[slot] = false;
        _pendingSid[slot] = false;
        _pendingScoreboardFlair[slot] = false;
        _appliedCrosshair[slot] = string.Empty;
        _appliedScoreboardFlair[slot] = 0U;
    }

    // Reconcile slots and clear released state.
    private void ApplyManagedSlots()
    {
        if (_client == null) return;
        for (int slot = 0; slot < _observedIncarnations.Length; slot++)
            ApplyManagedSlot(slot);
    }

    // Reconcile one managed bot.
    private void ApplyManagedSlot(int slot)
    {
        var client = _client;
        if (client == null || slot < 0 || slot >= _observedIncarnations.Length) return;
        try
        {
            if (!client.ReadSlot(slot, out var snapshot) || snapshot.Managed == 0)
            {
                ResetAppliedSlot(slot, 0UL);
                return;
            }

            ulong incarnation = snapshot.Incarnation;
            if (_observedIncarnations[slot] != incarnation)
                ResetAppliedSlot(slot, incarnation);
            if (incarnation == 0) return;

            var player = Utilities.GetPlayerFromSlot(slot);
            if (player == null || !player.IsValid) return;

            ReconcileVisibleIdentity(snapshot, slot, player);

            int ping = snapshot.Ping;
            if (ping > 0)
            {
                try
                {
                    Schema.SetSchemaValue(player.Handle, "CCSPlayerController", "m_iPing", ping);
                }
                catch (Exception e)
                {
                    Server.PrintToConsole($"[BotHider] ping write failed slot={slot}: {e.Message}");
                }
            }

            string cross = snapshot.CrosshairCode;
            if (_appliedCrosshair[slot] != cross ||
                !string.Equals(player.CrosshairCodes, cross, StringComparison.Ordinal))
            {
                try
                {
                    WriteVisibleCrosshair(player, cross);
                    _appliedCrosshair[slot] = cross;
                }
                catch (Exception e)
                {
                    Server.PrintToConsole($"[BotHider] crosshair write failed slot={slot}: {e.Message}");
                }
            }

            uint flair = snapshot.ScoreboardFlair;
            if (_pendingScoreboardFlair[slot] || _appliedScoreboardFlair[slot] != flair ||
                !ScoreboardFlairMatches(player, flair))
            {
                if (TryApplyScoreboardFlair(slot, player, flair))
                    _appliedScoreboardFlair[slot] = flair;
            }

            _clanTags.Apply(slot, player, snapshot.Clan);
        }
        catch (Exception e)
        {
            Server.PrintToConsole($"[BotHider] presentation update failed slot={slot}: {e.Message}");
        }
    }

    // Sync the controller's published name and SteamID.
    private void ReconcileVisibleIdentity(BridgeClient.SlotSnapshot snapshot, int slot, CCSPlayerController player)
    {
        string name = snapshot.PersonaName;
        try
        {
            if (_pendingName[slot] || !string.Equals(player.PlayerName, name, StringComparison.Ordinal))
                WriteVisibleName(slot, player, name);
        }
        catch (Exception e)
        {
            Server.PrintToConsole($"[BotHider] name sync failed slot={slot}: {e.Message}");
        }

        ulong steamId = snapshot.SteamId;
        try
        {
            if (_pendingSid[slot] || player.SteamID != steamId) WriteVisibleSid(slot, player, steamId);
        }
        catch (Exception e)
        {
            Server.PrintToConsole($"[BotHider] SteamID sync failed slot={slot}: {e.Message}");
        }
    }

    // Check all flair ranks.
    private static bool ScoreboardFlairMatches(CCSPlayerController player, uint itemDefIndex)
    {
        var inventory = player.InventoryServices;
        if (inventory == null) return false;
        var ranks = inventory.Rank;
        if (ranks.Length == 0) return false;
        for (int index = 0; index < ranks.Length; index++)
        {
            if ((uint)ranks[index] != itemDefIndex) return false;
        }
        return true;
    }

    // Apply flair and notify the rank array once.
    private bool TryApplyScoreboardFlair(int slot, CCSPlayerController player, uint itemDefIndex)
    {
        try
        {
            var inventory = player.InventoryServices;
            if (inventory == null) return false;
            var ranks = inventory.Rank;
            if (ranks.Length == 0) return false;
            var flair = (MedalRank_t)itemDefIndex;
            for (int i = 0; i < ranks.Length; i++)
            {
                if (ranks[i] == flair) continue;
                _pendingScoreboardFlair[slot] = true;
                ranks[i] = flair;
            }
            if (_pendingScoreboardFlair[slot])
            {
                int rankOffset = Schema.GetSchemaOffset("CCSPlayerController_InventoryServices", "m_rank");
                NativeAPI.SchemaNetworkStateChanged(inventory.__m_pChainEntity.Handle,
                    (uint)rankOffset, uint.MaxValue, uint.MaxValue);
                _pendingScoreboardFlair[slot] = false;
            }
            return true;
        }
        catch (Exception e)
        {
            Server.PrintToConsole($"[BotHider] flair write failed slot={slot}: {e.Message}");
            return false;
        }
    }

}

internal sealed class BotHiderCapabilityApi : IBotHiderApi
{
    private readonly BridgeClient _client;

    public BotHiderCapabilityApi(BridgeClient client)
    {
        _client = client;
    }

    // Check slot ownership.
    public bool IsManagedBot(int slot) => _client.IsManagedBot(slot);

    // Read the synthetic SteamID64.
    public ulong GetBotSteamId(int slot) => _client.GetBotSteamId(slot);

    // List managed engine slots.
    public int[] GetManagedSlots() => _client.GetManagedSlots();

    // Read the persona name.
    public string GetPersonaName(int slot) => _client.GetPersonaName(slot);

    // Read the visible ping.
    public int GetPing(int slot) => _client.GetPing(slot);

    // Read the crosshair code.
    public string GetCrosshairCode(int slot) => _client.GetCrosshairCode(slot);

    // Check avatar application.
    public bool HasBotAvatar(int slot) => _client.HasBotAvatar(slot);

    // Read the flair item definition index.
    public uint GetScoreboardFlair(int slot) => _client.GetScoreboardFlair(slot);

    // Read resolved signatures.
    public (string Name, ulong Addr)[] GetSignatures() => _client.GetSignatures();

    // Set a managed bot's SteamID64.
    public bool SetBotSteamId(int slot, ulong steamId64) =>
        _client.SetBotSteamId(slot, steamId64);

    // Set the visible name through its callback.
    public bool SetPersonaName(int slot, string name) =>
        _client.SetPersonaName(slot, name);

    // Set flair through the rank writer.
    public bool SetScoreboardFlair(int slot, uint itemDefIndex) =>
        _client.SetScoreboardFlair(slot, itemDefIndex);

    // Set crosshair; empty or "0" clears it.
    public bool SetCrosshairCode(int slot, string code) =>
        _client.SetCrosshairCode(slot, code);

    // Apply a PNG avatar; "0" clears it.
    public bool SetBotAvatar(int slot, string pngPath) =>
        _client.SetBotAvatar(slot, pngPath);

    // Set the global identity mode.
    public bool SetIdentityMode(BotIdentityMode mode) => _client.SetIdentityMode(mode);

    // Set the global name source.
    public bool SetNameSource(bool useBotInfo) => _client.SetNameSource(useBotInfo);
}
