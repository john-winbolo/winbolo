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
| Version | 1 | `0`, `1`, or `2` (current)                                      |
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

Events live inside `LOG_EVENT` / `LOG_EVENT_LONG` blocks. In the current
format (V2) each event is framed as:

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
| 27–30 | `log_Pill*` | pillbox owner / health / placement / in-tank |
| 31 | `log_SaveMap` | the host saved the map mid-game |
| 32–33 | `log_LostMan`, `log_KillPlayer` | man lost / player killed |
| 53 | `log_GameSettings` | Pascal-form blob of every lobby setting (below) |
| 54 | `log_Ping` | Smart ping: sender, kind, world x/y (below) |
| 55 | `log_TankSetStock` | Tank stocks: player, shells, mines, armour, trees (below) |

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
| 9 | Flags | bit0 hidden mines, bit1 time limit on, bit2 auto-lock on game start, bit3 ranked, bit4 password set, bit5 allow new players |
| 10–11 | Time minutes | Big-endian; meaningless when the time-limit bit is clear |
| 12–13 | Lobby locks | Big-endian `LOBBY_LOCK_*` mask (`src/bolo/public/wire_limits.h`) — which settings the host was allowed to change |

Bit 4 of the flags says only that a password is set; the password itself is
never recorded. The event is written by `src/server/server_dedicated_log.c` when
the lobby opens, when the round starts, and when a lobby edit changes any of
these values, so a recording seeked to the middle needs the earlier events to
know the current settings.

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
| V2 (`2`, current) | Plaintext; events gain the `[type][u16 len][payload]` framing. Payload shapes unchanged from V1. |

In V0/V1 the event stream is XOR-encrypted: the key starts at
`gmeCreateTime & 0xFF` and, after each event block, advances to the type code of
the last event processed. Snapshot opcodes and bodies are never XOR'd. V2 uses
no encryption (`blockKey = 0`). The Log Viewer reads all three versions.

## Playback sequence

1. Open the ZIP, locate `log.dat`, begin DEFLATE decompression.
2. Read and verify the `WBOLOMOV` magic, then the version byte.
3. Read the header fields; seed the XOR key (0 for V2).
4. Read the first `LOG_EVENT_SNAPSHOT` and reconstruct the initial world.
5. Loop on opcodes — `LOG_QUIT` stops; `LOG_NOEVENTS` / `LOG_NOEVENTS_LONG`
   advance ticks; `LOG_EVENT` / `LOG_EVENT_LONG` parse and apply events;
   `LOG_EVENT_SNAPSHOT` reloads the world.
