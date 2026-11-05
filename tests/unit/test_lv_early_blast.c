/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The Log Viewer's temporary "Toggle Screen-True Local Explosions
 * (Proposed Fix)" test toggle (src/logviewer/lv_early_blast.h): when an own
 * shell ends, its early blast, and the match that finds and hides the
 * server's explosion of that shell in the lag view. It works only while
 * "Toggle Screen-True Own Shells" is on.
 */
#include <stdint.h>
#include <string.h>

#include "global.h"
#include "tank.h"        /* TANK_SUBTRACT */
#include "explosions.h"  /* EXPLOSION_START, EXPLOAD_UPDATE_TIME, EXPLODE_DEATH */
#include "lv_early_blast.h"
#include "test_harness.h"

static LvShellPt lebPt(int32_t wx, int32_t wy, uint8_t dir) {
  LvShellPt p;
  p.wx = wx;
  p.wy = wy;
  p.dir = dir;
  return p;
}

static LvBlastPt lebExpl(int32_t wx, int32_t wy, uint8_t frame) {
  LvBlastPt e;
  e.wx = wx;
  e.wy = wy;
  e.frame = frame;
  return e;
}

/* The early blast is drawn only with both lag view toggles on: Toggle
   Screen-True Local Explosions needs Toggle Screen-True Own Shells. */
int run_lv_early_blast_needs_own_shells(void) {
  UT_ASSERT(lv_earlyBlastOn(true, true));
  UT_ASSERT(!lv_earlyBlastOn(true, false));
  UT_ASSERT(!lv_earlyBlastOn(false, true));
  UT_ASSERT(!lv_earlyBlastOn(false, false));
  return 0;
}

/* The header's numbers are the game's explosion rules. */
int run_lv_early_blast_rules(void) {
  UT_ASSERT(LV_EARLY_BLAST_START_FRAME == EXPLOSION_START);
  UT_ASSERT(LV_EARLY_BLAST_FRAME_MS == EXPLOAD_UPDATE_TIME * 20);
  UT_ASSERT(EXPLODE_DEATH == 1);
  UT_ASSERT(LV_EARLY_BLAST_EXPL_OFFSET == TANK_SUBTRACT);
  UT_ASSERT(lv_earlyBlastIsExplFrame(1) && lv_earlyBlastIsExplFrame(8));
  UT_ASSERT(!lv_earlyBlastIsExplFrame(0) && !lv_earlyBlastIsExplFrame(9));
  return 0;
}

/* A shell that moves on has not ended; one with nothing after it has. */
int run_lv_early_blast_ended(void) {
  LvShellPt prev = lebPt(100 * 256, 100 * 256, 4); /* east */
  LvShellPt moved = lebPt(100 * 256 + 32, 100 * 256, 4);
  LvShellPt still = lebPt(100 * 256, 100 * 256, 4);  /* a dead shell's last record */
  LvShellPt other = lebPt(90 * 256, 100 * 256, 4);
  LvShellPt both[2];
  UT_ASSERT(!lv_earlyBlastShellEnded(&prev, &moved, 1));
  UT_ASSERT(lv_earlyBlastShellEnded(&prev, &still, 1));
  UT_ASSERT(lv_earlyBlastShellEnded(&prev, &other, 1));
  UT_ASSERT(lv_earlyBlastShellEnded(&prev, NULL, 0));
  both[0] = other;
  both[1] = moved;
  UT_ASSERT(!lv_earlyBlastShellEnded(&prev, both, 2));
  return 0;
}

/* The early blast is drawn from its end entry for the explosion's life,
   8 down to 1, at the shell's last place as an explosion record. */
int run_lv_early_blast_frames(void) {
  LvEarlyBlast b;
  LvShellPt last = lebPt(100 * 256 + 64, 50 * 256 + 128, 8); /* south */
  memset(&b, 0, sizeof(b));
  UT_ASSERT(lv_earlyBlastFrameAt(&b, 1000) == 0); /* not used */
  lv_earlyBlastStart(&b, 3, 1000, &last);
  UT_ASSERT(b.owner == 3 && b.endMs == 1000);
  UT_ASSERT(b.wx == last.wx - TANK_SUBTRACT && b.wy == last.wy - TANK_SUBTRACT);
  /* Looked for half a step on, south. */
  UT_ASSERT(b.expectX == b.wx && b.expectY == b.wy + 16);
  UT_ASSERT(lv_earlyBlastFrameAt(&b, 980) == 0);
  UT_ASSERT(lv_earlyBlastFrameAt(&b, 1000) == 8);
  UT_ASSERT(lv_earlyBlastFrameAt(&b, 1040) == 8);
  UT_ASSERT(lv_earlyBlastFrameAt(&b, 1060) == 7);
  UT_ASSERT(lv_earlyBlastFrameAt(&b, 1000 + 7 * 60) == 1);
  UT_ASSERT(lv_earlyBlastFrameAt(&b, 1000 + 7 * 60 + 40) == 1);
  UT_ASSERT(lv_earlyBlastFrameAt(&b, 1000 + 8 * 60) == 0);
  return 0;
}

/* The server explosion is the nearest new one in the radius that is not
   taken; an old one, a far one or a taken one is not matched. */
int run_lv_early_blast_match(void) {
  LvEarlyBlast b;
  LvShellPt last = lebPt(100 * 256, 100 * 256, 4); /* east */
  LvBlastPt c[6];
  bool taken[6];
  int32_t ex, ey;

  memset(&b, 0, sizeof(b));
  lv_earlyBlastStart(&b, 1, 2000, &last);
  ex = b.expectX;
  ey = b.expectY;
  UT_ASSERT(ex == last.wx - 128 + 16 && ey == last.wy - 128);

  c[0] = lebExpl(ex + 200, ey, 8);       /* in the radius, farther */
  c[1] = lebExpl(ex + 16, ey + 16, 8);   /* the nearest */
  c[2] = lebExpl(ex, ey, 5);             /* an old explosion, on the place */
  c[3] = lebExpl(ex + 400, ey, 8);       /* out of the radius */
  c[4] = lebExpl(ex - 32, ey, 7);        /* new, a bit farther than c[1] */
  c[5] = lebExpl(ex + 1000, ey + 1000, 8);
  UT_ASSERT(lv_earlyBlastPick(&b, c, 6, NULL) == 1);
  memset(taken, 0, sizeof(taken));
  taken[1] = true;
  UT_ASSERT(lv_earlyBlastPick(&b, c, 6, taken) == 4);
  taken[4] = true;
  UT_ASSERT(lv_earlyBlastPick(&b, c, 6, taken) == 0);
  taken[0] = true;
  UT_ASSERT(lv_earlyBlastPick(&b, c, 6, taken) == -1);
  UT_ASSERT(lv_earlyBlastPick(&b, &c[3], 1, NULL) == -1);
  UT_ASSERT(lv_earlyBlastPick(&b, NULL, 0, NULL) == -1);

  /* Looked for from the end entry for LV_EARLY_BLAST_MATCH_MS. */
  UT_ASSERT(!lv_earlyBlastLooking(&b, 1980));
  UT_ASSERT(lv_earlyBlastLooking(&b, 2000));
  UT_ASSERT(lv_earlyBlastLooking(&b, 2060));
  UT_ASSERT(!lv_earlyBlastLooking(&b, 2080));
  lv_earlyBlastMatch(&b, &c[1], 2020);
  UT_ASSERT(b.matched && b.explX == c[1].wx && b.explY == c[1].wy && b.explMs == 2020);
  UT_ASSERT(!lv_earlyBlastLooking(&b, 2040));
  return 0;
}

/* The lag view hides the matched explosion at its exact place for its life
   in the old frames, and nothing else. */
int run_lv_early_blast_hide(void) {
  LvEarlyBlast b, unmatched;
  LvShellPt last = lebPt(60 * 256, 70 * 256, 0); /* north */
  LvBlastPt e, moved, shell;
  uint32_t at = 5000;

  memset(&b, 0, sizeof(b));
  lv_earlyBlastStart(&b, 2, at, &last);
  unmatched = b;
  e = lebExpl(b.expectX, b.expectY - 16, 8);
  lv_earlyBlastMatch(&b, &e, at);

  /* The old frames run from about the end entry to its whole life. */
  UT_ASSERT(!lv_earlyBlastHides(&b, at - 40, &e));
  UT_ASSERT(lv_earlyBlastHides(&b, at - 20, &e));
  UT_ASSERT(lv_earlyBlastHides(&b, at, &e));
  e.frame = 1;
  UT_ASSERT(lv_earlyBlastHides(&b, at + LV_EARLY_BLAST_LIFE_MS, &e));
  UT_ASSERT(lv_earlyBlastHides(&b, at + LV_EARLY_BLAST_HIDE_MS - 20, &e));
  UT_ASSERT(!lv_earlyBlastHides(&b, at + LV_EARLY_BLAST_HIDE_MS, &e));

  /* Another place, even one pixel off, is another explosion. */
  moved = e;
  moved.wx += 16;
  UT_ASSERT(!lv_earlyBlastHides(&b, at + 100, &moved));
  /* A shell record at the place is not an explosion. */
  shell = e;
  shell.frame = 12;
  UT_ASSERT(!lv_earlyBlastHides(&b, at + 100, &shell));
  /* With no match nothing is hidden. */
  UT_ASSERT(!lv_earlyBlastHides(&unmatched, at + 100, &e));
  return 0;
}
