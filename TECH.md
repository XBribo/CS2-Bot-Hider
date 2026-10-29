# BotHider Developer Guide

Installation and usage: [README.md](README.md).

- [Build](#build)
- [C# Integration](#c-integration)
- [API Reference](#api-reference)
- [Behavior and Limits](#behavior-and-limits)

## Build

Run commands from the repository root.

### Requirements

- **Windows:** Visual Studio 2026 C++ build tools and CMake.
- **Linux:** `g++`, CMake, and Make.
- **C#:** .NET 10 SDK.
- **Build script:** PowerShell 7+; WSL for Linux (`Ubuntu-24.04` by default).
- **Dependencies:** Git and the `cs2_sdk` submodule.

```bash
git submodule update --init --recursive
```

### Build Script

On a Windows host:

```powershell
./build.ps1                  # Windows, Linux (WSL), and C#
./build.ps1 -Windows         # Windows and C#
./build.ps1 -Linux -CSharp   # Linux (WSL) and C#
./build.ps1 -CSharp          # C# only
```

Packages: `dist/windows/` and `dist/linux/`. C# output: `build/csharp/`. Add `-CSharp` to Linux builds to include the C# provider. Use `-Clean` for a clean rebuild.

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

C# provider and API contract:

```bash
dotnet build csharp/BotHiderImpl/BotHiderImpl.csproj -c Release -o build/csharp
```

C++ output: `<build-directory>/package/addons/`. Use `build.ps1` for complete packages.

## C# Integration

Install the BotHider Metamod plugin and `BotHiderImpl`. Shared API contract:

```text
game/csgo/addons/counterstrikesharp/shared/BotHiderApi/BotHiderApi.dll
```

Reference the matching `BotHiderApi.dll` in your CounterStrikeSharp plugin. Set `HintPath` to the DLL and `Private=false` to use the shared contract:

```xml
<ItemGroup>
  <Reference Include="BotHiderApi">
    <HintPath>libs/BotHiderApi.dll</HintPath>
    <Private>false</Private>
  </Reference>
</ItemGroup>
```

Resolve `bothider:api` after plugins load and check for null:

```csharp
using System.Collections.Generic;
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

    // Resolve the API after providers load.
    public override void OnAllPluginsLoaded(bool hotReload)
    {
        IBotHiderApi? api;
        try { api = BotHider.Get(); }
        catch (KeyNotFoundException) { return; }
        if (api == null) return;

        foreach (int slot in api.GetManagedSlots())
        {
            Server.PrintToConsole(
                $"slot={slot} name='{api.GetPersonaName(slot)}' sid={api.GetBotSteamId(slot)}");
        }
    }
}
```

See CounterStrikeSharp's [shared plugin API guide](https://docs.cssharp.dev/docs/features/shared-plugin-api.html) for details.

## API Reference

Contract: [IBotHiderApi.cs](csharp/BotHiderApi/IBotHiderApi.cs). Use player slots (`CCSPlayerController.Slot`) and check ownership with `IsManagedBot(slot)`.

### Read State

| Method | Returns |
| --- | --- |
| `IsManagedBot(int slot)` | `bool`: slot is managed by BotHider |
| `GetManagedSlots()` | `int[]`: managed bot slots |
| `GetBotSteamId(int slot)` | `ulong`: assigned SteamID64 |
| `GetPersonaName(int slot)` | `string`: assigned display name |
| `GetPing(int slot)` | `int`: displayed ping in milliseconds |
| `GetCrosshairCode(int slot)` | `string`: assigned crosshair share code |
| `HasBotAvatar(int slot)` | `bool`: avatar applied to the current SteamID64 |
| `GetScoreboardFlair(int slot)` | `uint`: flair item definition index; `0` = empty |
| `GetSignatures()` | `(string Name, ulong Addr)[]`: hook addresses; `0` = unresolved |

### Change a Bot

Setters return `bool`; client presentation updates asynchronously. See [Behavior and Limits](#behavior-and-limits).

| Method | Input |
| --- | --- |
| `SetBotSteamId(int slot, ulong steamId64)` | Full SteamID64 |
| `SetPersonaName(int slot, string name)` | Visible name, normalized to at most 31 UTF-8 bytes |
| `SetCrosshairCode(int slot, string code)` | Crosshair share code; empty string or `"0"` clears it |
| `SetBotAvatar(int slot, string pngPath)` | Server-local PNG path; `"0"` clears it |
| `SetScoreboardFlair(int slot, uint itemDefIndex)` | Item definition index, at most `65535`; `0` clears it |

### Global Settings

| Method | Effect |
| --- | --- |
| `SetIdentityMode(BotIdentityMode mode)` | `Player` enables synthetic-player presentation; `Bot` retains Valve's bot flags |
| `SetNameSource(bool useBotInfo)` | New-bot names: `true` = `bot_info.json`; `false` = `botprofile.db` |

Returns `bool`. Changes apply to runtime state.

## Behavior and Limits

### Identity Updates

- API calls run on the game thread through C exports. Identity and global setters report acceptance; client presentation follows on `Server.NextFrame`.
- Name setters publish the accepted name. Crosshair and flair overrides persist in slot state across provider reloads.
- Names are trimmed, stripped of invisible-only text elements, and limited to `BotHiderContract.MaxPlayerNameUtf8Bytes` at Unicode text-element boundaries. A non-empty normalized name is required.
- Explicit SteamID64 conflicts and unavailable collision checks reject the request and preserve the current identity. Automatic assignment selects an available configured identity.
- Player mode requires the active server's change-level hook. Failed level-hook rebinding switches to Bot mode.
- `BotHiderImpl` patches `CCSPlayerController.IsBot` to return `true` for managed bots. Use `IsManagedBot` to check BotHider ownership.

### Paired Reloads

- `BotHider` and `BotHiderImpl` ship together; their C layouts and exports form a private contract.
- Unload `BotHiderImpl`, unload `BotHider`, then load `BotHider` and `BotHiderImpl` in that order.
- `BotHider` unload is rejected while the provider is attached. Disposed API references remain unavailable; acquire the capability again after reload.
- `BotHider` reload re-adopts existing bots from configured identities and resets runtime overrides.

### Avatars

- `SetBotAvatar` validates and queues the file. Check applied state later with `HasBotAvatar`.
- Requests belong to the bot occupying the slot. Overrides follow its SteamID64 changes and clear on disconnect.
- Scoreboard avatars use `ServerAvatarOverrides`. Prefer absolute file paths.

File requirements and HUD caching: [Custom Avatars](README.md#custom-avatars).
