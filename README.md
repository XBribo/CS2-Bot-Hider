# BotHider

**Disguise CS2 bots as real players — for Metamod:Source & CounterStrikeSharp plugins**

## Your stars⭐ are my motivation to keep updating

------------------------------------------------------------------------

## Overview

`BotHider` is a **Metamod:Source plugin** for **Counter-Strike 2**
servers that makes bots indistinguishable from humans.

When the engine spawns a fake client, BotHider:

- Strips the `BOT` scoreboard label (clears `m_bFakePlayer`)
- Assigns a real-looking **SteamID64**
- Renames the bot to a curated **persona name**
- Applies a **jittered ping** and a **crosshair share-code(not completed)**

Identities are sourced from `bot_info.json`. A companion
**CounterStrikeSharp** plugin applies the live ping / crosshair and
exposes an inter-plugin API, `IBotHiderApi`, consumable from any other
C# plugin.

------------------------------------------------------------------------

## Features

- Removes the `BOT` tag from the scoreboard
- Per-bot SteamID64, name, ping, and crosshair
- Same-name priority, otherwise random persona assignment
- Re-randomizes on every map change
- `IBotHiderApi` capability for other CSS plugins
- Read **and** write access (override a bot's SteamID / name at runtime)
- Zero engine detours on the C# side (shared-memory bridge)

------------------------------------------------------------------------

## Components

| Project        | Output             | Role                                            |
|----------------|--------------------|-------------------------------------------------|
| `src/`         | `BotHider.dll`     | Metamod plugin (C++). Core disguise logic.      |
| `BotHiderImpl` | `BotHiderImpl.dll` | CSS plugin. Applies ping/crosshair, serves API. |
| `BotHiderApi`  | `BotHiderApi.dll`  | Shared interface (`IBotHiderApi`) for consumers.|

The C++ and C# sides share state through a named memory mapping
(`CS2BotHider_Slots`); CSS runs in the same process as the server.

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

## Build

### C++ (Metamod plugin)

Env: `HL2SDKCS2`, `MMSOURCE_DEV`, `CSGO_PROTO`, `protoc` on PATH.

``` text
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

### C# (CSS plugins)

``` text
dotnet build csharp/BotHiderImpl/BotHiderImpl.csproj -c Release
dotnet build csharp/BotHiderApi/BotHiderApi.csproj -c Release
```

## Install

1. Copy the Metamod plugin + config into the game tree:

        addons/BotHider/bin/win64/BotHider.dll
        addons/metamod/BotHider.vdf
        addons/BotHider/bot_info.json

2. Install **BotHiderImpl** as a CS# plugin:

        addons/counterstrikesharp/plugins/BotHiderImpl/BotHiderImpl.dll

3. Install **BotHiderApi** as a shared assembly:

        addons/counterstrikesharp/shared/BotHiderApi/BotHiderApi.dll

4. Restart the server.

------------------------------------------------------------------------

## Configuration

`bot_info.json` maps a player name to a SteamID (32-bit account id) and a
crosshair share-code:

``` json
{
    "s1mple": {
        "steamid": 73936547,
        "crosshair_code": "CSGO-pE5f8-6RQvk-HLpdN-KW3J6-BQwLA"
    }
}
```

On spawn, BotHider prefers an entry whose key matches the engine's
proposed bot name; otherwise it picks a random unused entry. Assignments
reset on map change. If the file is missing, the plugin falls back to a
built-in roster (no SteamID / crosshair).

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

Because BotHider clears `m_bFakePlayer`, **disguised bots no longer
report as bots** through the usual checks. Use the API instead:

| Before                        | After                            |
|-------------------------------|----------------------------------|
| `player.IsBot`                | `_api.IsManagedBot(player.Slot)` |

``` csharp
foreach (int slot in _api.GetManagedSlots())
{
    Console.WriteLine(
        $"slot={slot} sid={_api.GetSyntheticSteamId(slot)} " +
        $"name='{_api.GetPersonaName(slot)}' ping={_api.GetPing(slot)}");
}
```

## Overriding identity at runtime (C#)

`SetBotSteamId` and `SetPersonaName` post a command to the C++ side; the
change is applied on the **next server frame**, so re-query to confirm.
Both return `false` for an invalid slot (and `SetPersonaName` rejects an
empty name).

``` csharp
// Give the bot in slot 3 a specific SteamID64 + name.
ulong steamId64 = 76561197960287930;   // a full SteamID64

if (_api.SetBotSteamId(3, steamId64))
    Console.WriteLine("SteamID queued");

if (_api.SetPersonaName(3, "ZywOo"))
    Console.WriteLine("Name queued");

// Next frame: the scoreboard shows 'ZywOo' with the new SteamID.
```

`SetPersonaName` also drives the scoreboard via the controller schema, so
the new name is networked to clients — not just stored internally.

------------------------------------------------------------------------

## Console commands

- `bh_status` — list every managed slot: sid, name, ping, crosshair.

------------------------------------------------------------------------

## License

GPL-v3.0

------------------------------------------------------------------------

## Author

**XBribo**
