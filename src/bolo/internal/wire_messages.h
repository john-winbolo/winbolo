/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Wire field lists for the flat fixed-layout leaf snapshots.
 *
 * Each *_FIELDS(F) macro names a message's fields once, in wire order, with its
 * type token (U8/U16/U32). DEFINE_WIRE_CODEC (production) and the differential
 * test both expand these lists, so the encoder, decoder, and on-wire size all
 * derive from a single definition. This is the only place these lists live.
 */
#ifndef WINBOLO_WIRE_MESSAGES_H
#define WINBOLO_WIRE_MESSAGES_H

#include "input_packet.h"  /* snapshot struct types + *_WIRE_SIZE */

#define SHELL_SNAPSHOT_FIELDS(F) \
    F(U16, worldX) F(U16, worldY) F(U8, angle) F(U8, owner) F(U8, length)

#define TK_EXPLOSION_SNAPSHOT_FIELDS(F) \
    F(U16, worldX) F(U16, worldY) F(U8, angle) F(U8, length) \
    F(U8, explodeType) F(U8, creator)

#define BASE_SNAPSHOT_FIELDS(F) \
    F(U8, owner) F(U8, armour) F(U8, shells) F(U8, mines)

#define PILL_SNAPSHOT_FIELDS(F) \
    F(U8, x) F(U8, y) F(U8, owner) F(U8, armourInTank)

/* Presence-bitmask message. Field order is WIRE order (follows the packer), not
 * the struct declaration order: the 11-byte core first, then the omit-zero
 * groups keyed on their TANK_PRESENT_* bit. */
#define TANK_SNAPSHOT_FIELDS(F, FMASK, FGROUP)               \
    F(U8, playerNum)                                         \
    FMASK()                                                  \
    F(U16, worldX) F(U16, worldY) F(U16, angle) F(U16, speed) \
    F(U8, tankStatus)                                        \
    FGROUP(TANK_PRESENT_OWNER_RES, U8, armour)               \
    FGROUP(TANK_PRESENT_OWNER_RES, U8, shells)               \
    FGROUP(TANK_PRESENT_OWNER_RES, U8, mines)                \
    FGROUP(TANK_PRESENT_OWNER_RES, U8, trees)                \
    FGROUP(TANK_PRESENT_OWNER_RES, U8, gunsightLen)          \
    FGROUP(TANK_PRESENT_RELOAD,    U8, reload)               \
    FGROUP(TANK_PRESENT_DEATHWAIT, U8, deathWait)            \
    FGROUP(TANK_PRESENT_LGM, U8, lgmFrame)                   \
    FGROUP(TANK_PRESENT_LGM, U8, lgmMX)                      \
    FGROUP(TANK_PRESENT_LGM, U8, lgmMY)                      \
    FGROUP(TANK_PRESENT_LGM, U8, lgmPX)                      \
    FGROUP(TANK_PRESENT_LGM, U8, lgmPY)                      \
    FGROUP(TANK_PRESENT_TURNRAMP, U8, firstLeft)             \
    FGROUP(TANK_PRESENT_TURNRAMP, U8, firstRight)            \
    FGROUP(TANK_PRESENT_PING,  U16, pingMs)                  \
    FGROUP(TANK_PRESENT_FLAGS, U8, clientFlags)

#endif /* WINBOLO_WIRE_MESSAGES_H */
