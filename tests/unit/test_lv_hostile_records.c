/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * The log viewer reading a recording it cannot trust.
 *
 * A .wbv is a file anyone can hand anyone, so every count, slot and length
 * byte in it is an input, not a fact. These cases build the bytes by hand and
 * run them through the production entry points — lv_specSeedLoad for a
 * snapshot, lv_specRecordPump for forward records — and assert that a bad
 * value costs the display and never the reader's memory or its alignment.
 *
 * The snapshot body is the empty-world shape the recorder writes for a lobby
 * segment, with the pillbox, base and start blocks replaced by the ones under
 * test; the player blocks are the not-in-use stubs.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "lv_log.h"
#include "lv_pillbox.h"
#include "lv_bases.h"
#include "lv_starts.h"
#include "lv_players.h"
#include "test_harness.h"

static void packU32BE(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t) (v >> 24);
  p[1] = (uint8_t) (v >> 16);
  p[2] = (uint8_t) (v >> 8);
  p[3] = (uint8_t) v;
}

/* Append one length-prefixed block: [len][payload]. */
static size_t appendBlock(uint8_t *out, size_t pos, const uint8_t *payload,
                          size_t n) {
  size_t i;
  out[pos++] = (uint8_t) n;
  for (i = 0; i < n; i++) {
    out[pos++] = payload[i];
  }
  return pos;
}

/* A whole snapshot body around the three item blocks: start delay, game
 * length, the blocks, a terminator-only map, then sixteen not-in-use player
 * stubs. Returns the body length. */
static size_t buildBody(uint8_t *out, const uint8_t *pills, size_t pillsLen,
                        const uint8_t *bs, size_t bsLen,
                        const uint8_t *ss, size_t ssLen) {
  size_t pos = 0;
  BYTE slot;

  memset(out, 0, 8);
  pos = 8;
  pos = appendBlock(out, pos, pills, pillsLen);
  pos = appendBlock(out, pos, bs, bsLen);
  pos = appendBlock(out, pos, ss, ssLen);
  out[pos++] = 4; out[pos++] = 255; /* map: terminator run only */
  out[pos++] = 255; out[pos++] = 255;
  for (slot = 0; slot < MAX_TANKS; slot++) {
    out[pos++] = 2;
    out[pos++] = slot;
    out[pos++] = 0;
  }
  return pos;
}

static bool loadBody(const uint8_t *body, size_t bodyLen) {
  uint8_t *seed;
  size_t seedLen = 4 + bodyLen + 4;
  bool loaded;

  seed = (uint8_t *) malloc(seedLen);
  if (seed == NULL) return FALSE;
  packU32BE(seed, (uint32_t) bodyLen);
  memcpy(seed + 4, body, bodyLen);
  packU32BE(seed + 4 + bodyLen, 0);
  loaded = lv_specSeedLoad(NULL, seed, seedLen);
  free(seed);
  return loaded;
}

/* Two live pillboxes, two bases, two starts: the ordinary seed the record
 * cases start from. */
static size_t buildOrdinaryBody(uint8_t *out) {
  uint8_t pills[1 + 2 * 9];
  uint8_t bs[1 + 2 * 10];
  uint8_t ss[1 + 2 * 3];
  size_t i;

  memset(pills, 0, sizeof(pills));
  memset(bs, 0, sizeof(bs));
  memset(ss, 0, sizeof(ss));
  pills[0] = 2;
  bs[0] = 2;
  ss[0] = 2;
  for (i = 0; i < 2; i++) {
    pills[1 + i * 9] = (uint8_t) (40 + i);      /* x */
    pills[2 + i * 9] = 40;                      /* y */
    pills[3 + i * 9] = 15;                      /* armour */
    pills[4 + i * 9] = 0xFF;                    /* owner: neutral */
    bs[1 + i * 10] = (uint8_t) (60 + i);
    bs[2 + i * 10] = 60;
    bs[3 + i * 10] = 0xFF;
    ss[1 + i * 3] = (uint8_t) (80 + i);
    ss[2 + i * 3] = 80;
  }
  return buildBody(out, pills, sizeof(pills), bs, sizeof(bs), ss, sizeof(ss));
}

/* A count byte of 255 in each item block, with one byte of payload behind it,
 * stores a count of zero: the array holds sixteen and the block describes
 * none. A count of twenty with twenty real records behind it stores sixteen. */
int run_lv_hostile_item_counts(void) {
  uint8_t body[2048];
  uint8_t pills[1 + 20 * 9];
  uint8_t bs[1 + 20 * 10];
  uint8_t ss[1 + 20 * 3];
  uint8_t hostile[1];
  size_t bodyLen;
  LogViewerState *lv;

  lv = lv_decoderCreate(false);
  UT_ASSERT_MSG(lv != NULL, "lv_decoderCreate returned NULL");
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);

  hostile[0] = 255;
  bodyLen = buildBody(body, hostile, 1, hostile, 1, hostile, 1);
  UT_ASSERT_MSG(loadBody(body, bodyLen) == TRUE,
                "snapshot with a count of 255 in every block failed to load");
  UT_ASSERT_MSG(lv_pillsGetNumPills(&lv->pb) == 0,
                "pill count = %d after a hostile 255 (want 0)",
                lv_pillsGetNumPills(&lv->pb));
  UT_ASSERT_MSG(lv_basesGetNumBases(&lv->bs) == 0,
                "base count = %d after a hostile 255 (want 0)",
                lv_basesGetNumBases(&lv->bs));
  UT_ASSERT_MSG(lv_startsGetNumStarts(&lv->ss) == 0,
                "start count = %d after a hostile 255 (want 0)",
                lv_startsGetNumStarts(&lv->ss));

  /* Twenty complete records: the block is well formed, the count is simply
   * more than the array holds. */
  memset(pills, 0, sizeof(pills));
  memset(bs, 0, sizeof(bs));
  memset(ss, 0, sizeof(ss));
  pills[0] = 20;
  bs[0] = 20;
  ss[0] = 20;
  bodyLen = buildBody(body, pills, sizeof(pills), bs, sizeof(bs),
                      ss, sizeof(ss));
  UT_ASSERT_MSG(loadBody(body, bodyLen) == TRUE,
                "snapshot with twenty records per block failed to load");
  UT_ASSERT_MSG(lv_pillsGetNumPills(&lv->pb) == MAX_PILLS,
                "pill count = %d from twenty records (want %d)",
                lv_pillsGetNumPills(&lv->pb), MAX_PILLS);
  UT_ASSERT_MSG(lv_basesGetNumBases(&lv->bs) == MAX_BASES,
                "base count = %d from twenty records (want %d)",
                lv_basesGetNumBases(&lv->bs), MAX_BASES);
  UT_ASSERT_MSG(lv_startsGetNumStarts(&lv->ss) == MAX_STARTS,
                "start count = %d from twenty records (want %d)",
                lv_startsGetNumStarts(&lv->ss), MAX_STARTS);

  lv_specSeedControlClear();
  lv_decoderDestroy(lv);
  return 0;
}

/* Append one framed record: [type][u16 BE len][body]. */
static size_t appendRecord(uint8_t *out, size_t pos, uint8_t type,
                           const uint8_t *body, size_t n) {
  out[pos++] = type;
  out[pos++] = (uint8_t) (n >> 8);
  out[pos++] = (uint8_t) n;
  memcpy(out + pos, body, n);
  return pos + n;
}

/* A log_EntityChange removal naming an index past the array is refused for
 * every kind, and the items the recording does have keep their flags. */
int run_lv_hostile_entity_remove_index(void) {
  uint8_t body[2048];
  uint8_t records[64];
  uint8_t rec[4];
  size_t pos = 0;
  size_t bodyLen;
  LogViewerState *lv;

  lv = lv_decoderCreate(false);
  UT_ASSERT_MSG(lv != NULL, "lv_decoderCreate returned NULL");
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);

  bodyLen = buildOrdinaryBody(body);
  UT_ASSERT_MSG(loadBody(body, bodyLen) == TRUE, "seed snapshot failed to load");
  UT_ASSERT_MSG(lv_pillsIsActive(&lv->pb, 1) && lv_pillsIsActive(&lv->pb, 2),
                "seed pillboxes are not both on the map");

  /* kind, index (0-based), on-map, then an empty pascal blob. */
  rec[2] = 0;
  rec[3] = 0;
  rec[0] = LV_ENTITY_KIND_PILL;  rec[1] = 200;
  pos = appendRecord(records, pos, (uint8_t) log_EntityChange, rec, 4);
  rec[0] = LV_ENTITY_KIND_BASE;  rec[1] = 200;
  pos = appendRecord(records, pos, (uint8_t) log_EntityChange, rec, 4);
  rec[0] = LV_ENTITY_KIND_START; rec[1] = 200;
  pos = appendRecord(records, pos, (uint8_t) log_EntityChange, rec, 4);
  rec[0] = LV_ENTITY_KIND_BASE;  rec[1] = 254;   /* wraps to number 255 */
  pos = appendRecord(records, pos, (uint8_t) log_EntityChange, rec, 4);

  UT_ASSERT_MSG(lv_specRecordPump(false, records, pos) == TRUE,
                "record pump stopped playback on an out-of-range removal");

  UT_ASSERT_MSG(lv_pillsIsActive(&lv->pb, 1) && lv_pillsIsActive(&lv->pb, 2),
                "an out-of-range removal touched a pillbox flag");
  UT_ASSERT_MSG(lv_basesIsActive(&lv->bs, 1) && lv_basesIsActive(&lv->bs, 2),
                "an out-of-range removal touched a base flag");
  UT_ASSERT_MSG(lv_startsIsActive(&lv->ss, 1) && lv_startsIsActive(&lv->ss, 2),
                "an out-of-range removal touched a start flag");

  /* The functions themselves refuse, whatever count the list holds. */
  UT_ASSERT(lv_pillsRemoveItem(&lv->pb, (BYTE) (MAX_PILLS + 1)) == FALSE);
  UT_ASSERT(lv_basesRemoveItem(&lv->bs, (BYTE) (MAX_BASES + 1)) == FALSE);
  UT_ASSERT(lv_startsRemoveItem(&lv->ss, (BYTE) (MAX_STARTS + 1)) == FALSE);

  /* And a real removal still works, so the guard refuses only what it should. */
  rec[0] = LV_ENTITY_KIND_PILL; rec[1] = 1;
  pos = appendRecord(records, 0, (uint8_t) log_EntityChange, rec, 4);
  UT_ASSERT_MSG(lv_specRecordPump(false, records, pos) == TRUE,
                "record pump stopped playback on a valid removal");
  UT_ASSERT_MSG(lv_pillsIsActive(&lv->pb, 2) == FALSE,
                "a valid removal of pillbox 2 left it on the map");
  UT_ASSERT_MSG(lv_pillsIsActive(&lv->pb, 1) == TRUE,
                "a valid removal of pillbox 2 took pillbox 1 with it");

  lv_specSeedControlClear();
  lv_decoderDestroy(lv);
  return 0;
}

/* A server line addressed to a slot past the roster reads as an empty seat,
 * and the name lookup itself answers the same for any slot past the array. */
int run_lv_hostile_server_text_slot(void) {
  uint8_t body[2048];
  uint8_t records[64];
  uint8_t rec[8];
  size_t pos = 0;
  size_t bodyLen;
  char hostile[32];
  char empty[32];
  LogViewerState *lv;

  lv = lv_decoderCreate(false);
  UT_ASSERT_MSG(lv != NULL, "lv_decoderCreate returned NULL");
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);

  bodyLen = buildOrdinaryBody(body);
  UT_ASSERT_MSG(loadBody(body, bodyLen) == TRUE, "seed snapshot failed to load");

  /* destTeam 0, destPlayer 200, then the pascal text "hi". */
  rec[0] = 0;
  rec[1] = 200;
  rec[2] = 2;
  rec[3] = 'h';
  rec[4] = 'i';
  pos = appendRecord(records, pos, (uint8_t) log_ServerText, rec, 5);
  UT_ASSERT_MSG(lv_specRecordPump(false, records, pos) == TRUE,
                "record pump stopped playback on a server line to slot 200");

  memset(hostile, 'x', sizeof(hostile));
  memset(empty, 'y', sizeof(empty));
  lv_playersGetPlayerName(200, hostile, sizeof(hostile));
  lv_playersGetPlayerName((BYTE) (MAX_TANKS - 1), empty, sizeof(empty));
  UT_ASSERT_MSG(strcmp(hostile, empty) == 0,
                "slot 200 reads '%s', an empty seat reads '%s' (want the same)",
                hostile, empty);

  lv_specSeedControlClear();
  lv_decoderDestroy(lv);
  return 0;
}

/* A log_TankSetModifiers record whose length byte is not six is consumed by
 * that length, so the record behind it still decodes. */
int run_lv_hostile_modifiers_length(void) {
  uint8_t body[2048];
  uint8_t records[64];
  uint8_t rec[8];
  size_t pos = 0;
  size_t bodyLen;
  LogViewerState *lv;

  lv = lv_decoderCreate(false);
  UT_ASSERT_MSG(lv != NULL, "lv_decoderCreate returned NULL");
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);

  bodyLen = buildOrdinaryBody(body);
  UT_ASSERT_MSG(loadBody(body, bodyLen) == TRUE, "seed snapshot failed to load");

  /* slot 2, a five-byte blob: one short of a modifier set. */
  rec[0] = 2;
  rec[1] = 5;
  rec[2] = 50; rec[3] = 50; rec[4] = 50; rec[5] = 50; rec[6] = 50;
  pos = appendRecord(records, pos, (uint8_t) log_TankSetModifiers, rec, 7);
  /* Then a stock record the reader must still land on. */
  rec[0] = 2;
  rec[1] = 9;
  rec[2] = 8;
  rec[3] = 7;
  rec[4] = 6;
  pos = appendRecord(records, pos, (uint8_t) log_TankSetStock, rec, 5);

  UT_ASSERT_MSG(lv_specRecordPump(false, records, pos) == TRUE,
                "record pump stopped playback after a short modifiers blob");
  UT_ASSERT_MSG(lv->tankInv[2].shells == 9 && lv->tankInv[2].mines == 8 &&
                lv->tankInv[2].armour == 7 && lv->tankInv[2].trees == 6,
                "slot 2 stocks = %d/%d/%d/%d after a short modifiers blob "
                "(want 9/8/7/6: the reader lost its alignment)",
                lv->tankInv[2].shells, lv->tankInv[2].mines,
                lv->tankInv[2].armour, lv->tankInv[2].trees);

  /* A six-byte blob is a modifier set and is applied. */
  rec[0] = 3;
  rec[1] = 6;
  rec[2] = 50; rec[3] = 60; rec[4] = 70; rec[5] = 80; rec[6] = 90; rec[7] = 100;
  pos = appendRecord(records, 0, (uint8_t) log_TankSetModifiers, rec, 8);
  UT_ASSERT_MSG(lv_specRecordPump(false, records, pos) == TRUE,
                "record pump stopped playback on a modifiers record");
  UT_ASSERT_MSG(lv->tankMods[3].speed == 50 && lv->tankMods[3].taken == 100,
                "slot 3 modifiers = %d..%d (want 50..100)",
                lv->tankMods[3].speed, lv->tankMods[3].taken);

  lv_specSeedControlClear();
  lv_decoderDestroy(lv);
  return 0;
}
