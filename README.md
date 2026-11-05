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

Give CS2 bots player names, avatars and ping on the scoreboard, and hide the `BOT` tag.

<details>
<summary>Signature Status</summary>

[![Signature Status](https://raw.githubusercontent.com/XBribo/cs2-bot-signatures/main/.github/docs/summary.svg)](https://github.com/XBribo/cs2-bot-signatures)

</details>

## Quick Start

1. Install Metamod:Source and CounterStrikeSharp.
2. Download the Windows or Linux package from [Releases](https://github.com/XBribo/CS2-Bot-Hider/releases/latest).
3. Copy `addons/` into `game/csgo/` and restart the server.
4. Run `bh_status` to list managed bots.

## Configuration

Edit [`config.json`](configs/addons/BotHider/config.json) in `game/csgo/addons/BotHider/`.

| Setting | Default | Description |
| --- | --- | --- |
| `identity_mode` | `"player"` | Show bots as players; use `"bot"` to keep the bot tag |
| `fake_ping.enabled` | `true` | Show simulated ping; `fake_ping.min` and `max` set the range (20–90 ms by default) |
| `commands.require_permissions` | `false` | Restrict commands to admins or players with `commands.permission` (default: `@bothider/admin`) |

**Public servers:** enable `commands.require_permissions` before opening the server to players. Otherwise anyone can run the commands below. The server console and RCON keep access.

## Commands

Use player slots (`0–63`) from `bh_status`. Quote names and paths containing spaces.

| Command | Description |
| --- | --- |
| `bh_status` | List managed bots and details |
| `bh_setname <slot> <name>` | Set display name |
| `bh_setsid <slot> <SteamID64>` | Set SteamID64 |
| `bh_setcrosshair <slot> <code>` | Set crosshair; `0` clears |
| `bh_setflair <slot> <item_def_index>` | Set scoreboard flair; `0` clears |
| `bh_setavatar <slot> <file.png\|0>` | Set a PNG avatar from `addons/avatars/`; `0` clears |
| `bh_identity_mode <player\|bot>` | Set identity mode |
| `bh_namesource <0\|1>` | Names for new bots: `0` = engine (default), `1` = configured name |

## Customization

### Bot identities

Add one entry per bot to [`bot_info.json`](configs/addons/BotHider/bot_info.json), using a unique Steam account ID as the key. Only `player_name` is required:

```json
{
  "players": {
    "73936547": {
      "player_name": "s1mple",
      "crosshair_code": "CSGO-pE5f8-6RQvk-HLpdN-KW3J6-BQwLA",
      "scoreboard_flair": 6034
    }
  }
}
```

Restart the server after editing the file. Use `bh_namesource 1` for new bots to take their configured names. If all entries are in use, additional bots keep their normal appearance.

For clan tags, set both `clan_tag` and `clan_group_id` and enable `sv_clantags_enabled 1`. For flair IDs, see [cs2-econ-id-index](https://github.com/unicbm/cs2-econ-id-index).

### Custom avatars

Put a PNG up to **16 KiB** in `game/csgo/addons/avatars/`. To set it for slot 1:

```text
bh_setavatar 1 player.png
```

After replacing the image, run the same command again. Use `bh_setavatar 1 0` to clear it.

## Credits

Thanks to the [replica](https://github.com/44076-meow/replica) project.

Created by [XBribo](https://github.com/XBribo), with thanks to [un1](https://github.com/unicbm), [ed0ard](https://github.com/ed0ard), [御坂17032号](https://github.com/Misaka17032), [Miksen](https://github.com/mrc4tt), and all contributors.

[![BotHider contributors](https://contrib.rocks/image?repo=XBribo/CS2-Bot-Hider)](https://github.com/XBribo/CS2-Bot-Hider/graphs/contributors)

## License

[GNU Affero General Public License v3.0](LICENSE).
