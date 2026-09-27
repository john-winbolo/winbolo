# INFO_PACKET wire format

The LAN/tracker `INFO_RESPONSE` packet (`BOLOPACKET_INFORESPONSE`, type `14`) is the
game-browser advertisement a server sends in reply to an info request, both to LAN
broadcasters and to the WinBolo.net tracker. The canonical definition is
`INFO_PACKET` in `src/bolo/internal/netpacks.h`; this document mirrors it for
out-of-tree consumers (e.g. the tracker's parser).

The packet is **113 bytes**, byte-packed (`BOLO_PACK_ATTR`), with no implicit
padding. There is no protocol-version negotiation: a server always emits the full
113-byte layout, and the layout only ever grows at the end. So a consumer reads
by prefix rather than by one exact length: accept a packet at least as long as
the newest field you understand, read every field the received length covers,
and substitute the documented default for each byte that is not there. Four
lengths are accepted today (`src/bolo/discovery.c`):

| Bytes | Constant | What it carries |
|----:|---|---|
| 76 | `INFO_PACKET_LEGACY_SIZE` | a server that predates the `flags`, count and md5 fields. The whole block is treated as absent, including the `flags` byte at 59 that falls inside the prefix |
| 111 | `INFO_PACKET_PRE_VIEWS_SIZE` | everything through `map_md5`; predates `view_policies` |
| 112 | `INFO_PACKET_PRE_VIEWS2_SIZE` | adds `view_policies`; predates `view_policies2` |
| 113 | `sizeof(INFO_PACKET)` | the full layout |

Any other length is dropped. A consumer that was written against the 112-byte
layout still reads every field it knows about out of a 113-byte packet, because
the byte that grew the packet went on the end.

## Where the packet is built

Every INFO_RESPONSE the server emits comes from `buildInfoPacket()` in
`src/server/udp/udp_server_query.c`. Two callers send what it produces:

- `serverHandleInfoRequest()` — replies to an info request from a LAN browser or
  a game finder, addressed to the requester.
- `transportUdpServerSendTrackerUpdate()` (`src/server/udp/udp_server_tracker.c`)
  — the periodic advertisement to the WinBolo.net tracker.

Both send the identical 113 bytes, so a consumer can parse either the same way,
and a field added to the layout reaches both paths at once.

## Byte layout

| Off | Size | Type | Field | Notes |
|----:|-----:|------|-------|-------|
| 0 | 4 | char[4] | signature | `'Bolo'` |
| 4 | 1 | u8 | versionMajor | |
| 5 | 1 | u8 | versionMinor | |
| 6 | 1 | u8 | versionRevision | |
| 7 | 1 | u8 | type | `14` = INFO_RESPONSE |
| 8 | 36 | char[36] | mapname | Pascal string (byte 0 = length, then chars) |
| 44 | 4 | in_addr | gameid.serveraddress | network order; `0` ⇒ use UDP source address. Filled with the configured public IP (`inet_addr`) when a public address is set, else `0` |
| 48 | 2 | u16 | gameid.serverport | host order (written raw, no `htons`) |
| 50 | 2 | u16 | gameid._padding | explicit padding, `0` |
| 52 | 4 | u32 | gameid.start_time | big-endian (`htonl`) — the one byte-swapped field |
| 56 | 1 | u8 | gametype | 1/2/3 = open / tournament / strict |
| 57 | 1 | u8 | allow_mines | bit `0x80` = mines on (`0x80` hidden, `0xC0` all visible) |
| 58 | 1 | u8 | allow_AI | legacy; currently always `0` |
| 59 | 1 | u8 | flags | bitfield, see below (was `spare1`) |
| 60 | 4 | i32 | start_delay | host order; 50ths-of-a-second until game start |
| 64 | 4 | i32 | time_limit | host order; game length in 50ths-of-a-second ticks |
| 68 | 2 | u16 | num_players | host order; total connected (humans + bots) |
| 70 | 2 | u16 | free_pills | host order; neutral pillboxes |
| 72 | 2 | u16 | free_bases | host order; neutral bases |
| 74 | 1 | u8 | has_password | non-zero = password set |
| 75 | 1 | u8 | spectator_count | `0` for now — spectators are future work (was `spare2`) |
| 76 | 1 | u8 | num_humans | human players among `num_players` |
| 77 | 1 | u8 | num_bots | AI bots among `num_players` (`num_humans + num_bots == num_players`) |
| 78 | 1 | u8 | max_players | server join-slot cap (16 unless configured lower) |
| 79 | 32 | char[32] | map_md5 | 32 lowercase hex chars, no NUL; see below |
| 111 | 1 | u8 | view_policies | 2 bits per visibility category plus the classic-mode and allies-in-trees bits, see below |
| 112 | 1 | u8 | view_policies2 | overview window and line of sight, 2 bits each, and the positional-sound bit, see below |

Total: **113 bytes**.

## `flags` byte (offset 59)

```
0x01  ALLOW_NEW_PLAYERS   server is accepting joins
0x02  LOCKED              server locked / not accepting joins
0x04  RANKED              ranked game
0x08  RANDOM_MAP          random map mode
0x10  ALLOW_SPECTATORS    spectators allowed (future work; currently 0)
0x20  IN_LOBBY            server is in the pre-game lobby (else in-game)
0x40, 0x80                voice mode — 2 bits, see below
```

Bits 6-7 are not two flags but one two-bit `ServerVoiceMode`
(`INFO_FLAG_VOICE_MASK`, `src/bolo/public/server_voice_mode.h`): `0` = voice on,
`1` = off, `2` = proximity. `serverVoiceOn` is `0`, so a server that predates the
encoding sends both bits clear and reads back as on — which is what it runs. The
unused fourth value reads as on for the same reason.

## `map_md5` field (offset 79)

- 32 ASCII lowercase hex characters — the MD5 of the canonical map bytes. **Fixed
  width, not NUL-terminated**; do not read it as a C string past 32 bytes.
- For a random or unknown map the field is zero-filled (`0x00` × 32). A consumer
  treats a leading `0x00` byte as "no md5" and skips it.
- It is never a raw 16-byte digest — always the 32-char hex text.

## `view_policies` byte (offset 111)

Two bits per visibility category, low bits first, then the two mode flags:

```
bits 0-1  pillboxes
bits 2-3  bases
bits 4-5  allied tanks
bit  6    classic mode (set = server runs the classic Bolo view)
bit  7    allies in trees (set = server sends allied tanks standing in forest)
```

Each two-bit field holds a policy value:

```
0  always   visible for the whole round
1  key      revealed on the player's view key
2  decay    revealed, then fades after the server's decay seconds
3  off      never revealed
```

The decay seconds themselves are not on this packet — they reach clients
through the lobby-settings control event. A packet shorter than 112 bytes
predates this byte; read it as pill = `always`, base = `off`, ally = `always`,
classic mode off and allies in trees off. A 112-byte packet does carry the byte,
so read it — only the two settings in `view_policies2` fall back at that length.

## `view_policies2` byte (offset 112)

Two bits for each of the first two settings and one for the third, low bits
first:

```
bits 0-1  overview window   0 = expanded, 1 = classic
bits 2-3  line of sight     0 = off, 1 = blocked by buildings and trees
bit  4    positional sound  0 = off, 1 = sounds panned by where they happen
bits 5-7  spare — always clear
```

The values are `OverviewWindow` and `LineOfSightMode` in
`src/bolo/public/view_policy.h`. The window is which block of map squares the
overview keeps live around the player's own tank: `expanded` is everything the
classic 15x15 view could scroll to, `classic` narrows it to the window that view
is actually showing. Line of sight is whether anything stops the player seeing
inside that block — a selector rather than a bool, so another rule can join it
without a second field.

Two bits can hold a value neither enum names. Read such a value as the enum's
zero — the expanded window, sight off — rather than reporting a mode you cannot
name; `infoPacketReadViewPolicies2()` range-checks both fields against
`OVERVIEW_WINDOW_COUNT` and `LINE_OF_SIGHT_COUNT` and does exactly that. A packet
shorter than 113 bytes predates the byte and reads the same way, with positional
sound off. A packet from a server that predates bit 4 sends it clear, so it also
reads as off.

## What the view defaults mean

The eight values a short packet reports — pill `always`, base `off`, ally
`always`, classic mode off, allies in trees off, the expanded window, sight
`off`, positional sound off — are the rules a server ran before each byte existed. They are
deliberately **not** the settings an unconfigured current server runs, which are
pill `key`, base `off`, ally `off`, the **classic** window and sight `off`. A
server old enough to leave the bytes out really does play differently from a
stock one. So compare against the back-compatibility set when deciding what an
advertisement said, and against the stock set when deciding whether a browser row
is worth tagging as non-standard; treating "absent" as "stock" mislabels the old
server. `src/gui/sdl3/dialogs/imgui_gamebrowser.cpp` keeps the two apart.

## Endianness

The packet is predominantly host byte order (little-endian in practice on the
servers that produce it) for the multi-byte numerics: `serverport`, `start_delay`,
`time_limit`, `num_players`, `free_pills`, `free_bases`. The exceptions are
`start_time` (big-endian, `htonl`) and `serveraddress` (network order, from
`inet_addr`). Every field at offset 59 and beyond that this format adds is either a
single byte or a char array, so none of the new fields need byte-swapping.

The mDNS TXT record set below carries the same two view bytes as four hex
characters, so a consumer reading a game off mDNS sees the rules the server
actually runs rather than the defaults. A record with no `view` key gets the
same defaults a 111-byte packet gets.

## `time_limit` interpretation

`time_limit` is the game length in 50ths-of-a-second ticks; minutes =
`time_limit / (50 * 60)`. It is the configured limit while the server is in lobby
(`IN_LOBBY` set) and the remaining time once a game is in progress (it decrements
during play).

## Same data over mDNS

The `_winbolo._udp.local` TXT record set carries almost the same information as
string key/value pairs (see `src/server/mdns_records.h` for the source struct and
`src/server/mdns_advertise.c` for the emission; `discoveryMdnsFillServer()` in
`src/bolo/discovery_mdns.c` is the parser). One TXT record is emitted per key,
`MDNS_WINBOLO_TXT_COUNT` = 23 of them, in this order:

```
map=<name>   ver=<maj.min.rev>   players=<n>   bases=<n>   pills=<n>
pass=<0|1 password set>   mines=<0|1 mines hidden>   game=<gameType int>
ai=<aiType int>   lobby=<0|1 in pre-game lobby>   locked=<0|1 locked>
md5=<32 hex chars, empty if random/unknown>   newp=<0|1 allow new players>
spingoff=<0|1 smart pings banned>   spec=<0|1 allow spectators>   nspec=<spectator count>   ranked=<0|1>
rnd=<0|1 random map>   tlim=<game length in 50ths-tick, 0 if none>
humans=<n>   bots=<n>   max=<max players>   view=<4 hex chars>
```

A record with no `spingoff` key reads as smart pings allowed, which is what
every server that predates the key does.

The `md5 newp spec nspec ranked rnd tlim humans bots max` keys mirror the
INFO_PACKET fields added alongside `flags`; `map ver players bases pills pass
mines game ai lobby locked` are the common fields the packet has always carried.
The server address and port are not TXT keys — they are the A and SRV records of
the same answer set.

Three things the packet carries have no TXT key at all: `start_time`,
`start_delay`, and the voice mode in the top two bits of `flags`. One key means
something different from the packet field it looks like: `mines=1` says mines are
**hidden**, where the packet's `allow_mines` uses `0x80` hidden against `0xC0`
all visible.

### `view` key

`view` is the two packed view bytes as `%02X%02X` — `view_policies` first, then
`view_policies2`, four uppercase hex characters in all. The advertiser packs them
with `infoPacketPackViewPolicies()` and `infoPacketPackViewPolicies2()` off the
same server getters the INFO packet uses, and the parser feeds them back through
`infoPacketReadViewPolicies()` and `infoPacketReadViewPolicies2()`, so both
discovery paths agree on what the bits mean — including what an unnamed value
reads as.

A value that does not parse as two hex bytes leaves the defaults in place, and so
does a record with no `view` key: pill `always`, base `off`, ally `always`,
classic mode off, allies in trees off, the expanded window, sight `off` and
positional sound off — the same set a 111-byte packet gets, with the same caveat
that it is not what a stock server runs. Positional sound is bit 4 of the second
byte, so a record from a server that predates the bit reads as off.

TXT values are text — the md5 is the hex string, never raw bytes, and `view` is
hex text rather than the two raw bytes.
