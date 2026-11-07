/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer shell owner
 *Filename:      lv_shell_owner.h
 *Purpose:
 *  Which player fired each shell. With the Options menu item
 *  "Toggle Screen-True Own Shells" on (the default), the lag
 *  view draws the chosen player's own shells from the present
 *  frame, as that player's own screen drew them at once, and
 *  every other shell from the old frame. With it off, every
 *  shell comes from the old frame. The log's shell record
 *  (log_Shell) has no owner, so the lag view finds it from
 *  what the recording does hold:
 *
 *  - The server writes each shell once at every 20 ms entry
 *    (server_sim_tick.c, serverSimLogTick on game ticks), with
 *    its place to 16 world units and its frame, which is
 *    utilGetDir(angle) + 9: one of 16 directions.
 *  - A tank's shell starts at the tank's place plus
 *    SHELL_START_ADD (5) steps of SHELL_SPEED (32 world units)
 *    along the tank's angle (shells.c shellsAddItem,
 *    shellApplyStartOffset). shellsUpdate moves it one step in
 *    the same game tick, so its first record is 5 or 6 steps out.
 *    A tank on a boat starts its shell the same way.
 *  - The fire takes one shell off the tank's stock, and the
 *    log_TankSetStock record of that entry says so.
 *
 *  A shell that does not continue a shell of the entry before
 *  is new. Its owner is the one tank that fired at this entry
 *  (or the one before), points the same way (one direction
 *  either side, for a tank that turned on the fire tick), and
 *  stands on the shell's line at the start distance. No such
 *  tank, or more than one, is no owner: a pillbox's shell, or
 *  a match that is not clear.
 *
 *  A shell that continues one of the entry before keeps that
 *  shell's owner: same frame, moved about one step along its
 *  direction. When two shells could be the one before, the
 *  owner is kept only when they have the same owner.
 *
 *  The windows are for the default shell rules. A mod with
 *  other shell speeds finds no match, and its shells have no
 *  owner, which is what the viewer had before.
 *
 *  Header only, with no includes but the C library's, so the
 *  viewer and the unit tests use the same code.
 *********************************************************/

#ifndef LV_SHELL_OWNER_H
#define LV_SHELL_OWNER_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* No player owns the shell, or the viewer cannot tell which one does. */
#define LV_SHELL_OWNER_NONE (-1)

/* The most shells and tanks one call looks at. */
#define LV_SHELL_OWNER_MAX_SHELLS 256
#define LV_SHELL_OWNER_MAX_TANKS 16

/* The default shell rules (shells.h SHELL_SPEED, SHELL_START_ADD). */
#define LV_SHELL_OWNER_STEP 32
#define LV_SHELL_OWNER_START_STEPS 5

/* One entry's move: along the direction, from STEP - 24 to STEP + 24, and
   at most 28 across it. A place is cut to 16 world units, so a move read
   from two places is up to 15 off on each axis, and the real angle is up
   to 8 of 256 off the direction's middle (6 world units across a step). */
#define LV_SHELL_OWNER_MOVE_SLACK 24
#define LV_SHELL_OWNER_MOVE_ACROSS 28

/* The first record from the tank's place: from 5 steps less 48 to 8 steps
   plus 32 along, and across at most 32 plus a quarter of the way along
   (the angle's 8 of 256 is a fifth). The slack is for the cut places and
   for the tank's own move on the fire tick. */
#define LV_SHELL_OWNER_START_MIN \
  (LV_SHELL_OWNER_START_STEPS * LV_SHELL_OWNER_STEP - 48)
#define LV_SHELL_OWNER_START_MAX \
  ((LV_SHELL_OWNER_START_STEPS + 3) * LV_SHELL_OWNER_STEP + 32)
#define LV_SHELL_OWNER_START_ACROSS 32

/* A shell at one entry: its place in world units (map square * 256 +
   pixel * 16) and its direction, 0 to 15. */
typedef struct {
  int32_t wx, wy;
  uint8_t dir;
} LvShellPt;

/* A tank at the same entry. fired is a drop in its shell stock at this
   entry or the one before. */
typedef struct {
  bool    on;
  int32_t wx, wy;
  uint8_t dir;
  bool    fired;
} LvGunTank;

/* Counts of what the matcher did, for the tests and the recording check. */
typedef struct {
  uint32_t started;         /* new shells */
  uint32_t startOwned;      /* new shells given an owner */
  uint32_t startNoTank;     /* new shells with no tank that fits (pillbox shells too) */
  uint32_t startTwoTanks;   /* new shells two or more tanks fit */
  uint32_t startSharedTank; /* new shells whose one tank fits another new shell too */
  uint32_t continued;       /* shells that continue one before, one to one */
  uint32_t continuedShared; /* shells with more than one shell before, owner kept */
  uint32_t continuedLost;   /* shells with more than one shell before, owner not clear */
  uint32_t notTracked;      /* shells of an entry with no entry before it */
} LvShellOwnerStats;

/* A shell record's frame is 9 to 24; 1 to 8 is an explosion. */
static inline bool lv_shellOwnerIsShellFrame(uint8_t frame) {
  return frame >= 9 && frame <= 24;
}

/* The unit vector of each direction (0 north, then clockwise, 16 to the
   turn), the way utilCalcDistance turns an angle into a move. Each entry
   is cos and sin of (dir * 16 - 64) * (2 pi / 256), as the C library gives
   them, so the matcher does no trig call per test. */
static const double lv_shellOwnerUnit[16][2] = {
  {6.123233995736766e-17, -1.0}, /* 0 */
  {0.38268343236508984, -0.9238795325112867}, /* 1 */
  {0.7071067811865476, -0.7071067811865476}, /* 2 */
  {0.9238795325112867, -0.3826834323650898}, /* 3 */
  {1.0, 0.0}, /* 4 */
  {0.9238795325112867, 0.3826834323650898}, /* 5 */
  {0.7071067811865476, 0.7071067811865476}, /* 6 */
  {0.38268343236508984, 0.9238795325112867}, /* 7 */
  {6.123233995736766e-17, 1.0}, /* 8 */
  {-0.3826834323650897, 0.9238795325112867}, /* 9 */
  {-0.7071067811865475, 0.7071067811865476}, /* 10 */
  {-0.9238795325112867, 0.3826834323650899}, /* 11 */
  {-1.0, 1.2246467991473532e-16}, /* 12 */
  {-0.9238795325112868, -0.38268343236508967}, /* 13 */
  {-0.7071067811865477, -0.7071067811865475}, /* 14 */
  {-0.38268343236509034, -0.9238795325112865}, /* 15 */
};

/* The unit vector of direction dir. */
static inline void lv_shellOwnerDirVector(uint8_t dir, double *ux, double *uy) {
  *ux = lv_shellOwnerUnit[dir & 15][0];
  *uy = lv_shellOwnerUnit[dir & 15][1];
}

/* How far (dx, dy) goes along direction dir, and how far across it. */
static inline void lv_shellOwnerSplit(int32_t dx, int32_t dy, uint8_t dir,
                                      double *along, double *across) {
  double ux, uy;
  lv_shellOwnerDirVector(dir, &ux, &uy);
  *along = (double)dx * ux + (double)dy * uy;
  *across = fabs((double)dx * uy - (double)dy * ux);
}

/* Whether cur is prev one entry later: the same direction, moved about one
   step along it. */
static inline bool lv_shellOwnerContinues(const LvShellPt *prev, const LvShellPt *cur) {
  double along, across;
  if (prev->dir != cur->dir) {
    return false;
  }
  lv_shellOwnerSplit(cur->wx - prev->wx, cur->wy - prev->wy, cur->dir, &along, &across);
  return along >= (double)(LV_SHELL_OWNER_STEP - LV_SHELL_OWNER_MOVE_SLACK) &&
         along <= (double)(LV_SHELL_OWNER_STEP + LV_SHELL_OWNER_MOVE_SLACK) &&
         across <= (double)LV_SHELL_OWNER_MOVE_ACROSS;
}

/* Whether a new shell s fits as a shell tank t just fired: t points the
   same way or one direction either side, and s is on t's line at the
   start distance. */
static inline bool lv_shellOwnerFitsGun(const LvShellPt *s, const LvGunTank *t) {
  int turn;
  double along, across;
  if (!t->on) {
    return false;
  }
  turn = ((int)s->dir - (int)t->dir) & 15;
  if (turn != 0 && turn != 1 && turn != 15) {
    return false;
  }
  lv_shellOwnerSplit(s->wx - t->wx, s->wy - t->wy, s->dir, &along, &across);
  return along >= (double)LV_SHELL_OWNER_START_MIN &&
         along <= (double)LV_SHELL_OWNER_START_MAX &&
         across <= (double)LV_SHELL_OWNER_START_ACROSS + along / 4.0;
}

/* The tank that fired new shell s, or LV_SHELL_OWNER_NONE. needFired is
   true when the recording has tank stocks: then only a tank that fired can
   own it. *fits gets how many tanks fit (0, 1, or 2 for two or more). */
static inline int lv_shellOwnerStartMatch(const LvShellPt *s, const LvGunTank *tanks,
                                          int nTanks, bool needFired, int *fits) {
  int i, found = LV_SHELL_OWNER_NONE, count = 0;
  for (i = 0; i < nTanks && i < LV_SHELL_OWNER_MAX_TANKS; i++) {
    if ((needFired && !tanks[i].fired) || !lv_shellOwnerFitsGun(s, &tanks[i])) {
      continue;
    }
    count++;
    found = i;
  }
  if (fits != NULL) {
    *fits = count > 2 ? 2 : count;
  }
  return count == 1 ? found : LV_SHELL_OWNER_NONE;
}

/* Give each shell of one entry its owner (outOwner[i] for cur[i]).

   prev and prevOwner are the entry before; prevValid is false when there
   is none (the first entry after a seek), and then every shell stays with
   no owner, since a shell that was already flying cannot be matched to a
   gun. tanks is indexed by player slot. stats may be NULL. */
static inline void lv_shellOwnerLink(const LvShellPt *prev, const int8_t *prevOwner, int nPrev,
                                     bool prevValid, const LvShellPt *cur, int nCur,
                                     const LvGunTank *tanks, int nTanks, bool needFired,
                                     int8_t *outOwner, LvShellOwnerStats *stats) {
  int16_t claims[LV_SHELL_OWNER_MAX_SHELLS];   /* cur shells that could be each prev */
  int16_t match[LV_SHELL_OWNER_MAX_SHELLS];    /* the one prev of each cur, -1 none, -2 more */
  int16_t gunUse[LV_SHELL_OWNER_MAX_TANKS];    /* new shells each tank was matched to */
  int i, j;

  if (nPrev > LV_SHELL_OWNER_MAX_SHELLS) nPrev = LV_SHELL_OWNER_MAX_SHELLS;
  if (nCur > LV_SHELL_OWNER_MAX_SHELLS) nCur = LV_SHELL_OWNER_MAX_SHELLS;
  if (nTanks > LV_SHELL_OWNER_MAX_TANKS) nTanks = LV_SHELL_OWNER_MAX_TANKS;
  for (i = 0; i < nCur; i++) {
    outOwner[i] = LV_SHELL_OWNER_NONE;
  }
  if (!prevValid) {
    if (stats != NULL) stats->notTracked += (uint32_t)nCur;
    return;
  }
  for (j = 0; j < nPrev; j++) claims[j] = 0;
  for (j = 0; j < nTanks; j++) gunUse[j] = 0;

  /* Continuation: which shells of the entry before each one could be. */
  for (i = 0; i < nCur; i++) {
    match[i] = -1;
    for (j = 0; j < nPrev; j++) {
      if (lv_shellOwnerContinues(&prev[j], &cur[i])) {
        claims[j]++;
        match[i] = (match[i] == -1) ? (int16_t)j : (int16_t)-2;
      }
    }
  }
  for (i = 0; i < nCur; i++) {
    if (match[i] >= 0 && claims[match[i]] == 1) {
      outOwner[i] = prevOwner[match[i]];
      if (stats != NULL) stats->continued++;
    } else if (match[i] != -1) {
      /* More than one shell before could be this one, or one before could
         be more than one now. The owner is kept only when all of them
         have the same one. */
      int8_t common = 0;
      bool first = true, same = true;
      for (j = 0; j < nPrev; j++) {
        if (!lv_shellOwnerContinues(&prev[j], &cur[i])) continue;
        if (first) { common = prevOwner[j]; first = false; }
        else if (prevOwner[j] != common) same = false;
      }
      outOwner[i] = same ? common : (int8_t)LV_SHELL_OWNER_NONE;
      if (stats != NULL) {
        if (same && common != LV_SHELL_OWNER_NONE) stats->continuedShared++;
        else if (!same) stats->continuedLost++;
        else stats->continued++;
      }
    }
  }

  /* New shells: the gun each came from. */
  for (i = 0; i < nCur; i++) {
    int fits = 0, slot;
    if (match[i] != -1) continue;
    if (stats != NULL) stats->started++;
    slot = lv_shellOwnerStartMatch(&cur[i], tanks, nTanks, needFired, &fits);
    outOwner[i] = (int8_t)slot;
    if (slot >= 0) gunUse[slot]++;
    if (stats != NULL) {
      if (fits == 0) stats->startNoTank++;
      else if (fits >= 2) stats->startTwoTanks++;
    }
  }
  /* A tank that fits two new shells at one entry fired one of them at
     most; the other is a pillbox's or another tank's. Neither is clear. */
  for (i = 0; i < nCur; i++) {
    if (match[i] != -1 || outOwner[i] < 0) continue;
    if (gunUse[outOwner[i]] > 1) {
      outOwner[i] = LV_SHELL_OWNER_NONE;
      if (stats != NULL) stats->startSharedTank++;
    } else if (stats != NULL) {
      stats->startOwned++;
    }
  }
}

#endif /* LV_SHELL_OWNER_H */
