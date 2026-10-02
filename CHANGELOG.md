# Changelog

## 2.2.0

Fixes:

- `silent_disconnect` never worked: the old Utils `HookEvent` runs before the pre hooks, so the hide state was reset before they looked at it. With Utils 1.9.1+ team and disconnect handling now runs in post hooks.
- The team in `jointeam` was read wrong on Utils 1.7.9 and older (arguments come without the command name).
- Utils knows the SteamID only after Steam authorization: the module uses the controller's `m_steamID` and never remembers SteamID 0.
- Restore after a map change gave up 60 s after authorization and waited for a pawn the team select screen may not have. It now starts at `player_connect_full`, waits for the admin data up to 2 minutes and is cancelled on reconnect.
- A late authorization could make the client's automatic `jointeam` look like the admin's own choice: the hide state is reset on `player_connect_full`.
- The Utils version is detected by its `ILayoutApi` interface (1.9.1+) instead of calling `GetVersion()`, which old Utils builds do not have.
- Schema offsets are no longer cached as missing before the server module loads.

New:

- `block_chat` / `block_voice`: a hidden admin's chat messages and voice do not reach other players (commands and `@` admin chat still work).
- `silent_connect`: no join message for admins that come back hidden.
- `auto_hide_permission`: admins with it join hidden (the permission has to be given explicitly).
- `!hidelist` / `mm_hidelist`: hidden admins, for admins only.
- Several permissions in `permission`, separated by `|`.
- Two server builds: Metamod:Source 2.0 with KHook (plugin API 18) and Metamod with SourceHook (plugin API 17); `build-linux.sh` builds both.

## 2.1.2

- Author is glazki.

## 2.1.1

- `/hide` registered too; the command never reaches the chat.

## 2.1.0

- The client's automatic `jointeam` right after a map change is swallowed for admins that get hide mode back.
- `m_flForceTeamTime` is pushed away for hidden admins (`mp_force_pick_time`).
- A hidden admin moved into a team by the server or another plugin is hidden again, at most 3 times in 30 seconds.
- `auto_hide_on_spectate`.

## 2.0.0

- `!hide` / `mm_hide` and an admin menu item.
- The admin goes to "unassigned" through spectators and is not listed in the scoreboard.
- The observer pawn and dead body of a hidden admin are not transmitted to other players.
- Team change, suicide and disconnect events of a hidden admin are not broadcast.
- `IHideApi::IsClientHidden` for other plugins, `hide_on` / `hide_off` admin actions.
