using BotHiderApi;
using CounterStrikeSharp.API;

namespace BotHiderImpl;

// Reserves managed slot lifetimes while a consumer publishes temporary fields.
internal sealed class Claims(
    SharedMemory client, Action onRelease)
{
    private sealed class Claim(string token, string owner, Dictionary<int, ulong> slots)
    {
        public string Token { get; } = token;
        public string Owner { get; } = owner;
        public Dictionary<int, ulong> Slots { get; set; } = slots;
        public CancellationTokenRegistration Registration { get; set; }
    }

    private readonly Dictionary<string, Claim> _claims = new(StringComparer.Ordinal);
    private readonly string?[] _slotOwners = new string?[64];
    private readonly Dictionary<int, ulong> _pendingRestore = new();
    public string Epoch { get; } = Guid.NewGuid().ToString("N");

    // Acquires an entire set or leaves the existing ownership unchanged.
    public bool TryAcquire(string owner, BotHiderSlotRef[] slots,
        CancellationToken ownerLifetime, out string token)
    {
        token = string.Empty;
        if (string.IsNullOrWhiteSpace(owner) || owner.Length > 64 ||
            !ownerLifetime.CanBeCanceled || ownerLifetime.IsCancellationRequested ||
            !NativeIdentityClient.IsAvailable() ||
            !TryValidate(slots, null, out var normalized)) return false;

        token = $"{Epoch}:{Guid.NewGuid():N}";
        var claim = new Claim(token, owner, normalized);
        _claims.Add(token, claim);
        foreach (int slot in normalized.Keys)
        {
            _slotOwners[slot] = token;
            _pendingRestore.Remove(slot);
        }
        claim.Registration = ownerLifetime.Register(() =>
            Server.NextFrame(() => Release(claim.Token)));
        return true;
    }

    // Replaces one claim only after every new slot has passed validation.
    public bool TryReplace(string token, BotHiderSlotRef[] slots)
    {
        if (!_claims.TryGetValue(token, out var claim) ||
            !TryValidate(slots, token, out var normalized)) return false;

        foreach (var slot in normalized)
            if (claim.Slots.TryGetValue(slot.Key, out ulong oldIncarnation) &&
                oldIncarnation != slot.Value) return false;

        foreach (var old in claim.Slots)
        {
            if (normalized.ContainsKey(old.Key)) continue;
            _slotOwners[old.Key] = null;
            Restore(old.Key, old.Value);
        }
        foreach (int slot in normalized.Keys)
        {
            if (claim.Slots.ContainsKey(slot)) continue;
            _slotOwners[slot] = token;
            _pendingRestore.Remove(slot);
        }
        claim.Slots = normalized;
        onRelease();
        return true;
    }

    // Publishes only to a slot owned by the matching claim and incarnation.
    public bool TryPublishIdentity(string token, int slot, ulong incarnation,
        ulong steamId, string name)
    {
        return _claims.TryGetValue(token, out var claim) &&
               claim.Slots.TryGetValue(slot, out ulong expected) &&
               expected == incarnation && client.GetSlotIncarnation(slot) == incarnation &&
               (steamId != 0 || client.GetBaseBotSteamId(slot) == 0) &&
               NativeIdentityClient.PublishIdentity(slot, incarnation, steamId, name);
    }

    // Releases one claim and restores current BotHider persona bases.
    public bool Release(string token)
    {
        if (!_claims.Remove(token, out var claim)) return false;
        claim.Registration.Unregister();
        foreach (var slot in claim.Slots)
        {
            _slotOwners[slot.Key] = null;
            Restore(slot.Key, slot.Value);
        }
        onRelease();
        return true;
    }

    // Releases all claims owned by one consumer.
    public int ReleaseOwner(string owner)
    {
        var tokens = _claims.Values.Where(claim => claim.Owner == owner)
            .Select(claim => claim.Token).ToArray();
        foreach (string token in tokens) Release(token);
        return tokens.Length;
    }

    // Keeps claims tied to the original native slot lifetime.
    public bool IsClaimed(int slot, ulong incarnation)
    {
        if (slot is < 0 or >= 64 || _slotOwners[slot] is not { } token ||
            !_claims.TryGetValue(token, out var claim)) return false;
        if (claim.Slots.TryGetValue(slot, out ulong expected) && expected == incarnation)
            return true;
        RemoveSlot(slot);
        return false;
    }

    // Prunes a departing slot without affecting surviving claim participants.
    public void RemoveSlot(int slot)
    {
        if (slot is < 0 or >= 64) return;
        _pendingRestore.Remove(slot);
        if (_slotOwners[slot] is not { } token ||
            !_claims.TryGetValue(token, out var claim)) return;
        _slotOwners[slot] = null;
        claim.Slots.Remove(slot);
        if (claim.Slots.Count == 0)
        {
            _claims.Remove(token);
            claim.Registration.Unregister();
        }
    }

    // Retries a failed base-identity restore only for the original incarnation.
    public void RetryRestores()
    {
        foreach (var pending in _pendingRestore.ToArray())
            Restore(pending.Key, pending.Value);
    }

    // Clears map-local ownership and pending writes.
    public void Clear()
    {
        foreach (var claim in _claims.Values)
            claim.Registration.Unregister();
        _claims.Clear();
        Array.Clear(_slotOwners);
        _pendingRestore.Clear();
    }

    // Releases every consumer before the provider unloads or the map ends.
    public void ReleaseAll()
    {
        foreach (string token in _claims.Keys.ToArray()) Release(token);
    }

    // Validates a whole batch against managed lifetimes and other owners.
    private bool TryValidate(BotHiderSlotRef[]? slots, string? allowedToken,
        out Dictionary<int, ulong> normalized)
    {
        normalized = [];
        if (slots is not { Length: > 0 }) return false;
        foreach (var slot in slots)
        {
            if (slot.Slot is < 0 or >= 64 || slot.Incarnation == 0 ||
                !client.IsManagedBot(slot.Slot) ||
                client.GetSlotIncarnation(slot.Slot) != slot.Incarnation ||
                (_slotOwners[slot.Slot] is { } token && token != allowedToken) ||
                !normalized.TryAdd(slot.Slot, slot.Incarnation)) return false;
        }
        return true;
    }

    // Restores one claimed identity without ever writing into a reused slot.
    private void Restore(int slot, ulong incarnation)
    {
        if (!client.IsManagedBot(slot) || client.GetSlotIncarnation(slot) != incarnation)
        {
            _pendingRestore.Remove(slot);
            return;
        }
        if (NativeIdentityClient.PublishIdentity(slot, incarnation,
                client.GetBaseBotSteamId(slot), client.GetBasePersonaName(slot)))
            _pendingRestore.Remove(slot);
        else
            _pendingRestore[slot] = incarnation;
    }
}
