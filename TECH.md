# BotHider developer guide

BotHider owns identity, crosshair, flair and avatar presentation. It never assigns
teams or respawns players. Install the native plugin and managed provider together.

## Build and test

Requirements: .NET 10, an x64 C++20 compiler, CMake 3.20+, protoc 3.21.x,
HL2SDKCS2 and MMSOURCE_DEV (including the KHook submodule). CSGO_PROTO and PROTOC
can override the proto directory and compiler. PROTOBUF_IMPORT_DIR selects the
well-known proto import directory when needed.

```powershell
./build.ps1 -Windows -CSharp
dotnet test tests/managed/BotHider.Tests.csproj -c Release
cmake -S tests/native -B build/tests -A x64
cmake --build build/tests --config Release
ctest --test-dir build/tests -C Release --output-on-failure
```

Linux uses SteamRT3: configure with -DCMAKE_BUILD_TYPE=Release and omit -A x64.
The Build workflow also builds/tests and packages both platforms.

## Installation and configuration

Install one native BotHider and one CSS BotHiderImpl. The only API assembly is
shared/BotHiderApi/BotHiderApi.dll; Harmony is shared/0Harmony/0Harmony.dll.
Preserve config.json and bot_info.json on upgrade. Restart after replacing native
binaries, API assemblies or configuration.

Native ABI and managed API versions are both **1**. Native slots are 172 bytes
and signature entries 40 bytes. The old internal shared-memory transport has
been replaced by in-process calls; native/provider files must match.

config.json supports identity_mode (player or bot) and fake_ping
(enabled/min/max). bot_info.json maps persona names to steamid (32-bit account ID),
optional crosshair_code and scoreboard_flair (0 through 65535). A missing or zero
flair clears it. bh_namesource controls the name source for future bots.
Managed commands require the server console or CSS root permission.

## One managed API

Reference BotHiderApi with Private=false. Resolve
PluginCapability<IBotHiderApi>("bothider:api") after all plugins load.
BotHiderContract.ApiVersion is 1. Calls and owner cancellation must run on the
server thread. Do not package private copies of the shared assembly.

Existing getters and setters, presentation leases and direct avatar overrides
are all members of IBotHiderApi. The facade shares one presentation service.

```csharp
using BotHiderApi;
using CounterStrikeSharp.API.Core.Capabilities;
private static readonly PluginCapability<IBotHiderApi> Cap =
    new(BotHiderContract.Capability);
private readonly CancellationTokenSource lifetime = new();

// On the server thread:
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
    // Release result.LeaseToken when finished.
}
// On consumer unload, also on the server thread:
lifetime.Cancel();
lifetime.Dispose();
```

Acquire/Replace reports synchronous native/controller readback, not a remote
client ACK. Conflicting IDs are rejected rather than substituted. Null fields
inherit the current base; empty crosshair explicitly clears it. Release restores
the current base. Failed writes revert ownership and attempt restoration.

Disconnect releases only that slot; surviving lease slots remain. Map, native
session and provider changes invalidate stale requests. Subscribe to
BotHiderContract.ProviderChanged, defer engine work to the next frame, and
unsubscribe on unload. No heartbeat is required.

Individual setters commit new base values through the same validation.
They reject leased/unavailable slots and report synchronous success, except
SetBotAvatar, which queues a slot-bound avatar request.
Names retain complete Unicode text elements within 31 UTF-8 bytes.

## Avatar publisher

Both bh_setavatar/SetBotAvatar and the direct SteamID API use the same native
publisher and its restoration records. PNGs must have a PNG signature and be
8 bytes through 16 KiB. This is a signature/size check, not a PNG decoder.

```csharp
if (!api.TryPublishAvatarOverride(steamId, pngBytes, out var error))
    Console.WriteLine(error);
// On stop/unload:
api.TryClearAvatarOverride(steamId, out error);
```

The direct API can target bots or real players without changing their identity.
ClearAvatarOverrides clears direct overrides only; consumers must coordinate
their use of this shared direct API. It does not clear slot-owned avatars.
A direct override and a slot-bound request cannot own the same SteamID at once;
conflicts return -7. Clear an existing override before changing its source.

The publisher snapshots prior data and restores it only if its own bytes remain.
A later writer's data is preserved. Updates retain the original restoration
value. Slot incarnations prevent stale requests from following slot reuse;
changing the bot SteamID releases its old avatar before publishing the new one.
Native unload and map teardown drain publication state.

On supported Windows listen servers, a validated local client hook observes the
replicated bytes and dispatches Valve's asynchronous ReloadAvatarImage event.
Only matching publication evidence triggers refresh, once per revision/client
table epoch. Failed dispatch can retry. No remote client commands or JavaScript
are sent. Missing/ambiguous signatures disable only local HUD refresh.
Dedicated servers and Linux retain server publication without this local bridge.

GetAvatarStatus/bh_status reports the bridge, refresh count and tracked entries.
Error codes: -1 invalid input/session, -2 table unavailable, -3 unreadable prior
data, -4 capacity, -5 write/readback failure, -6 reliable-avatar cvar unavailable,
-7 ownership conflict, -8 wrong thread.

## Publication and validation

Player/native lifecycle events coalesce full presentation work per frame; ping
events target changed slots. There is no repeating intro repair timer.
After population transactions, unchanged fake flags/SteamID mirrors no longer
trigger redundant userinfo refreshes.

bh_status userinfo_refreshes counts explicit successful RefreshClientUserInfo
calls since native load. It excludes engine-internal SetName publication and
does not measure frame time.

Regression tests cover slot/session isolation, encoding, identity publication,
lease cleanup, and avatar arrival/restoration evidence. Runtime acceptance must
also cover intro frame time, quota changes, human/HLTV joins, slot reuse, map
changes, consumer/provider unload, avatar replacement and restoration.
