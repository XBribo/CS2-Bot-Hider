using BotHiderApi;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Attributes.Registration;
using CounterStrikeSharp.API.Core.Capabilities;
using CounterStrikeSharp.API.Modules.Commands;

namespace BotHiderTest;

public class BotHiderTestPlugin : BasePlugin
{
    public override string ModuleName => "BotHiderTest";
    public override string ModuleVersion => "0.1.0";
    public override string ModuleAuthor => "Xbribo";
    public override string ModuleDescription => "Exercises the bothider:api capability";

    private static readonly PluginCapability<IBotHiderApi> Cap = new("bothider:api");
    private IBotHiderApi? _api;

    public override void OnAllPluginsLoaded(bool hotReload)
    {
        _api = Cap.Get();
        Server.PrintToConsole(_api != null
            ? "[BotHiderTest] capability resolved OK"
            : "[BotHiderTest] capability NOT found — is BotHiderImpl loaded?");
    }

    // css_bh_query <slot>
    [ConsoleCommand("css_bh_query", "Query a single slot: css_bh_query <slot>")]
    public void OnQuery(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_api == null) { cmd.ReplyToCommand("[BotHiderTest] api null"); return; }
        if (cmd.ArgCount < 2 || !int.TryParse(cmd.GetArg(1), out int slot))
        { cmd.ReplyToCommand("usage: css_bh_query <slot>"); return; }
        cmd.ReplyToCommand(
            $"[BotHiderTest] slot={slot} isBot={_api.IsManagedBot(slot)} " +
            $"sid={_api.GetSyntheticSteamId(slot)} name='{_api.GetPersonaName(slot)}'");
    }

    // css_bh_setsid <slot> <steamid64>
    [ConsoleCommand("css_bh_setsid", "Set a bot's SteamID64: css_bh_setsid <slot> <sid64>")]
    public void OnSetSid(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_api == null) { cmd.ReplyToCommand("[BotHiderTest] api null"); return; }
        if (cmd.ArgCount < 3 || !int.TryParse(cmd.GetArg(1), out int slot)
            || !ulong.TryParse(cmd.GetArg(2), out ulong sid))
        { cmd.ReplyToCommand("usage: css_bh_setsid <slot> <sid64>"); return; }
        bool ok = _api.SetBotSteamId(slot, sid);
        cmd.ReplyToCommand($"[BotHiderTest] SetBotSteamId({slot},{sid}) -> {ok}");
    }

    // css_bh_setname <slot> <name>
    [ConsoleCommand("css_bh_setname", "Set a bot's name: css_bh_setname <slot> <name>")]
    public void OnSetName(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_api == null) { cmd.ReplyToCommand("[BotHiderTest] api null"); return; }
        if (cmd.ArgCount < 3 || !int.TryParse(cmd.GetArg(1), out int slot))
        { cmd.ReplyToCommand("usage: css_bh_setname <slot> <name>"); return; }
        string name = cmd.GetArg(2);
        bool ok = _api.SetPersonaName(slot, name);
        cmd.ReplyToCommand($"[BotHiderTest] SetPersonaName({slot},'{name}') -> {ok}");
    }
}
