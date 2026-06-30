using BotHiderApi;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Attributes.Registration;
using CounterStrikeSharp.API.Core.Capabilities;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.Memory;
using CounterStrikeSharp.API.Modules.Timers;
using CounterStrikeSharp.API.Modules.Utils;
using HarmonyLib;

namespace BotHiderImpl;

public class BotHiderImplPlugin : BasePlugin
{
    public override string ModuleName => "BotHiderImpl";
    public override string ModuleVersion => "0.2.7";
    public override string ModuleAuthor => "XBribo";
    public override string ModuleDescription =>
        "BotHider CSS Plugin";

    public static PluginCapability<IBotHiderApi> Capability { get; } =
        new("bothider:api");

    private SharedMemoryClient? _client;
    private IBotHiderApi? _api;
    private readonly string[] _appliedCrosshair = new string[64];
    private Harmony? _harmony;

    public override void Load(bool hotReload)
    {
        // Inject the visible-write actions so SetPersonaName / SetBotSteamId
        // also update the scoreboard
        _client = new SharedMemoryClient(ApplyVisibleName, ApplyVisibleSid);
        _api = new BotHiderCapabilityApi(_client);
        _client.TryConnect();
        Capabilities.RegisterPluginCapability(Capability, () => _api);

        // IsBot override
        IsBotPatch.Api = _client;
        _harmony = new Harmony("net.linyz.bothider.isbot");
        _harmony.PatchAll(typeof(BotHiderImplPlugin).Assembly);

        AddTimer(2.0f, ApplyManagedSlots, TimerFlags.REPEAT);
    }

    public override void Unload(bool hotReload)
    {
        // Undo the patch first
        _harmony?.UnpatchAll(_harmony.Id);
        _harmony = null;
        IsBotPatch.Api = null;
        _api = null;
        _client?.Dispose();
    }

    // Match end
    [GameEventHandler]
    public HookResult OnWinPanelMatch(EventCsWinPanelMatch @event, GameEventInfo info)
    {
        return HookResult.Continue;
    }

    // Round start — respawn managed bots that ended the prior round dead.
    [GameEventHandler]
    public HookResult OnRoundStart(EventRoundStart @event, GameEventInfo info)
    {
        AddTimer(0.3f, RespawnDeadManagedBots);
        return HookResult.Continue;
    }

    // Respawn any managed bot that is not alive
    private void RespawnDeadManagedBots()
    {
        if (_client == null) return;

        // Current team headcount across everyone, for balancing unassigned bots
        int tCount = 0, ctCount = 0;
        foreach (var pl in Utilities.GetPlayers())
        {
            if (pl == null || !pl.IsValid) continue;
            if (pl.Team == CsTeam.Terrorist) ++tCount;
            else if (pl.Team == CsTeam.CounterTerrorist) ++ctCount;
        }

        foreach (int slot in _client.GetManagedSlots())
        {
            var player = Utilities.GetPlayerFromSlot(slot);
            if (player == null || !player.IsValid || player.PawnIsAlive) continue;

            // Dead but unassigned (team=None/Spectator): give it the smaller team first
            if (player.Team != CsTeam.Terrorist && player.Team != CsTeam.CounterTerrorist)
            {
                CsTeam target = (tCount <= ctCount) ? CsTeam.Terrorist : CsTeam.CounterTerrorist;
                try
                {
                    player.SwitchTeam(target);
                    if (target == CsTeam.Terrorist) ++tCount; else ++ctCount;
                }
                catch (Exception e)
                {
                    Server.PrintToConsole($"[BotHider] SwitchTeam failed slot={slot}: {e.Message}");
                    continue;
                }
            }

            try
            {
                player.Respawn();
            }
            catch (Exception e)
            {
                Server.PrintToConsole($"[BotHider] respawn failed slot={slot}: {e.Message}");
            }
        }
    }

    // Set CCSPlayerController.m_iszPlayerName
    private static void ApplyVisibleName(int slot, string name)
    {
        Server.NextFrame(() =>
        {
            var player = Utilities.GetPlayerFromSlot(slot);
            if (player == null || !player.IsValid) return;
            player.PlayerName = name;
            Utilities.SetStateChanged(player, "CBasePlayerController", "m_iszPlayerName");
        });
    }

    // Write CBasePlayerController.m_steamID
    private static void ApplyVisibleSid(int slot, ulong sid)
    {
        Server.NextFrame(() =>
        {
            var player = Utilities.GetPlayerFromSlot(slot);
            if (player == null || !player.IsValid) return;
            try
            {
                Schema.SetSchemaValue(player.Handle, "CBasePlayerController", "m_steamID", sid);
                Utilities.SetStateChanged(player, "CBasePlayerController", "m_steamID");
            }
            catch (Exception e)
            {
                Server.PrintToConsole($"[BotHider] m_steamID write failed slot={slot}: {e.Message}");
            }
        });
    }

    // Timer body
    private void ApplyManagedSlots()
    {
        if (_client == null) return;
        foreach (int slot in _client.GetManagedSlots())
        {
            var player = Utilities.GetPlayerFromSlot(slot);
            if (player == null || !player.IsValid) continue;

            int ping = _client.GetPing(slot);
            if (ping > 0)
            {
                try
                {
                    // m_iPing not networked: write the field only, no SetStateChanged
                    Schema.SetSchemaValue(player.Handle, "CCSPlayerController", "m_iPing", ping);
                }
                catch (Exception e)
                {
                    Server.PrintToConsole($"[BotHider] m_iPing write failed slot={slot}: {e.Message}");
                }
            }

            string cross = _client.GetCrosshairCode(slot);
            if (!string.IsNullOrEmpty(cross) && _appliedCrosshair[slot] != cross)
            {
                try
                {
                    // m_szCrosshairCodes not networked: write the field only, no SetStateChanged
                    player.CrosshairCodes = cross;
                    _appliedCrosshair[slot] = cross;
                }
                catch (Exception e)
                {
                    Server.PrintToConsole($"[BotHider] crosshair write failed slot={slot}: {e.Message}");
                }
            }
        }
    }

    // bh_status — dump every managed slot's state
    [ConsoleCommand("bh_status", "List all BotHider-managed slots")]
    public void OnStatus(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_client == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        // Hook/sig resolution line: ok only if every signature resolved
        var sigs = _client.GetSignatures();
        if (sigs.Length > 0)
        {
            bool allOk = sigs.All(s => s.Addr != 0);
            string detail = string.Join(" ", sigs.Select(s => $"{s.Name}={s.Addr:X16}"));
            cmd.ReplyToCommand($"[BotHider] hooks: {(allOk ? "ok" : "FAIL")} | {detail}");
        }
        var slots = _client.GetManagedSlots();
        cmd.ReplyToCommand($"[BotHider] managed slots: {slots.Length}");
        foreach (int s in slots)
        {
            var p = Utilities.GetPlayerFromSlot(s);
            string isBot = (p != null && p.IsValid) ? p.IsBot.ToString() : "n/a";
            cmd.ReplyToCommand(
                $"  slot={s} sid={_client.GetSyntheticSteamId(s)} " +
                $"name='{_client.GetPersonaName(s)}' ping={_client.GetPing(s)} " +
                $"crosshair='{_client.GetCrosshairCode(s)}' isbot={isBot}");
        }
    }

    // bh_setsid <slot> <sid64> — set a bot's SteamID64
    [ConsoleCommand("bh_setsid", "Set a bot's SteamID64: bh_setsid <slot> <sid64>")]
    public void OnSetSid(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_client == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        if (cmd.ArgCount < 3 || !int.TryParse(cmd.GetArg(1), out int slot)
            || !ulong.TryParse(cmd.GetArg(2), out ulong sid))
        { cmd.ReplyToCommand("usage: bh_setsid <slot> <sid64>"); return; }
        bool ok = _client.SetBotSteamId(slot, sid);
        cmd.ReplyToCommand($"[BotHider] SetBotSteamId({slot},{sid}) -> {ok}");
    }

    // bh_setname <slot> <name> — set a bot's persona name
    [ConsoleCommand("bh_setname", "Set a bot's name: bh_setname <slot> <name>")]
    public void OnSetName(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_client == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        if (cmd.ArgCount < 3 || !int.TryParse(cmd.GetArg(1), out int slot))
        { cmd.ReplyToCommand("usage: bh_setname <slot> <name>"); return; }
        string name = cmd.GetArg(2);
        bool ok = _client.SetPersonaName(slot, name);
        cmd.ReplyToCommand($"[BotHider] SetPersonaName({slot},'{name}') -> {ok}");
    }

    // bh_disguise <0|1> — toggle the m_bFakePlayer disguise
    [ConsoleCommand("bh_disguise", "Toggle disguise: bh_disguise <0|1>")]
    public void OnDisguise(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_client == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        if (cmd.ArgCount < 2 || !int.TryParse(cmd.GetArg(1), out int v))
        { cmd.ReplyToCommand("usage: bh_disguise <0|1>"); return; }
        bool enabled = v != 0;
        bool ok = _client.SetDisguise(enabled);
        cmd.ReplyToCommand($"[BotHider] disguise -> {(enabled ? "ON" : "OFF")} ({ok})");
    }

    // bh_namesource <0|1> — 0=botprofile name (default), 1=bot_info.json name
    [ConsoleCommand("bh_namesource", "Set display-name source: bh_namesource <0|1> (0=botprofile 1=bot_info)")]
    public void OnNameSource(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_client == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        if (cmd.ArgCount < 2 || !int.TryParse(cmd.GetArg(1), out int v))
        { cmd.ReplyToCommand("usage: bh_namesource <0|1> (0=botprofile 1=bot_info)"); return; }
        bool useBotInfo = v != 0;
        bool ok = _client.SetNameSource(useBotInfo);
        cmd.ReplyToCommand($"[BotHider] name source -> {(useBotInfo ? "bot_info" : "botprofile")} ({ok})");
    }
}

internal sealed class BotHiderCapabilityApi : IBotHiderApi
{
    private readonly SharedMemoryClient _client;

    public BotHiderCapabilityApi(SharedMemoryClient client)
    {
        _client = client;
    }

    // Returns whether the slot is managed by BotHider.
    public bool IsManagedBot(int slot) => _client.IsManagedBot(slot);

    // Returns the current synthetic SteamID64 for the slot.
    public ulong GetSyntheticSteamId(int slot) => _client.GetSyntheticSteamId(slot);

    // Returns all managed engine slots.
    public int[] GetManagedSlots() => _client.GetManagedSlots();

    // Returns the current persona name for the slot.
    public string GetPersonaName(int slot) => _client.GetPersonaName(slot);

    // Returns the current visible ping for the slot.
    public int GetPing(int slot) => _client.GetPing(slot);

    // Returns the current crosshair code for the slot.
    public string GetCrosshairCode(int slot) => _client.GetCrosshairCode(slot);

    // Returns the resolved signature table.
    public (string Name, ulong Addr)[] GetSignatures() => _client.GetSignatures();

    // Updates the synthetic SteamID64 for a managed bot.
    public bool SetBotSteamId(int slot, ulong steamId64) =>
        _client.SetBotSteamId(slot, steamId64);

    // Updates the visible PlayerName through the existing callback path.
    public bool SetPersonaName(int slot, string name) =>
        _client.SetPersonaName(slot, name);

    // Toggles the global disguise behavior.
    public bool SetDisguise(bool enabled) => _client.SetDisguise(enabled);

    // Toggles the global display-name source behavior.
    public bool SetNameSource(bool useBotInfo) => _client.SetNameSource(useBotInfo);
}
