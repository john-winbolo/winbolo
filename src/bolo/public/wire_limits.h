/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * wire_limits.h
 *
 * Public wire-protocol values that legitimately surface in GUI code:
 * payload-size caps (so input widgets can show the right limit) and
 * the version string used in compatibility checks. The wire format
 * itself stays internal in netpacks.h.
 *
 * No #includes, no behaviour.
 */

#ifndef WIRE_LIMITS_H
#define WIRE_LIMITS_H

/* Maximum bytes per chat message payload on the wire. The chat input
 * widget in the desktop GUI sizes its buffer to this so the user sees
 * the cap as they type. Must match the wire layout in
 * transport_udp_client.c / transport_udp_server.c. */
#define PACKET_MAX_CHAT_MESSAGE 128

/* Max size of player name in join request. Surfaced publicly so the
 * client-side lobby slot mirror (ClientLobbySlot in client_sim.h) can
 * size its playerName[] buffer without pulling in the internal
 * netpacks.h wire-protocol header. */
#define PACKET_MAX_PLAYER_NAME 64

/* Version string used in info-packet responses and surfaced in the
 * server browser to gate "join" against version-skewed servers.
 * WINBOLO_VERSION is supplied as a compile definition by CMake. */
#define STRVER WINBOLO_VERSION

/* Maximum bytes accepted by the lobby map-upload path
 * (PACKET_LOBBY_MAP_UPLOAD_BEGIN and PACKET_LOBBY_MAP_USE_LOCAL). The
 * .map RLE format encodes a fully-pathological map (every playable
 * cell different from its neighbour, all 215 playable rows populated)
 * in roughly 27 KiB; a fully-pathological 256×256 grid maxes out
 * around 37 KiB. 64 KiB therefore accepts every legitimate map with
 * comfortable headroom for editor quirks while shrinking the
 * attacker's working set ~16× compared with the historical 1 MiB
 * cap. Enforced on the wire by transport_udp_server.c (UPLOAD_BEGIN
 * and USE_LOCAL handlers), by serverSimReadMapFile, and pre-flighted
 * client-side in imgui_lobby.cpp. Verified by the unit test
 * upload_cap_enforced. */
#define LOBBY_MAP_UPLOAD_MAX_BYTES (64u * 1024u)

/* Maximum bytes accepted for a scenario package: a .scenario file, or a
 * .map with a WBSC container appended to it. The cap above stays the one
 * a plain map is held to — a package is bigger because it carries a
 * manifest, a script and whatever brain directories the scenario ships
 * with, all deflated inside the container.
 *
 * 4 MiB, the same as ROUND_LOG_MAX_BYTES. The heaviest brain in this tree
 * is GoalHunter, 5.8 MB of Lua on disk and about 1 MB deflated, so a map
 * shipping one comes to roughly a megabyte and this leaves room for
 * several. On the wire, PACKET_LOBBY_MAP_UPLOAD_BEGIN holds a script
 * upload (UPLOAD_KIND_SCRIPT) to it, and so does the server's bulk sink
 * when the bytes arrive. The disk readers read against it too: the
 * scenario directory lister, and the scenario host, which reads a map
 * file from the operator's own disk looking for a container in it. One
 * number for all of them, so a package an operator can play on their
 * own server is one they can upload. */
#define LOBBY_PACKAGE_UPLOAD_MAX_BYTES (4u * 1024u * 1024u)

/* Maximum bytes the server will serve for the last completed round's
 * replay log (PACKET_ROUND_LOG_REQ, carried as BULK_KIND_ROUND_LOG).
 * A busy full lobby records about 2.3 KB/s, so 4 MiB is roughly half
 * an hour of a round. Above the cap the server refuses rather than
 * truncating: a .wbv is a zip whose central directory minizip writes
 * only at zipClose(), so a truncated one is unopenable, not merely
 * shorter. The size is checked before the file is read, so an
 * over-cap log never enters memory. */
#define ROUND_LOG_MAX_BYTES (4u * 1024u * 1024u)

/* ServerLocks bitmask — 32 bits wide, sent in extended
 * PACKET_LOBBY_STATE. Set by bolod CLI flags (--lock-game-type etc);
 * never changes after server startup. Hosts cannot modify locks;
 * clients render matching settings disabled with a lock badge.
 * Surfaced publicly so servermain.c (which parses the CLI flags) and
 * the GUI lobby (which renders the disabled state) can both reach
 * these without including internal/netpacks.h. */
#define LOBBY_LOCK_GAME_TYPE         (1u << 0)
#define LOBBY_LOCK_AI_POLICY         (1u << 1)
#define LOBBY_LOCK_MINES             (1u << 2)
#define LOBBY_LOCK_TIME_LIMIT        (1u << 3)
#define LOBBY_LOCK_AUTO_LOCK_ON_GAME (1u << 4)
#define LOBBY_LOCK_PASSWORD          (1u << 5)
#define LOBBY_LOCK_RANKED            (1u << 6)
#define LOBBY_LOCK_OPEN_HOST         (1u << 7)
#define LOBBY_LOCK_MAP               (1u << 8)
#define LOBBY_LOCK_PILL_VIEW         (1u << 9)
#define LOBBY_LOCK_BASE_VIEW         (1u << 10)
#define LOBBY_LOCK_ALLY_VIEW         (1u << 11)
#define LOBBY_LOCK_CLASSIC_MODE      (1u << 12)
#define LOBBY_LOCK_ALLIES_IN_TREES   (1u << 13)
#define LOBBY_LOCK_OVERVIEW_WINDOW   (1u << 14)
#define LOBBY_LOCK_LINE_OF_SIGHT     (1u << 15)
#define LOBBY_LOCK_SMART_PINGS       (1u << 16)
#define LOBBY_LOCK_MODS              (1u << 17)
#define LOBBY_LOCK_POSITIONAL_SOUND  (1u << 18)
/* The lobby's script list itself, rather than the Mods/Scenario checkbox
 * LOBBY_LOCK_MODS covers: nothing may be added to it, taken off it or moved
 * on it. Set by the dedicated server's -mod-locked, never by -lock, and it
 * implies LOBBY_LOCK_MODS (serverSimAddImpliedLocks) so the checkbox cannot
 * switch the fixed list off either. A client that predates the bit ignores
 * it and offers the chooser as before; the server refuses the list it
 * sends with CMD_REJECT_LOCKED. */
#define LOBBY_LOCK_SCRIPT_LIST       (1u << 19)

/* LST_TIME_MINUTES accepted range. Surfaced publicly so the lobby
 * UI can validate the user's value before sending. Authoritative
 * range check is wire-side (transport_udp_server.c). */
#define LOBBY_TIME_MINUTES_MIN 1
#define LOBBY_TIME_MINUTES_MAX 4320

/* Setting types used inside PACKET_LOBBY_SET_SETTING /
 * PACKET_LOBBY_SETTING_CHG payloads. Surfaced publicly so the GUI
 * lobby can name the wire value it's sending. Wire ids are PINNED —
 * each member's byte value is part of the protocol. New settings
 * append with the next free id; existing ids are NEVER renumbered.
 * Forward-compat: receivers must skip unknown types via valueLen. */
typedef enum {
    LST_GAME_TYPE         = 1,  /* 1 byte enum: open|tournament|strict */
    LST_HIDDEN_MINES      = 2,  /* 1 byte bool */
    LST_AI_POLICY         = 3,  /* 1 byte enum: none|allow|advantage|full */
    LST_TIME_LIMIT        = 4,  /* 1 byte bool */
    LST_TIME_MINUTES      = 5,  /* 2 bytes uint16 BE */
    LST_AUTO_LOCK_ON_GAME = 6,  /* 1 byte bool */
    LST_RANKED            = 7,  /* 1 byte bool. When true the server
                                 * forces ai=none, refuses game_type
                                 * Open, and removes any existing
                                 * bots. The client mirrors the value
                                 * so every viewer sees the ranked
                                 * badge — toggle is still host /
                                 * admin only. */
    LST_PILL_VIEW         = 8,  /* 3 bytes: [policy][decaySecs hi][decaySecs lo] */
    LST_BASE_VIEW         = 9,  /* same */
    LST_ALLY_VIEW         = 10, /* same */
    LST_CLASSIC_MODE      = 11, /* 1 byte bool. When true the server
                                 * sets pill view to key and base and
                                 * ally view to off, and refuses an
                                 * edit to any of those three while it
                                 * stays on. */
    LST_ALLIES_IN_TREES   = 12, /* 1 byte bool. When true an allied
                                 * tank standing in trees is sent to
                                 * its allies instead of being
                                 * withheld. Off is the classic
                                 * behaviour, and classic mode forces
                                 * it off and refuses an edit while it
                                 * stays on. */
    LST_OVERVIEW_WINDOW   = 13, /* 1 byte OverviewWindow. Which block of
                                 * squares the map overview keeps live
                                 * round the player's own tank. Classic
                                 * mode forces the narrow window and
                                 * refuses an edit while it stays on. */
    LST_LINE_OF_SIGHT     = 14, /* 1 byte LineOfSightMode. What stops the
                                 * player seeing inside that block.
                                 * Classic mode forces it off and refuses
                                 * an edit while it stays on. */
    LST_SMART_PINGS_OFF   = 15, /* 1 byte bool, carried in the NEGATIVE
                                 * sense: non-zero means the server refuses
                                 * smart pings. Allowing them is the legacy
                                 * behaviour, and every optional field on
                                 * this wire reads as zero when the sender
                                 * never learned it, so refusing them is the
                                 * value that has to cost a byte to say. */
    LST_MODS_OFF          = 16, /* 1 byte bool, carried in the NEGATIVE
                                 * sense for the same reason as
                                 * LST_SMART_PINGS_OFF above: non-zero means
                                 * the round composes none of the scripts
                                 * the host has picked. Running them is what
                                 * every build before this one did, so it is
                                 * the zero, and a server or client that
                                 * never writes the byte behaves as it did.
                                 *
                                 * Mods and picked scenarios alike: the
                                 * lobby labels it Mods/Scenario. Every pick
                                 * is skipped when this is set, and only the
                                 * map's own script still plays. The pick
                                 * list is not touched, so checking the box
                                 * back on brings the same scripts back in
                                 * the same order. A server built before the
                                 * scenario half skips the mods alone. */
    LST_POSITIONAL_SOUND  = 17  /* 1 byte bool, carried the plain way
                                 * round: non-zero means on. When on,
                                 * sound events tell a human which side a
                                 * sound is on and roughly how far. Off is
                                 * the classic behaviour, and classic mode
                                 * forces it off and refuses an edit while
                                 * it stays on. */
} LobbySettingType;

#endif /* WIRE_LIMITS_H */
