# BotHider Developer Guide

Build BotHider or consume its C# API from another CounterStrikeSharp plugin.
For installation, configuration, and console commands, see [README.md](README.md).

- [Build](#build)
- [C# Integration](#c-integration)
- [API Reference](#api-reference)
- [Behavior and Limits](#behavior-and-limits)

## Build

Run commands from the repository root. Requirements:

- **Windows:** Visual Studio 2026 C++ build tools and CMake.
- **Linux:** `g++`, CMake, and Make.
- **C#:** .NET 10 SDK.
- **Build script:** PowerShell 7 or newer; Linux builds use WSL (`Ubuntu-24.04` by default).
- **Dependencies:** Git and recursively initialized submodules.

```bash
git submodule update --init --recursive
```

Dependencies are provided by the `cs2_sdk` submodule.

### Build Script

On a Windows host:

```powershell
./build.ps1                  # Windows, Linux (WSL), and C#
./build.ps1 -Windows         # Windows and C#
./build.ps1 -Linux -CSharp   # Linux (WSL) and C#
./build.ps1 -CSharp          # C# only
```

Packages are assembled in `dist/windows/` and `dist/linux/`. C#-only output is in `build/csharp/`. `-Linux` without `-CSharp` excludes C#. Add `-Clean` for a clean rebuild.

### Manual Build

Windows:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Linux:

```bash
cmake -S . -B out/linux -DCMAKE_BUILD_TYPE=Release
cmake --build out/linux -j$(nproc)
```

C# implementation and API contract:

```bash
dotnet build csharp/BotHiderImpl/BotHiderImpl.csproj -c Release -o build/csharp
```

CMake output is under `<build-directory>/package/addons/`. It excludes C# files; use the build script for a complete package.

## C# Integration

Install the BotHider Metamod plugin and `BotHiderImpl`. Keep one shared API contract at:

```text
game/csgo/addons/counterstrikesharp/shared/BotHiderApi/BotHiderApi.dll
```

Reference the matching `BotHiderApi.dll` in an existing CounterStrikeSharp plugin project. Adjust `HintPath` to its local location; `Private=false` avoids copying a second contract into the consumer plugin's output.

```xml
<ItemGroup>
  <Reference Include="BotHiderApi">
    <HintPath>libs/BotHiderApi.dll</HintPath>
    <Private>false</Private>
  </Reference>
</ItemGroup>
```

Resolve `bothider:api` after all plugins load and check for an unavailable provider:

```csharp
using BotHiderApi;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Capabilities;

public class BotHiderConsumer : BasePlugin
{
    public override string ModuleName => "BotHider Consumer";
    public override string ModuleVersion => "1.0.0";
    public override string ModuleAuthor => "Example";
    public override string ModuleDescription => "Reads managed bot identities";

    private static readonly PluginCapability<IBotHiderApi> BotHider = new("bothider:api");

    // Resolve the shared API after its provider has registered the capability.
    public override void OnAllPluginsLoaded(bool hotReload)
    {
        var api = BotHider.Get();
        if (api == null) return;

        foreach (int slot in api.GetManagedSlots())
        {
            Server.PrintToConsole(
                $"slot={slot} name='{api.GetPersonaName(slot)}' sid={api.GetBotSteamId(slot)}");
        }
    }
}
```

See CounterStrikeSharp's [shared plugin API guide](https://docs.cssharp.dev/docs/features/shared-plugin-api.html) for the contract/capability mechanism.

## API Reference

The contract is defined in [IBotHiderApi.cs](csharp/BotHiderApi/IBotHiderApi.cs). `slot` is the engine player slot (`CCSPlayerController.Slot.Value`), not a user ID or entity index. Check `IsManagedBot(slot)` before working with a particular player.

### Read State

| Method | Returns |
| --- | --- |
| `IsManagedBot(int slot)` | `bool`: whether BotHider manages this slot |
| `GetManagedSlots()` | `int[]`: managed bot slots |
| `GetBotSteamId(int slot)` | `ulong`: assigned SteamID64 |
| `GetPersonaName(int slot)` | `string`: assigned display name |
| `GetPing(int slot)` | `int`: displayed ping in milliseconds |
| `GetCrosshairCode(int slot)` | `string`: assigned crosshair share code |
| `HasBotAvatar(int slot)` | `bool`: avatar override is applied to the current SteamID |
| `GetScoreboardFlair(int slot)` | `uint`: scoreboard flair item definition index; `0` means none |
| `GetSignatures()` | `(string Name, ulong Addr)[]`: resolved addresses; `0` means unresolved |

### Change a Bot

All setters return `bool`. A successful request is not necessarily visible to clients immediately; see [Behavior and Limits](#behavior-and-limits).

| Method | Input |
| --- | --- |
| `SetBotSteamId(int slot, ulong steamId64)` | Full SteamID64, not a 32-bit account ID |
| `SetPersonaName(int slot, string name)` | Visible name, normalized to at most 31 UTF-8 bytes |
| `SetCrosshairCode(int slot, string code)` | Crosshair share code; empty string or `"0"` clears it |
| `SetBotAvatar(int slot, string pngPath)` | Server-local PNG path; `"0"` clears it |
| `SetScoreboardFlair(int slot, uint itemDefIndex)` | Item definition index, at most `65535`; `0` clears it |

### Global Settings

| Method | Effect |
| --- | --- |
| `SetIdentityMode(BotIdentityMode mode)` | `Player` enables synthetic-player presentation; `Bot` retains Valve's bot flags |
| `SetNameSource(bool useBotInfo)` | `true`: names from `bot_info.json`; `false`: names from `botprofile.db`. Affects newly created bots |

These methods also return `bool`. They change runtime state, not the configuration files.

## Behavior and Limits

### Identity Updates

- `SetBotSteamId`, `SetPersonaName`, and the global settings queue commands. `true` means queued, not applied. Requests can fail if shared memory is unavailable, the slot is no longer managed, or the command queue is full.
- Visible name and SteamID writes are scheduled with `Server.NextFrame`, not performed immediately. Do not use an immediate getter as proof that the engine/client has applied a change; `GetPersonaName` can already return a pending managed override.
- Crosshair and flair setters update managed/shared state and schedule presentation separately; they do not use the command queue.
- Names are trimmed, stripped of invisible-only text elements, and truncated at a complete Unicode text-element boundary to `BotHiderContract.MaxPlayerNameUtf8Bytes` (31). Empty normalized names are rejected.
- Give each bot a distinct SteamID64. If an ID is already in use, the plugin may apply a different one.
- `BotHiderImpl` patches `CCSPlayerController.IsBot` to return `true` for managed bots. Use `IsManagedBot` to identify BotHider ownership independently of that Harmony patch.

### Avatars

- The file must be a non-empty PNG no larger than **16 KiB**, readable by the server process. Prefer absolute paths.
- `SetBotAvatar` validates and queues the file; query `HasBotAvatar` later to check applied state.
- Avatar requests belong to the bot occupying the slot when requested. A replacement bot does not inherit them, and an active override is rebound when the same bot's SteamID changes.
- The scoreboard avatar uses `ServerAvatarOverrides`. Other HUD surfaces can retain cached avatars and may not refresh immediately.
