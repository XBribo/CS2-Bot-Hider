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
- Applies a **scoreboard flair** via `CCSPlayerController_InventoryServices.m_rank`
- Applies an optional **custom PNG avatar** through `ServerAvatarOverrides`
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

Located in `/game/csgo/addons/BotHider/bot_info.json`.
Maps a persona name to a **32-bit Steam account ID**, optional crosshair share code, and optional scoreboard flair defidx.

```json
{
    "s1mple": {
        "steamid": 73936547,
        "crosshair_code": "CSGO-pE5f8-6RQvk-HLpdN-KW3J6-BQwLA",
        "scoreboard_flair": 874
    }
}
```

On spawn, BotHider selects an entry from this file (preferring one matching the engine's proposed bot name, otherwise random unused entry).
The selected entry supplies the **SteamID**, **crosshair**, **scoreboard flair**, and **ping** for the bot.
Display name is controlled separately by `bh_namesource`.

`scoreboard_flair` is a CS2 item definition index. Missing, invalid, or `0` values are treated as clear/no flair. Use [unicbm/cs2-econ-id-index](https://github.com/unicbm/cs2-econ-id-index) to look up valid scoreboard flair item definition IDs.

------------------------------------------------------------------------

## Exposed Interface (C#)

```csharp
public interface IBotHiderApi
{
    // --- read ---
    bool     IsManagedBot(int slot);        // is this one of ours?
    ulong    GetBotSteamId(int slot);        // assigned SteamID64 (0 if none)
    int[]    GetManagedSlots();             // all managed bot slots
    string   GetPersonaName(int slot);      // assigned display name
    int      GetPing(int slot);             // current jittered ping (ms)
    string   GetCrosshairCode(int slot);    // assigned crosshair share-code
    bool     HasBotAvatar(int slot);        // native override is active for the current SteamID
    uint     GetScoreboardFlair(int slot);  // assigned scoreboard flair defidx
    (string Name, ulong Addr)[] GetSignatures();

    // --- write (applied on the next server frame) ---
    bool     SetBotSteamId(int slot, ulong steamId64);
    bool     SetCrosshairCode(int slot, string code); // empty or "0" to clear
    bool     SetBotAvatar(int slot, string pngPath);  // valid PNG up to 16 KiB, or "0" to clear
    bool     SetPersonaName(int slot, string name);
    bool     SetScoreboardFlair(int slot, uint itemDefIndex);

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
        $"slot={slot} sid={_api.GetBotSteamId(slot)} " +
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

## Custom Avatar Pipeline

`SetBotAvatar` is implemented by the managed/native bridge rather than by CounterStrikeSharp schema writes:

1. `BotHiderImpl` resolves the server-local path and rejects missing, empty, non-PNG, or larger-than-16-KiB files before reading the complete file.
2. The PNG bytes, byte length, request sequence, and current slot incarnation are written to a per-slot shared-memory region. An odd/even seqlock prevents native code from consuming a partially written PNG.
3. The native plugin processes changed requests on the game thread, enables `sv_reliableavatardata`, and finds the `ServerAvatarOverrides` network string table.
4. The bot's final SteamID64 is used as the string-table key and the PNG bytes are stored as its user data. Index `0` is reserved as an empty sentinel because player avatar data must use a nonzero index.
5. Applied state and the applied SteamID64 are published back to C#, which is what `HasBotAvatar` and `bh_status` report.

The shared-memory wire version remains `v1`. The mapping is enlarged to `1,064,960` bytes and contains a 16-KiB PNG area for each of the 64 player slots, so the native plugin and `BotHiderImpl` must be updated together.

Avatar requests are bound to the current native slot incarnation. A disconnected bot therefore cannot leak its avatar to a new bot that later occupies the same slot. If the managed bot's SteamID changes, native code clears the old SteamID entry and reapplies the same PNG under the final new SteamID. A recreated map string table also forces reapplication.

```csharp
if (_api.SetBotAvatar(slot, @"E:\avatars\player.png"))
    Console.WriteLine("Avatar queued");

// SetBotAvatar reports that the request was accepted; native applies it next frame
bool applied = _api.HasBotAvatar(slot);

_api.SetBotAvatar(slot, "0");
```

Console equivalents:

```text
bh_setavatar <slot> <png_path|0>
```

Use `0` in place of `png_path` to clear the avatar. The command accepts server console/RCON callers and clients with CounterStrikeSharp `@css/root`. `bh_status` is available to both clients and the server and includes `avatar=<applied>/<configured_bytes>` for each managed slot.

`ServerAvatarOverrides` reliably changes the scoreboard avatar. The compact score strip is a separate CS2 HUD surface and can retain cached avatar state; BotHider cannot force that client-side cache to refresh through the network string table alone.

------------------------------------------------------------------------

## Scoreboard Flair

Default flair selection happens in the C++ plugin:

1. `BotInfoStore` reads `scoreboard_flair` from `bot_info.json`.
2. Missing, invalid, or `0` values are kept as clear/no flair.
3. The selected value is published through shared memory at `kOff_ScoreboardFlair`.
4. The C# plugin reads that value and writes every entry in `InventoryServices.Rank`.

Runtime overrides stay in C#:

```csharp
uint current = _api.GetScoreboardFlair(3);

if (_api.SetScoreboardFlair(3, 4974))
    Console.WriteLine("Scoreboard flair applied");
```

The console equivalent is:

```text
bh_setflair <slot> <item_def_index>
```

Use `0` to clear the flair.

------------------------------------------------------------------------

## License

CS2-Bot-Hider is licensed under the GNU Affero General Public License version 3 (AGPL-3.0).
Commercial use involving closed-source distribution or hosted services may require a separate license.
See the LICENSE file for details.

------------------------------------------------------------------------

## Author

- **XBribo**
- Other contributors
