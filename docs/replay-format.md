# WinBolo Replay (`.wbv`) Format

This document describes the recorded-game format that the **Log Viewer** plays
back. It is distinct from the per-session debug logging covered in
[logging.md](logging.md).

A `.wbv` file is a complete recording of a game: an initial world snapshot
followed by a tick-by-tick stream of events, with periodic re-snapshots. The
Log Viewer replays it by reconstructing the snapshot and applying each event to
the simulation.

## Container

A `.wbv` file is a standard **ZIP archive** (DEFLATE) containing a single member
named `log.dat`. Because it is ordinary ZIP, the payload can be extracted with
any unzip tool.

Files are named `<timestamp>_<mapname>.wbv`, e.g.
`20260609t015459_DH-Oil_Rig.wbv`.

| Code path | Files |
|---|---|
| Writing | `src/bolo/log.c` (`logStart`, `logWriteSnapshot`, `logAddEvent`, `logWriteTick`) |
| Reading | `src/logviewer/blocks.c` (ZIP open + decompress), `src/logviewer/screen.c` (`lv_logLoad`, `lv_screenProcessLog`) |
| Constants | `src/bolo/public/log.h`, `src/logviewer/lv_log.h` |

## Header

The decompressed `log.dat` begins with a plaintext header, immediately followed
by the first `LOG_EVENT_SNAPSHOT` record.

| Field | Size | Notes                                                           |
|---|---|-----------------------------------------------------------------|
| Magic | 8 | Literal `WBOLOMOV`                                              |
| Version | 1 | `0`, `1`, `2`, or `3` (current)                                 |
| Map name | 1 + N | Length byte + UTF-8 name                                        |
| Game type | 1 | From `gameTypeGet()`                                            |
| Allow hidden mines | 1 | Boolean                                                         |
| AI type | 1 | `aiType` — whether brains are allowed, not a bot difficulty     |
| Password | 1 | Boolean (game is password-protected)                            |
| Max players | 1 | 1–16                                                            |
| Version major / minor / revision | 3 | `BOLO_VERSION_*`                                                |
| Server IP | 4 | Currently `0.0.0.0`                                             |
| Server port | 2 | Big-endian; currently `0`                                       |
| Game create time | 4 | Big-endian Unix timestamp; its low byte seeds the V0/V1 XOR key |
| WBN key | 32 | WinBolo.net game server key                                     |

## Records

After the header the body is a stream of records, each introduced by a 1-byte
opcode (`src/bolo/public/log.h`):

| Code | Name | Layout |
|---|---|---|
| 0 | `LOG_QUIT` | opcode only — end of stream |
| 1 | `LOG_NOEVENTS` | opcode + `count:u8` — advance 1–255 ticks with no events |
| 2 | `LOG_NOEVENTS_LONG` | opcode + `count:u16` (big-endian) — advance up to 65535 ticks |
| 3 | `LOG_EVENT` | opcode + `count:u8` + events |
| 4 | `LOG_EVENT_LONG` | opcode + `count:u16` (big-endian) + events |
| 5 | `LOG_EVENT_SNAPSHOT` | opcode + snapshot body (see below) |

## Events

Events live inside `LOG_EVENT` / `LOG_EVENT_LONG` blocks. From V2 on each
event is framed as:

```
[type:u8][payload_len:u16 big-endian][payload:payload_len bytes]
```

`type` is one of the `logitem` values (`src/bolo/public/log.h`). Strings inside
payloads use a Pascal form: a 1-byte length followed by that many bytes.

Selected event types (see the `logitem` enum for the complete list):

| Value | Name | Payload |
|---|---|---|
| 1 | `log_PlayerJoined` | flags + country + account flags + Pascal name |
| 2 | `log_PlayerQuit` | player |
| 3 | `log_PlayerLocation` | player + map x/y + packed pixel x/y + frame/boat nibbles |
| 4 | `log_LgmLocation` | LGM (man) position |
| 5 | `log_MapChange` | cell x, y, terrain type |
| 6 | `log_Shell` | shell  (position, direction, owner) |
| 7–17 | `log_Sound*` | sound x/y for each of 11 sound events |
| 18–20 | `log_Message*` | server / all-players / specific-players chat |
| 21 | `log_ChangeName` | player + Pascal name |
| 22–24 | `log_Ally*` | alliance request / accept / leave |
| 25–26 | `log_BaseSet*` | base owner / stock (shells, mines, armour) |
| 27 | `log_PillSetOwner` | pillbox owner |
| 28 | `log_PillSetHealth` | Version-dependent. V3: pillbox index, then its armour — one byte each. V2 and earlier: one byte holding the index in the high nibble and the armour in the low. A reader must take the length from the file's version byte; reading two bytes from a V2 file eats the next event's type code |
| 29–30 | `log_PillSetPlace`, `log_PillSetInTank` | pillbox placement / in-tank |
| 31 | `log_SaveMap` | the host saved the map mid-game |
| 32–33 | `log_LostMan`, `log_KillPlayer` | man lost / player killed |
| 53 | `log_GameSettings` | Pascal-form blob of every lobby setting (below) |
| 54 | `log_Ping` | Smart ping: sender, kind, world x/y (below) |
| 55 | `log_TankSetStock` | Tank stocks: player, shells, mines, armour, trees (below) |
| 56 | `log_TankSetModifiers` | Per-tank modifiers: `player:u8`, then a Pascal blob of six bytes: speed, acceleration, turn, reload, damage dealt, damage taken, each a percent with 0 meaning classic. A reader consumes the blob by its length byte and applies it only when the length is 6 |
| 57 | `log_EntityChange` | One pillbox, base or start joined the map or left it (below) |
| 58 | `log_EntityMasks` | Which pillboxes, bases and starts are on the map (below) |
| 59 | `log_ServerText` | A server line a scenario wrote: `destTeam:u8` (0 = everyone), `destPlayer:u8` (0xFF = everyone), Pascal text |
| 60 | `log_GameTimeSet` | The round's game time after a scenario changed it: `ticks:i32` big-endian |
| 61 | `log_RuleSet` | One simulation rule a scenario changed (below) |

### `log_GameSettings` payload

The blob is a Pascal form like the name payloads — a 1-byte length followed by
that many bytes — but the bytes are binary and may contain `0x00`. The length is
14 today and the layout is append-only, so a reader takes the fields it knows
and skips the rest by the framed length.

| Bytes | Field | Notes |
|---|---|---|
| 0 | View policies | 2 bits per category: pill 0–1, base 2–3, ally 4–5; bit 6 classic mode, bit 7 allies in trees. Same packing as `INFO_PACKET.view_policies` |
| 1–2 | Pill view decay | Big-endian seconds; meaningful only when the pill policy is `decay` |
| 3–4 | Base view decay | Big-endian seconds; same condition |
| 5–6 | Ally view decay | Big-endian seconds; same condition |
| 7 | Game type | `gameType` — 1 open, 2 tournament, 3 strict |
| 8 | AI policy | `aiType` — 0 `aiNone`, 1 `aiYes`, 2 `aiYesAdvantage`, 3 `aiFull` |
| 9 | Flags | bit0 hidden mines, bit1 time limit on, bit2 auto-lock on game start, bit3 ranked, bit4 password set, bit5 allow new players, bit6 overview window is classic, bit7 line of sight is not off |
| 10–11 | Time minutes | Big-endian; meaningless when the time-limit bit is clear |
| 12–13 | Lobby locks | Big-endian — the **low 16 bits** of the `LOBBY_LOCK_*` mask (`src/bolo/public/wire_limits.h`), which settings the host was allowed to change |

Bit 4 of the flags says only that a password is set; the password itself is
never recorded.

The flags byte is **full**. Bits 6 and 7 are one bit each because the overview
window and the line-of-sight mode have two values apiece today
(`OverviewWindow` and `LineOfSightMode` in `src/bolo/public/view_policy.h`); a
third value in either setting, or any new flag, needs the blob to grow rather
than another bit in this byte.

The lock mask is a **16-bit truncation**: `sim->serverLocks` is a `uint32_t` and
the blob writes only its low two bytes. `LOBBY_LOCK_LINE_OF_SIGHT` (`1u << 15`)
is the last bit that fits. Whoever defines lock 17 must grow the blob at the same
time, or that lock will silently read as clear in every recording.

The event is written by `src/server/server_dedicated_log.c` when the lobby opens,
when the round starts, and when a lobby edit changes any of these values, so a
recording seeked to the middle needs the earlier events to know the current
settings.

### `log_Ping` payload

A smart ping — the League-style pie-menu marker a player drops on the map for
their team. Six bytes, no Pascal string:

| Bytes | Field | Notes |
|---|---|---|
| 0 | Sender | Player slot |
| 1 | Kind | `PING_KIND_*` (`src/bolo/public/input_packet.h`) — 0 standard, 1 caution, 2 assist me, 3 attack, 4 on my way, 5 bot command |
| 2–3 | World X | Big-endian |
| 4–5 | World Y | Big-endian |

The position is in WORLD units — 256 per map square, the same units the tank
positions in a snapshot use — rather than a map square, so the marker lands
where the sender's cursor was rather than snapped to a tile.

In a live game a ping is delivered only to the sender's own team and allies;
the recording is not filtered that way, because a replay is watched from
outside and has no team to be on. The viewer draws every ping in the file, in
the kind's colour, for `PING_DISPLAY_MS` of playback time.

### `log_TankSetStock` payload

One tank's four stock values. Five bytes, no Pascal string:

| Bytes | Field | Notes |
|---|---|---|
| 0 | Player | Player slot |
| 1 | Shells | |
| 2 | Mines | |
| 3 | Armour | |
| 4 | Trees | |

Written by the same per-tick pass that writes `log_PlayerLocation`
(`serverSimLogTick`), and only when at least one of the four differs from the
last record written for that tank — so a tank whose stocks are unchanged costs
nothing, and there is at most one record per tank per tick. A recording written
before this event existed carries none; the reader then has only what the
snapshot player blocks give it.

### `log_EntityChange` payload

The map's pillbox, base and start lists are mutable mid-round. Three header
bytes and then the item's map record as a Pascal form — a 1-byte length
followed by that many binary bytes, which may contain `0x00`:

| Bytes | Field | Notes |
|---|---|---|
| 0 | Kind | 0 pillbox, 1 base, 2 start (`ENTITY_KIND_*`, `src/bolo/public/control_event.h`) |
| 1 | Index | The item's number in its list, counting from 0 |
| 2 | On the map | 1 the item has joined the map, 0 it has left it |
| 3 | Record length | 6 for a pillbox or a base, 3 for a start |
| 4.. | Record | The item's map data, below |

The record for a pillbox is x, y, owner, armour, speed, in-tank; for a base
x, y, owner, armour, shells, mines; for a start x, y, direction. A pillbox's
reload, cool-down and just-seen and a base's refuel time, base time and
just-stopped are the server's per-tick working state and are not in the
record — nothing rebuilds them from a recording.

A removal is a tombstone: the item's number and the list's count stay, so
every number above the removed one goes on meaning the same item, and the
record the removal carries is the one the item had as it went — enough for a
script or a reader to put it back. An add names the number the server's list
chose, which is the lowest removed slot or, failing that, one past the end;
the count rises to cover a number past it and the slots the gap opens up are
off the map.

The snapshot's pill, base and start blocks carry a count and a record each
and have nowhere to say which of them are on the map, so a reader keeps its
own flags across a snapshot: a number it already had keeps the flag these
records gave it, a number the blob has grown past the old count arrives on
the map, and a number the blob no longer reaches is off it. The flags a
snapshot cannot state come from the `log_EntityMasks` record after it. The
same event travels live as `CTRL_ENTITY_CHANGE`, carrying the identical
record.

### `log_EntityMasks` payload

Which indices are on the map. Six bytes, no Pascal string:

| Bytes | Field | Notes |
|---|---|---|
| 0–1 | Pillbox mask | Big-endian; bit i set means pillbox index i, counting from 0, is on the map |
| 2–3 | Base mask | Big-endian; same meaning for bases |
| 4–5 | Start mask | Big-endian; same meaning for starts |

Each list holds at most 16 items, so 16 bits covers every index a list can
name. A bit at or above a list's own count names no item and is ignored.
These are the same three masks `CTRL_ENTITY_SYNC` carries to a live client,
and the writer builds both from one function.

Written immediately after every snapshot, including the one a recording opens
with, as a one-event `LOG_EVENT` block. A snapshot restates every record and
every count but has nowhere to put the live flags, so without this a reader
starting at a snapshot — a fresh open, or a scrub back to one — would put
every item back on the map and keep it there. The record applies the flags and
nothing else: the counts and the records are the ones the snapshot installed,
and an index taken off the map keeps its record, so an index put back holds
the item it always held.

It is **not** written when every index within every count is on the map. That
is what loading a map produces and what a reader's own load assumes, so a
round that never takes an item off the map carries none of these records and
its bytes are unchanged.

### `log_RuleSet` payload

One number in the simulation's rules table — the per-simulation table of
gameplay values in `src/bolo/internal/sim_rules.h` — after a scenario changed
it. Two header bytes, then the value as a Pascal form: a 1-byte length
followed by that many binary bytes, which may contain `0x00`:

| Bytes | Field | Notes |
|---|---|---|
| 0–1 | Rule | Big-endian; the field's index in the table, counting from 0 in the order `SimRules` declares its fields (`ScnRuleIndex`, `src/bolo/scenario_api/scenario_defs.h`) |
| 2 | Value length | Always 8 |
| 3–10 | Value | Big-endian IEEE-754 binary64 of the value the field ended up holding |

The value is written as a double rather than as a scaled integer because the
table holds both kinds of field: most are `int32_t` and sixteen are `float`,
and one fixed width has to carry either. A double is exact for an `int32_t`
field across its whole range and for every value a `float` field can hold, so
the record states the number the rule actually took rather than a rounded
copy of it. The eight bytes are the double's bit pattern serialised most
significant first, so a host's own byte order does not reach the file.

The rules control event on the wire (`CTRL_SIM_RULES`) makes the same choice
for the same reason, in four bytes rather than eight: a float rule travels as
its own IEEE-754 bit pattern, most significant byte first, not as a
fixed-point scaling of it. Scaling a float rule would round it — an
acceleration allowed down to 0.01 comes back as 0.0117 at a ×256 scale, which
is not the number the server is simulating with.

The record carries the value **after** the write, not the value the scenario
asked for: a script that asks an integer rule for `3.7` leaves the field
holding `3`, and `3` is what a reader sees. A rule the scenario asked for and
was refused writes no record at all, because a refused change leaves the
table exactly as it was.

Written by the scenario funnel's set-rule arm
(`src/server/sim/server_sim_scenario.c`). The viewer consumes the record to
keep its place in the stream and does not yet show it; showing a rule needs
the recording's rules manifest, which states the table a round opened with.

## Snapshot body

A `LOG_EVENT_SNAPSHOT` captures the full world state, used both for the
initial frame and for periodic resyncs:

| Field | Size | Notes |
|---|---|---|
| Start delay | 4 | Big-endian int32, ms until game start |
| Game length | 4 | Big-endian int32, total ms |
| Pills block | 1 + N | Length byte + pillbox network data |
| Bases block | 1 + N | Length byte + base network data |
| Starts block | 1 + N | Length byte + start-square network data |
| Map runs | variable | RLE terrain (below) |
| Player blocks | 16 × (1 + N) | One length-prefixed block per tank slot (`MAX_TANKS = 16`) |

### Map run-length encoding

The map is stored as a series of runs, terminated by a sentinel:

```
run:        [datalen:u8][y:u8][startx:u8][endx:u8][data: datalen-4 bytes]
terminator: [4][255][255][255]
```

`datalen` includes the 4-byte header. `y` is the row, `startx`..`endx` is the
half-open column range, and `data` holds packed terrain — one tile per nibble,
two tiles per byte. Decoded in `src/logviewer/bolo_map.c` (`lv_mapReadRuns`).

### Player block

Each block is `[dataLen:u8][payload:dataLen bytes]`. A block whose `dataLen` is
2 is a "slot not in use" stub — player number and a zero in-use byte, nothing
else. A slot in use carries:

| Bytes | Field | Notes |
|---|---|---|
| 0 | Player | Player slot |
| 1 | In use | 1 |
| 2–3 | Tank map x, y | |
| 4 | Tank pixel x/y | One per nibble |
| 5 | Tank frame | |
| 6 | On boat | |
| 7–8 | Man map x, y | (0,0) means the man is aboard |
| 9 | Man pixel x/y | One per nibble |
| 10 | Man frame | |
| 11… | Player name | Pascal form |
| … | Location | Pascal form |
| … | Alliances | `[count][count player slots]` |
| … | Tank stocks | Four bytes: shells, mines, armour, trees |

The four stock bytes are optional: `dataLen` delimits the block, so a reader
that runs out of block at the end of the alliance list is reading a recording
written before the stocks were carried and has no stock values for that tank —
which is what every recording made before this looks like. A reader must treat
their absence as "not present" rather than as a malformed block, and a slot with
no stocks reads as zero rather than as a guess.

## Versions

| Version | Notes |
|---|---|
| V0 (`0`) | Player-join events carry raw IP octets. Body XOR-encrypted. |
| V1 (`1`) | Join events carry country code + account flags (WBN / Steam / bot) instead of IP. Body XOR-encrypted. |
| V2 (`2`) | Plaintext; events gain the `[type][u16 len][payload]` framing. Payload shapes unchanged from V1. |
| V3 (`3`, current) | `log_PillSetHealth` carries the pillbox index and its armour in a byte each, where V2 and earlier packed the pair into one byte's nibbles. Every other payload, and the framing, unchanged from V2. |

V3 exists because that one record changed shape without changing its name: a
reader that takes two bytes from a V2 file reads the next event's type code as
the armour and loses the stream from there, and nothing in a V2 file says which
shape it holds. So the version byte says it instead, and a reader sizes the
record by the version the file states rather than by the writer it was built
alongside. The widening it follows is the pillbox armour's own: armour moved out
of a shared nibble and can now hold more than 15.

In V0/V1 the event stream is XOR-encrypted: the key starts at
`gmeCreateTime & 0xFF` and, after each event block, advances to the type code of
the last event processed. Snapshot opcodes and bodies are never XOR'd. V2 and V3
use no encryption (`blockKey = 0`). The Log Viewer reads all four versions.

## Playback sequence

1. Open the ZIP, locate `log.dat`, begin DEFLATE decompression.
2. Read and verify the `WBOLOMOV` magic, then the version byte.
3. Read the header fields; seed the XOR key (0 for V2 and V3).
4. Read the first `LOG_EVENT_SNAPSHOT` and reconstruct the initial world.
5. Loop on opcodes — `LOG_QUIT` stops; `LOG_NOEVENTS` / `LOG_NOEVENTS_LONG`
   advance ticks; `LOG_EVENT` / `LOG_EVENT_LONG` parse and apply events;
   `LOG_EVENT_SNAPSHOT` reloads the world.
