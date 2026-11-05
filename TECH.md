# BotHider Developer Guide

CounterStrikeSharp plugins use the `bothider:api` capability. Install the native plugin and `BotHiderImpl` as described in the [README](README.md).

## Plugin integration

Reference the release's shared `BotHiderApi.dll`, not the provider. It is installed in `addons/counterstrikesharp/shared/BotHiderApi/`. Do not bundle a second copy in your plugin.

```xml
<Reference Include="BotHiderApi">
  <HintPath>libs/BotHiderApi.dll</HintPath>
  <Private>false</Private>
</Reference>
```

Resolve the capability on the game thread after plugins load. Look it up again after a provider reload.

Import `BotHiderApi` and `CounterStrikeSharp.API.Core.Capabilities`. In your `BasePlugin` subclass:

```csharp
private static readonly PluginCapability<IBotHiderApi> BotHider = new("bothider:api");

public override void OnAllPluginsLoaded(bool hotReload)
{
    IBotHiderApi? api;
    try { api = BotHider.Get(); }
    catch (KeyNotFoundException) { return; }
    if (api == null) return;

    foreach (int slot in api.GetManagedSlots())
        Console.WriteLine($"{slot}: {api.GetPersonaName(slot)}");
}
```

## API reference

Use [`IBotHiderApi`](csharp/BotHiderApi/IBotHiderApi.cs) on the game thread. A slot is `CCSPlayerController.Slot` (0–63), not a user ID. Check `IsManagedBot(slot)` for ownership; `IsBot` also includes unmanaged bots. Slots can be reused after disconnect.

| Read | Result |
| --- | --- |
| `GetManagedSlots`, `IsManagedBot` | Managed slots and ownership |
| `GetBotSteamId`, `GetPersonaName` | Assigned SteamID64 and engine-applied name |
| `GetPing`, `GetCrosshairCode`, `GetScoreboardFlair` | Ping (ms), crosshair share code, flair item index |
| `HasBotAvatar` | An override is applied to the bot's current SteamID64 |
| `GetSignatures` | Hook names and addresses; `0` means unresolved |

Setters return `bool`. An accepted change may appear on the scoreboard later.

| Write | Behavior |
| --- | --- |
| `SetBotSteamId(slot, id)` | Requires a nonzero, unused SteamID64. Rejection preserves the current ID. |
| `SetPersonaName(slot, name)` | Normalizes to 31 UTF-8 bytes; rejects empty names. |
| `SetCrosshairCode(slot, code)` | Empty string or `"0"` clears. |
| `SetScoreboardFlair(slot, index)` | Accepts 0–65535; `0` clears. |
| `SetBotAvatar(slot, pngPath)` | Queues a file read; `"0"` clears. See below. |
| `SetIdentityMode(mode)` | `Player` requires the active server's change-level hook. Binding failure keeps bots in `Bot` mode. |
| `SetNameSource(useBotInfo)` | New bots only: `true` uses the configured name; `false` uses the engine name. |

**Avatars:** Use a PNG filename from `game/csgo/addons/avatars/` (≤16 KiB), or its full path. For file requests, `true` means queued, not applied. One read per slot can be pending. Call again after editing the file. An invalid file leaves the current override unchanged; disconnect or slot reuse discards a pending result. `HasBotAvatar` reports current state, not completion of a particular request.

## Build from source

Initialize submodules with `git submodule update --init --recursive`. On Windows, run `./build.ps1 -Windows` for native and managed binaries, or `./build.ps1 -Linux -CSharp` for the Linux package via WSL. For the provider alone, run `./build.ps1 -CSharp` (.NET 10).
