/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PLAYER_FLAGS_H
#define PLAYER_FLAGS_H

/* Standalone client-identity defines. Kept in a tiny header (no structs)
 * so it can be included from both bolo/players.h and gui/lang.h without
 * dragging in conflicting type definitions — logviewer/players.h declares
 * its own (different) `player` / `players` types, so any file that pulls
 * in lang.h via messages.h must not see bolo/players.h's structs. */

/* Self-reported by client (untrusted, but no in-game advantage from lying). */
typedef enum {
    CLIENT_TYPE_UNKNOWN   = 0,
    CLIENT_TYPE_WINDOWS   = 1,
    CLIENT_TYPE_LINUX     = 2,
    CLIENT_TYPE_MACOS     = 3,
    CLIENT_TYPE_IOS       = 4,
    CLIENT_TYPE_ANDROID   = 5,
    CLIENT_TYPE_STEAMDECK = 6,
    CLIENT_TYPE_WEB       = 7,
    CLIENT_TYPE_COUNT     = 8   /* sentinel - keep last */
} ClientType;

#define PLAYER_FLAG_WBN_VERIFIED     0x01  /* server-set, trusted */
#define PLAYER_FLAG_WBN_STEAM_LINKED 0x02  /* server-set, trusted */
#define PLAYER_FLAG_SUPPORTER        0x04  /* server OR client; trusted iff WBN_VERIFIED */
#define PLAYER_FLAG_STEAM_BUILD      0x08  /* client-set, untrusted */
#define PLAYER_FLAG_ADMIN            0x10  /* server-set, trusted — joined
                                            * from an IP in -admins list,
                                            * has host-level lobby authority */
#define PLAYER_FLAG_BOT              0x20  /* server-set, trusted — slot is
                                            * driven by a brain (Lua AI) and
                                            * never sourced from a connected
                                            * human. Never honour from a
                                            * client packet. */
#define PLAYER_FLAG_HAS_MIC      0x40  /* client-set; voice enabled and a
                                        * working input device */
#define PLAYER_FLAG_VOICE_MUTED  0x80  /* client-set; has a mic but is not
                                        * transmitting */

#define PLAYER_CLIENT_HINT_MASK \
    (PLAYER_FLAG_SUPPORTER | PLAYER_FLAG_STEAM_BUILD)

/* The two mic bits. Masked out of a snapshot for recipients who could not
 * hear that player's voice, so visibility follows audibility.
 *
 * These take the last two free bits of the uint8_t clientFlags carries. A
 * third voice flag would mean widening clientFlags on the wire (the tank
 * snapshot and the lobby slot) and in playersGetClientFlags /
 * playersSetClientFlags — pack any further mic state into the existing two
 * rather than adding one. */
#define PLAYER_VOICE_FLAG_MASK (PLAYER_FLAG_HAS_MIC | PLAYER_FLAG_VOICE_MUTED)

#endif /* PLAYER_FLAGS_H */
