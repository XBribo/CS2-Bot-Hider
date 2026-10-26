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

    private SharedMemoryClient? _client;
    private IBotHiderApi? _api;
    private readonly string[] _appliedCrosshair = new string[64];
    private readonly uint[] _appliedScoreboardFlair = new uint[64];
    private readonly ulong[] _observedIncarnations = new ulong[64];
    private readonly ClanTagPresenter _clanTags = new();
    private Harmony? _harmony;
    private int _presentationGeneration;

    public override void Load(bool hotReload)
    {
        Server.PrintToConsole($"[BotHider] {PluginBuildInfo.DisplayVersion}, built {PluginBuildInfo.BuildTime}");
        LoadCommandPermissions();
        // Bind scoreboard updates.
        _client = new SharedMemoryClient(
            ApplyVisibleName,
            ApplyVisibleSid,
            ApplyVisibleScoreboardFlair,
            ApplyVisibleCrosshair);
        _api = new BotHiderCapabilityApi(_client);
        _client.TryConnect();
        Capabilities.RegisterPluginCapability(Capability, () => _api);

        // IsBot override
        IsBotPatch.Api = _client;
        _harmony = new Harmony("net.linyz.bothider.isbot");
        _harmony.PatchAll(typeof(BotHiderImplPlugin).Assembly);

        RegisterListener<Listeners.OnMapStart>(OnMapStart);
        RegisterListener<Listeners.OnMapEnd>(OnMapEnd);
        RegisterListener<Listeners.OnClientDisconnect>(OnClientDisconnect);
        AddTimer(2.0f, ApplyManagedSlots, TimerFlags.REPEAT);
        ApplyManagedSlots();
    }

    public override void Unload(bool hotReload)
    {
        ResetAppliedState();
        // Unpatch before releasing the client.
        _harmony?.UnpatchAll(_harmony.Id);
        _harmony = null;
        IsBotPatch.Api = null;
        _api = null;
        _client?.Dispose();
        _client = null;
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
    private void QueueVisibleWrite(int slot, ulong incarnation, string field, Action<CCSPlayerController> write)
    {
        var client = _client;
        int generation = Volatile.Read(ref _presentationGeneration);
        if (client == null || incarnation == 0) return;
        Server.NextFrame(() =>
        {
            try
            {
                if (!ReferenceEquals(_client, client) || Volatile.Read(ref _presentationGeneration) != generation ||
                    !client.IsManagedBot(slot) || client.GetSlotIncarnation(slot) != incarnation) return;
                var player = Utilities.GetPlayerFromSlot(slot);
                if (player == null || !player.IsValid) return;
                write(player);
            }
            catch (Exception e)
            {
                Server.PrintToConsole($"[BotHider] {field} write failed slot={slot}: {e.Message}");
            }
        });
    }

    // Queues the visible name.
    private void ApplyVisibleName(int slot, string name, ulong incarnation)
        => QueueVisibleWrite(slot, incarnation, "name", player => WriteVisibleName(player, name));

    // Read SteamID after lifetime validation.
    private void ApplyVisibleSid(int slot, ulong incarnation)
    {
        var client = _client;
        if (client == null) return;
        QueueVisibleWrite(slot, incarnation, "SteamID", player =>
        {
            ulong sid = client.GetBotSteamId(slot);
            if (player.SteamID != sid) WriteVisibleSid(player, sid);
        });
    }

    // Queues the visible crosshair.
    private void ApplyVisibleCrosshair(int slot, string code, ulong incarnation)
    {
        QueueVisibleWrite(slot, incarnation, "crosshair", player =>
        {
            WriteVisibleCrosshair(player, code);
            _appliedCrosshair[slot] = code;
        });
    }

    // Queues the visible scoreboard flair.
    private void ApplyVisibleScoreboardFlair(int slot, uint itemDefIndex, ulong incarnation)
    {
        QueueVisibleWrite(slot, incarnation, "scoreboard flair", player =>
        {
            if (TryApplyScoreboardFlair(slot, player, itemDefIndex))
                _appliedScoreboardFlair[slot] = itemDefIndex;
        });
    }

    // Writes and publishes the name.
    private static void WriteVisibleName(CCSPlayerController player, string name)
    {
        player.PlayerName = name;
        Utilities.SetStateChanged(player, "CBasePlayerController", "m_iszPlayerName");
    }

    // Writes and publishes the SteamID.
    private static void WriteVisibleSid(CCSPlayerController player, ulong sid)
    {
        Schema.SetSchemaValue(player.Handle, "CBasePlayerController", "m_steamID", sid);
        Utilities.SetStateChanged(player, "CBasePlayerController", "m_steamID");
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
        _clanTags.Restore(slot);
        _observedIncarnations[slot] = incarnation;
        _appliedCrosshair[slot] = string.Empty;
        _appliedScoreboardFlair[slot] = 0U;
    }

    // Reconcile slots and clear released state.
    private void ApplyManagedSlots()
    {
        if (_client == null) return;
        int[] managedSlots = _client.GetManagedSlots();
        Span<bool> managed = stackalloc bool[64];
        managed.Clear();
        foreach (int slot in managedSlots)
            managed[slot] = true;
        for (int slot = 0; slot < managed.Length; slot++)
        {
            if (managed[slot]) continue;
            ResetAppliedSlot(slot, 0UL);
        }

        foreach (int slot in managedSlots)
            ApplyManagedSlot(slot);
    }

    // Reconcile one managed bot.
    private void ApplyManagedSlot(int slot)
    {
        var client = _client;
        if (client == null || slot < 0 || slot >= _observedIncarnations.Length) return;
        if (!client.IsManagedBot(slot))
        {
            ResetAppliedSlot(slot, 0UL);
            return;
        }

        ulong incarnation = client.GetSlotIncarnation(slot);
        if (_observedIncarnations[slot] != incarnation)
            ResetAppliedSlot(slot, incarnation);
        if (incarnation == 0) return;

        var player = Utilities.GetPlayerFromSlot(slot);
        if (player == null || !player.IsValid) return;

        ReconcileVisibleIdentity(client, slot, player);

        int ping = client.GetPing(slot);
        if (ping > 0)
        {
            try
            {
                // Server-side field.
                Schema.SetSchemaValue(player.Handle, "CCSPlayerController", "m_iPing", ping);
            }
            catch (Exception e)
            {
                Server.PrintToConsole($"[BotHider] ping write failed slot={slot}: {e.Message}");
            }
        }

        string cross = client.GetCrosshairCode(slot);
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

        uint flair = client.GetScoreboardFlair(slot);
        if (_appliedScoreboardFlair[slot] != flair ||
            !ScoreboardFlairMatches(player, flair))
        {
            if (TryApplyScoreboardFlair(slot, player, flair))
                _appliedScoreboardFlair[slot] = flair;
        }

        _clanTags.Apply(slot, player, client.GetClan(slot));
    }

    // Sync the controller's published name and SteamID.
    private static void ReconcileVisibleIdentity(SharedMemoryClient client, int slot,
                                                 CCSPlayerController player)
    {
        string name = client.GetPersonaName(slot);
        if (!string.Equals(player.PlayerName, name, StringComparison.Ordinal))
        {
            WriteVisibleName(player, name);
        }

        ulong steamId = client.GetBotSteamId(slot);
        if (player.SteamID == steamId) return;
        try
        {
            WriteVisibleSid(player, steamId);
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
    private static bool TryApplyScoreboardFlair(int slot, CCSPlayerController player, uint itemDefIndex)
    {
        try
        {
            var inventory = player.InventoryServices;
            if (inventory == null) return false;
            var ranks = inventory.Rank;
            if (ranks.Length == 0) return false;
            var flair = (MedalRank_t)itemDefIndex;
            bool changed = false;
            for (int i = 0; i < ranks.Length; i++)
            {
                if (ranks[i] == flair) continue;
                ranks[i] = flair;
                changed = true;
            }
            if (changed)
            {
                int rankOffset = Schema.GetSchemaOffset("CCSPlayerController_InventoryServices", "m_rank");
                NativeAPI.SchemaNetworkStateChanged(inventory.__m_pChainEntity.Handle,
                    (uint)rankOffset, uint.MaxValue, uint.MaxValue);
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
    private readonly SharedMemoryClient _client;

    public BotHiderCapabilityApi(SharedMemoryClient client)
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

    // Check native avatar application.
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
