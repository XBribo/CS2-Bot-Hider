using BotHiderApi;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Attributes.Registration;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.Admin;
using CounterStrikeSharp.API.Modules.Utils;
namespace BotHiderImpl;

public sealed partial class BotHiderImplPlugin
{
    // Respawn any managed bot that is not alive
    private void RespawnDeadManagedBots()
    {
        if (_legacy == null) return;

        // Current team headcount across everyone, for balancing unassigned bots
        int tCount = 0, ctCount = 0;
        foreach (var pl in Utilities.GetPlayers())
        {
            if (pl == null || !pl.IsValid) continue;
            if (pl.Team == CsTeam.Terrorist) ++tCount;
            else if (pl.Team == CsTeam.CounterTerrorist) ++ctCount;
        }

        foreach (int slot in _legacy.GetManagedSlots())
        {
            if (_presentation?.IsLeased(slot) == true) continue;
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

    // bh_status — dump every managed slot's state
    [ConsoleCommand("bh_status", "List all BotHider-managed slots")]
    [CommandHelper(0, "", CommandUsage.CLIENT_AND_SERVER)]
    public void OnStatus(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_legacy == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        var diagnostics = _presentation!.GetDiagnostics();
        cmd.ReplyToCommand($"[BotHider] leases={diagnostics.ActiveLeases} writes={diagnostics.PublishedWrites} repairs={diagnostics.ControllerRepairs} userinfo_refreshes={_client!.UserInfoPublications} auto_respawn={_client.AutoRespawn} external_avatars={_client.ExternalAvatars}");
        // Hook/sig resolution line: ok only if every signature resolved
        var sigs = _legacy.GetSignatures();
        if (sigs.Length > 0)
        {
            bool allOk = sigs.All(s => s.Addr != 0);
            string detail = string.Join(" ", sigs.Select(s => $"{s.Name}={s.Addr:X16}"));
            cmd.ReplyToCommand($"[BotHider] hooks: {(allOk ? "ok" : "FAIL")} | {detail}");
        }
        var slots = _legacy.GetManagedSlots();
        cmd.ReplyToCommand($"[BotHider] managed slots: {slots.Length}");
        foreach (int s in slots)
        {
            var p = Utilities.GetPlayerFromSlot(s);
            string isBot = (p != null && p.IsValid) ? p.IsBot.ToString() : "n/a";
            cmd.ReplyToCommand(
                $"  slot={s} incarnation={_legacy.GetSlotIncarnation(s)} " +
                $"sid={_legacy.GetBotSteamId(s)}/{_legacy.GetBaseBotSteamId(s)} " +
                $"name='{_legacy.GetPersonaName(s)}'/'{_legacy.GetBasePersonaName(s)}' " +
                $"ping={_legacy.GetPing(s)} " +
                $"crosshair='{_legacy.GetCrosshairCode(s)}' " +
                $"avatar={_legacy.HasBotAvatar(s)}/{_legacy.GetConfiguredAvatarSize(s)}B " +
                $"isbot={isBot}");
        }
    }

    // bh_setsid <slot> <sid64> — set a bot's SteamID64
    [ConsoleCommand("bh_setsid", "Set a bot's SteamID64: bh_setsid <slot> <sid64>")]
    public void OnSetSid(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_legacy == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        if (cmd.ArgCount < 3 || !int.TryParse(cmd.GetArg(1), out int slot)
            || !ulong.TryParse(cmd.GetArg(2), out ulong sid))
        { cmd.ReplyToCommand("usage: bh_setsid <slot> <sid64>"); return; }
        bool ok = _legacy.SetBotSteamId(slot, sid);
        cmd.ReplyToCommand($"[BotHider] SetBotSteamId({slot},{sid}) -> {ok}");
    }

    // bh_setname <slot> <name> — set a bot's persona name
    [ConsoleCommand("bh_setname", "Set a bot's name: bh_setname <slot> <name>")]
    public void OnSetName(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_legacy == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        if (cmd.ArgCount < 3 || !int.TryParse(cmd.GetArg(1), out int slot))
        { cmd.ReplyToCommand("usage: bh_setname <slot> <name>"); return; }
        string name = cmd.GetArg(2);
        bool ok = _legacy.SetPersonaName(slot, name);
        string appliedName = ok ? _legacy.GetPersonaName(slot) : name;
        cmd.ReplyToCommand($"[BotHider] SetPersonaName({slot},'{appliedName}') -> {ok}");
    }

    // bh_setflair <slot> <item_def_index> — set a bot's scoreboard flair
    [ConsoleCommand("bh_setflair", "Set a bot's scoreboard flair: bh_setflair <slot> <item_def_index>")]
    public void OnSetFlair(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_legacy == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        if (cmd.ArgCount < 3 || !int.TryParse(cmd.GetArg(1), out int slot)
            || !uint.TryParse(cmd.GetArg(2), out uint itemDefIndex))
        { cmd.ReplyToCommand("usage: bh_setflair <slot> <item_def_index>"); return; }
        bool ok = _legacy.SetScoreboardFlair(slot, itemDefIndex);
        cmd.ReplyToCommand($"[BotHider] SetScoreboardFlair({slot},{itemDefIndex}) -> {ok}");
    }

    // bh_setcrosshair <slot> <code> — set a bot's crosshair code
    [ConsoleCommand("bh_setcrosshair", "Set a bot's crosshair: bh_setcrosshair <slot> <code>")]
    public void OnSetCrosshair(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_legacy == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        if (cmd.ArgCount < 3 || !int.TryParse(cmd.GetArg(1), out int slot))
        { cmd.ReplyToCommand("usage: bh_setcrosshair <slot> <code>"); return; }
        string code = cmd.GetArg(2);
        bool ok = _legacy.SetCrosshairCode(slot, code);
        cmd.ReplyToCommand($"[BotHider] SetCrosshairCode({slot},'{code}') -> {ok}");
    }

    // bh_setavatar <slot> <png_path|0> applies or clears a custom avatar
    [ConsoleCommand("bh_setavatar", "Set a bot avatar: bh_setavatar <slot> <png_path|0>")]
    [CommandHelper(2, "<slot> <png_path|0>", CommandUsage.CLIENT_AND_SERVER)]
    [RequiresPermissions("@css/root")]
    public void OnSetAvatar(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_legacy == null)
        {
            cmd.ReplyToCommand("[BotHider] not initialized");
            return;
        }
        if (cmd.ArgCount < 3 || !int.TryParse(cmd.GetArg(1), out int slot))
        {
            cmd.ReplyToCommand("usage: bh_setavatar <slot> <png_path|0>");
            return;
        }

        string path = cmd.GetArg(2);
        bool ok = _legacy.TrySetBotAvatar(slot, path, out string error);
        cmd.ReplyToCommand(ok
            ? path == "0"
                ? $"[BotHider] avatar clear queued slot={slot}"
                : $"[BotHider] avatar queued slot={slot} bytes={_legacy.GetConfiguredAvatarSize(slot)}"
            : $"[BotHider] avatar rejected slot={slot}: {error}");
    }

    // bh_identity_mode <player|bot> - changes the managed-bot identity mode
    [ConsoleCommand("bh_identity_mode", "Set identity mode: bh_identity_mode <player|bot>")]
    public void OnIdentityMode(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_legacy == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        BotIdentityMode mode;
        if (cmd.ArgCount < 2)
        {
            cmd.ReplyToCommand("usage: bh_identity_mode <player|bot>");
            return;
        }

        string value = cmd.GetArg(1);
        if (value.Equals("player", StringComparison.OrdinalIgnoreCase))
            mode = BotIdentityMode.Player;
        else if (value.Equals("bot", StringComparison.OrdinalIgnoreCase))
            mode = BotIdentityMode.Bot;
        else
        {
            cmd.ReplyToCommand("usage: bh_identity_mode <player|bot>");
            return;
        }

        bool ok = _legacy.SetIdentityMode(mode);
        cmd.ReplyToCommand($"[BotHider] identity mode -> {mode.ToString().ToLowerInvariant()} ({ok})");
    }

    // bh_namesource <0|1> — 0=botprofile name (default), 1=bot_info.json name
    [ConsoleCommand("bh_namesource", "Set display-name source: bh_namesource <0|1> (0=botprofile 1=bot_info)")]
    public void OnNameSource(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_legacy == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        if (cmd.ArgCount < 2 || !int.TryParse(cmd.GetArg(1), out int v))
        { cmd.ReplyToCommand("usage: bh_namesource <0|1> (0=botprofile 1=bot_info)"); return; }
        bool useBotInfo = v != 0;
        bool ok = _legacy.SetNameSource(useBotInfo);
        cmd.ReplyToCommand($"[BotHider] name source -> {(useBotInfo ? "bot_info" : "botprofile")} ({ok})");
    }
}
