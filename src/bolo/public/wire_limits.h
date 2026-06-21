/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
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

/* ServerLocks bitmask — sent in extended PACKET_LOBBY_STATE. Set by
 * bolod CLI flags (--lock-game-type etc); never changes after server
 * startup. Hosts cannot modify locks; clients render matching settings
 * disabled with a lock badge. Surfaced publicly so servermain.c (which
 * parses the CLI flags) and the GUI lobby (which renders the disabled
 * state) can both reach these without including internal/netpacks.h. */
#define LOBBY_LOCK_GAME_TYPE         (1u << 0)
#define LOBBY_LOCK_AI_POLICY         (1u << 1)
#define LOBBY_LOCK_MINES             (1u << 2)
#define LOBBY_LOCK_TIME_LIMIT        (1u << 3)
#define LOBBY_LOCK_AUTO_LOCK_ON_GAME (1u << 4)
#define LOBBY_LOCK_PASSWORD          (1u << 5)
#define LOBBY_LOCK_RANKED            (1u << 6)
#define LOBBY_LOCK_OPEN_HOST         (1u << 7)
#define LOBBY_LOCK_MAP               (1u << 8)

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
    LST_RANKED            = 7   /* 1 byte bool. When true the server
                                 * forces ai=none, refuses game_type
                                 * Open, and removes any existing
                                 * bots. The client mirrors the value
                                 * so every viewer sees the ranked
                                 * badge — toggle is still host /
                                 * admin only. */
} LobbySettingType;

#endif /* WIRE_LIMITS_H */
