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
    F(U8, x) F(U8, y) F(U8, owner) F(U8, armour) F(U8, speed) F(U8, inTank)

#endif /* WINBOLO_WIRE_MESSAGES_H */
