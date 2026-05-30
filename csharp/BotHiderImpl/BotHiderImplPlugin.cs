using BotHiderApi;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Attributes.Registration;
using CounterStrikeSharp.API.Core.Capabilities;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.Memory;
using CounterStrikeSharp.API.Modules.Timers;

namespace BotHiderImpl;

public class BotHiderImplPlugin : BasePlugin
{
    public override string ModuleName => "BotHiderImpl";
    public override string ModuleVersion => "0.1.0";
    public override string ModuleAuthor => "XBribo";
    public override string ModuleDescription =>
        "Bridges BotHider shared memory to a CSS PluginCapability";

    public static PluginCapability<IBotHiderApi> Capability { get; } =
        new("bothider:api");

    private SharedMemoryClient? _client;
    private readonly string[] _appliedCrosshair = new string[64];

    public override void Load(bool hotReload)
    {
        // Inject the visible-write actions so SetPersonaName / SetBotSteamId
        // also update the scoreboard via controller schema
        _client = new SharedMemoryClient(ApplyVisibleName, ApplyVisibleSid);
        _client.TryConnect();
        Capabilities.RegisterPluginCapability(Capability, () => (IBotHiderApi)_client);

        AddTimer(2.0f, ApplyManagedSlots, TimerFlags.REPEAT);
    }

    public override void Unload(bool hotReload)
    {
        _client?.Dispose();
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
                    Schema.SetSchemaValue(player.Handle, "CCSPlayerController", "m_iPing", ping);
                    Utilities.SetStateChanged(player, "CCSPlayerController", "m_iPing");
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
                    player.CrosshairCodes = cross;
                    Utilities.SetStateChanged(player, "CCSPlayerController", "m_szCrosshairCodes");
                    _appliedCrosshair[slot] = cross;
                }
                catch (Exception e)
                {
                    Server.PrintToConsole($"[BotHider] crosshair write failed slot={slot}: {e.Message}");
                }
            }
        }
    }

    // bh_status — dump every managed slot's state (sid + persona name)
    [ConsoleCommand("bh_status", "List all BotHider-managed slots")]
    public void OnStatus(CCSPlayerController? player, CommandInfo cmd)
    {
        if (_client == null) { cmd.ReplyToCommand("[BotHider] not initialized"); return; }
        var slots = _client.GetManagedSlots();
        cmd.ReplyToCommand($"[BotHider] managed slots: {slots.Length}");
        foreach (int s in slots)
        {
            cmd.ReplyToCommand(
                $"  slot={s} sid={_client.GetSyntheticSteamId(s)} " +
                $"name='{_client.GetPersonaName(s)}' ping={_client.GetPing(s)} " +
                $"crosshair='{_client.GetCrosshairCode(s)}'");
        }
    }
}
