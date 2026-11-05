/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer early blast (TEMPORARY TEST TOGGLE)
 *Filename:      lv_early_blast.h
 *Purpose:
 *  A temporary test toggle, "Toggle Screen-True Own Shells",
 *  to see an idea before the game has it: when a player's own
 *  shell ends, that player's client draws the explosion at once
 *  where its own shell stopped, and hides the server's later
 *  explosion of that shell. Remove this file, its calls in
 *  screen.c and the menu item when the test is done.
 *
 *  In the lag view the chosen player's own shells come from the
 *  present frame (lv_shell_owner.h) and every other shell and
 *  explosion from the old frame. With the toggle on:
 *
 *  - An owned shell of the entry before that no shell of this
 *    entry continues has ended. Its early blast starts at this
 *    entry, at the shell's last place, and is drawn at the
 *    present for the explosion's life.
 *  - The server's explosion of that shell is found in the
 *    present frame: a new explosion record (frame 8 or 7) near
 *    where the shell ended, the nearest one not taken. Explosions
 *    do not move, so the lag view then hides the record at that
 *    exact place for that explosion's life in the old frames, at
 *    most one record per frame.
 *  - No match: the early blast is still drawn, and the miss is
 *    counted.
 *
 *  The stop place is the server's: the shell's last record. The
 *  client's own view could stop it earlier (clientShellVisualBlocked
 *  in client_sim.c), which the viewer does not try to find.
 *
 *  Places are in world units as a log record holds them (map
 *  square * 256 + pixel * 16). An explosion's record is at its
 *  world place less TANK_SUBTRACT (128) on each axis
 *  (shells.c, explosionsAddItem), a shell's at its own place.
 *
 *  Header only, with no includes but the C library's and
 *  lv_shell_owner.h, so the viewer and the unit tests use the
 *  same code.
 *********************************************************/

#ifndef LV_EARLY_BLAST_H
#define LV_EARLY_BLAST_H

#include <stdbool.h>
#include <stdint.h>

#include "lv_shell_owner.h"

/* The explosion's frames: it starts at 8 (EXPLOSION_START) and goes down
   one each 3 game ticks of 20 ms (EXPLOAD_UPDATE_TIME). */
#define LV_EARLY_BLAST_START_FRAME 8
#define LV_EARLY_BLAST_FRAME_MS 60
#define LV_EARLY_BLAST_LIFE_MS (LV_EARLY_BLAST_START_FRAME * LV_EARLY_BLAST_FRAME_MS)

/* An explosion record is this far up and left of the world place. */
#define LV_EARLY_BLAST_EXPL_OFFSET 128

/* A server explosion matches when it is this near the expected place: the
   shell's last place, plus half a step along its way. A hit on a pillbox,
   base or terrain is put at the middle of the square. */
#define LV_EARLY_BLAST_MATCH_RADIUS 320

/* How long after the end the server explosion may first be found. */
#define LV_EARLY_BLAST_MATCH_MS 60

/* The hidden explosion's life in the old frames, from the entry it was
   found at: one entry before (it can start one entry before the shell's
   record goes) to its longest life and one frame more. */
#define LV_EARLY_BLAST_HIDE_BEFORE_MS 20
#define LV_EARLY_BLAST_HIDE_MS (LV_EARLY_BLAST_LIFE_MS + LV_EARLY_BLAST_FRAME_MS)

/* The most early blasts held at once. The oldest is used again. */
#define LV_EARLY_BLAST_MAX 64

/* An explosion record at one entry: place and frame (1 to 8). */
typedef struct {
  int32_t wx, wy;
  uint8_t frame;
} LvBlastPt;

typedef struct {
  bool     used;
  int8_t   owner;        /* the player whose shell it was */
  uint32_t endMs;        /* the first entry with no shell */
  int32_t  wx, wy;       /* where the early blast is drawn, as an explosion record */
  int32_t  expectX, expectY; /* where the server explosion is looked for */
  bool     matched;      /* a server explosion was found */
  bool     missed;       /* none was found in the time */
  int32_t  explX, explY; /* the server explosion's record place */
  uint32_t explMs;       /* the entry it was found at */
} LvEarlyBlast;

typedef struct {
  uint32_t ended;    /* owned shells that ended */
  uint32_t matched;  /* of those, a server explosion was found */
  uint32_t missed;   /* of those, none was found */
} LvEarlyBlastStats;

/* A shell record's frame of 1 to 8 is an explosion. */
static inline bool lv_earlyBlastIsExplFrame(uint8_t frame) {
  return frame >= 1 && frame <= 8;
}

/* Whether shell prev of the entry before has no shell of this entry that
   continues it (lv_shellOwnerContinues): it has ended. */
static inline bool lv_earlyBlastShellEnded(const LvShellPt *prev, const LvShellPt *cur, int nCur) {
  int i;
  for (i = 0; i < nCur; i++) {
    if (lv_shellOwnerContinues(prev, &cur[i])) {
      return false;
    }
  }
  return true;
}

/* An early blast for player owner's shell, whose last record is last, at
   the entry endMs. */
static inline void lv_earlyBlastStart(LvEarlyBlast *b, int8_t owner, uint32_t endMs,
                                      const LvShellPt *last) {
  double ux, uy;
  lv_shellOwnerDirVector(last->dir, &ux, &uy);
  b->used = true;
  b->owner = owner;
  b->endMs = endMs;
  b->wx = last->wx - LV_EARLY_BLAST_EXPL_OFFSET;
  b->wy = last->wy - LV_EARLY_BLAST_EXPL_OFFSET;
  b->expectX = b->wx + (int32_t)lround(ux * (LV_SHELL_OWNER_STEP / 2));
  b->expectY = b->wy + (int32_t)lround(uy * (LV_SHELL_OWNER_STEP / 2));
  b->matched = false;
  b->missed = false;
  b->explX = b->explY = 0;
  b->explMs = 0;
}

/* The explosion record of this entry that is b's server explosion: a new
   one (frame 8 or 7), not taken[], the nearest to the expected place and
   within the radius. Returns its index, or -1. taken may be NULL. */
static inline int lv_earlyBlastPick(const LvEarlyBlast *b, const LvBlastPt *c, int n,
                                    const bool *taken) {
  int i, best = -1;
  double bestD2 = (double)LV_EARLY_BLAST_MATCH_RADIUS * LV_EARLY_BLAST_MATCH_RADIUS;
  for (i = 0; i < n; i++) {
    double dx, dy, d2;
    if (c[i].frame < LV_EARLY_BLAST_START_FRAME - 1 || c[i].frame > LV_EARLY_BLAST_START_FRAME) {
      continue;
    }
    if (taken != NULL && taken[i]) {
      continue;
    }
    dx = (double)(c[i].wx - b->expectX);
    dy = (double)(c[i].wy - b->expectY);
    d2 = dx * dx + dy * dy;
    if (d2 <= bestD2) {
      bestD2 = d2;
      best = i;
    }
  }
  return best;
}

/* Note the found server explosion. */
static inline void lv_earlyBlastMatch(LvEarlyBlast *b, const LvBlastPt *e, uint32_t ms) {
  b->matched = true;
  b->explX = e->wx;
  b->explY = e->wy;
  b->explMs = ms;
}

/* Whether b still looks for its server explosion at entry ms; false also
   once the time is past (then the caller counts the miss). */
static inline bool lv_earlyBlastLooking(const LvEarlyBlast *b, uint32_t ms) {
  return b->used && !b->matched && !b->missed && ms >= b->endMs &&
         ms <= b->endMs + LV_EARLY_BLAST_MATCH_MS;
}

/* The frame the early blast is drawn with at the present ms: 8 at its end
   entry, one less each LV_EARLY_BLAST_FRAME_MS, 0 before or after. */
static inline uint8_t lv_earlyBlastFrameAt(const LvEarlyBlast *b, uint32_t nowMs) {
  uint32_t age;
  if (!b->used || nowMs < b->endMs) {
    return 0;
  }
  age = nowMs - b->endMs;
  if (age >= (uint32_t)LV_EARLY_BLAST_LIFE_MS) {
    return 0;
  }
  return (uint8_t)(LV_EARLY_BLAST_START_FRAME - age / LV_EARLY_BLAST_FRAME_MS);
}

/* Whether the old frame at frameMs is in the life of b's server explosion. */
static inline bool lv_earlyBlastHideTime(const LvEarlyBlast *b, uint32_t frameMs) {
  return b->used && b->matched &&
         frameMs + LV_EARLY_BLAST_HIDE_BEFORE_MS >= b->explMs &&
         frameMs < b->explMs + LV_EARLY_BLAST_HIDE_MS;
}

/* Whether explosion record e of the old frame at frameMs is b's server
   explosion, which the lag view hides. */
static inline bool lv_earlyBlastHides(const LvEarlyBlast *b, uint32_t frameMs, const LvBlastPt *e) {
  return lv_earlyBlastIsExplFrame(e->frame) && lv_earlyBlastHideTime(b, frameMs) &&
         e->wx == b->explX && e->wy == b->explY;
}

#endif /* LV_EARLY_BLAST_H */
