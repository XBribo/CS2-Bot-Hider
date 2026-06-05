# BotHider

**Disguise CS2 bots as real players — for Metamod:Source & CounterStrikeSharp plugins**

## Your stars⭐ are my motivation to keep updating

------------------------------------------------------------------------

## Overview

`BotHider` is a **Metamod:Source plugin** for **Counter-Strike 2**
servers that makes bots indistinguishable from humans.

When the engine spawns a fake client, BotHider:

- Strips the `BOT` scoreboard label
- Assigns a **SteamID64**
- Renames the bot to a curated **persona name**
- Applies a **jittered ping** and a **crosshair share-code(not completed)**

`IBotHiderApi`, consumable from any other C# plugin.

------------------------------------------------------------------------

## Features

- Removes the `BOT` tag from the scoreboard
- Per-bot SteamID64, name, ping, and crosshair
- `IBotHiderApi` capability for other CSS plugins
- Read **and** write access (override a bot's SteamID / name at runtime)
- `IsBot` override: managed bots report `IsBot == true` to every CSS plugin

------------------------------------------------------------------------

## Console commands

- `bh_status` — list every managed slot: sid, name, ping, crosshair.
- `bh_setname <slot> <name>` — set bot's name.
- `bh_setsid <slot> <SteamID64>` — set bot's SteamID.
- `bh_disguise <0/1>` — toggle bot disguise off/on

------------------------------------------------------------------------

## Install

1. Download the latest release for your platform from the
   [**GitHub Releases**](https://github.com/XBribo/CS2-Bot-Hider/releases/latest) page:

        BotHider-windows.zip   # for Windows servers
        BotHider-linux.zip     # for Linux servers

2. Extract the archive and copy the `/addons/` directory into `/game/csgo/`.

3. Restart your game server.

------------------------------------------------------------------------

## Exposed Interface (C#)

``` csharp
public interface IBotHiderApi
{
    // --- read ---
    bool     IsManagedBot(int slot);        // is this one of ours?
    ulong    GetSyntheticSteamId(int slot); // assigned SteamID64 (0 if none)
    int[]    GetManagedSlots();             // all managed bot slots
    string   GetPersonaName(int slot);      // assigned display name
    int      GetPing(int slot);             // current jittered ping (ms)
    string   GetCrosshairCode(int slot);    // assigned crosshair share-code

    // --- write (applied on the next server frame) ---
    bool     SetBotSteamId(int slot, ulong steamId64);
    bool     SetPersonaName(int slot, string name);
}
```

`slot` is the engine player slot (`CCSPlayerController.Slot.Value`).

------------------------------------------------------------------------

## How to Build

### One-click (Windows host, all targets)

`build.ps1` builds the Windows `.dll`, the Linux `.so` (via WSL), and the
C# plugins, then assembles ready-to-ship packages under `dist/windows/`
and `dist/linux/`.

``` powershell
./build.ps1            # build everything
./build.ps1 -Windows   # Windows
./build.ps1 -Linux     # Linux (via WSL)
./build.ps1 -CSharp    # C# plugins
./build.ps1 -Clean     # wipe first, then build all
```

Env required: `HL2SDKCS2`, `MMSOURCE_DEV`, `CSGO_PROTO`, plus `protoc`
(3.21.x) on PATH. The Linux target needs a WSL distro (default
`Ubuntu-24.04`) with `g++`, `cmake`, and `protobuf-compiler` installed.

### Manual

**C++ (Metamod plugin) — Windows:**

``` text
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

**C++ (Metamod plugin) — Linux:**

``` text
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

**C# (CSS plugins):**

``` text
dotnet build csharp/BotHiderImpl/BotHiderImpl.csproj -c Release
dotnet build csharp/BotHiderApi/BotHiderApi.csproj -c Release
```

------------------------------------------------------------------------

## Configuration

`bot_info.json` maps a player name to a SteamID (32-bit account id) and a crosshair share-code:

``` json
{
    "s1mple": {
        "steamid": 73936547,
        "crosshair_code": "CSGO-pE5f8-6RQvk-HLpdN-KW3J6-BQwLA"
    }
}
```

On spawn, BotHider prefers an entry whose key matches the engine's proposed bot name
Otherwise it picks a random unused entry.

## Getting the API (C#)

Reference `BotHiderApi.dll` in your plugin `.csproj`:

``` xml
<ItemGroup>
  <Reference Include="BotHiderApi">
    <HintPath>libs/BotHiderApi.dll</HintPath>
  </Reference>
</ItemGroup>
```

Resolve the capability once all plugins are loaded:

``` csharp
using BotHiderApi;
using CounterStrikeSharp.API.Core.Capabilities;

private static readonly PluginCapability<IBotHiderApi> Cap = new("bothider:api");
private IBotHiderApi? _api;

public override void OnAllPluginsLoaded(bool hotReload) => _api = Cap.Get();
```

------------------------------------------------------------------------

## Reading bot state (C#)

| Check                         | bot result                       |
|-------------------------------|----------------------------------|
| `player.IsBot`                | `true` (restored by the patch)   |
| `_api.IsManagedBot(slot)`     | `true` (direct, patch-free)      |

`player.IsBot` is the convenient path. Prefer `_api.IsManagedBot(slot)`
when you need a guarantee independent of the patch
e.g. a call site the JIT may have already inlined before the patch was applied,
which the Harmony override cannot reach.

``` csharp
foreach (int slot in _api.GetManagedSlots())
{
    Console.WriteLine(
        $"slot={slot} sid={_api.GetSyntheticSteamId(slot)} " +
        $"name='{_api.GetPersonaName(slot)}' ping={_api.GetPing(slot)}");
}
```

## Overriding identity at runtime (C#)

`SetBotSteamId` and `SetPersonaName` post a command to the C++ side, so re-query to confirm.
Both return `false` for an invalid slot (and `SetPersonaName` rejects an empty name).

``` csharp
// Give the bot in slot 3 a specific SteamID64 + name.
ulong steamId64 = 76561197960287930;   // a full SteamID64

if (_api.SetBotSteamId(3, steamId64))
    Console.WriteLine("SteamID queued");

if (_api.SetPersonaName(3, "ZywOo"))
    Console.WriteLine("Name queued");

```

`SetPersonaName` also drives the scoreboard via the controller schema

------------------------------------------------------------------------

## Special thanks

- [cs2-insanity](https://github.com/Frad70/cs2-insanity) Helped me determine the framework
- [CS2Fixes](https://github.com/Source2ZE/CS2Fixes) Helped me identify the 'UTIL_Remove' signature
- [Misaka17032](https://github.com/Misaka17032) Contributed Linux support for the plugin

------------------------------------------------------------------------

## License

GPL-v3.0

------------------------------------------------------------------------

## Author

**XBribo**
