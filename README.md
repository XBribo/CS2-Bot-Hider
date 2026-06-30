# BotHider

**Make Bot Vivid Again**

> For developer, see [TECH.md](TECH.md).

## Your stars⭐ are my motivation to keep updating

------------------------------------------------------------------------

## Overview

`BotHider` is a plugin for **Counter-Strike 2** that makes bots look like real human players.

When a bot joins, BotHider automatically:

- Hides the `BOT` label on the scoreboard
- Gives the bot a unique SteamID
- Gives the bot a realistic player name
- Adds a fake ping (with random jitter)

------------------------------------------------------------------------

## Features

- Removes the `BOT` tag from the scoreboard
- Each bot gets its own SteamID64, display name, ping, and crosshair
- Toggle disguise on/off with a simple command
- Choose whether bot names come from `botprofile.db` or a custom list

------------------------------------------------------------------------

## Console Commands

| Command | Description |
|---------|-------------|
| `bh_status` | Show every bot's details: slot, SteamID, name, ping, crosshair |
| `bh_setname <slot> <name>` | Change a bot's name |
| `bh_setsid <slot> <SteamID64>` | Change a bot's SteamID |
| `bh_disguise <0/1>` | Turn bot disguise off (0) or on (1) |
| `bh_namesource <0/1>` | **0** = use name from `botprofile.db` (default)<br>**1** = use name from `bot_info.json` (only affects new bots) |

------------------------------------------------------------------------

## Install

1. Download the latest `BotHider-windows.zip` or `BotHider-linux.zip` from the [Releases page](https://github.com/XBribo/CS2-Bot-Hider/releases/latest).
2. Extract the archive and copy the `/addons/` folder into your server's `/game/csgo/` directory.
3. Restart the server.

------------------------------------------------------------------------

## Custom Identities (bot_info.json)

You can create a file named `bot_info.json` inside `/game/csgo/addons/bothider/` to define custom identities for your bots. Example:

```json
{
    "s1mple": {
        "steamid": 73936547,
        "crosshair_code": "CSGO-pE5f8-6RQvk-HLpdN-KW3J6-BQwLA"
    },
    "ZywOo": {
        "steamid": 12345678,
        "crosshair_code": "CSGO-xxxxx-xxxxx-xxxxx-xxxxx-xxxxx"
    }
}
```

- **steamid**: The 32-bit account ID (will be converted to a full SteamID64 automatically).
- **crosshair_code**: The crosshair share code to apply to the bot (optional).

When a bot is spawned, BotHider will pick an identity from this list (preferring a name match if possible).  
To use the names from this file as the bot's **display name**, set `bh_namesource 1`.

------------------------------------------------------------------------

## FAQ

**Q: I changed the `steamid` in an existing `bot_info.json` entry, but the bot still loads the old account. Why?**

A: The plugin selects an identity entry by matching the bot's name (from `botprofile.db`) against the JSON **keys**, not by the SteamID value. If no name match is found, a random entry is used. To force a specific bot to use a specific Steam profile:

1. Use a real `botprofile.db` bot name as the key in `bot_info.json`.
2. Set your custom `steamid` (32-bit account ID) and `crosshair_code` under that key.
3. Set `bh_namesource 1` so the display name also comes from this entry (ensuring a one-to-one name/SteamID link).
4. Spawn the bot with `bot_add <that name>`.

**Q: Can I give the same SteamID to multiple bots (e.g., to share a team logo avatar)?**

A: No. The CS2 scoreboard distinguishes players by SteamID. If multiple bots share the same SteamID, some will not appear correctly. Each bot that needs a specific avatar must have its own distinct SteamID (a separate Steam account with that avatar).

**Q: Can I change a bot's identity without restarting the server?**

A: Yes. Use `bh_setsid <slot> <SteamID64>` and `bh_setname <slot> <name>` to assign a new SteamID or name to a bot already in the game.

For more technical details on how identities are assigned, see [TECH.md](TECH.md).

------------------------------------------------------------------------

## Special thanks

- [cs2-insanity](https://github.com/Frad70/cs2-insanity) for helping determine the framework.
- [CS2Fixes](https://github.com/Source2ZE/CS2Fixes) for helping identify the `UTIL_Remove` signature.
- [Misaka17032](https://github.com/Misaka17032) for adding Linux support.
- [ed0ard](https://github.com/ed0ard) for helping with testing and bug fixes.

------------------------------------------------------------------------

## License

CS2-Bot-Hider is licensed under the GNU Affero General Public License version 3 (AGPL-3.0).
Commercial use involving closed-source distribution or hosted services may require a separate license.
See the LICENSE file for details.

------------------------------------------------------------------------

## Author

- **XBribo**
- Other contributors
