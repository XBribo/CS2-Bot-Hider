# BotHider developer guide

The native plugin and managed provider must be upgraded together. See
[README.md](README.md) for usage and [MAINTENANCE.md](MAINTENANCE.md) for provenance.

## Build and test

Use .NET 10, an x64 C++20 compiler, CMake 3.20+, and protoc 3.21.x.
Set HL2SDKCS2 to the CS2 HL2SDK checkout and MMSOURCE_DEV to Metamod with its KHook
submodule. CSGO_PROTO optionally selects the proto directory; PROTOC selects
the compiler executable. PROTOBUF_IMPORT_DIR is a CMake cache override for
well-known proto imports.

```powershell
./build.ps1 -Windows -CSharp
cmake -S tests/native -B build/tests -A x64
cmake --build build/tests --config Release
ctest --test-dir build/tests -C Release --output-on-failure
dotnet test tests/managed/BotHider.Tests.csproj -c Release
```

For Linux, configure the native project in SteamRT3 with
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`, then build with
`cmake --build build`. Configure the portable tests without `-A x64`.
The manually dispatched Build workflow tests/builds both platforms.
Leave its release option disabled for candidates.

## Installation boundaries

Install exactly one BotHider native binary and one BotHiderImpl CSS provider.
Remove duplicate provider folders such as BotHider or DemoTracerBotHider.
Packages include shared/BotHiderApi, shared/DemoTracerBotHiderApi and
shared/0Harmony beneath addons/counterstrikesharp.

Preserve config.json and bot_info.json when upgrading. Restart after replacing
native binaries, shared assemblies or config. CSS hot reload cannot safely
replace the shared contracts.

The native ABI is **4** (slot size 172 bytes, signature size 40 bytes).
It replaces upstream shared memory; direct SHM consumers must migrate.
It rejects the older DemoTracer native ABI 3. Managed API compatibility does
not permit mixing native/provider versions.

## Configuration and avatars

Configuration is read at native load:

```json
{
  "identity_mode": "player",
  "auto_respawn": false,
  "external_avatars": false,
  "fake_ping": { "enabled": true, "min": 20, "max": 90 }
}
```

identity_mode selects player disguise or native bot flags. Fake ping stays in
the configured inclusive range. auto_respawn opts into the upstream round-start
team assignment/respawn behavior; leased bots are skipped.

**For DemoTracer set external_avatars=true and auto_respawn=false**. See
configs/examples/demotracer.json. This disables BotHider avatar-table writes and
rejects avatar setters, leaving BotController responsible for replay avatars.
Restart when changing modes; this is not a live ownership handoff protocol.
Only one publisher may own ServerAvatarOverrides.

In ordinary mode, SetBotAvatar validates PNG signature and size (8 bytes through
16 KiB), copies bytes into native storage, and returns request acceptance.
Native frame processing updates ServerAvatarOverrides; HasBotAvatar reports
applied state. This is not a client rendering ACK or a full PNG decoder.
Requests carry slot incarnation so replacement bots cannot inherit them.
SteamID changes rebind avatars; disconnect/map teardown clears them.
The compact HUD can retain its engine cache after the scoreboard updates.

bot_info.json maps persona names to steamid (32-bit account ID), optional
crosshair_code and scoreboard_flair (0 through 65535). Missing/zero flair clears
it. bh_namesource chooses whether future bots use the selected persona name.

## Managed APIs

All API calls and owner lifetime cancellation run on the server thread.
Reference shared assemblies with Private=false; do not package private copies.

| Capability | Contract | Purpose |
| --- | --- | --- |
| bothider:api | BotHiderApi.IBotHiderApi | Existing upstream getters/setters |
| bothider:presentation:v1 | BotHiderApi.IBotHiderPresentationApi | Neutral lease API v1 |
| demotracer:bot-hider:v2 | DemoTracerBotHiderApi.IBotHiderApi | Existing DemoTracer API v2 |

All three use one service and lease registry. The compatibility adapter maps
DTOs, not state. Native sessions, map/provider epochs and slot incarnations
invalidate stale requests.

### Lease API

```csharp
using BotHiderApi;
using CounterStrikeSharp.API.Core.Capabilities;
private static readonly PluginCapability<IBotHiderPresentationApi> Cap =
    new(BotHiderPresentationContract.Capability);
private readonly CancellationTokenSource lifetime = new();

// On server thread, after all plugins load:
var api = Cap.Get();
if (api != null && api.TryGetManagedSlot(slot, out var state))
{
    var result = api.AcquirePresentationLease("my-plugin", [
        new BotHiderPresentationOverride {
            Slot = slot, Incarnation = state.Incarnation,
            PlayerName = "Replay player", SteamId = uniqueSteamId,
            CrosshairCode = "", ScoreboardFlair = 0
        }
    ], lifetime.Token);
    // Keep result.LeaseToken if result.Ok; release it when finished.
}
// On consumer unload, on the server thread:
lifetime.Cancel();
lifetime.Dispose();
```

Acquire/Replace succeeds after native identity and requested controller fields
pass synchronous readback; this is not a remote client ACK. Requested IDs are
exact: conflicts are rejected, not substituted. Batches may permute identities
across their managed slots. Failure reverts lease ownership and attempts to
restore previous presentation; engine failure can still prevent restoration
and is logged. Null fields inherit base values; an empty crosshair clears it.

Release restores current base presentation. Disconnect removes only the affected
slot, preserving surviving lease slots. Empty leases are revoked. Native/provider
reload and map boundaries invalidate old ownership. ProviderChanged subscribers
must unsubscribe on unload and defer engine work until a frame.
No heartbeat is required.

### Legacy setters

The original BotHiderApi.IBotHiderApi and bothider:api capability remain.
Identity/crosshair/flair setters pass through lease validation/publication and
then commit a new base persona. They return synchronous success/failure rather
than queue acceptance. Leased/unavailable slots, duplicate IDs and failed
publication return false. Identity-mode changes are rejected while leases exist.
Name normalization preserves upstream Unicode text elements and the 31-byte
UTF-8 limit. Avatar setters still return asynchronous acceptance.

## Publication and intro performance

Native changes and player events coalesce into at most one pending full
presentation pass per frame. Ping events target changed slots unless a full
pass is pending. The two-second repeating pass and 0.25-second intro retry loop
are removed. Fields publish only changes or required first-controller repairs.

After a population transaction restores its temporary identity snapshot,
ApplyManagedDisguise now refreshes userinfo only if fake flags or SteamID
mirrors changed. Previously every such pass refreshed all managed bots.
The native regression covers 1,280 unchanged passes, mirror repair and mode
transitions.

bh_status reports leases, writes, repairs, userinfo_refreshes and the two new
options. userinfo_refreshes counts explicit successful RefreshClientUserInfo
calls since native load; it excludes engine-internal SetName publication and
does not measure frame time.

## Runtime acceptance

Compare matched game/Metamod/CSS builds, maps and bot counts:

1. Check bh_status hooks/managed slots and all legacy setters.
2. Compare repeated team intros: client/server frame time and counter deltas.
3. Exercise late human/HLTV joins, quota changes, kick/re-add slot reuse, team
   changes, map restart and map change.
4. Acquire/replace/release leases, cancel owners, unload consumers/providers;
   check restoration and stale-request rejection.
5. With DemoTracer's external-avatar config, test play, pause, stop, natural
   finish and replay replacement; verify names, IDs, crosshairs and avatars.
6. For rollback, stop the server and restore the previous matched native/managed
   bundle and saved config.

Offline regressions do not prove intro performance or current-game compatibility.
DemoTracer's installer/version manifest requires separate integration work after
acceptance.
