# INFO_PACKET wire format

The LAN/tracker `INFO_RESPONSE` packet (`BOLOPACKET_INFORESPONSE`, type `14`) is the
game-browser advertisement a server sends in reply to an info request, both to LAN
broadcasters and to the WinBolo.net tracker. The canonical definition is
`INFO_PACKET` in `src/bolo/internal/netpacks.h`; this document mirrors it for
out-of-tree consumers (e.g. the tracker's parser).

The packet is **112 bytes**, byte-packed (`BOLO_PACK_ATTR`), with no implicit
padding. There is no protocol-version negotiation: a server always emits the full
112-byte layout, so a consumer must read this layout and gate any length check on
`112`. A 111-byte packet is one from a server that predates `view_policies`; a
consumer that accepts it reads every field except that byte and substitutes the
view defaults below.

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

Total: **112 bytes**.

## `flags` byte (offset 59)

```
0x01  ALLOW_NEW_PLAYERS   server is accepting joins
0x02  LOCKED              server locked / not accepting joins
0x04  RANKED              ranked game
0x08  RANDOM_MAP          random map mode
0x10  ALLOW_SPECTATORS    spectators allowed (future work; currently 0)
0x20  IN_LOBBY            server is in the pre-game lobby (else in-game)
0x40, 0x80                reserved — ignore
```

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
through the lobby-settings control event. A packet that stops at 111 bytes
predates this byte; read it as pill = `always`, base = `off`, ally = `always`,
classic mode off and allies in trees off.

## Endianness

The packet is predominantly host byte order (little-endian in practice on the
servers that produce it) for the multi-byte numerics: `serverport`, `start_delay`,
`time_limit`, `num_players`, `free_pills`, `free_bases`. The exceptions are
`start_time` (big-endian, `htonl`) and `serveraddress` (network order, from
`inet_addr`). Every field at offset 59 and beyond that this format adds is either a
single byte or a char array, so none of the new fields need byte-swapping.

The mDNS TXT record set below does not carry the view policies, the
classic-mode bit or the allies-in-trees bit; a consumer reading a game off mDNS
uses the same defaults a 111-byte packet gets.

## `time_limit` interpretation

`time_limit` is the game length in 50ths-of-a-second ticks; minutes =
`time_limit / (50 * 60)`. It is the configured limit while the server is in lobby
(`IN_LOBBY` set) and the remaining time once a game is in progress (it decrements
during play).

## Same data over mDNS

The `_winbolo._udp.local` TXT record set carries the identical information as
string key/value pairs (see `src/server/mdns_records.h` for the source struct and
`src/server/mdns_advertise.c` for the emission). One TXT record is emitted per
key, in this order:

```
map=<name>   ver=<maj.min.rev>   players=<n>   bases=<n>   pills=<n>
pass=<0|1 password set>   mines=<0|1 mines on>   game=<gameType int>
ai=<aiType int>   lobby=<0|1 in pre-game lobby>   locked=<0|1 locked>
md5=<32 hex chars, empty if random/unknown>   newp=<0|1 allow new players>
spec=<0|1 allow spectators>   nspec=<spectator count>   ranked=<0|1>
rnd=<0|1 random map>   tlim=<game length in 50ths-tick, 0 if none>
humans=<n>   bots=<n>   max=<max players>
```

The `md5 newp spec nspec ranked rnd tlim humans bots max` keys mirror the
INFO_PACKET fields added alongside `flags`; `map ver players bases pills pass
mines game ai lobby locked` are the common fields the packet has always carried.

TXT values are text — the md5 is the hex string, never raw bytes.
