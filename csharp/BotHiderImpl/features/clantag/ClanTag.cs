using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Memory;

namespace BotHiderImpl;

// Applies configured clan presentation while retaining each controller's original pair.
internal sealed class ClanTagPresenter
{
    private readonly ClanPresentationState?[] _states = new ClanPresentationState?[64];
    private readonly int[] _userIds = new int[64];
    private readonly uint[] _controllerHandles = new uint[64];

    // Applies the selected identity's clan pair to this exact controller.
    public void Apply(int slot, CCSPlayerController player, (string Tag, uint GroupId) clan)
    {
        if (player.UserId is not int userId) return;
        if (clan.GroupId == 0 || clan.Tag.Length == 0)
        {
            Restore(slot);
            return;
        }

        uint controller = player.EntityHandle.Raw;
        if (_states[slot] != null &&
            (_controllerHandles[slot] != controller || _userIds[slot] != userId))
            Restore(slot);
        if (_states[slot] == null)
        {
            _states[slot] = new ClanPresentationState();
            _controllerHandles[slot] = controller;
            _userIds[slot] = userId;
        }

        try
        {
            _states[slot]!.Apply(new ClanValue(clan.Tag, clan.GroupId),
                () => ReadClan(player), value => WriteClan(player, value));
        }
        catch (Exception e)
        {
            Server.PrintToConsole($"[BotHider] clan write failed slot={slot}: {e.Message}");
        }
    }

    // Restores the captured clan only while the original controller still owns the slot.
    public void Restore(int slot)
    {
        var state = _states[slot];
        if (state == null) return;
        var player = Utilities.GetPlayerFromSlot(slot);
        if (player is { IsValid: true } &&
            player.EntityHandle.Raw == _controllerHandles[slot] &&
            player.UserId == _userIds[slot])
        {
            try
            {
                state.Apply(null, () => ReadClan(player), value => WriteClan(player, value));
            }
            catch (Exception e)
            {
                Server.PrintToConsole($"[BotHider] clan restore failed slot={slot}: {e.Message}");
                return;
            }
        }
        _states[slot] = null;
    }

    // Reads both controller clan fields as one presentation value.
    private static ClanValue ReadClan(CCSPlayerController player)
        => new(player.Clan ?? string.Empty,
               Schema.GetRef<uint>(player.Handle, "CCSPlayerController", "m_unClanId32bit"));

    // Publishes the clan tag and group ID together.
    private static void WriteClan(CCSPlayerController player, ClanValue value)
    {
        player.Clan = value.Tag;
        Schema.SetSchemaValue(player.Handle, "CCSPlayerController", "m_unClanId32bit", value.GroupId);
        Utilities.SetStateChanged(player, "CCSPlayerController", "m_szClan");
        Utilities.SetStateChanged(player, "CCSPlayerController", "m_unClanId32bit");
    }
}

internal readonly record struct ClanValue(string Tag, uint GroupId);

// Retains the controller's original clan pair until the configured override is released.
internal sealed class ClanPresentationState
{
    private ClanValue? _base;
    private bool _pending;

    // Applies or restores both clan fields and retries failed notifications.
    public void Apply(ClanValue? requested, Func<ClanValue> read, Action<ClanValue> write)
    {
        if (requested == null && _base == null) return;
        ClanValue current = read();
        if (requested != null) _base ??= current;
        ClanValue target = requested ?? _base!.Value;
        if (current != target || _pending)
        {
            _pending = true;
            write(target);
            if (read() != target)
                throw new InvalidOperationException("controller clan write was not retained");
            _pending = false;
        }
        if (requested == null) _base = null;
    }
}
