/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The Log Viewer's shell owner matcher (src/logviewer/lv_shell_owner.h),
 * which the lag view uses to draw the chosen player's own shells at the
 * present. The places a shell has are made with the game's own shell code
 * (shellSpawnPos, shellAdvance1Tick), then cut to the 16 world units a
 * log_Shell record holds, so the windows are checked against what the
 * server writes.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "global.h"
#include "shells.h"   /* shellSpawnPos, shellAdvance1Tick, SHELL_SPEED, SHELL_START_ADD */
#include "util.h"     /* utilGetDir, utilCalcDistanceHP */
#include "lv_shell_owner.h"
#include "test_harness.h"

/* A world place as a log record holds it: the map square and the pixel
   nibble, so the low 4 bits are gone. */
static int32_t sooCut(WORLD w) {
  return (int32_t)(((unsigned)w >> 8) * 256u + (((unsigned)w & 255u) >> 4) * 16u);
}

static LvShellPt sooPt(WORLD x, WORLD y, TURNTYPE angle) {
  LvShellPt p;
  p.wx = sooCut(x);
  p.wy = sooCut(y);
  p.dir = utilGetDir(angle);
  return p;
}

static LvGunTank sooTank(WORLD x, WORLD y, TURNTYPE angle, bool fired) {
  LvGunTank t;
  t.on = true;
  t.wx = sooCut(x);
  t.wy = sooCut(y);
  t.dir = utilGetDir(angle);
  t.fired = fired;
  return t;
}

/* The default rules in the header are the game's. */
int run_lv_shell_owner_rules(void) {
  UT_ASSERT(LV_SHELL_OWNER_STEP == SHELL_SPEED);
  UT_ASSERT(LV_SHELL_OWNER_START_STEPS == SHELL_START_ADD);
  UT_ASSERT(lv_shellOwnerIsShellFrame(9) && lv_shellOwnerIsShellFrame(24));
  UT_ASSERT(!lv_shellOwnerIsShellFrame(8) && !lv_shellOwnerIsShellFrame(1) &&
            !lv_shellOwnerIsShellFrame(25));
  /* The unit vector table is the cos and sin the matcher used to call. */
  {
    uint8_t d;
    for (d = 0; d < 32; d++) {
      double ux, uy;
      double a = ((double)(d & 15) * 16.0 - 64.0) * (6.283185307179586 / 256.0);
      lv_shellOwnerDirVector(d, &ux, &uy);
      UT_ASSERT(fabs(ux - cos(a)) <= 1e-15 && fabs(uy - sin(a)) <= 1e-15);
    }
  }
  return 0;
}

/* A tank's shell, fired at every angle in half steps, from two places:
   its first record (0 or 1 moves after the start) fits the gun, and each
   record continues the one before for its whole flight. */
int run_lv_shell_owner_real_flight(void) {
  int half;
  for (half = 0; half < 512; half++) {
    TURNTYPE angle = (TURNTYPE)half / 2.0f;
    int place;
    for (place = 0; place < 2; place++) {
      WORLD tx = (WORLD)(place == 0 ? 100 * 256 + 37 : 60 * 256 + 250);
      WORLD ty = (WORLD)(place == 0 ? 90 * 256 + 201 : 140 * 256 + 3);
      WORLD sx, sy;
      int32_t xStep, yStep, xAcc = 0, yAcc = 0;
      int moves, k;
      LvShellPt last;

      shellSpawnPos(tx, ty, angle, SHELL_SPEED, SHELL_START_ADD, &sx, &sy);
      utilCalcDistanceHP(&xStep, &yStep, angle, SHELL_SPEED);
      for (moves = 0; moves <= 1; moves++) {
        WORLD fx = sx, fy = sy;
        int32_t ax = 0, ay = 0;
        LvShellPt s;
        LvGunTank t;
        if (moves == 1) {
          shellAdvance1Tick(&fx, &fy, &ax, &ay, xStep, yStep);
        }
        s = sooPt(fx, fy, angle);
        t = sooTank(tx, ty, angle, true);
        UT_ASSERT_MSG(lv_shellOwnerFitsGun(&s, &t),
                      "angle %.1f place %d moves %d: the gun does not fit", (double)angle,
                      place, moves);
        /* The tank turned a little on the fire tick. */
        t = sooTank(tx, ty, angle + 7.0f, true);
        UT_ASSERT_MSG(lv_shellOwnerFitsGun(&s, &t),
                      "angle %.1f: a tank turned 7 does not fit", (double)angle);
        /* The tank moved 16 world units on, after it fired. */
        t = sooTank((WORLD)(tx + 16), (WORLD)(ty - 16), angle, true);
        UT_ASSERT_MSG(lv_shellOwnerFitsGun(&s, &t),
                      "angle %.1f: a tank that moved does not fit", (double)angle);
        /* A tank that points the other way does not. */
        t = sooTank(tx, ty, angle + 128.0f >= 256.0f ? angle - 128.0f : angle + 128.0f, true);
        UT_ASSERT_MSG(!lv_shellOwnerFitsGun(&s, &t),
                      "angle %.1f: a tank pointing back fits", (double)angle);
      }

      last = sooPt(sx, sy, angle);
      for (k = 0; k < 60; k++) {
        LvShellPt now;
        shellAdvance1Tick(&sx, &sy, &xAcc, &yAcc, xStep, yStep);
        now = sooPt(sx, sy, angle);
        UT_ASSERT_MSG(lv_shellOwnerContinues(&last, &now),
                      "angle %.1f move %d: (%d,%d) -> (%d,%d) does not continue",
                      (double)angle, k, (int)last.wx, (int)last.wy, (int)now.wx, (int)now.wy);
        last = now;
      }
    }
  }
  return 0;
}

/* Who owns a new shell. */
int run_lv_shell_owner_start(void) {
  WORLD tx = 100 * 256 + 128, ty = 100 * 256 + 128;
  WORLD sx, sy;
  LvShellPt s;
  LvGunTank tanks[3];
  int fits = -1;

  /* East, angle 64. */
  shellSpawnPos(tx, ty, 64.0f, SHELL_SPEED, SHELL_START_ADD, &sx, &sy);
  s = sooPt(sx, sy, 64.0f);
  memset(tanks, 0, sizeof(tanks));
  tanks[1] = sooTank(tx, ty, 64.0f, true);

  UT_ASSERT(lv_shellOwnerStartMatch(&s, tanks, 3, true, &fits) == 1 && fits == 1);

  /* The tank's stock did not go down: with stocks recorded that is no
     owner (a pillbox's shell from near the tank); with none it is. */
  tanks[1].fired = false;
  UT_ASSERT(lv_shellOwnerStartMatch(&s, tanks, 3, true, &fits) == LV_SHELL_OWNER_NONE &&
            fits == 0);
  UT_ASSERT(lv_shellOwnerStartMatch(&s, tanks, 3, false, &fits) == 1);
  tanks[1].fired = true;

  /* Two tanks close together, the same way, both firing: not clear. */
  tanks[2] = sooTank(tx, (WORLD)(ty + 24), 64.0f, true);
  UT_ASSERT(lv_shellOwnerStartMatch(&s, tanks, 3, true, &fits) == LV_SHELL_OWNER_NONE &&
            fits == 2);
  /* The second one did not fire: the first is the owner. */
  tanks[2].fired = false;
  UT_ASSERT(lv_shellOwnerStartMatch(&s, tanks, 3, true, &fits) == 1 && fits == 1);

  /* A tank one map square to the side does not fit. */
  tanks[1].wy += 256;
  UT_ASSERT(lv_shellOwnerStartMatch(&s, tanks, 3, true, &fits) == LV_SHELL_OWNER_NONE);
  /* Nor does one that is far back on the line. */
  tanks[1].wy -= 256;
  tanks[1].wx -= 5 * 256;
  UT_ASSERT(lv_shellOwnerStartMatch(&s, tanks, 3, true, &fits) == LV_SHELL_OWNER_NONE);
  return 0;
}

/* One entry's shells given owners from the entry before. */
int run_lv_shell_owner_link(void) {
  LvShellPt prev[4], cur[4];
  int8_t prevOwner[4], out[4];
  LvGunTank tanks[LV_SHELL_OWNER_MAX_TANKS];
  LvShellOwnerStats st;

  memset(tanks, 0, sizeof(tanks));
  memset(&st, 0, sizeof(st));

  /* Two shells crossing at one place, east and south: each keeps its own. */
  prev[0].wx = 1000; prev[0].wy = 2000; prev[0].dir = 4;  prevOwner[0] = 3;
  prev[1].wx = 1032; prev[1].wy = 1968; prev[1].dir = 8;  prevOwner[1] = 5;
  cur[0].wx = 1032;  cur[0].wy = 2000;  cur[0].dir = 4;
  cur[1].wx = 1032;  cur[1].wy = 2000;  cur[1].dir = 8;
  lv_shellOwnerLink(prev, prevOwner, 2, true, cur, 2, tanks, LV_SHELL_OWNER_MAX_TANKS, true,
                    out, &st);
  UT_ASSERT_MSG(out[0] == 3 && out[1] == 5, "crossing shells got %d and %d", out[0], out[1]);
  UT_ASSERT(st.continued == 2 && st.started == 0);

  /* Two shells side by side the same way, 16 apart: either could continue
     either, and each goes on as the one nearest its own next place. */
  memset(&st, 0, sizeof(st));
  prev[0].wx = 1000; prev[0].wy = 2000; prev[0].dir = 4; prevOwner[0] = 3;
  prev[1].wx = 1000; prev[1].wy = 2016; prev[1].dir = 4; prevOwner[1] = 5;
  cur[0].wx = 1032;  cur[0].wy = 2000;  cur[0].dir = 4;
  cur[1].wx = 1032;  cur[1].wy = 2016;  cur[1].dir = 4;
  lv_shellOwnerLink(prev, prevOwner, 2, true, cur, 2, tanks, LV_SHELL_OWNER_MAX_TANKS, true,
                    out, &st);
  UT_ASSERT_MSG(out[0] == 3 && out[1] == 5, "side by side shells got %d and %d", out[0],
                out[1]);
  UT_ASSERT(st.continued == 2 && st.continuedLost == 0);
  /* The two go on as one shell between them: their owners differ, so it
     has none. */
  memset(&st, 0, sizeof(st));
  cur[0].wx = 1032;  cur[0].wy = 2008;  cur[0].dir = 4;
  lv_shellOwnerLink(prev, prevOwner, 2, true, cur, 1, tanks, LV_SHELL_OWNER_MAX_TANKS, true,
                    out, &st);
  UT_ASSERT_MSG(out[0] == LV_SHELL_OWNER_NONE, "the one shell got %d", out[0]);
  UT_ASSERT(st.continuedLost == 1);
  /* The same owner for both: kept. */
  memset(&st, 0, sizeof(st));
  prevOwner[1] = 3;
  lv_shellOwnerLink(prev, prevOwner, 2, true, cur, 1, tanks, LV_SHELL_OWNER_MAX_TANKS, true,
                    out, &st);
  UT_ASSERT(out[0] == 3 && st.continuedShared == 1);
  cur[0].wy = 2000;

  /* Two shells far apart the same way: one to one. */
  memset(&st, 0, sizeof(st));
  prev[1].wx = 1000 - 13 * 32; prevOwner[1] = 5;
  prev[1].wy = 2000;
  cur[1].wx = prev[1].wx + 32; cur[1].wy = 2000;
  lv_shellOwnerLink(prev, prevOwner, 2, true, cur, 2, tanks, LV_SHELL_OWNER_MAX_TANKS, true,
                    out, &st);
  UT_ASSERT(out[0] == 3 && out[1] == 5 && st.continued == 2);

  /* A new shell from tank 2's gun, and one from nowhere. */
  memset(&st, 0, sizeof(st));
  tanks[2].on = true; tanks[2].wx = 5000; tanks[2].wy = 5000; tanks[2].dir = 4;
  tanks[2].fired = true;
  cur[0].wx = 5000 + 6 * 32; cur[0].wy = 5000; cur[0].dir = 4;
  cur[1].wx = 9000;          cur[1].wy = 9000; cur[1].dir = 4;
  lv_shellOwnerLink(prev, prevOwner, 0, true, cur, 2, tanks, LV_SHELL_OWNER_MAX_TANKS, true,
                    out, &st);
  UT_ASSERT(out[0] == 2 && out[1] == LV_SHELL_OWNER_NONE);
  UT_ASSERT(st.started == 2 && st.startOwned == 1 && st.startNoTank == 1);

  /* Two new shells both fit tank 2 (a pillbox's shell next to it): a tank
     fires one shell at a time, so neither is clear. */
  memset(&st, 0, sizeof(st));
  cur[1].wx = 5000 + 5 * 32; cur[1].wy = 5000 + 16; cur[1].dir = 4;
  lv_shellOwnerLink(prev, prevOwner, 0, true, cur, 2, tanks, LV_SHELL_OWNER_MAX_TANKS, true,
                    out, &st);
  UT_ASSERT(out[0] == LV_SHELL_OWNER_NONE && out[1] == LV_SHELL_OWNER_NONE);
  UT_ASSERT(st.startSharedTank == 2 && st.startOwned == 0);

  /* No entry before (the first after a seek): nothing is given an owner,
     even a shell at a gun. */
  memset(&st, 0, sizeof(st));
  lv_shellOwnerLink(prev, prevOwner, 0, false, cur, 1, tanks, LV_SHELL_OWNER_MAX_TANKS, true,
                    out, &st);
  UT_ASSERT(out[0] == LV_SHELL_OWNER_NONE && st.notTracked == 1 && st.started == 0);
  return 0;
}

/* Tank 6 fires east along the line of tank 3's shell, and its first record
   is just ahead of where tank 3's shell moves to: both shells now continue
   tank 3's shell of the entry before. The one nearest that shell's next
   place keeps tank 3; the other is new, and its owner comes from the guns. */
int run_lv_shell_owner_one_line(void) {
  LvShellPt prev[1], cur[2];
  int8_t prevOwner[1] = {3}, out[2];
  LvGunTank tanks[LV_SHELL_OWNER_MAX_TANKS];
  LvShellOwnerStats st;

  memset(tanks, 0, sizeof(tanks));
  memset(&st, 0, sizeof(st));
  prev[0].wx = 1000; prev[0].wy = 2000; prev[0].dir = 4;
  /* B's shell first, so the nearest, not the first, is what counts. */
  cur[0].wx = 1050;  cur[0].wy = 2000;  cur[0].dir = 4;
  cur[1].wx = 1032;  cur[1].wy = 2000;  cur[1].dir = 4;
  UT_ASSERT(lv_shellOwnerContinues(&prev[0], &cur[0]) &&
            lv_shellOwnerContinues(&prev[0], &cur[1]));
  tanks[6].on = true; tanks[6].wx = 1050 - 6 * 32; tanks[6].wy = 2000; tanks[6].dir = 4;
  tanks[6].fired = true;
  lv_shellOwnerLink(prev, prevOwner, 1, true, cur, 2, tanks, LV_SHELL_OWNER_MAX_TANKS, true,
                    out, &st);
  UT_ASSERT_MSG(out[1] == 3 && out[0] == 6, "the shells got %d (want 3) and %d (want 6)",
                out[1], out[0]);
  UT_ASSERT(st.continued == 1 && st.started == 1 && st.startOwned == 1);

  /* With no tank that fired there, the new one has no owner: it is never
     given tank 3's. */
  memset(&st, 0, sizeof(st));
  tanks[6].fired = false;
  lv_shellOwnerLink(prev, prevOwner, 1, true, cur, 2, tanks, LV_SHELL_OWNER_MAX_TANKS, true,
                    out, &st);
  UT_ASSERT(out[1] == 3 && out[0] == LV_SHELL_OWNER_NONE);
  UT_ASSERT(st.startNoTank == 1);
  return 0;
}

/* Shell ids: a shell keeps its id while it flies, a new shell takes the
   next one, and two shells that go on as one give the events that keep a
   shell's owner the same in every entry. */
int run_lv_shell_owner_ids(void) {
  LvShellPt prev[3], cur[3];
  int8_t prevOwner[3], out[3];
  uint16_t prevId[3], outId[3], next = 100;
  LvGunTank tanks[LV_SHELL_OWNER_MAX_TANKS];
  LvShellOwnerIdEvents ev;
  LvShellOwnerStats st;

  memset(tanks, 0, sizeof(tanks));
  memset(&st, 0, sizeof(st));

  /* No entry before: each shell a new id. */
  cur[0].wx = 1000; cur[0].wy = 2000; cur[0].dir = 4;
  cur[1].wx = 1000; cur[1].wy = 2016; cur[1].dir = 4;
  lv_shellOwnerLinkIds(prev, prevOwner, prevId, 0, false, cur, 2, tanks, 0, true, out, outId,
                       &next, &ev, &st);
  UT_ASSERT(outId[0] == 100 && outId[1] == 101 && next == 102);
  UT_ASSERT(ev.nMerge == 0 && ev.nLost == 0);

  /* One entry on: each keeps its id, and a new shell far away takes 102. */
  prev[0] = cur[0]; prev[1] = cur[1];
  prevId[0] = 100; prevId[1] = 101;
  prevOwner[0] = 3; prevOwner[1] = LV_SHELL_OWNER_NONE;
  cur[0].wx = 1032;
  cur[1].wx = 1032;
  cur[2].wx = 9000; cur[2].wy = 9000; cur[2].dir = 0;
  lv_shellOwnerLinkIds(prev, prevOwner, prevId, 2, true, cur, 3, tanks, 0, true, out, outId,
                       &next, &ev, &st);
  UT_ASSERT_MSG(outId[0] == 100 && outId[1] == 101 && outId[2] == 102 && next == 103,
                "ids %u %u %u next %u", outId[0], outId[1], outId[2], next);
  UT_ASSERT(out[0] == 3 && out[1] == LV_SHELL_OWNER_NONE);
  UT_ASSERT(ev.nMerge == 0 && ev.nLost == 0);

  /* Tank 3's shell and the pillbox's go on as one: it has no owner, and
     tank 3's shell loses its owner in the entries before too (its id is
     lost). The pillbox's had none to lose. */
  prev[0] = cur[0]; prev[1] = cur[1];
  prevId[0] = 100; prevId[1] = 101;
  cur[0].wx = 1064; cur[0].wy = 2016; cur[0].dir = 4;
  lv_shellOwnerLinkIds(prev, prevOwner, prevId, 2, true, cur, 1, tanks, 0, true, out, outId,
                       &next, &ev, &st);
  UT_ASSERT(out[0] == LV_SHELL_OWNER_NONE && outId[0] == 100 && next == 103);
  UT_ASSERT_MSG(ev.nLost == 1 && ev.lost[0] == 100 && ev.nMerge == 0,
                "lost %d (first %u), merged %d", ev.nLost, ev.lost[0], ev.nMerge);
  UT_ASSERT(st.continuedLost == 1);

  /* Two shells of tank 3 go on as one: it keeps tank 3, and the second id
     is merged into the first. */
  prevOwner[1] = 3;
  lv_shellOwnerLinkIds(prev, prevOwner, prevId, 2, true, cur, 1, tanks, 0, true, out, outId,
                       &next, &ev, &st);
  UT_ASSERT(out[0] == 3 && outId[0] == 100);
  UT_ASSERT(ev.nLost == 0 && ev.nMerge == 1 && ev.mergeFrom[0] == 101 && ev.mergeTo[0] == 100);
  return 0;
}
