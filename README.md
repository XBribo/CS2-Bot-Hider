# BotHider

**Make Bot Vivid Again**

> Developer guide: [TECH.md](TECH.md).

## Your stars⭐ are my motivation to keep updating

------------------------------------------------------------------------

## Overview

`BotHider` is a plugin for **Counter-Strike 2** that makes bots look like real human players.

- Removes the `BOT` tag from the scoreboard
- Each bot gets its own SteamID64, display name, ping, crosshair, scoreboard flair, and optional custom PNG avatar

------------------------------------------------------------------------

## Install

1. Install Metamod:Source and CounterStrikeSharp.
2. Download the Windows or Linux package from [Releases](https://github.com/XBribo/CS2-Bot-Hider/releases/latest).
3. Copy its `addons/` folder into `game/csgo/` and restart the server.

The packaged `addons/BotHider/config.json` selects the identity mode and fake-ping behavior. Use `"player"` for synthetic-player presentation or `"bot"` to retain Valve's bot flags. Fake ping stays within the configured range. Missing settings use defaults.

------------------------------------------------------------------------

## Console Commands

| Command | Description |
| --------- | ------------- |
| `bh_status` | Show every bot's details |
| `bh_setname <slot> <name>` | Change a bot's name |
| `bh_setsid <slot> <SteamID64>` | Change a bot's SteamID |
| `bh_setcrosshair <slot> <code>` | Change a bot's crosshair (`0` clears it) |
| `bh_setflair <slot> <item_def_index>` | Change a bot's scoreboard flair (`0` clears it) |
| `bh_setavatar <slot> <png_path/0>` | Apply a server-local PNG avatar, or use `0` to clear it |
| `bh_identity_mode <player/bot>` | Change the managed-bot identity mode |
| `bh_namesource <0/1>` | **0** = use name from `botprofile.db` (default)<br>**1** = use name from `bot_info.json` (only affects new bots) |

### Command Permissions

All `bh_*` commands, including avatars, are public by default. To require permission, set this in `addons/BotHider/config.json` and reload `BotHiderImpl`:

```json
"commands": {
    "require_permissions": true,
    "permission": "@bothider/admin"
}
```

- Players need the configured permission, `@css/root`, or `@css/admin`. Server console and RCON are always allowed.
- Missing settings keep commands public; invalid or unreadable config uses restricted defaults.

For a BotHider-only group, merge this into CSS `configs/admin_groups.json`, keeping existing groups:

```json
{
    "#bothider/admin": {
        "flags": ["@bothider/admin"]
    }
}
```

Add `"#bothider/admin"` to the admin's `groups` in CSS `configs/admins.json`, then run `css_groups_reload` followed by `css_admins_reload` from the server console.

If you change `commands.permission`, update the group's flag to match. Disabling checks lets any player modify bots and select server-local avatar PNGs.

------------------------------------------------------------------------

## Custom Bot Avatars

BotHider can apply a server-local PNG file as the avatar of a managed bot. The file must:

- Be a valid, non-empty PNG
- Be no larger than **16 KiB**
- Be readable by the game server

Use the engine player slot shown by `bh_status`. Quote paths that contain spaces:

```text
bh_setavatar 1 "E:/game/csgo/addons/avatars/player.png"
bh_setavatar 1 0
```

This command follows the same configurable permission rules as every other `bh_*` command. `bh_status` reports avatar state as `avatar=<applied>/<bytes>`, for example `avatar=True/8011B`.

The avatar is associated with the bot's final SteamID64. BotHider automatically rebinds it when that bot's SteamID changes, clears the old override when the bot disconnects, and prevents a new bot in the same slot from inheriting it.

CS2 uses separate caches for some HUD surfaces. `ServerAvatarOverrides` updates the scoreboard avatar, but the compact score strip can retain its previous cached avatar and is not guaranteed to refresh immediately.

------------------------------------------------------------------------

## Custom Identities (bot_info.json)

Edit `game/csgo/addons/BotHider/bot_info.json` to define bot identities:

```json
{
  "players": {
    "73936547": {
        "player_name": "s1mple",
        "crosshair_code": "CSGO-pE5f8-6RQvk-HLpdN-KW3J6-BQwLA",
        "scoreboard_flair": 6034
    },
    "153400465": {
        "player_name": "ZywOo",
        "crosshair_code": "CSGO-FqJYj-kLuW3-V2QZ3-xbkQK-PHPYE",
        "scoreboard_flair": 5226,
        "clan_tag": "DRAGODINDE",
        "clan_group_id": 33294804
    }
  }
}
```

- **players keys**: Nonzero 32-bit account IDs, converted to SteamID64 automatically.
- **player_name**: Required name, used to match the engine's bot name.
- **crosshair_code**: The crosshair share code to apply to the bot (optional).
- **scoreboard_flair**: The item definition index used as the bot's scoreboard flair (optional, `0` clears it).
- **clan_tag**, **clan_group_id**: Optional pair for the bot's displayed Steam group tag and 32-bit group ID. Both must be present; invalid or incomplete pairs are ignored.

Clan tags also require the server setting `sv_clantags_enabled 1`.

If `scoreboard_flair` is missing, invalid, or set to `0`, BotHider leaves the scoreboard flair empty.
Use [unicbm/cs2-econ-id-index](https://github.com/unicbm/cs2-econ-id-index) to look up valid scoreboard flair item definition IDs.

BotHider prefers an exact `player_name` match with the engine's bot name; otherwise it picks an unused entry, reusing one if necessary. Set `bh_namesource 1` to use the selected entry's name for newly created bots.

------------------------------------------------------------------------

## FAQ

**Q: Why does a bot use a different identity from the one I edited?**

A: Account IDs are keys inside `players`, not a `steamid` field. Set `player_name` to the bot's `botprofile.db` name for a preferred match. Restart the server after editing and spawn the bot again. `bh_namesource` controls display names, not identity selection.

**Q: Can I give the same SteamID to multiple bots?**

A: Give each bot a distinct account ID. If the requested SteamID is already in use, BotHider selects another available ID, so the final SteamID may differ from the configured one.

**Q: Can I change a bot's identity in game?**

A: Yes. Use `bh_setsid <slot> <SteamID64>` and `bh_setname <slot> <name>` to assign a new SteamID or name to a bot already in the game.

For API integration and build instructions, see [TECH.md](TECH.md).

------------------------------------------------------------------------

## Special thanks

- [replica](https://github.com/44076-meow/replica)
- [un1](https://github.com/unicbm)
- [ed0ard](https://github.com/ed0ard)
- [御坂17032号](https://github.com/Misaka17032)
- [Miksen](https://github.com/mrc4tt)

------------------------------------------------------------------------

## License

CS2-Bot-Hider is licensed under the GNU Affero General Public License version 3 (AGPL-3.0).
Commercial use involving closed-source distribution or hosted services may require a separate license.
See the LICENSE file for details.

------------------------------------------------------------------------

## Author

- **XBribo**
- Other contributors
