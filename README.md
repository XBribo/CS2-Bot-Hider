<div align="center">

# BotHider

**Make Bot Vivid Again**

<p>
  <a href="https://github.com/XBribo/CS2-Bot-Hider/releases/latest"><img src="https://img.shields.io/github/v/release/XBribo/CS2-Bot-Hider?style=flat-square" alt="Release"></a>
  <a href="https://github.com/XBribo/CS2-Bot-Hider/releases/latest"><img src="https://img.shields.io/badge/platforms-Windows%20%7C%20Linux-0078D4?style=flat-square" alt="Platforms"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-AGPL--3.0-blue?style=flat-square" alt="License"></a>
</p>

[Quick Start](#quick-start) · [Configuration](#configuration) · [Commands](#commands) · [Customization](#customization) · [Developer Guide](TECH.md)

</div>

A Counter-Strike 2 plugin that removes the scoreboard's `BOT` tag and gives bots custom SteamID64s, names, ping, crosshairs, scoreboard flair, clan tags, and PNG avatars.

### Signature Status

[![CS2 Bot Signatures Tracker](https://raw.githubusercontent.com/XBribo/cs2-bot-signatures/main/.github/docs/summary.svg)](https://github.com/XBribo/cs2-bot-signatures)

## Quick Start

1. Install Metamod:Source and CounterStrikeSharp.
2. Download the Windows or Linux package from [Releases](https://github.com/XBribo/CS2-Bot-Hider/releases/latest).
3. Copy `addons/` into `game/csgo/` and restart the server.

For paired reloads, unload `BotHiderImpl` before native `BotHider`; load them in reverse order using matching package files.

## Configuration

Edit `game/csgo/addons/BotHider/config.json`. See the [default config](configs/addons/BotHider/config.json).

| Setting | Default | Description |
| --- | --- | --- |
| `identity_mode` | `"player"` | Player-like presentation; `"bot"` retains Valve's bot flags |
| `fake_ping.enabled` | `true` | Fake ping between `fake_ping.min` and `fake_ping.max` (20–90 ms by default) |
| `commands.require_permissions` | `false` | Require admin permission for all `bh_*` commands |
| `commands.permission` | `"@bothider/admin"` | Required flag when checks are enabled |

> **Public servers:** all commands are public by default. Set `commands.require_permissions` to `true` and reload `BotHiderImpl` to restrict access.

## Commands

Use engine player slots (`0–63`) from `bh_status`. Quote names and paths containing spaces.

| Command | Description |
| --- | --- |
| `bh_status` | List managed bots and details |
| `bh_setname <slot> <name>` | Set display name |
| `bh_setsid <slot> <SteamID64>` | Set SteamID64 |
| `bh_setcrosshair <slot> <code>` | Set crosshair; `0` clears |
| `bh_setflair <slot> <item_def_index>` | Set scoreboard flair; `0` clears |
| `bh_setavatar <slot> <png_path\|0>` | Set server-local PNG avatar; `0` clears |
| `bh_identity_mode <player\|bot>` | Set identity mode |
| `bh_namesource <0\|1>` | Names from `botprofile.db` (**0**, default) or `bot_info.json` (**1**); new bots only |

<details>
<summary><strong>Permissions and admin groups</strong></summary>

When checks are enabled, players need `commands.permission`, `@css/root`, or `@css/admin`. Console and RCON bypass checks. Missing settings keep commands public; invalid or unreadable config restricts access.

Add a BotHider group to CSS `configs/admin_groups.json`, preserving existing groups:

```json
{
    "#bothider/admin": {
        "flags": ["@bothider/admin"]
    }
}
```

Add `"#bothider/admin"` to the admin's `groups` in CSS `configs/admins.json`. Run `css_groups_reload`, then `css_admins_reload` from the server console.

Keep the group's flag in sync with `commands.permission`.

</details>

## Customization

### Bot Identities

Edit `game/csgo/addons/BotHider/bot_info.json`, then restart the server and respawn bots:

```json
{
  "players": {
    "73936547": {
      "player_name": "s1mple",
      "crosshair_code": "CSGO-pE5f8-6RQvk-HLpdN-KW3J6-BQwLA",
      "scoreboard_flair": 6034,
      "clan_tag": "DRAGODINDE",
      "clan_group_id": 33294804
    }
  }
}
```

- **Identity:** `players` keys are nonzero 32-bit account IDs, auto-converted to SteamID64. Use unique IDs; duplicates may be replaced.
- **Name:** `player_name` is required. Exact engine-name matches take priority; otherwise, an unused entry is chosen, reusing one if needed. `bh_namesource 1` applies the entry's name to new bots.
- **Crosshair and flair:** optional. Missing, invalid, or `0` flair stays empty. Look up flair IDs in [cs2-econ-id-index](https://github.com/unicbm/cs2-econ-id-index).
- **Clan tag:** `clan_tag` and the 32-bit `clan_group_id` must both be valid. Requires `sv_clantags_enabled 1`.

### Custom Avatars

Use a valid server-local PNG up to **16 KiB**, readable by the server:

```text
bh_setavatar 1 "E:/game/csgo/addons/avatars/player.png"
bh_setavatar 1 0
```

`bh_status` shows avatar state and size, e.g. `avatar=True/8011B`.

> The scoreboard updates, but the compact score strip may keep a cached avatar.

## Credits

Thanks to the [replica](https://github.com/44076-meow/replica) project.

Created by [XBribo](https://github.com/XBribo), with thanks to [un1](https://github.com/unicbm), [ed0ard](https://github.com/ed0ard), [御坂17032号](https://github.com/Misaka17032), [Miksen](https://github.com/mrc4tt), and all contributors.

[![BotHider contributors](https://contrib.rocks/image?repo=XBribo/CS2-Bot-Hider)](https://github.com/XBribo/CS2-Bot-Hider/graphs/contributors)

Star BotHider to support development.

## License

CS2-Bot-Hider is licensed under the GNU Affero General Public License version 3 (AGPL-3.0).
Commercial use involving closed-source distribution or hosted services may require a separate license.
See [LICENSE](LICENSE) for details.
