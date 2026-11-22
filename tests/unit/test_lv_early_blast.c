/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The Log Viewer's "Toggle Screen-True Local Explosions" option
 * (src/logviewer/lv_early_blast.h): when an own
 * shell ends, its early blast, and the match that finds and hides the
 * server's explosion of that shell in the lag view. It works only while
 * "Toggle Screen-True Own Shells" is on.
 */
#include <stdint.h>
#include <stdlib.h>
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

/* ---- the early blast list over many entries ------------------------------ */

/* One shell of the made-up game below: it flies east for five entries, one
   step each, and its last record is at (endX, endY) at entry endMs - 20.
   The server's explosion record of it is at (explX, explY) from endMs, frame
   8, one less each 60 ms. */
typedef struct {
  int8_t owner;
  int32_t endX, endY;
  int32_t explX, explY;
  uint32_t endMs;
} LebShell;

#define LEB_FLIGHT_ENTRIES 5

static bool lebShellAt(const LebShell *s, uint32_t ms, LvShellPt *out) {
  int32_t back;
  if (ms + 20 > s->endMs || ms + 20 + LEB_FLIGHT_ENTRIES * 20 <= s->endMs) {
    return false;
  }
  back = (int32_t)((s->endMs - 20 - ms) / 20);
  *out = lebPt(s->endX - 32 * back, s->endY, 4);
  return true;
}

static bool lebExplAt(const LebShell *s, uint32_t ms, LvBlastPt *out) {
  uint32_t age;
  if (ms < s->endMs) {
    return false;
  }
  age = ms - s->endMs;
  if (age >= (uint32_t)LV_EARLY_BLAST_LIFE_MS) {
    return false;
  }
  *out = lebExpl(s->explX, s->explY, (uint8_t)(8 - age / 60));
  return true;
}

/* The shells and explosion records of the game at entry ms. owners[i] is
   the owner of pts[i]; explOwner[k] the owner of expl[k]. */
static void lebFrame(const LebShell *sh, int n, uint32_t ms, LvShellPt *pts, int8_t *owners,
                     int *nPts, LvBlastPt *expl, int8_t *explOwner, int *nExpl) {
  int i;
  *nPts = 0;
  *nExpl = 0;
  for (i = 0; i < n; i++) {
    if (lebShellAt(&sh[i], ms, &pts[*nPts])) {
      owners[(*nPts)++] = sh[i].owner;
    }
    if (lebExplAt(&sh[i], ms, &expl[*nExpl])) {
      explOwner[(*nExpl)++] = sh[i].owner;
    }
  }
}

/* Play the made-up game from fromMs to toMs through the early blast list,
   as the viewer does at each entry, and at each entry check the old frames
   behindA and behindB ms back: the lag view for player watch hides each of
   that player's server explosions in them, and no other. Returns 0 when it
   all holds. */
static int lebPlay(const LebShell *sh, int n, uint32_t fromMs, uint32_t toMs, int watch,
                   uint32_t behindA, uint32_t behindB, LvEarlyBlastList *l) {
  static LvShellPt prev[LV_SHELL_OWNER_MAX_SHELLS], cur[LV_SHELL_OWNER_MAX_SHELLS];
  static int8_t prevOwner[LV_SHELL_OWNER_MAX_SHELLS], curOwner[LV_SHELL_OWNER_MAX_SHELLS];
  static LvBlastPt expl[LV_SHELL_OWNER_MAX_SHELLS];
  static int8_t explOwner[LV_SHELL_OWNER_MAX_SHELLS];
  static bool hidden[LV_SHELL_OWNER_MAX_SHELLS];
  uint32_t ms;
  int nPrev = 0, nCur, nExpl, k, pass;

  memset(l, 0, sizeof(*l));
  for (ms = fromMs; ms <= toMs; ms += 20) {
    lebFrame(sh, n, ms, cur, curOwner, &nCur, expl, explOwner, &nExpl);
    (void)lv_earlyBlastListTick(l, ms, prev, prevOwner, nPrev, cur, nCur, expl, nExpl, watch,
                                NULL);
    memcpy(prev, cur, sizeof(cur[0]) * (size_t)nCur);
    memcpy(prevOwner, curOwner, sizeof(curOwner[0]) * (size_t)nCur);
    nPrev = nCur;
    for (pass = 0; pass < 2; pass++) {
      uint32_t behind = pass == 0 ? behindA : behindB;
      uint32_t old;
      LvShellPt tmp[LV_SHELL_OWNER_MAX_SHELLS];
      int8_t tmpOwner[LV_SHELL_OWNER_MAX_SHELLS];
      int nTmp;
      if (ms < fromMs + behind) continue;
      old = ms - behind;
      lebFrame(sh, n, old, tmp, tmpOwner, &nTmp, expl, explOwner, &nExpl);
      lv_earlyBlastHideRecords(l, watch, old, expl, nExpl, hidden);
      for (k = 0; k < nExpl; k++) {
        bool want = ((int)explOwner[k] == watch);
        UT_ASSERT_MSG(hidden[k] == want,
                      "at %u ms the old frame %u ms back %s player %d's explosion at %d,%d "
                      "frame %u", (unsigned)ms, (unsigned)behind,
                      hidden[k] ? "hides" : "shows", (int)explOwner[k], (int)expl[k].wx,
                      (int)expl[k].wy, (unsigned)expl[k].frame);
      }
    }
  }
  return 0;
}

/* One player hits one pillbox every 260 ms (one reload). Every hit's
   explosion is at the middle of the square, the same place each time, so
   it is the frame that tells them apart. Each early blast finds its own
   explosion, and the lag view hides every one, also the second while the
   first is still there. */
int run_lv_early_blast_same_place(void) {
  LebShell sh[6];
  LvEarlyBlastList *l = (LvEarlyBlastList *)calloc(1, sizeof(LvEarlyBlastList));
  int i;

  UT_ASSERT(l != NULL);
  for (i = 0; i < 6; i++) {
    sh[i].owner = 0;
    sh[i].endX = 50 * 256 + 64;           /* the last record, just west of the middle */
    sh[i].endY = 50 * 256 + 128;
    sh[i].explX = 50 * 256 + 128 - 128;   /* the middle, as an explosion record */
    sh[i].explY = 50 * 256 + 128 - 128;
    sh[i].endMs = 1000 + (uint32_t)i * 260;
  }
  if (lebPlay(sh, 6, 800, 3400, 0, 100, 620, l) != 0) {
    free(l);
    return 1;
  }
  UT_ASSERT_MSG(l->stats.ended == 6 && l->stats.matched == 6 && l->stats.missed == 0,
                "ended %u matched %u missed %u, want 6 6 0", (unsigned)l->stats.ended,
                (unsigned)l->stats.matched, (unsigned)l->stats.missed);
  for (i = 0; i < 6; i++) {
    UT_ASSERT_MSG(l->b[i].matched && l->b[i].explMs == sh[i].endMs,
                  "early blast %d matched %d at %u, want %u", i, (int)l->b[i].matched,
                  (unsigned)l->b[i].explMs, (unsigned)sh[i].endMs);
  }
  free(l);
  return 0;
}

/* Sixteen players fire as fast as they can for four seconds, and at one
   entry each has four more shells end at once. The list keeps every early
   blast for as long as the oldest old frame needs it, so the lag view for
   player 0 hides every one of player 0's explosions, with the old frame
   at the whole lag history back. */
int run_lv_early_blast_many_players(void) {
  enum { PLAYERS = 16, PERIODIC = 16, BURST = 4, PER = PERIODIC + BURST };
  static LebShell sh[PLAYERS * PER];
  LvEarlyBlastList *l = (LvEarlyBlastList *)calloc(1, sizeof(LvEarlyBlastList));
  int o, n, count = 0;

  UT_ASSERT(l != NULL);
  for (o = 0; o < PLAYERS; o++) {
    for (n = 0; n < PER; n++) {
      LebShell *s = &sh[count++];
      s->owner = (int8_t)o;
      /* Each shell at its own place, far from every other. */
      s->endX = (10 + o * 14) * 256 + 64;
      s->endY = (10 + n * 9) * 256 + 128;
      s->explX = s->endX - 128 + 32;
      s->explY = s->endY - 128;
      if (n < PERIODIC) {
        s->endMs = 1000 + (uint32_t)o * 20 + (uint32_t)n * 260;
      } else {
        s->endMs = 3000;
      }
    }
  }
  if (lebPlay(sh, count, 800, 6000, 0, 100, 620, l) != 0) {
    free(l);
    return 1;
  }
  UT_ASSERT_MSG(l->stats.ended == (uint32_t)count && l->stats.matched == (uint32_t)count &&
                l->stats.missed == 0, "ended %u matched %u missed %u, want %d each",
                (unsigned)l->stats.ended, (unsigned)l->stats.matched,
                (unsigned)l->stats.missed, count);
  /* The list is big enough for every early blast that can start while one
     is needed: sixteen tanks at one shell a reload. */
  UT_ASSERT(LV_EARLY_BLAST_RELOAD_MS == TANK_RELOAD_TIME * 20);
  UT_ASSERT(LV_EARLY_BLAST_TANKS >= MAX_TANKS);
  UT_ASSERT(LV_EARLY_BLAST_MAX >=
            LV_EARLY_BLAST_TANKS * (LV_EARLY_BLAST_KEEP_MS / LV_EARLY_BLAST_RELOAD_MS + 1));
  free(l);
  return 0;
}

/* A miss is told for the watched player's early blasts only, with the end
   entry; every player's misses are counted. */
int run_lv_early_blast_watch_miss(void) {
  LvEarlyBlastList *l = (LvEarlyBlastList *)calloc(1, sizeof(LvEarlyBlastList));
  LvShellPt prev[2];
  int8_t prevOwner[2] = {0, 1};
  uint32_t missMs = 0, ms;
  int told = 0;

  UT_ASSERT(l != NULL);
  prev[0] = lebPt(40 * 256, 40 * 256, 4);
  prev[1] = lebPt(80 * 256, 80 * 256, 4);
  /* Both shells end at 2000, and no explosion comes. */
  told += lv_earlyBlastListTick(l, 2000, prev, prevOwner, 2, NULL, 0, NULL, 0, 0, &missMs);
  for (ms = 2020; ms <= 2200; ms += 20) {
    told += lv_earlyBlastListTick(l, ms, NULL, NULL, 0, NULL, 0, NULL, 0, 0, &missMs);
  }
  UT_ASSERT_MSG(told == 1 && missMs == 2000, "told %d misses, last at %u, want 1 at 2000",
                told, (unsigned)missMs);
  UT_ASSERT(l->stats.ended == 2 && l->stats.missed == 2);
  free(l);
  return 0;
}
