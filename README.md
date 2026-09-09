# mod-discord-chat

Native Discord integration for AzerothCore 3.3.5a (WotLK). The bot runs directly inside
`worldserver`: no separate Node.js process, no Eluna, no extra dependencies beyond the core's
Boost and OpenSSL. It acts as a bridge between the server and Discord: two-way global chat,
event logging, server status, presence, emojis, and in-game management commands.

All Discord networking runs on a dedicated worker thread, so the worldserver gameplay thread is
never blocked by Discord traffic.

```
[Global] [Discord] [faction icon] [class icon] Name: hello
```

## Features

| Feature | Description |
| --- | --- |
| Discord bot | Gateway websocket + REST on a dedicated worker thread |
| Global Chat bridge | Two-way chat with `azerothcore/mod-global-chat`, `azerothcore/mod-world-chat` or `Gozzim/mod-globalchat` |
| Webhooks | Game chat posted through an invisible-name, transparent-avatar webhook (no bot icon or name) |
| Class / faction icons | Discord senders show the same faction and class icons that `.chat` uses, with class-colored names |
| Emoji system | Configurable faction / class / race emojis, resolved by name from the guild or the bot application, cached |
| In-game commands | `.discord status`, `population`, `uptime`, `version`, `setup roles`, `reload`, `say` |
| Event logging | Logins, logouts, achievements, boss kills, bans, mutes, GM commands, server lifecycle, Discord member join/leave |
| Server status | One persistent status message, edited in place, survives restarts |
| Discord presence | Automatic bot activity with player counts |
| Playerbots | Separate real / playerbot / total population; bot chat suppressed by default |
| Chat filter | Simple slur/hate-speech filter, URL-safe |
| Role setup | `.discord setup roles` auto-creates the recommended Discord roles |

## Requirements

- AzerothCore 3.3.5a (WotLK), C++20
- Boost and OpenSSL (already used by AzerothCore)
- One Global Chat provider installed (`mod-global-chat`, `mod-world-chat`, or `Gozzim/mod-globalchat`)

## Installation

### 1. Get the module

```bash
cd "$ACORE_SOURCE/modules"
git clone <your-repo-url> mod-discord-chat
```

### 2. Configure and build

Re-run CMake (the module is auto-discovered), then build:

```bash
cd "$ACORE_BUILD"
cmake .. -DMODULES=static
cmake --build . --config RelWithDebInfo
```

### 3. Database

The module ships SQL in `data/sql/db-characters/` (`mod_discord_audit_log`, `mod_discord_status`).
The AzerothCore DB updater applies it automatically on first start. If the updater is disabled,
run it manually:

```bash
mysql -u root -p acore_characters < modules/mod-discord-chat/data/sql/db-characters/mod_discord_characters.sql
```

### 4. Install the configuration

Copy the template to the server config folder and edit it:

```bash
cp modules/mod-discord-chat/conf/discord-integration.conf.dist configs/modules/discord-integration.conf
```

At minimum, set:

```ini
Discord.Bot.Token = "YOUR_BOT_TOKEN"
Discord.Bot.ApplicationId = 123456789012345678
Discord.Bot.GuildId = 123456789012345678
Discord.GlobalChat.ChannelId = 123456789012345678
Discord.GlobalChat.Provider = "azerothcore"   # or AUTO
```

### 5. Start worldserver

Stop the running worldserver, then start the new build. Look for the `Discord Integration`
startup block in the log.

> Module diagnostics are INFO-level and hidden by AzerothCore's default log level. Add this to
> `configs/worldserver.conf` to see them:
> ```ini
> Logger.modules=3,Console Server
> ```

## Discord Developer Portal setup

### 1. Create the application

Go to <https://discord.com/developers/applications>, click **New Application**, name it, and
click **Create**.

### 2. Create the bot

Left menu, **Bot**, **Add Bot**, confirm. Set the bot icon or name if desired.

### 3. Enable privileged intents

On the **Bot** page, enable **Server Members Intent** and **Message Content Intent**, then
**Save Changes**. These must be enabled here as well as in the config, or Discord rejects the
connection.

### 4. Copy the token

**Bot**, **Reset Token**, **Copy**. This is `Discord.Bot.Token`. Keep it secret and never commit
it.

### 5. Copy the application ID

**General Information**, **Application ID**, Copy. This is `Discord.Bot.ApplicationId`.

### 6. Generate the invite URL

**OAuth2**, **URL Generator**:

- **Scopes:** `bot` (and optionally `applications.commands` for the optional slash commands).
- **Bot permissions:** `View Channels`, `Send Messages`, `Manage Messages`, `Embed Links`,
  `Read Message History`, `Manage Roles`, `Manage Webhooks`, and `Use Slash Commands` if the
  commands scope is selected.

Open the generated URL, select the server, and click **Authorize**.

### 7. Get IDs

Enable Developer Mode (User Settings, Advanced, Developer Mode), then right-click:

- Server icon, **Copy Server ID** -> `Discord.Bot.GuildId`
- The global chat channel, **Copy Channel ID** -> `Discord.GlobalChat.ChannelId`
- (optional) events channel -> `Discord.Events.ChannelId`
- (optional) status channel -> `Discord.ServerStatus.ChannelId`
- A role (Server Settings, Roles), **Copy Role ID** -> role config

## Global Chat

### Provider

```ini
Discord.GlobalChat.Provider = "AUTO"
```

Allowed values: `AUTO`, `azerothcore`, `world-chat`, `gozzim`, `none`. `AUTO` detects the
installed provider from the world DB `command` table (`chat`, `world`, or `global`). The
detected provider is shown at startup (`Provider: mod-global-chat`).

### In-game usage (mod-global-chat)

Players opt in with `.chat on`, then chat with `.chat <message>`. Discord messages are delivered
only to players who have `.chat on`, formatted like a normal global chat message.

### Discord to game

Type a normal message in the global Discord channel. It appears in-game as:

```
[Global] [Discord] [faction icon] [class icon] Name: message
```

![Discord to game](https://i.imgur.com/It1mM40.png)

The name is the Discord display name (nickname if set), colored by class when a class role is
assigned. `[Discord]` is blurple, message text is white. `@everyone`, `@here`, and mentions are
stripped; messages starting with `/` are not relayed; links pass through normally.

### Game to Discord

`.chat <message>` (or `.discord say <message>`) is posted to Discord through a webhook with an
invisible username and transparent avatar, so only the content shows:

```
:classdk: **Name**: message
```

![Game to Discord](https://i.imgur.com/E4oxZh5.png)

If a webhook URL is not configured, the module auto-creates one (invisible name and transparent
avatar). The bot needs the **Manage Webhooks** permission. Messages that use application emojis
fall back to a bot post (which shows the bot icon); keep emojis in the guild for the icon-free
look.

## Roles, class/faction icons and colors

### Auto-create the roles

In-game (GM): `.discord setup roles`. The bot (with **Manage Roles**) creates the factions
(Alliance/Horde), the ten races, the ten classes, and `Admin` plus `Moderator` (positioned above
the others), then reports every role ID.

### Configure which roles map to icons

```ini
Discord.Roles.Tags = "1234567890:Alliance,1234567890:Horde,1234567890:Paladin,1234567890:Orc"
```

Labels are matched by name (case-insensitive):

- `Alliance` or `Horde` -> faction icon.
- A class name (`Paladin`, `Warrior`, and so on) -> class icon, and the name is colored with the
  class color.
- Other labels are ignored for icons.

### Staff

```ini
Discord.Roles.AdminIds   = "1234...AdminRoleId"
Discord.Admin.AllowedRoleIds = "1234...AdminRoleId,1234...ModeratorRoleId"
```

`AdminIds` shows the Blizz icon and a red name in chat. `AllowedRoleIds` grants Discord admin
commands (if the bot has the commands scope).

## Emoji setup

```ini
Discord.Emojis.Enable = 1
Discord.Emojis.Faction.Enable = 1
Discord.Emojis.Class.Enable  = 1
Discord.Emojis.Race.Enable   = 1
Discord.Emojis.ResolveByName = 1
```

Emojis are resolved by name (no IDs). Upload them to the guild (or the bot application; guild
overrides app) using the naming scheme:

- Faction: `factionalliance`, `factionhorde`
- Classes: `classdk`, `warriorwarrior`, `classpaladin`, `classhunter`, `classrogue`, `classpriest`,
  `classshaman`, `classmage`, `classwarlock`, `classdruid`
- Races (gender suffix `ma`/`fe`): `racehumanma`, `racehumanfe`, `racedraeneima`, `racedraeneife`,
  and so on
- GM badge: `gmbadge`

Missing emojis are reported once and skipped. Refresh with `/config reload` or
`/discord emojis reload`.

## In-game commands

| Command | Description | Access |
| --- | --- | --- |
| `.discord status` | Server status (realm, players, uptime, expansion) | Everyone |
| `.discord population` | Player population (real / playerbots / total) | Everyone |
| `.discord uptime` | Server uptime | Everyone |
| `.discord version` | Realm / DB / module version | Everyone |
| `.discord setup roles` | Create the recommended Discord roles | GM+ |
| `.discord reload` | Reload module configuration | GM+ |
| `.discord say <msg>` | Relay a message to Discord | Everyone |
| `.discord help` | List commands | Everyone |

## Discord slash commands (optional)

If the bot was invited with the `applications.commands` scope, an optional admin console is
registered:

- `/server`: status, population, uptime, version, restart, shutdown, cancel
- `/player`: info, kick, mute, unmute, teleport, summon, level, money, item
- `/account`: info, ban, unban, mute, unmute
- `/announce`: global, notification, server
- `/config`: reload
- `/discord`: emojis reload
- `/setup`: roles
- `/console`: raw worldserver command (disabled by default)

Authorization uses `Discord.Admin.AllowedRoleIds` and `AllowedUserIds` only; a GM rank does not
grant Discord admin access. Destructive commands require Confirm/Cancel buttons, and every
action is written to `mod_discord_audit_log`.

## Event logging

```ini
Discord.Events.Enable = 1
Discord.Events.ChannelId = 123456789012345678
```

Compact single-line messages for logins, logouts, achievements, dungeon/raid/world boss kills
(`FinalBossOnly` supported), bans, mutes, GM commands, server startup/shutdown/restart, and
Discord member join/leave. Normal creature kills, battleground kills, and Playerbot activity are
not posted.

![Event logging](https://i.imgur.com/7M1Ze64.png)

## Playerbots

```ini
Discord.Playerbots.ShowPopulation  = 1
Discord.Playerbots.IncludeInChat   = 0
Discord.Playerbots.IncludeInEvents = 0
```

When `mod-playerbots` is compiled in, bots are detected and reported separately
(`Players / Playerbots / Total`). Bot chat and bot events are suppressed by default.

## Chat filter

```ini
Discord.Filter.Enable          = 0
Discord.Filter.DiscordToGame   = 1
Discord.Filter.GameToDiscord   = 1
Discord.Filter.Mode            = "block"    # block | censor
Discord.Filter.Words           = ""
```

Case-insensitive whole-word filtering with light leetspeak normalization. URLs are never
blocked. Mild profanity is allowed by default; the starter list contains severe slurs.

## Server status channel and presence

```ini
Discord.ServerStatus.Enable         = 1
Discord.ServerStatus.ChannelId      = 123456789012345678
Discord.ServerStatus.UpdateInterval = 60
```

One persistent message, edited in place (ID stored in `mod_discord_status`).

![Server status](https://i.imgur.com/zTtkEDj.png)

```ini
Discord.Presence.Enable         = 1
Discord.Presence.ActivityType   = "Playing"
Discord.Presence.Text           = "Playing {realm} | {players} players"
```

Variables: `{realm}`, `{players}`, `{playerbots}`, `{total}`, `{uptime}`, `{expansion}`.

## Configuration reference

Full documentation with comments for every option lives in
`conf/discord-integration.conf.dist`. Sections: General, Discord Bot, Server Branding, Global
Chat, Global Chat Providers, Faction Channels, Webhooks, Discord Roles, Emojis, Faction Emojis,
Class Emojis, Race Emojis, Admin Console, Admin Permissions, Confirmations, Event Logging,
Achievements, Boss Kills, Playerbots, Chat Filter, Server Status, Discord Presence, Debugging.

## Startup diagnostics

With `Logger.modules=3,Console Server` you should see:

```
Discord Integration
Bot: Connected
Guild: WotLK Plus
Global Chat: Enabled
Provider: mod-global-chat
Global Channel: Found
Discord roles loaded: 24
Discord Emojis: Enabled
Faction Emojis: Enabled - 2/2 resolved
Class Emojis: Enabled - 10/10 resolved
Race Emojis: Enabled - 20/20 resolved
Auto webhook created for channel ... (invisible name + transparent avatar).
```

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| `Bot: Connecting...` then 401 | Wrong token; reset it in the Developer Portal |
| Bot will not connect | Enable the privileged intents in the portal, or set `GuildMembersIntent`/`MessageContentIntent` to 0 |
| Guild unavailable | `Discord.Bot.GuildId` is wrong, or the bot is not in the server |
| Slash commands missing | Invite with the `applications.commands` scope (optional; everything works in-game) |
| No messages reach the game | Enable `MessageContentIntent`, set the channel, provider detected |
| No messages reach Discord | Run `.chat on` first; bot not muted; channel/webhook correct |
| Emojis not found | Upload to the guild with the naming scheme; check startup warnings |
| Bot icon shows in Discord | Messages using app emojis fall back to bot posts; move emojis to the guild, or grant the bot Manage Webhooks so the auto webhook works |
| Roles not created | Bot needs Manage Roles |
| Database tables missing | Apply `data/sql/db-characters/mod_discord_characters.sql` manually |

## License

GNU Affero General Public License v3, matching the AzerothCore project.