using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Memory;

namespace BotHiderImpl;

// Override clan fields and retain the controller's original values.
internal sealed class ClanTagPresenter
{
    private readonly ClanPresentationState?[] _states = new ClanPresentationState?[64];
    private readonly int[] _userIds = new int[64];
    private readonly uint[] _controllerHandles = new uint[64];

    // Apply the identity's clan pair to the controller.
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

    // Restore clan fields on the matching controller and user ID.
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

    // Read the clan tag and group ID.
    private static ClanValue ReadClan(CCSPlayerController player)
        => new(player.Clan ?? string.Empty,
               Schema.GetRef<uint>(player.Handle, "CCSPlayerController", "m_unClanId32bit"));

    // Publish both clan fields.
    private static void WriteClan(CCSPlayerController player, ClanValue value)
    {
        player.Clan = value.Tag;
        Schema.SetSchemaValue(player.Handle, "CCSPlayerController", "m_unClanId32bit", value.GroupId);
        Utilities.SetStateChanged(player, "CCSPlayerController", "m_szClan");
        Utilities.SetStateChanged(player, "CCSPlayerController", "m_unClanId32bit");
    }
}

internal readonly record struct ClanValue(string Tag, uint GroupId);

// Retain original clan fields during an override.
internal sealed class ClanPresentationState
{
    private ClanValue? _base;
    private bool _pending;

    // Apply or restore clan fields; retry failed notifications.
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
