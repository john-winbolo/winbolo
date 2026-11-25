/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Builder check record
 *Filename:      builder_check_record.h
 *Purpose:
 *  The payload of the log_BuilderCheck record in a round log
 *  (.wbv): what the server tested when a shell's blast came
 *  near a builder, and what it found. The Log Viewer's
 *  "Server Check" picture draws it.
 *
 *  The server writes one when a shell ends and a builder is
 *  out (out now, or out on the shooter's screen), and his
 *  tested place or his present place is on the blast's square
 *  or one of its eight neighbours (shells.c,
 *  shellsBlastTestBuilder). It is written to the .wbv only,
 *  not to the spectator stream, so nothing on the network
 *  changes, and it changes nothing in the game.
 *
 *  Layout (all multi-byte fields big-endian):
 *    0      version (BUILDER_CHECK_RECORD_VERSION)
 *    1      builder's player slot
 *    2      shooter's player slot, 0xFF for a pillbox's shell
 *    3      the pillbox index whose shell it was, 0xFF for none
 *    4      result (BUILDER_CHECK_*)
 *    5      flags (BUILDER_CHECK_FLAG_*)
 *    6      the lgm_blast_shape rule (LGM_BLAST_SHAPE_*)
 *    7      the rewind the test used, in game ticks (0 = the present)
 *    8      the shell's own rewind before the builder's one tick less
 *    9-10   blast place X, world units
 *    11-12  blast place Y
 *    13-14  tested place X: where the server tested him
 *    15-16  tested place Y
 *    17-18  present place X: where he was on the server at that tick
 *    19-20  present place Y
 *    21-22  the pill_range rule, world units (it caps the open-ground area)
 *
 *  A reader takes the bytes it knows and skips the rest by the
 *  record's framed length, so a later version can add fields at
 *  the end. A viewer that does not know log_BuilderCheck skips
 *  the whole record by its framed length.
 *
 *  Header only, with no includes but the C library's, so the
 *  engine, the viewer and the unit tests use the same code.
 *********************************************************/

#ifndef BUILDER_CHECK_RECORD_H
#define BUILDER_CHECK_RECORD_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BUILDER_CHECK_RECORD_VERSION 1
#define BUILDER_CHECK_RECORD_BYTES 23

/* No shooter (a pillbox's shell) and no pillbox. */
#define BUILDER_CHECK_NONE 0xFF

/* The result. */
#define BUILDER_CHECK_LIVES      0  /* tested, the blast did not catch him */
#define BUILDER_CHECK_DIES       1  /* tested, caught, and killed */
#define BUILDER_CHECK_REFUSED    2  /* caught, and the host refused the death */
#define BUILDER_CHECK_NOT_TESTED 3  /* not tested: the shooter's screen showed
                                       no builder, or he was in a tank that
                                       could not be hit */

/* The flags. */
#define BUILDER_CHECK_FLAG_SOLID      0x01 /* the square rule: the blast's
                                              square is solid or the shell hit
                                              the thing on it */
#define BUILDER_CHECK_FLAG_ON_SQUARE  0x02 /* the tested place is on the
                                              blast's square */
#define BUILDER_CHECK_FLAG_REWOUND    0x04 /* the tested place is from the
                                              builder's history, as the
                                              shooter's screen had him */
#define BUILDER_CHECK_FLAG_IN_TANK    0x08 /* he was in his tank on the server */
#define BUILDER_CHECK_FLAG_SQUARE_HIT 0x10 /* the shell hit a pillbox, wall,
                                              forest, boat, shore or bridge */

typedef struct {
  uint8_t  version;
  uint8_t  builder;
  uint8_t  shooter;
  uint8_t  pill;
  uint8_t  result;
  uint8_t  flags;
  uint8_t  shape;
  uint8_t  rewind;
  uint8_t  shellRewind;
  uint16_t blastX, blastY;
  uint16_t testedX, testedY;
  uint16_t presentX, presentY;
  uint16_t pillRange;
} BuilderCheckRecord;

static inline void builderCheckPut16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xFF);
}

static inline uint16_t builderCheckGet16(const uint8_t *p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}

/* Writes r into out (cap bytes). Returns the bytes written, or 0 when out
   is too small. The version byte is always BUILDER_CHECK_RECORD_VERSION. */
static inline int builderCheckRecordEncode(const BuilderCheckRecord *r, uint8_t *out, int cap) {
  if (r == NULL || out == NULL || cap < BUILDER_CHECK_RECORD_BYTES) {
    return 0;
  }
  out[0] = BUILDER_CHECK_RECORD_VERSION;
  out[1] = r->builder;
  out[2] = r->shooter;
  out[3] = r->pill;
  out[4] = r->result;
  out[5] = r->flags;
  out[6] = r->shape;
  out[7] = r->rewind;
  out[8] = r->shellRewind;
  builderCheckPut16(out + 9, r->blastX);
  builderCheckPut16(out + 11, r->blastY);
  builderCheckPut16(out + 13, r->testedX);
  builderCheckPut16(out + 15, r->testedY);
  builderCheckPut16(out + 17, r->presentX);
  builderCheckPut16(out + 19, r->presentY);
  builderCheckPut16(out + 21, r->pillRange);
  return BUILDER_CHECK_RECORD_BYTES;
}

/* Reads len bytes into r. False for a record too short or of version 0.
   Bytes past the ones this version knows are left alone. */
static inline bool builderCheckRecordDecode(const uint8_t *in, int len, BuilderCheckRecord *r) {
  if (in == NULL || r == NULL || len < BUILDER_CHECK_RECORD_BYTES || in[0] == 0) {
    return false;
  }
  memset(r, 0, sizeof(*r));
  r->version = in[0];
  r->builder = in[1];
  r->shooter = in[2];
  r->pill = in[3];
  r->result = in[4];
  r->flags = in[5];
  r->shape = in[6];
  r->rewind = in[7];
  r->shellRewind = in[8];
  r->blastX = builderCheckGet16(in + 9);
  r->blastY = builderCheckGet16(in + 11);
  r->testedX = builderCheckGet16(in + 13);
  r->testedY = builderCheckGet16(in + 15);
  r->presentX = builderCheckGet16(in + 17);
  r->presentY = builderCheckGet16(in + 19);
  r->pillRange = builderCheckGet16(in + 21);
  return true;
}

/* Whether square (sx, sy) is the blast's square (bx, by) or one of its
   eight neighbours: the squares the record is written for. */
static inline bool builderCheckNearSquare(int sx, int sy, int bx, int by) {
  int dx = sx - bx, dy = sy - by;
  return dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1;
}

#endif /* BUILDER_CHECK_RECORD_H */
