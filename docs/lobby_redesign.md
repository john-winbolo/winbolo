# Lobby Redesign — Layout A

Design doc for the new pre-game lobby. Replaces the existing tab-based
`imgui_lobby.cpp` with a single-window team-grouped UI based on the
mockup at `lobby comps layout-a-export/`.

## Goal

A single 1180×820 lobby window with four zones:

1. **Status bar** — server addr, game type, mines, AI policy, time limit,
   settings gear, connectivity check.
2. **Game settings** (collapsible) — radio groups for Game Type / AI
   Policy / Hidden Mines / Time Limit. Each setting can show a "server
   lock" badge.
3. **Main body** (1fr / 280 px split):
   - Left: stacked team containers with color-tinted headers,
     drag-to-assign player rows, inline-edit team names, per-team
     `+ Bot` button + naming pool dropdown, expandable per-bot
     AiConfig (difficulty / personality / name override).
   - Right: map panel (thumbnail + picker dialog).
4. **Bottom band** (1fr / 280 px split): chat pane + action buttons
   (Start Game / Ready / Leave).

## Architectural decisions

### Teams as auto-ally, not first-class server objects

Teams in the lobby are a UI grouping. At game start, the server walks
slots and groups by `teamNumber`, then uses the existing alliance
system to mutually ally each group. Tank colors stay on the existing
per-player palette; team colors are lobby presentation only.

The server *does* keep a small `TeamMetadata[MAX_TEAMS]` table (color,
name, naming pool) so all clients render team labels consistently.
Membership lives in the existing per-slot `teamNumber` field.

### Two color domains, kept separate

| Domain | Owner | Use |
|---|---|---|
| **UI / identity** | `WbTheme` (new) | Dialog chrome, status text, team colors, badges |
| **Game render** | Skin system (existing/other PR) | Tile colors, tank palette, projectiles, HUD |

A user can pick "Light theme + Old-school skin" without either system
caring about the other. `WbTheme` is a struct with a single global
`g_theme` pointer; switching themes is `g_theme = &theLight; g_theme->apply();`.

### Server-locked settings come from CLI

Admin-only. Configured at `bolod` startup via flags:

```
--lock-game-type
--lock-ai-policy
--lock-mines
--lock-time-limit
--lock-auto-lock
```

Sets a bitmask read by `PACKET_LOBBY_STATE`; clients render the
matching settings disabled with a lock badge. Hosts never see lock
controls — locking is admin territory, not host-as-game-creator.

### Round persistence

When a game ends and the server returns to lobby state, **everything
is preserved**: team metadata, slot assignments, bots and their
configs, all settings, the `autoLockOnGameStart` toggle. Players come
back to exactly what they had.

The single exception is `allowNewPlayers` when `autoLockOnGameStart`
is true: the server flips `allowNewPlayers=false` on game start, then
auto-restores it on game end to whatever it was before the start.

### Hard break on protocol version

Bump the protocol version. Old clients can't see new servers.
Additive `PACKET_LOBBY_STATE` fields are still a wire change because
parsers walk fixed-offset; bump and move on. WBN tracker advertises
the new version; old clients filter it out of browser results.

## Auto-unready

Any meaningful change clears all `ready` flags and broadcasts
`PACKET_LOBBY_AUTO_UNREADY` (no payload). Players re-confirm before the
host can start. Originator included — they may have intended *one*
change but should still confirm the resulting whole.

| Trigger | Auto-unready |
|---|---|
| Setting change (game type, AI, mines, time, locks, allow-new, auto-lock) | yes |
| Team metadata change (add / rename / recolor / naming pool) | yes |
| Slot moved between teams | yes |
| Bot added / removed / configured | yes |
| Map change | yes |
| `openHost` toggle | yes |
| Player joins / leaves / kicked | yes |
| Chat message | no |
| Ping update | no |

## Wire format

Per-packet-type, matching John's existing pattern in `netpacks.h`. One
top-level `PACKET_*` ID per command, fixed binary layout, documented
inline.

### Client → Server

```c
PACKET_LOBBY_SET_SETTING    160   /* { settingType 1, valueLen 1, value N } */
PACKET_LOBBY_OPEN_HOST      161   /* { bool 1 } */
PACKET_LOBBY_TEAM_META      162   /* { teamId 1, color 1, namingPool 1,
                                     *   nameLen 1, name N }
                                     * Single packet handles create + rename +
                                     * recolor + naming-pool change. */
PACKET_LOBBY_TEAM_CLEAR     163   /* { teamId 1 } — frees metadata; members
                                     * stay on teamId until manually moved */
PACKET_LOBBY_BOT_CONFIG     164   /* { slot 1, difficulty 1, personality 1,
                                     *   nameLen 1, name N } */
PACKET_LOBBY_KICK           165   /* { slot 1 } */

/* Existing packets — extended payload, version-bumped: */
/*   PACKET_LOBBY_TEAM_SET (130): host can move others, not just self */
/*   PACKET_LOBBY_ADD_BOT  (132): + { teamId, name, difficulty, personality } */
/*   PACKET_LOCK_TOGGLE    (128): unchanged — exposed via "Allow new players"
 *                                UI in the Other section */
```

### Server → Client

```c
PACKET_LOBBY_SETTING_CHG     175  /* echo of CLIENT SET_SETTING after apply */
PACKET_LOBBY_OPEN_HOST_CHG   176  /* { bool 1 } */
PACKET_LOBBY_TEAM_META_CHG   177  /* same payload as client TEAM_META */
PACKET_LOBBY_BOT_CONFIG_CHG  178
PACKET_LOBBY_REJECT          179  /* { origPacket 1, reasonCode 1 } */
PACKET_LOBBY_AUTO_UNREADY    180  /* (empty payload) */

/* PACKET_LOBBY_STATE (140) — extended payload, version-bumped:
 *   - openHost (1 byte bool)
 *   - serverLocks (2-byte bitmask, BE)
 *   - autoLockOnGameStart (1 byte bool)
 *   - team metadata table:
 *       teamCount (1 byte)
 *       per team: teamId (1), color (1), namingPool (1), nameLen (1), name (N)
 *   - per-bot configs:
 *       botCount (1 byte)
 *       per bot:  slot (1), difficulty (1), personality (1), nameLen (1), name (N)
 */
```

### Setting types (inside SET_SETTING)

```c
LST_GAME_TYPE          = 1   /* 1 byte enum: open|tournament|strict */
LST_HIDDEN_MINES       = 2   /* 1 byte bool */
LST_AI_POLICY          = 3   /* 1 byte enum: none|allow|advantage|full */
LST_TIME_LIMIT         = 4   /* 1 byte bool */
LST_TIME_MINUTES       = 5   /* 2 bytes uint16 BE */
LST_AUTO_LOCK_ON_GAME  = 6   /* 1 byte bool */

/* allowNewPlayers stays on PACKET_LOCK_TOGGLE — not duplicated here.
 * serverLocks is read-only (CLI) — no SET_SETTING for it. */
```

### Authority and validation

For every client→server lobby command, server checks:

1. **Sender allowed?** `senderIsHost || (game.openHost && senderIsActivePlayer)`.
2. **Setting locked?** If `setting ∈ serverLocks`, reject regardless of sender.
3. **Apply** to authoritative state, **broadcast** corresponding `_CHG` packet
   to all clients (including originator — they don't update from their own send,
   they update from the broadcast echo, so server clamping is honored).
4. **Auto-unready** broadcast after the change.

Reject path: `PACKET_LOBBY_REJECT { origPacket, reasonCode }` to originator only.
Reason codes: `REJECT_NOT_HOST`, `REJECT_LOCKED`, `REJECT_INVALID`.

### Concrete byte counts

| Change | Wire bytes (excluding 8-byte header) |
|---|---|
| Toggle hidden mines | 3 |
| Change game type to tournament | 3 |
| Set time limit to 45 min | 4 (`LST_TIME_MINUTES, len=2, 0x002D`) |
| Move slot 4 to team 2 | 2 (`PACKET_LOBBY_TEAM_SET`) |
| Rename team 2 to "Reds" | 7 (`teamId, color, pool, nameLen=4, R-e-d-s`) |
| Add bot to team 1, name "Mozart", hard, normal | ~12 |
| Kick slot 5 | 1 |
| Toggle openHost | 1 |
| Auto-unready broadcast | 0 |

Most changes are <10 bytes — well under one UDP datagram. Lobby
pacing is humans-clicking; bandwidth isn't the constraint, but
keeping packets compact and one-shot keeps the protocol legible.

## Server data model additions

```c
/* Per-team metadata table, indexed by teamNumber 1..MAX_TEAMS-1.
 * teamNumber 0 = unassigned (no metadata). */
typedef struct {
    uint8_t in_use;       /* 0 = team has no metadata; renders with defaults */
    uint8_t color;        /* index into kTeamColors[] */
    uint8_t naming_pool;  /* index into bot pool table */
    char    name[32];
} TeamMetadata;

TeamMetadata teams[MAX_TEAMS];

/* Per-bot config — extends existing BotContext */
typedef struct {
    uint8_t difficulty;   /* 0=easy, 1=normal, 2=hard */
    uint8_t personality;  /* 0=normal, 1=aggressive, 2=defensive, 3=sniper, ... */
    /* name and team already in BotContext */
} BotConfig;

/* Per-server lobby state — additions to existing struct */
struct {
    bool     openHost;
    bool     allowNewPlayers;          /* live state — drives PACKET_LOCK_TOGGLE */
    bool     autoLockOnGameStart;      /* persisted setting */
    bool     savedAllowNewPlayers;     /* what allowNewPlayers was before
                                        * autoLockOnGameStart kicked in */
    uint16_t serverLocks;              /* bitmask, set from CLI, read-only */
} lobbyState;
```

## Bot naming pools

Hardcoded on the client (and mirrored on the server only for the
list of pool keys, since the server picks pool indices for `naming_pool`
broadcasts). Names themselves come over the wire as strings — the server
never has to know the pool's contents.

Initial pools (mirroring the JSX):

- `classic` — HAL-9000, GLaDOS, Skynet, Wintermute, Cortana, Marvin, ...
- `painters` — Van Gogh, Picasso, Monet, Rembrandt, Dalí, Vermeer, ...
- `musicians` — Mozart, Beethoven, Hendrix, Bowie, Prince, Aretha, ...
- `martial-arts` — Bruce Lee, Jackie, Jet Li, Donnie Yen, Mas Oyama, ...
- `scientists` — Einstein, Newton, Curie, Tesla, Darwin, Hawking, ...
- `philosophers` — Socrates, Plato, Nietzsche, Kant, Sartre, Aristotle, ...
- `generals` — Hannibal, Caesar, Napoleon, Patton, Sun Tzu, Zhukov, ...
- `callsigns` — Maverick, Goose, Iceman, Viper, Slider, Hollywood, ...
- `numeric` — Bot-01, Bot-02, ..., Bot-16

Behavior:

1. Each team has exactly one `naming_pool` at any time.
2. Adding the **first** bot to a fresh team randomly rolls a pool (variety
   for an unattended lobby).
3. All subsequent bots on that team draw from the same pool.
4. Changing the dropdown swaps the pool for **future** bots on that team.
   Existing bots keep their names — renaming live tanks would be jarring.
5. Pool exhaustion (e.g. 14 bots vs 12-name pool) falls back to
   `"<label> N"` overflow.
6. Pool dropdown UI hidden until the team has ≥1 bot.
7. Unknown pool ID from the server (e.g. server has new pool a client
   doesn't know): client picks a known pool at random for the *display
   label only*. Bot names themselves are server-broadcast strings, so
   functionality isn't affected.

## Bot config: difficulty / personality

Plumbed through the protocol now. The brain side:

- v1: NewAutopilot accepts a `brain.set_config({difficulty, personality})`
  Lua call but ignores the values. Lets us ship the UI + protocol without
  rewriting the brain.
- Future: NewAutopilot consumes the values. e.g. `difficulty=easy` lowers
  pillbox aim accuracy, `personality=aggressive` shifts goal weighting
  toward attack_pill / attack_tank.

Difficulty enum: `easy | normal | hard`.
Personality enum: `normal | aggressive | defensive | sniper`. (Open to
adjust the personality list — the mockup shows these four.)

## WBN tracker

Server registers as today. New `INFO_RESPONSE` fields exposed in the
browser line:

- Team count (count of `teams[]` with `in_use=1`)
- Lock state summary (single character per locked setting?)
- `openHost` flag (icon in browser)

Exact format TBD — depends on existing `INFO_RESPONSE` layout, follow
the same pattern.

## Single-player path: ControlEvent pub/sub

Earlier iterations of this redesign used a cross-struct shortcut —
`serverSimSyncLobbyToClient(ServerSim *, ClientSim *)` — to push the
server's lobby state directly into the local `humanSim` after every
SP-host UI action. With the opaque-sims migration, that shortcut would
have required either:

- including `client_sim_internal.h` from `server_sim.c` (breaks the
  opacity wall server-side), or
- including `server_sim.h` from `client_sim.c` (creates an unwanted
  client→server compile-time dependency that doesn't exist in
  multiplayer).

Neither was acceptable. The function has been deleted; the SP path
now uses the same `ControlEvent` pub/sub system that bots and the
WASM frontend already rely on.

### How it works

`client_sim_control.c::clientSimApplyControl` is the single mutation
sink for ClientSim lobby state. It handles events emitted from two
sources:

1. **Network multiplayer** — `transport_udp_client.c`'s packet handlers
   decode incoming `PACKET_LOBBY_*` packets and call
   `clientSimApplyControl` directly with the resulting events.
2. **In-process (SP / LAN host, bots, WASM)** — the local ClientSim
   registers as a subscriber via `serverSimRegisterSubscriber(...)`.
   Registration triggers `serverSimSyncSubscriber` which delivers the
   full current state as a sequence of fill-events (game phase, lobby
   settings, lobby slots, player joins, **team metadata, bot configs,
   bot brain paths, brain list**). Live updates are published by
   the same `transportUdpServerBroadcast*Chg` functions the multiplayer
   path already uses — they now call `serverSimPublishControl` in
   addition to sending UDP, so in-process subscribers see every change
   without a separate code path.

### ControlEvent types touched by this redesign

| Event | Carries |
|---|---|
| `CTRL_LOBBY_SLOT` | one slot's full `ClientLobbySlot` (existing) |
| `CTRL_LOBBY_SETTINGS` | mapName + game settings; **extended** with `lobbyOpenHost`, `lobbyAutoLockOnGameStart`, `lobbyServerLocks` |
| `CTRL_LOBBY_TEAM_META` | **new** — `teamId` + `in_use/color/namingPool/name` |
| `CTRL_LOBBY_BOT_CONFIG` | **new** — `slot` + `difficulty/personality/name` |
| `CTRL_LOBBY_BOT_BRAIN` | **new** — `slot` + brain script path |
| `CTRL_LOBBY_BRAIN_LIST` | **new** — full discovered `BrainList` catalogue |

Server-side fill functions live in `server_sim.c`
(`serverSimFillLobbyTeamMetaEvent`, `serverSimFillLobbyBotConfigEvent`,
`serverSimFillLobbyBotBrainEvent`, `serverSimFillLobbyBrainListEvent`).
The matching apply cases live in `client_sim_control.c`.

### When something new is added to lobby state

If you add a new lobby field that needs to mirror server→client:

1. Add the field to the opaque struct in `server_sim_internal.h` and to
   the opaque struct in `client_sim_internal.h`.
2. Add public accessors on both sides.
3. Add a `CTRL_LOBBY_*` event type (or extend an existing one) in
   `control_event.h`.
4. Add a `serverSimFill*Event` in `server_sim.c` and call it from
   `serverSimSyncSubscriber` so new subscribers get initial state.
5. Add an apply case to `clientSimApplyControl`.
6. Add a `serverSimPublishControl(sim, &evt)` call inside the relevant
   `transportUdpServerBroadcast*Chg` function (and add a wire packet if
   live multiplayer needs it).
7. SP host code paths in `imgui_lobby.cpp` should call the broadcast
   function — never reach across to the ClientSim directly.

The opacity contract is the enforcement mechanism: any code outside
the allowed-includers list of `client_sim_internal.h` or
`server_sim_internal.h` *cannot* write the field directly. It must go
through the event system.

## Future / deferred

- **WBN auto-balance** (`PACKET_BALANCE_*`) — works alongside manual team
  management, but the UI flow isn't designed yet. Ship Layout A first;
  add a "Balance teams" button that calls into the existing flow.
- **Brain personality behavior** — protocol carries the field, brain
  ignores. Add real behavior in a follow-up PR.
- **Per-server custom naming pools** (modder-supplied via data file or
  server-broadcast pool table) — possible v2 if community asks.
- **Light / Classic theme variants** — `WbTheme` is structured to support
  this; actual additional themes are a separate effort.
