# BotHider

> This document contains build instructions, API references, and developer integration guides.
> For general usage and installation, see [README.md](README.md).

------------------------------------------------------------------------

## Overview

`BotHider` is a **Metamod:Source & CounterStrikeSharp plugin** that removes the `BOT` tag from fake clients and assigns them realistic player identities.  
It exposes `IBotHiderApi`, consumable from any other C# plugin, allowing full read/write control over bot identities at runtime.

When the engine spawns a fake client, BotHider:

- Strips the `BOT` scoreboard label
- Assigns a synthetic **SteamID64**
- Renames the bot to a curated **persona name**
- Applies a **jittered ping** and a **crosshair share-code**
- Patches `CCSPlayerController.IsBot` to report `true` for managed bots (preserving compatibility with other plugins)

------------------------------------------------------------------------

## How to Build

### Prerequisites

- **Windows**: `HL2SDKCS2`, `MMSOURCE_DEV`, `CSGO_PROTO` environment variables set; `protoc` 3.21.x on PATH.
- **Linux (WSL)**: A WSL distro (e.g., `Ubuntu-24.04`) with `g++`, `cmake`, and `protobuf-compiler` installed.
- **C#**: .NET SDK compatible with CounterStrikeSharp.

### One-click build (Windows host, all targets)

```powershell
./build.ps1            # Build everything (Windows, Linux via WSL, and C# plugins)
./build.ps1 -Windows   # Windows only
./build.ps1 -Linux     # Linux only (via WSL)
./build.ps1 -CSharp    # C# plugins only
./build.ps1 -Clean     # Clean build all
```

Output packages appear in `dist/windows/` and `dist/linux/`.

### Manual Build

**C++ (Metamod plugin) — Windows:**

```
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

**C++ (Metamod plugin) — Linux:**

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

**C# (CSS plugins):**

```
dotnet build csharp/BotHiderImpl/BotHiderImpl.csproj -c Release
dotnet build csharp/BotHiderApi/BotHiderApi.csproj -c Release
```

------------------------------------------------------------------------

## Configuration File (bot_info.json)

Located in `/game/csgo/addons/bothider/bot_info.json`.  
Maps a persona name to a **32-bit Steam account ID** and an optional crosshair share code.

```json
{
    "s1mple": {
        "steamid": 73936547,
        "crosshair_code": "CSGO-pE5f8-6RQvk-HLpdN-KW3J6-BQwLA"
    }
}
```

On spawn, BotHider selects an entry from this file (preferring one matching the engine's proposed bot name, otherwise random unused entry).  
The selected entry supplies the **SteamID**, **crosshair**, and **ping** for the bot.  
Display name is controlled separately by `bh_namesource`.

------------------------------------------------------------------------

## Exposed Interface (C#)

```csharp
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

    // --- global toggles ---
    bool     SetDisguise(bool enabled);     // off lets the bot manager spawn bots again
    bool     SetNameSource(bool useBotInfo); // true=bot_info name, false=botprofile name
}
```

`slot` is the engine player slot (`CCSPlayerController.Slot.Value`).

------------------------------------------------------------------------

## Getting the API (C# Integration)

1. Add a reference to `BotHiderApi.dll` in your plugin's `.csproj`:

```xml
<ItemGroup>
  <Reference Include="BotHiderApi">
    <HintPath>libs/BotHiderApi.dll</HintPath>
  </Reference>
</ItemGroup>
```

1. Resolve the capability after all plugins are loaded:

```csharp
using BotHiderApi;
using CounterStrikeSharp.API.Core.Capabilities;

private static readonly PluginCapability<IBotHiderApi> Cap = new("bothider:api");
private IBotHiderApi? _api;

public override void OnAllPluginsLoaded(bool hotReload) => _api = Cap.Get();
```

------------------------------------------------------------------------

## Reading Bot State

| Check | Result for managed bots |
|-------|--------------------------|
| `player.IsBot` | `true` (restored by BotHider's Harmony patch) |
| `_api.IsManagedBot(slot)` | `true` (direct, patch-free) |

```csharp
foreach (int slot in _api.GetManagedSlots())
{
    Console.WriteLine(
        $"slot={slot} sid={_api.GetSyntheticSteamId(slot)} " +
        $"name='{_api.GetPersonaName(slot)}' ping={_api.GetPing(slot)}");
}
```

Use `_api.IsManagedBot(slot)` when you need a guarantee independent of Harmony (e.g., code already inlined before the patch).

------------------------------------------------------------------------

## Overriding Identity at Runtime

`SetBotSteamId` and `SetPersonaName` queue a command to the C++ side. Always re-query after applying.

```csharp
ulong steamId64 = 76561197960287930;

if (_api.SetBotSteamId(3, steamId64))
    Console.WriteLine("SteamID queued");

if (_api.SetPersonaName(3, "ZywOo"))
    Console.WriteLine("Name queued");
```

`SetPersonaName` also immediately updates the scoreboard via the controller schema.

------------------------------------------------------------------------

## License

GPL-v3.0

## Author

**XBribo**
