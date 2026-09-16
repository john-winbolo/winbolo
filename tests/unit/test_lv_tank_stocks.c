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
 * Log-viewer decode of per-tank stocks (shells, mines, armour, trees).
 *
 * The viewer takes them from the recording and nowhere else: the four bytes on
 * the end of a snapshot's player block, and the log_TankSetStock records that
 * carry each change in between. These tests build the bytes by hand and run
 * them through the production entry points — lv_specSeedLoad for a snapshot,
 * lv_specRecordPump for a forward record — then read the decoded values back
 * through the accessor the game view's status panel uses.
 *
 * The hand-built snapshot body is the empty-world shape the recorder writes for
 * a lobby segment (no pills, bases, starts or map runs), with real player
 * blocks bolted on: the sections before the players are not what is under test
 * and the reader treats an empty world as ordinary.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "lv_log.h"
#include "lv_players.h"
#include "test_harness.h"

/* Read a slot's stocks the way lv_gameViewGetInventory does. That accessor
 * lives in src/logviewer/logviewer.c, which this target does not link, so go
 * through the decoder state screen.c hands out instead — the same tankInv the
 * parser writes. */
static void lv_tank_stocks_get(BYTE slot, BYTE *shells, BYTE *mines,
                               BYTE *armour, BYTE *trees) {
  if (slot >= MAX_TANKS) {
    *shells = 0; *mines = 0; *armour = 0; *trees = 0;
    return;
  }
  {
    LogViewerState *lv = lv_screenGetState();
    *shells = lv->tankInv[slot].shells;
    *mines  = lv->tankInv[slot].mines;
    *armour = lv->tankInv[slot].armour;
    *trees  = lv->tankInv[slot].trees;
  }
}

static void packU32BE(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t) (v >> 24);
  p[1] = (uint8_t) (v >> 16);
  p[2] = (uint8_t) (v >> 8);
  p[3] = (uint8_t) v;
}

/* Append one player block: [dataLen][payload]. A slot in use carries the 2
 * header bytes, 9 tank/man fields, a Pascal name, a Pascal location, the
 * alliance list, and — when withStocks is TRUE — the four stock bytes. A slot
 * not in use is the 2-byte stub. Returns the new write offset. */
static size_t appendPlayerBlock(uint8_t *out, size_t pos, BYTE slot, bool inUse,
                                bool withStocks, BYTE shells, BYTE mines,
                                BYTE armour, BYTE trees) {
  static const char kName[] = "Tester";
  size_t lenAt;
  size_t start;
  size_t i;

  if (!inUse) {
    out[pos++] = 2;
    out[pos++] = slot;
    out[pos++] = 0;
    return pos;
  }

  lenAt = pos++;          /* dataLen, filled in once the block is built */
  start = pos;
  out[pos++] = slot;
  out[pos++] = 1;         /* in use */
  out[pos++] = 10;        /* tank mx — non-zero, so the slot reads as alive */
  out[pos++] = 12;        /* tank my */
  out[pos++] = 0;         /* tank pixel x/y nibbles */
  out[pos++] = 0;         /* tank frame */
  out[pos++] = 0;         /* on boat */
  out[pos++] = 0;         /* man mx — (0,0) means aboard */
  out[pos++] = 0;         /* man my */
  out[pos++] = 0;         /* man pixel x/y nibbles */
  out[pos++] = 0;         /* man frame */
  out[pos++] = (uint8_t) (sizeof(kName) - 1);
  for (i = 0; i + 1 < sizeof(kName); i++) {
    out[pos++] = (uint8_t) kName[i];
  }
  out[pos++] = 0;         /* location: empty Pascal string */
  out[pos++] = 0;         /* alliance count */
  if (withStocks) {
    out[pos++] = shells;
    out[pos++] = mines;
    out[pos++] = armour;
    out[pos++] = trees;
  }
  out[lenAt] = (uint8_t) (pos - start);
  return pos;
}

/* Build a whole snapshot body: start delay, game length, empty pill/base/start
 * blocks, a terminator-only map, then 16 player blocks where slot 0 is in use.
 * Returns the body length. */
static size_t buildSnapshotBody(uint8_t *out, bool withStocks, BYTE shells,
                                BYTE mines, BYTE armour, BYTE trees) {
  size_t pos = 0;
  BYTE slot;

  memset(out, 0, 8);      /* start delay + game length, both zero */
  pos = 8;
  out[pos++] = 1; out[pos++] = 0;   /* pills:  1-byte payload, count 0 */
  out[pos++] = 1; out[pos++] = 0;   /* bases */
  out[pos++] = 1; out[pos++] = 0;   /* starts */
  out[pos++] = 4; out[pos++] = 255; /* map: terminator run only */
  out[pos++] = 255; out[pos++] = 255;

  pos = appendPlayerBlock(out, pos, 0, TRUE, withStocks,
                          shells, mines, armour, trees);
  for (slot = 1; slot < MAX_TANKS; slot++) {
    pos = appendPlayerBlock(out, pos, slot, FALSE, FALSE, 0, 0, 0, 0);
  }
  return pos;
}

/* Wrap a snapshot body as the ring-shaped seed lv_specSeedLoad takes —
 * [u32 bodyLen BE][body][u32 ctrlLen BE] with no control slice — and load it.
 * Returns what lv_specSeedLoad returned. */
static bool loadSnapshotBody(const uint8_t *body, size_t bodyLen) {
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

/* Non-adjacent allies exercise forward references in the snapshot. Later
 * keyframes split and restore the teams without any alliance events. */
int run_lv_team_colours_from_snapshot(void) {
  uint8_t body[1024], seed[1032];
  BYTE initialTeams[4];
  LogViewerState *lv = lv_decoderCreate(false);
  int pass;
  UT_ASSERT(lv != NULL);
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);

  for (pass = 0; pass < 4; pass++) {
    BYTE slot;
    size_t pos;
    buildSnapshotBody(body, FALSE, 0, 0, 0, 0);
    pos = 18; /* Empty world header, before the first player block. */
    for (slot = 0; slot < MAX_TANKS; slot++) {
      size_t start = pos;
      pos = appendPlayerBlock(body, pos, slot, slot < 4, FALSE, 0, 0, 0, 0);
      if (slot < 4) {
        body[pos - 1] = (pass == 2) ? 1 : 2;
        body[pos++] = slot;
        if (pass != 2) body[pos++] = slot ^ 2;
        body[start] = (uint8_t)(pos - start - 1);
      }
    }
    if (pass == 0) {
      UT_ASSERT(loadSnapshotBody(body, pos));
    } else {
      packU32BE(seed, (uint32_t)pos);
      memcpy(seed + 4, body, pos);
      packU32BE(seed + 4 + pos, 0);
      UT_ASSERT(lv_specRecordPump(TRUE, seed, pos + 8));
    }
    if (pass == 2) {
      for (slot = 0; slot < 4; slot++) {
        BYTE other;
        for (other = 0; other < slot; other++) {
          UT_ASSERT(lv_playersGetTeamId(slot) != lv_playersGetTeamId(other));
        }
      }
    } else {
      UT_ASSERT(lv_playersGetTeamId(0) == lv_playersGetTeamId(2));
      UT_ASSERT(lv_playersGetTeamId(1) == lv_playersGetTeamId(3));
      UT_ASSERT(lv_playersGetTeamId(0) != lv_playersGetTeamId(1));
      for (slot = 0; slot < 4; slot++) {
        UT_ASSERT(lv_playersGetTeamForOwner(slot) == lv_playersGetTeamId(slot));
        if (pass == 0) initialTeams[slot] = lv_playersGetTeamId(slot);
        else UT_ASSERT(lv_playersGetTeamId(slot) == initialTeams[slot]);
      }
    }
  }
  lv_specSeedControlClear();
  lv_decoderDestroy(lv);
  return 0;
}

/* Players who joined solo hold slot-order colours. A merge before the round
 * keeps the lower one, the round's first world snapshot deals compact colours
 * to the groups, and later merges and splits leave everyone who didn't move
 * alone. */
int run_lv_team_colours_events(void) {
  LogViewerState *lv = lv_decoderCreate(false);
  char location[1] = "";
  BYTE slot;
  UT_ASSERT(lv != NULL);

  for (slot = 0; slot < 4; slot++) {
    char name[8];
    snprintf(name, sizeof(name), "P%d", slot);
    lv_playersSetPlayer(slot, name, location, 0, 0, 0, 0, 0, FALSE, 0, NULL, FALSE, FALSE, 0);
    UT_ASSERT(lv_playersGetTeamId(slot) == slot);
  }

  /* Lobby pairing, 0+1 against 2+3: each pair keeps its lower colour. */
  lv_playersAcceptAlliance(0, 1);
  lv_playersAcceptAlliance(2, 3);
  UT_ASSERT(lv_playersGetTeamId(0) == 0 && lv_playersGetTeamId(1) == 0);
  UT_ASSERT(lv_playersGetTeamId(2) == 2 && lv_playersGetTeamId(3) == 2);

  /* Round start: two teams are Team 1 and Team 2. */
  lv_playersRebuildTeams(FALSE);
  UT_ASSERT(lv_playersGetTeamId(0) == 0 && lv_playersGetTeamId(1) == 0);
  UT_ASSERT(lv_playersGetTeamId(2) == 1 && lv_playersGetTeamId(3) == 1);

  /* The leaver takes the lowest free colour; the group it left keeps its own. */
  lv_playersLeaveAlliance(3);
  UT_ASSERT(lv_playersGetTeamId(0) == 0 && lv_playersGetTeamId(1) == 0);
  UT_ASSERT(lv_playersGetTeamId(2) == 1 && lv_playersGetTeamId(3) == 2);

  /* Rejoining adopts the group's colour and frees the old one. */
  lv_playersAcceptAlliance(2, 3);
  UT_ASSERT(lv_playersGetTeamId(2) == 1 && lv_playersGetTeamId(3) == 1);

  /* A keeping rebuild, as every later snapshot runs, changes nothing. */
  lv_playersRebuildTeams(TRUE);
  UT_ASSERT(lv_playersGetTeamId(0) == 0 && lv_playersGetTeamId(1) == 0);
  UT_ASSERT(lv_playersGetTeamId(2) == 1 && lv_playersGetTeamId(3) == 1);

  lv_decoderDestroy(lv);
  return 0;
}

/* A snapshot's four stock bytes reach the slot they belong to, and a slot that
 * is not in use reads as no stocks at all. */
int run_lv_tank_stocks_from_snapshot(void) {
  uint8_t body[1024];
  size_t bodyLen;
  LogViewerState *lv;
  BYTE shells = 0, mines = 0, armour = 0, trees = 0;

  lv = lv_decoderCreate(false);
  UT_ASSERT_MSG(lv != NULL, "lv_decoderCreate returned NULL");
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);

  bodyLen = buildSnapshotBody(body, TRUE, 7, 11, 23, 5);
  UT_ASSERT_MSG(loadSnapshotBody(body, bodyLen) == TRUE,
                "snapshot with stocks failed to load");

  lv_tank_stocks_get(0, &shells, &mines, &armour, &trees);
  UT_ASSERT_MSG(shells == 7,  "slot 0 shells = %d (want 7)", shells);
  UT_ASSERT_MSG(mines  == 11, "slot 0 mines = %d (want 11)", mines);
  UT_ASSERT_MSG(armour == 23, "slot 0 armour = %d (want 23)", armour);
  UT_ASSERT_MSG(trees  == 5,  "slot 0 trees = %d (want 5)", trees);

  /* Slot 1 is a not-in-use stub: no stocks, and nothing borrowed from slot 0. */
  lv_tank_stocks_get(1, &shells, &mines, &armour, &trees);
  UT_ASSERT_MSG(shells == 0 && mines == 0 && armour == 0 && trees == 0,
                "unused slot 1 = %d/%d/%d/%d (want all 0)",
                shells, mines, armour, trees);

  lv_specSeedControlClear();
  lv_decoderDestroy(lv);
  return 0;
}

/* Every recording made before the stocks were written ends its player blocks at
 * the alliance list. That block must still parse — the block length says where
 * it ends — and the slot reads as no stocks rather than as a guess. */
int run_lv_tank_stocks_snapshot_without_tail(void) {
  uint8_t body[1024];
  size_t bodyLen;
  LogViewerState *lv;
  char name[64];
  BYTE shells = 0, mines = 0, armour = 0, trees = 0;

  lv = lv_decoderCreate(false);
  UT_ASSERT_MSG(lv != NULL, "lv_decoderCreate returned NULL");
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);

  bodyLen = buildSnapshotBody(body, FALSE, 0, 0, 0, 0);
  UT_ASSERT_MSG(loadSnapshotBody(body, bodyLen) == TRUE,
                "snapshot without stocks failed to load");

  /* The rest of the block decoded, so the missing stocks did not cost the
   * reader the fields in front of them. */
  lv_screenGetPlayerName(name, 0, sizeof(name));
  UT_ASSERT_MSG(strcmp(name, "Tester") == 0,
                "slot 0 name = '%s' (want 'Tester')", name);

  lv_tank_stocks_get(0, &shells, &mines, &armour, &trees);
  UT_ASSERT_MSG(shells == 0 && mines == 0 && armour == 0 && trees == 0,
                "slot 0 = %d/%d/%d/%d (want all 0 with no stocks recorded)",
                shells, mines, armour, trees);

  lv_specSeedControlClear();
  lv_decoderDestroy(lv);
  return 0;
}

/* A log_TankSetStock record updates the slot it names, and a record type this
 * build does not know is skipped by its framed length — the known record after
 * it still decodes, which is what proves the cursor stayed aligned. */
int run_lv_tank_stocks_from_record(void) {
  uint8_t body[1024];
  uint8_t records[16];
  size_t bodyLen;
  size_t pos = 0;
  LogViewerState *lv;
  BYTE shells = 0, mines = 0, armour = 0, trees = 0;

  lv = lv_decoderCreate(false);
  UT_ASSERT_MSG(lv != NULL, "lv_decoderCreate returned NULL");
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);

  bodyLen = buildSnapshotBody(body, FALSE, 0, 0, 0, 0);
  UT_ASSERT_MSG(loadSnapshotBody(body, bodyLen) == TRUE,
                "seed snapshot failed to load");

  /* An unknown record first, so the known one behind it only decodes if the
   * unknown one was skipped by its length. Each is [type][u16 BE len][body]. */
  records[pos++] = 200;                 /* no such event type in this build */
  records[pos++] = 0;
  records[pos++] = 3;
  records[pos++] = 0xAA;
  records[pos++] = 0xBB;
  records[pos++] = 0xCC;
  records[pos++] = (uint8_t) log_TankSetStock;
  records[pos++] = 0;
  records[pos++] = 5;
  records[pos++] = 2;                   /* player slot */
  records[pos++] = 9;                   /* shells */
  records[pos++] = 8;                   /* mines */
  records[pos++] = 7;                   /* armour */
  records[pos++] = 6;                   /* trees */

  UT_ASSERT_MSG(lv_specRecordPump(false, records, pos) == TRUE,
                "record pump stopped playback");

  lv_tank_stocks_get(2, &shells, &mines, &armour, &trees);
  UT_ASSERT_MSG(shells == 9, "slot 2 shells = %d (want 9)", shells);
  UT_ASSERT_MSG(mines  == 8, "slot 2 mines = %d (want 8)", mines);
  UT_ASSERT_MSG(armour == 7, "slot 2 armour = %d (want 7)", armour);
  UT_ASSERT_MSG(trees  == 6, "slot 2 trees = %d (want 6)", trees);

  /* The record named slot 2 and touched nothing else. */
  lv_tank_stocks_get(0, &shells, &mines, &armour, &trees);
  UT_ASSERT_MSG(shells == 0 && mines == 0 && armour == 0 && trees == 0,
                "slot 0 = %d/%d/%d/%d (want all 0)",
                shells, mines, armour, trees);

  lv_specSeedControlClear();
  lv_decoderDestroy(lv);
  return 0;
}
