# WinBolo Replay (`.wbv`) Format

This document describes the recorded-game format that the **Log Viewer** plays
back. It is distinct from the per-session debug logging covered in
[logging.md](logging.md).

A `.wbv` file is a complete recording of a game: an initial world snapshot
followed by a tick-by-tick stream of events, with periodic re-snapshots. The
Log Viewer replays it by reconstructing the snapshot and applying each event to
the simulation.

## Container

A `.wbv` file is a standard **ZIP archive** (DEFLATE) whose main member is
named `log.dat`. Because it is ordinary ZIP, the payload can be extracted with
any unzip tool.

A logged round also carries `attribution.trk`
(`src/bolo/public/attribution_track.h`), and a round that ran scripts
carries `scripts.json`, described below. Both are written by `logStop`
after `log.dat` is closed.

Files are named `<timestamp>_<mapname>.wbv`, e.g.
`20260609t015459_DH-Oil_Rig.wbv`.

| Code path | Files |
|---|---|
| Writing | `src/bolo/log.c` (`logStart`, `logWriteSnapshot`, `logAddEvent`, `logWriteTick`) |
| Reading | `src/logviewer/blocks.c` (ZIP open + decompress), `src/logviewer/screen.c` (`lv_logLoad`, `lv_screenProcessLog`) |
| Constants | `src/bolo/public/log.h`, `src/logviewer/lv_log.h` |

### `scripts.json`

What the scripts of a scripted round were. The scenario host builds the
text at the end of a round boot that loaded its scripts and hands it to
the sim (`serverSimSetScenarioRecordText`); `logStop` writes it as this
member. A round that ran no script has no text, so a plain round's
archive has no `scripts.json` — including a plain round that follows a
scripted one on the same server, and a lobby-only log. Text over
`SCN_RECORD_TEXT_MAX` (256 KiB, `src/bolo/public/scripts_record.h`) is
not stored and so not written.

The log viewer reads the member when it opens the recording, alongside
`attribution.trk` (`src/logviewer/blocks.c`), into `LvScripts` on its state
(`src/logviewer/logviewer.h`). A member that is absent, over the cap, not
JSON or of another version leaves the viewer without one, and the recording
plays as it would have without it.

With Options → Regions on, the viewer outlines each of the member's `regions`
on the map with its name; a region a script defines while the round runs is
not in the member and is not drawn.

```json
{
  "version": 1,
  "map": "Survival.map",
  "mods_enabled": true,
  "rules": { "tank_reload_ticks": 8 },
  "regions": [ { "name": "keep", "x": 100, "y": 100, "w": 12, "h": 12,
                 "file": "Survival.map" } ],
  "scripts": [
    { "file": "Survival.map", "source": "map", "kind": "scenario",
      "manifest": { "...": "..." } },
    { "file": "NoLgmDeaths.scenario.lua", "source": "server", "kind": "mod",
      "manifest": { "...": "..." } }
  ]
}
```

| Key | Meaning |
|---|---|
| `version` | `1` |
| `map` | The committed map's file name |
| `mods_enabled` | Whether the lobby's mods switch was on |
| `rules` | The composed rules table the round opened on, keyed by rule name; a whole-number value is written as an integer |
| `regions` | The composed regions, each with `file`, the name of the script that declared it |
| `scripts` | One row per script in load order. Mods switched off did not run and are not listed |
| `scripts[].file` | The script's file name, not its path. A script packed into a map has the map's name |
| `scripts[].source` | `map` for the committed map's own script, `server` for a script from the server's scenarios directory |
| `scripts[].kind` | `mod` for a script whose manifest says `kind = "mod"`, otherwise `scenario` |
| `scripts[].manifest` | That script's own manifest as `manifest.json` writes it, rebuilt from the table the round loaded, so its `rules` are what that one file set |

## Header

The decompressed `log.dat` begins with a plaintext header, immediately followed
by the first `LOG_EVENT_SNAPSHOT` record.

| Field | Size | Notes                                                           |
|---|---|-----------------------------------------------------------------|
| Magic | 8 | Literal `WBOLOMOV`                                              |
| Version | 1 | `0`, `1`, `2`, or `3` (current)                                 |
| Map name | 1 + N | Length byte + UTF-8 name                                        |
| Game type | 1 | From `gameTypeGet()` — 1 open, 2 tournament, 3 strict, 4 scripted |
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
| 62 | `log_ScnPanel` | One scenario panel's display list (below) |
| 63 | `log_ScnScore` | A scenario's score for one player or one team (below) |
| 64 | `log_ScnAnnounce` | A centre-screen line a scenario put up (below) |
| 65 | `log_ScnMarker` | A scenario map marker (below) |
| 66 | `log_ScnHint` | An order a scenario gave one bot (below) |
| 67 | `log_ServerTick` | The server's game tick at this entry (below) |

### `log_GameSettings` payload

The blob is a Pascal form like the name payloads — a 1-byte length followed by
that many bytes — but the bytes are binary and may contain `0x00`. The length is
17 today and the layout is append-only, so a reader takes the fields it knows
and skips the rest by the framed length. A shorter payload is an older writer:
a reader takes what is there and treats the bytes past the end as zero, which
is the value each later field had before it was recorded.

| Bytes | Field | Notes |
|---|---|---|
| 0 | View policies | 2 bits per category: pill 0–1, base 2–3, ally 4–5; bit 6 classic mode, bit 7 allies in trees. Same packing as `INFO_PACKET.view_policies` |
| 1–2 | Pill view decay | Big-endian seconds; meaningful only when the pill policy is `decay` |
| 3–4 | Base view decay | Big-endian seconds; same condition |
| 5–6 | Ally view decay | Big-endian seconds; same condition |
| 7 | Game type | `gameType` — 1 open, 2 tournament, 3 strict, 4 scripted |
| 8 | AI policy | `aiType` — 0 `aiNone`, 1 `aiYes`, 2 `aiYesAdvantage`, 3 `aiFull` |
| 9 | Flags | bit0 hidden mines, bit1 time limit on, bit2 auto-lock on game start, bit3 ranked, bit4 password set, bit5 allow new players, bit6 overview window is classic, bit7 line of sight is not off |
| 10–11 | Time minutes | Big-endian; meaningless when the time-limit bit is clear |
| 12–13 | Lobby locks, low half | Big-endian — bits 0–15 of the `LOBBY_LOCK_*` mask (`src/bolo/public/wire_limits.h`), which settings the host was allowed to change |
| 14–15 | Lobby locks, high half | Big-endian — bits 16–31 of the same mask. Absent in a recording older than the field, where it reads as zero |
| 16 | Settings flags | bit0 smart pings banned (`LOG_SETTINGS_FLAG_SMART_PINGS_OFF`); bit1 positional sound on (`LOG_SETTINGS_FLAG_POSITIONAL_SOUND`), where sounds tell each player which side they are on and a banded distance. Absent in a recording older than the byte, where it reads as zero — smart pings allowed and every sound centred, which is what those servers did |

Bit 4 of the flags says only that a password is set; the password itself is
never recorded.

The flags byte at offset 9 is **full**. Bits 6 and 7 are one bit each because
the overview window and the line-of-sight mode have two values apiece today
(`OverviewWindow` and `LineOfSightMode` in `src/bolo/public/view_policy.h`); a
third value in either setting has nowhere to go in that byte. Byte 16 is where
a new flag belongs: it was added for smart pings, carries positional sound in
bit 1, and has six bits free.

The lock mask is carried **whole**, as a `uint32_t`, but in two pieces: bytes
12–13 hold its low half and bytes 14–15 its high half. The split is not a
layout choice so much as the history — the low half is as old as the event and
the high half was appended when `LOBBY_LOCK_SMART_PINGS` (`1u << 16`) became
the first lock above bit 15. Folding the mask into four contiguous bytes would
have made every reader written before that change misread a new recording, so
the old two bytes stay where they are and still mean what they always did.

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
(`src/server/sim/server_sim_scenario.c`). The viewer collects every change
when it loads the recording, with the time playback reaches it, and shows each
rule's value at the playhead: the last change at or before it, else the value
`scripts.json` says the round opened on, else the classic value. A record
whose length is not 8, whose rule index names no rule or whose value is not
finite is consumed and ignored. A live feed has no file to walk, so there the
changes are collected as playback reaches them.

### `log_ScnPanel` payload

One scenario panel's display list, as the panel op published it. Every update
replaces the whole list, so a record states a panel's entire contents and a
reader needs no history:

| Bytes | Field | Notes |
|---|---|---|
| 0 | Panel id and owner | Low four bits: the panel id, always 0, the in-game square, which is the only panel there is. High four bits: which script of the round's list drew it, 0 for the first script and for engine calls (`SCN_PANEL_WIRE` in `scenario_panel.h`). A recording made before owners existed has 0 there throughout |
| 1 | `destTeam` | 0 = everyone, otherwise the team number the list was held to. Teams run 1–15 |
| 2 | `destPlayer` | 0xFF = everyone, otherwise the 0-based player slot the list was held to |
| 3–4 | List length | Big-endian; 0 for a list that cleared the panel |
| 5… | List | That many bytes of display-list primitives |

The length is a big-endian `u16` rather than a Pascal string's single byte
because a list runs to `SCN_PANEL_MAX` (1017) bytes, well past what one byte
counts. This is the longest record the format carries, and the constant every
single-event buffer is sized by (`LOG_EVENT_MAX_BYTES`,
`src/bolo/public/log.h`) is worked out from it.

The list's own bytes are the drawing primitives described in
`src/bolo/public/scenario_panel.h` — rectangles, lines, text, a player's name,
a sprite, a bar and a timer, each an opcode byte followed by fixed operands
with multi-byte fields big-endian. A reader hands them to `scnPanelParse`,
which is the one function every frontend validates a list with, so a malformed
list is refused identically wherever it arrives.

A text, name or timer at the large size is stored with a size byte of normal
and followed by a size mark: a rect with `x` 2 and every other operand 0.
A reader from before the large size draws that rect as nothing and the item at
normal size; `scnPanelParse` folds the mark back into the item before it.

Written by the scenario funnel's panel arm
(`src/server/sim/server_sim_scenario.c`), which parses a list before it
publishes one, so a list in a recording is one that parses. The viewer keeps
the list for its destination, checking it with `scnPanelParse` first, and
rebuilds every panel at the playhead after a seek. It shows whichever script
wrote to a destination last. A record whose low four bits are a panel id
other than 0, a destination out of range, a length past `SCN_PANEL_MAX` or a
list that does not parse is consumed and ignored.

### `log_ScnScore` payload

A scenario's own score for one player or one team. Two header bytes, then the
number and its label:

| Bytes | Field | Notes |
|---|---|---|
| 0 | Kind | 0 = a player's score, 1 = a team's (`ScnScoreKind`) |
| 1 | Target | Under the player kind, the 0-based player slot; under the team kind, the team number, which runs 1–15 |
| 2–5 | Score | Big-endian signed `int32` |
| 6 | Label length | 0–15 |
| 7… | Label | That many bytes, the label the scenario gave the row |

The label is capped one below its sixteen-byte field so that what is recorded
and what the control event's body carries are the same string: the field has
no room for a terminator past its last byte.

The record is broadcast in the same sense the control event is — the target
says whose score it is, not who was meant to see it — so there is no
destination pair here.

Written by the scenario funnel's score arm. The viewer keeps the score for its
slot or team and rebuilds the scores at the playhead after a seek. A record
with another kind, a target out of range or a label past 15 bytes is consumed
and ignored.

### `log_ScnAnnounce` payload

A line a scenario put across the centre of the screen. Four header bytes,
then the line as a Pascal string:

| Bytes | Field | Notes |
|---|---|---|
| 0 | `destTeam` | 0 = everyone, otherwise the team number the line was held to. Teams run 1–15 |
| 1 | `destPlayer` | 0xFF = everyone, otherwise the 0-based player slot |
| 2–3 | Ticks | Big-endian; how long the line stays up |
| 4 | Text length | 0–128 |
| 5… | Text | That many bytes |

A text length of 0 is the clear: the scenario took the line down, and the
ticks alongside it were not read. A line with something in it is never
recorded with a tick count of 0 — the arm refuses that rather than putting a
line up for no time.

Written by the scenario funnel's announce arm. The viewer keeps the last line,
with the time it landed, until a clear replaces it, and rebuilds it at the
playhead after a seek. A record with a destination out of range or a text
past 128 bytes is consumed and ignored. Playback also posts each line to the
viewer's newswire once, whatever its destination, prefixed "[Team N]" or
"[name]" when it went to a team or a player; a clear posts nothing, and a
seek's rebuild posts nothing.

### `log_ScnMarker` payload

One scenario map marker, kept by id. Four header bytes, then the placement as
a Pascal form: a 1-byte length followed by that many binary bytes:

| Bytes | Field | Notes |
|---|---|---|
| 0 | Marker id | 0–15 |
| 1 | Kind | 0 = a map square, 1 = follow a player, 2 = clear the id (`ScnMarkerKind`) |
| 2 | `destTeam` | 0 = everyone, otherwise the team number. Teams run 1–15 |
| 3 | `destPlayer` | 0xFF = everyone, otherwise the 0-based player slot |
| 4 | Placement length | Always 4 |
| 5 | x | Map square, under the square kind |
| 6 | y | Map square, under the square kind |
| 7 | Slot | The 0-based player slot, under the follow kind |
| 8 | Colour | A palette index, 0–15 (`ScnPanelColour`) |

The clear kind reads none of the four placement bytes; they are written
whatever the kind so that every marker record is the same length.

Written by the scenario funnel's marker arm. The viewer keeps each marker by
id until a clear removes it — the record has no expiry — and rebuilds the
markers at the playhead after a seek. A record with an id past 15, a kind past
2, a destination out of range, a placement that is not four bytes, a colour
past 15 or a follow slot past the last slot is consumed and ignored. The
viewer draws the markers the followed player would see on its map — one to
everyone, one to that player's slot, or one to the team log_TeamSet last put
that slot on — a follow marker on its slot's tank while that tank is on the
map.

### `log_ScnHint` payload

One order a scenario gave one bot. The bot's seat, then the verb the order
led with as a Pascal string:

| Bytes | Field | Notes |
|---|---|---|
| 0 | Slot | The 0-based seat the hint was for |
| 1 | Verb length | 0–63 |
| 2… | Verb | That many bytes |

The rest of the hint's pairs are not recorded. What a key means is a contract
between the script and the brain it was written for, so nothing outside that
brain can read them; what a replay can say is which seat was given which
order, and that is what goes down.

Written by the scenario funnel's hint arm. The viewer consumes the record and
shows nothing for it.

### `log_ServerTick` payload

The server's game tick — `sim->tick`, a hundred a second and reset to 0 at
each round's start — for the entry the record sits in:

| Bytes | Field | Notes |
|---|---|---|
| 0–3 | Tick | Big-endian `u32` |

A scenario counts in this tick: a panel's timer target and an announcement's
arrival are both stated in it. Playback time is not, so without this record a
reader cannot tell what a timer read at a given moment.

Written by the server's per-tick log pass (`serverSimLogTick`,
`src/server/sim/server_sim_tick.c`) in a running round, queued just before
the tick's `logWriteTick`, so it lands in that tick's own `LOG_EVENT` block. It
is written at the round's first entry, at every entry where a snapshot was
written (`logSnapshotWrittenThisTick`), and at every entry whose tick is a
multiple of `FULL_SYNC_INTERVAL` (250) whether or not a snapshot was written
there. A tick that meets more than one of these still carries one record.

The interval is for a live spectator of a server that is not recording to a
file. Such a server writes no snapshot into the stream it feeds spectators:
`logWriteSnapshot` returns before noting one when no `.wbv` is open, and the
spectator ring's keyframes do not note theirs. Without the interval that feed
would carry the round's first anchor and no other. On a recording server the
interval ticks are the ones the periodic snapshot is written at, so the
interval adds a record to a file only where that snapshot was skipped.

The server writes one entry every other game tick, and the decoder spends
20 ms on every record it reads, so playback time and entries drift apart: a
`LOG_NOEVENTS` run, a snapshot and the entity-mask block each cost playback
20 ms with no entry behind them. The viewer therefore counts entries rather
than milliseconds. At load it walks the file and counts, record by record:

| Record | Entries |
|---|---|
| `LOG_NOEVENTS` / `_LONG` of n | n, one on each waiting step |
| `LOG_EVENT` / `_LONG` | 1 |
| `LOG_EVENT_SNAPSHOT` | 0 |
| The block holding `log_EntityMasks` after a snapshot | 0 |
| The block of events a snapshot flushes ahead of itself | 0 |

The last row is the block `logWriteSnapshot` writes when its tick had queued
events before the snapshot; the tick itself is counted later, by
`logWriteTick`. It is the block straight before a snapshot, but so is the
previous tick's own block when the snapshot's tick had queued nothing, and the
two are written alike. The viewer counts it as a flush and lets the next
record settle it: two records say how many entries passed between them, and a
count one short means the block was the previous tick's. A block holding
`log_ServerTick` is never a flush.

Each record becomes an anchor, and the tick at any playback time is the last
anchor's tick plus two for every entry counted since it. Before the first
anchor the viewer counts back from it, and not below 0. A later record with a
lower tick starts a new round's run and is kept like any other. A record whose
length is not 4 is consumed and ignored.

A live feed has no file to walk. Its records come from the spectator ring,
one per entry, so each counts one — a keyframe snapshot included — and the
viewer counts them and collects the anchors as playback first reads them.

A recording made before this record existed has no anchors, and the viewer
falls back to playback time: `ms / 10`.

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
