using BotHiderApi;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Attributes.Registration;
using CounterStrikeSharp.API.Modules.Commands;

namespace BotHiderImpl;

public partial class BotHiderImplPlugin
{
    // List managed slots and hook status.
    [ConsoleCommand("bh_status", "List all BotHider-managed slots")]
    [CommandHelper(0, "", CommandUsage.CLIENT_AND_SERVER)]
    public void OnStatus(CCSPlayerController? player, CommandInfo cmd)
    {
        if (!CanExecuteCommand(player, cmd)) return;
        if (_client == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        // All signatures must resolve.
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
            var clan = _client.GetClan(s);
            cmd.ReplyToCommand(
                $"  slot={s} incarnation={_client.GetSlotIncarnation(s)} " +
                $"sid={_client.GetBotSteamId(s)}/{_client.GetBaseBotSteamId(s)} " +
                $"name='{_client.GetPersonaName(s)}'/'{_client.GetBasePersonaName(s)}' " +
                $"ping={_client.GetPing(s)} " +
                $"crosshair='{_client.GetCrosshairCode(s)}' " +
                $"clan='{clan.Tag}'/{clan.GroupId} " +
                $"avatar={_client.HasBotAvatar(s)}/{_client.GetConfiguredAvatarSize(s)}B " +
                $"isbot={isBot}");
        }
    }

    // Set a bot's SteamID64.
    [ConsoleCommand("bh_setsid", "Set a bot's SteamID64: bh_setsid <slot> <sid64>")]
    public void OnSetSid(CCSPlayerController? player, CommandInfo cmd)
    {
        if (!CanExecuteCommand(player, cmd)) return;
        if (_client == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        if (cmd.ArgCount < 3 || !int.TryParse(cmd.GetArg(1), out int slot)
            || !ulong.TryParse(cmd.GetArg(2), out ulong sid))
        { cmd.ReplyToCommand("usage: bh_setsid <slot> <sid64>"); return; }
        bool ok = _client.SetBotSteamId(slot, sid);
        cmd.ReplyToCommand($"[BotHider] sid slot={slot} value={sid}: {(ok ? "queued" : "rejected")}");
    }

    // Set a bot's name.
    [ConsoleCommand("bh_setname", "Set a bot's name: bh_setname <slot> <name>")]
    public void OnSetName(CCSPlayerController? player, CommandInfo cmd)
    {
        if (!CanExecuteCommand(player, cmd)) return;
        if (_client == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        if (cmd.ArgCount < 3 || !int.TryParse(cmd.GetArg(1), out int slot))
        { cmd.ReplyToCommand("usage: bh_setname <slot> <name>"); return; }
        string name = cmd.GetArg(2);
        bool ok = _client.SetPersonaName(slot, name);
        string appliedName = ok ? _client.GetPersonaName(slot) : name;
        cmd.ReplyToCommand($"[BotHider] name slot={slot} value='{appliedName}': {(ok ? "queued" : "rejected")}");
    }

    // Set a bot's scoreboard flair.
    [ConsoleCommand("bh_setflair", "Set a bot's scoreboard flair: bh_setflair <slot> <item_def_index>")]
    public void OnSetFlair(CCSPlayerController? player, CommandInfo cmd)
    {
        if (!CanExecuteCommand(player, cmd)) return;
        if (_client == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        if (cmd.ArgCount < 3 || !int.TryParse(cmd.GetArg(1), out int slot)
            || !uint.TryParse(cmd.GetArg(2), out uint itemDefIndex))
        { cmd.ReplyToCommand("usage: bh_setflair <slot> <item_def_index>"); return; }
        bool ok = _client.SetScoreboardFlair(slot, itemDefIndex);
        cmd.ReplyToCommand($"[BotHider] flair slot={slot} value={itemDefIndex}: {(ok ? "ok" : "rejected")}");
    }

    // Set a bot's crosshair code.
    [ConsoleCommand("bh_setcrosshair", "Set a bot's crosshair: bh_setcrosshair <slot> <code>")]
    public void OnSetCrosshair(CCSPlayerController? player, CommandInfo cmd)
    {
        if (!CanExecuteCommand(player, cmd)) return;
        if (_client == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        if (cmd.ArgCount < 3 || !int.TryParse(cmd.GetArg(1), out int slot))
        { cmd.ReplyToCommand("usage: bh_setcrosshair <slot> <code>"); return; }
        string code = cmd.GetArg(2);
        bool ok = _client.SetCrosshairCode(slot, code);
        cmd.ReplyToCommand($"[BotHider] crosshair slot={slot}: {(ok ? "ok" : "rejected")}");
    }

    // Set or clear a bot's avatar.
    [ConsoleCommand("bh_setavatar", "Set a bot avatar: bh_setavatar <slot> <png_path|0>")]
    [CommandHelper(2, "<slot> <png_path|0>", CommandUsage.CLIENT_AND_SERVER)]
    public void OnSetAvatar(CCSPlayerController? player, CommandInfo cmd)
    {
        if (!CanExecuteCommand(player, cmd)) return;
        if (_client == null)
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
        bool ok = _client.TrySetBotAvatar(slot, path, out string error);
        cmd.ReplyToCommand(ok
            ? path == "0"
                ? $"[BotHider] avatar clear queued slot={slot}"
                : $"[BotHider] avatar queued slot={slot} bytes={_client.GetConfiguredAvatarSize(slot)}"
            : $"[BotHider] avatar rejected slot={slot}: {error}");
    }

    // Set the global bot identity mode.
    [ConsoleCommand("bh_identity_mode", "Set identity mode: bh_identity_mode <player|bot>")]
    public void OnIdentityMode(CCSPlayerController? player, CommandInfo cmd)
    {
        if (!CanExecuteCommand(player, cmd)) return;
        if (_client == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
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

        bool ok = _client.SetIdentityMode(mode);
        cmd.ReplyToCommand($"[BotHider] mode={mode.ToString().ToLowerInvariant()}: {(ok ? "queued" : "rejected")}");
    }

    // Select botprofile or bot_info names for new bots.
    [ConsoleCommand("bh_namesource", "Set display-name source: bh_namesource <0|1> (0=botprofile 1=bot_info)")]
    public void OnNameSource(CCSPlayerController? player, CommandInfo cmd)
    {
        if (!CanExecuteCommand(player, cmd)) return;
        if (_client == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        if (cmd.ArgCount < 2 || !int.TryParse(cmd.GetArg(1), out int v))
        { cmd.ReplyToCommand("usage: bh_namesource <0|1> (0=botprofile 1=bot_info)"); return; }
        bool useBotInfo = v != 0;
        bool ok = _client.SetNameSource(useBotInfo);
        cmd.ReplyToCommand($"[BotHider] name source={(useBotInfo ? "bot_info" : "botprofile")}: {(ok ? "queued" : "rejected")}");
    }
}
