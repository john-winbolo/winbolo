/*
 * $Id$
 *
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


/*********************************************************
*Name:          scroll
*Filename:      scroll.c
*Author:        John Morrison
*Creation Date: 16/01/99
*Last Modified: 10/06/01
*Purpose:
*  Handles scrolling on the screen. Auto scrolling and
*  keeping the object in the centre of the screen
*********************************************************/


#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "global.h"
#include "scroll.h"
#include "tank.h"
#include "players.h"
#include "pillbox.h"
#include "bases.h"
#include "game_sim.h"

/* Autoscroll tuning. */
#define AUTOSCROLL_CONCERN_RADIUS     8  /* tiles around tank counted as "near me";
                                          * = the farthest a threat can be and still
                                          * be on-screen (15-tile view, tank centred
                                          * at SCROLL_CENTER=8), so the camera never
                                          * reacts to threats the player can't see */
#define AUTOSCROLL_MAX_OFFSET         5  /* max signed view offset, in tiles */
#define AUTOSCROLL_RECALC_DEBOUNCE   30  /* min ticks between target recomputes */
#define AUTOSCROLL_PARKED_TICKS      60  /* stationary ticks before parked-rear can fire */
#define AUTOSCROLL_DEAD_ZONE          1  /* skip recompute if target change ≤ this */

/* ------------------------------------------------------------------
 * Scrolling mechanism selector + tuning knobs (see scroll.h). Process-
 * global so they can be flipped at runtime; default from the compile-
 * time SCROLL_MECHANISM_DEFAULT. */
static ScrollMechanism g_scrollMechanism      = SCROLL_MECHANISM_DEFAULT;
static int             g_scrollSmoothness      = 0;     /* 0 = none */
static bool            g_scrollSubTilePrecision = TRUE;  /* ENHANCED uses sub-tile */

ScrollMechanism scrollGetMechanism(void) { return g_scrollMechanism; }
void scrollSetMechanism(ScrollMechanism mech) { g_scrollMechanism = mech; }
int  scrollGetSmoothness(void) { return g_scrollSmoothness; }
void scrollSetSmoothness(int level) { g_scrollSmoothness = level; }
bool scrollGetSubTilePrecision(void) { return g_scrollSubTilePrecision; }
void scrollSetSubTilePrecision(bool on) { g_scrollSubTilePrecision = on; }

/* Sub-tile arithmetic. The view position is tracked in 1/256-tile units
 * internally; the BYTE *xValue/*yValue exposed to engine code is the
 * tile-aligned floor, with the fractional remainder in ScrollState's
 * subPosX/Y for the renderer to apply as a sub-pixel drag offset. */
#define AUTOSCROLL_SUB_PER_TILE     256
/* Proportional ease: each tick the offset moves (remaining / DIVISOR)
 * toward target. Higher divisor = slower / smoother. Asymptotic — the
 * slide is fast far from target and decelerates near it, so a new event
 * mid-slide redirects continuously instead of completing then restarting.
 * 32 ≈ 95% convergence in ~94 ticks (~1.9s). */
#define AUTOSCROLL_EASE_DIVISOR      32
/* Final-pixel snap threshold: when |remaining| ≤ this many sub-units we
 * step it the rest of the way to exactly hit target. Without this the
 * exponential never quite reaches and SLIDE_DONE never fires. 4 sub-units
 * = 1/4 of a pixel, well below perceptible. */
#define AUTOSCROLL_SNAP_THRESHOLD     4

/* Park settle: while moving, the view tracks the tank 1:1 (no lag). When the
 * tank stops, the view eases onto the whole tile nearest where it ended up, so
 * the framing settles tile-aligned instead of resting mid-tile (Bolo players
 * expect the map on tile boundaries). DIVISOR controls the ease speed (per-
 * tick step = remaining / DIVISOR; lower = snappier, higher = smoother/slower).
 * SNAP is the sub-unit window within which we jump the last bit so it actually
 * reaches the tile. The settle travels at most the sub-tile remainder (≤ half
 * a tile) since the moving view had no lag to unwind. See scrollSettleToTile. */
#define AUTOSCROLL_SETTLE_DIVISOR     6
#define AUTOSCROLL_SETTLE_SNAP        8

/* v1WithThreats: max view shift (tiles) the threat bias can add on top of
 * unmodified v1 autoscroll, and how gently it eases in — sub-tile units per
 * tick toward the target (256 = one tile, so 40 ≈ a tile every ~6 ticks). The
 * offset is tracked at sub-tile resolution and its fractional part is handed to
 * the renderer via subPos, so the camera glides instead of stepping a whole
 * tile at a time. Both tunable. */
#define V1THREATS_LATERAL_MAX         4
#define V1THREATS_EASE_SUB_PER_TICK   40
/* Tiles of view shift per net threat vote on an axis, clamped to LATERAL_MAX.
 * Scaling by agreement (rather than normalising the dominant axis to MAX) keeps
 * a lone or far threat from yanking the camera to a full corner: 1 vote -> 2
 * tiles, 2 aligned -> the 4-tile cap. */
#define V1THREATS_LATERAL_GAIN        2

/* Facing unit vectors (sin/cos × 256), 16-step BRADIANS index.
 * Used for the forward-bias term and the parked-rear hemisphere test. */
static const int kForwardX[16] = {
     0,   98,  181,  237,  256,  237,  181,   98,
     0,  -98, -181, -237, -256, -237, -181,  -98
};
static const int kForwardY[16] = {
  -256, -237, -181,  -98,    0,   98,  181,  237,
   256,  237,  181,   98,    0,  -98, -181, -237
};

/* Debug file logging. Off for shipping builds — flip to 1 to re-enable
 * the autoscroll.log trace. When 0 no file is opened or written. */
#define WB_DEBUG_FILE_LOG 0

/* Per-process autoscroll state (one camera per client). */
static DWORD g_autoscrollTick = 0;
static FILE *g_autoscrollLog = NULL;
static bool  g_autoscrollLogTried = FALSE;

static void autoscrollLogOpen(void) {
  if (g_autoscrollLog != NULL || g_autoscrollLogTried) return;
  g_autoscrollLogTried = TRUE;
  g_autoscrollLog = fopen("autoscroll.log", "a");
  if (g_autoscrollLog) {
    fprintf(g_autoscrollLog, "--- autoscroll session start ---\n");
    fflush(g_autoscrollLog);
  }
}

static void autoscrollLog(const char *fmt, ...) {
  if (!WB_DEBUG_FILE_LOG) return;
  if (!g_autoscrollLog) autoscrollLogOpen();
  if (!g_autoscrollLog) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_autoscrollLog, fmt, ap);
  va_end(ap);
  fflush(g_autoscrollLog);
}

/* Threat classification: enemy tank, enemy pill, neutral pill.
 * Friendlies and all bases never count. */
static bool isThreatTank(GameSim *sim, BYTE viewPlayer, int i, int *outX, int *outY) {
  int tx, ty;
  if (sim->tanks[i] == NULL) return FALSE;
  if (i == (int)viewPlayer) return FALSE;
  if (playersIsAllie(&sim->plyrs, viewPlayer, (BYTE)i) == TRUE) return FALSE;
  tx = (int)tankGetMX(&sim->tanks[i]);
  ty = (int)tankGetMY(&sim->tanks[i]);
  if (tx == 0 && ty == 0) return FALSE;  /* uninitialized slot */
  if (outX) *outX = tx;
  if (outY) *outY = ty;
  return TRUE;
}

static bool isThreatPill(GameSim *sim, BYTE viewPlayer, int pillNum, pillbox *outPill) {
  pillsGetPill(&sim->pb, outPill, pillNum);
  if (outPill->armour == 0) return FALSE;
  if (outPill->owner == NEUTRAL) return TRUE;        /* shoots everyone */
  if (outPill->owner == viewPlayer) return FALSE;
  if (playersIsAllie(&sim->plyrs, viewPlayer, outPill->owner) == TRUE) return FALSE;
  return TRUE;
}

/* Aggregate threat direction → unit-ish offset, blended with forward bias
 * when driving. Sign-only accumulation is intentional: precise magnitudes
 * would re-introduce the per-tick jitter that made v2 sickening. */
static void computeTargetOffset(GameSim *sim, BYTE viewPlayer,
                                int tankX, int tankY, BYTE speed, TURNTYPE angle,
                                int8_t *outOX, int8_t *outOY, int *outCount) {
  int sumX = 0, sumY = 0;
  int count = 0;
  int i;
  int tox, toy;

  for (i = 0; i < MAX_TANKS; i++) {
    int tx, ty, dx, dy;
    if (!isThreatTank(sim, viewPlayer, i, &tx, &ty)) continue;
    dx = tx - tankX; dy = ty - tankY;
    if (abs(dx) > AUTOSCROLL_CONCERN_RADIUS) continue;
    if (abs(dy) > AUTOSCROLL_CONCERN_RADIUS) continue;
    sumX += (dx > 0) - (dx < 0);
    sumY += (dy > 0) - (dy < 0);
    count++;
  }
  for (i = 1; i <= sim->pb->numPills; i++) {
    pillbox p;
    int dx, dy;
    if (!isThreatPill(sim, viewPlayer, i, &p)) continue;
    dx = (int)p.x - tankX; dy = (int)p.y - tankY;
    if (abs(dx) > AUTOSCROLL_CONCERN_RADIUS) continue;
    if (abs(dy) > AUTOSCROLL_CONCERN_RADIUS) continue;
    sumX += (dx > 0) - (dx < 0);
    sumY += (dy > 0) - (dy < 0);
    count++;
  }

  if (outCount) *outCount = count;

  if (count == 0) {
    tox = 0; toy = 0;
  } else {
    int mag = abs(sumX); if (abs(sumY) > mag) mag = abs(sumY);
    if (mag == 0) { tox = 0; toy = 0; }
    else {
      tox = (sumX * AUTOSCROLL_MAX_OFFSET) / mag;
      toy = (sumY * AUTOSCROLL_MAX_OFFSET) / mag;
    }
  }

  if (speed > 0) {
    /* Forward bias at 30% — keep "see what's ahead" without overpowering threats. */
    int idx = (((int)angle + 8) >> 4) & 15;
    int fwdX = (kForwardX[idx] * AUTOSCROLL_MAX_OFFSET) / 256;
    int fwdY = (kForwardY[idx] * AUTOSCROLL_MAX_OFFSET) / 256;
    tox = (tox * 7 + fwdX * 3) / 10;
    toy = (toy * 7 + fwdY * 3) / 10;
  }

  if (tox >  AUTOSCROLL_MAX_OFFSET) tox =  AUTOSCROLL_MAX_OFFSET;
  if (tox < -AUTOSCROLL_MAX_OFFSET) tox = -AUTOSCROLL_MAX_OFFSET;
  if (toy >  AUTOSCROLL_MAX_OFFSET) toy =  AUTOSCROLL_MAX_OFFSET;
  if (toy < -AUTOSCROLL_MAX_OFFSET) toy = -AUTOSCROLL_MAX_OFFSET;

  *outOX = (int8_t)tox;
  *outOY = (int8_t)toy;
}


void scrollCreate(ScrollState *ss) {
  int i;
  ss->autoScroll = FALSE;
  ss->scrollX = 0;
  ss->scrollY = 0;
  ss->xPositive = TRUE;
  ss->yPositive = TRUE;
  ss->autoScrollOverRide = FALSE;
  ss->mods = FALSE;
  ss->stickyX = FALSE;
  ss->stickyXDir = FALSE;
  ss->stickyY = FALSE;
  ss->stickyYDir = FALSE;
  ss->targetOffsetX = 0;
  ss->targetOffsetY = 0;
  ss->currentOffsetSubX = 0;
  ss->currentOffsetSubY = 0;
  ss->subPosX = 0;
  ss->subPosY = 0;
  ss->initialized = FALSE;
  ss->lastRecalcTick = 0;
  ss->parkedSinceTick = 0;
  ss->gunsightWasInside = TRUE;
  for (i = 0; i < MAX_TANKS; i++) ss->prevThreatTank[i] = FALSE;
  for (i = 0; i < MAX_PILLS; i++) ss->prevThreatPill[i] = FALSE;
  ss->peekPhase = 0;
  ss->peekTargetX = 0;
  ss->peekTargetY = 0;
  ss->peekUntilTick = 0;
  ss->peekReturnX = 0;
  ss->peekReturnY = 0;
  for (i = 0; i < MAX_TANKS; i++) ss->seenThreatTank[i] = FALSE;
  for (i = 0; i < MAX_PILLS; i++) ss->seenThreatPill[i] = FALSE;
  ss->threatLatX = 0;
  ss->threatLatY = 0;
  ss->threatLatDesiredX = 0;
  ss->threatLatDesiredY = 0;
  ss->threatLatSubX = 0;
  ss->threatLatSubY = 0;
}


void scrollSetScrollType(ScrollState *ss, bool isAuto) {
  ss->autoScroll = isAuto;
  ss->autoScrollOverRide = FALSE;
  ss->initialized = FALSE;     /* re-seed baseline on any mode change */
  if (isAuto == FALSE) {
    ss->scrollX = 0;
    ss->scrollY = 0;
    ss->mods = FALSE;
    ss->stickyX = FALSE;
    ss->stickyY = FALSE;
    ss->targetOffsetX = 0;
    ss->targetOffsetY = 0;
    ss->currentOffsetSubX = 0;
    ss->currentOffsetSubY = 0;
    ss->subPosX = 0;
    ss->subPosY = 0;
    ss->threatLatX = 0;
    ss->threatLatY = 0;
    ss->threatLatDesiredX = 0;
    ss->threatLatDesiredY = 0;
    ss->threatLatSubX = 0;
    ss->threatLatSubY = 0;
  }
}

void scrollCenterObject(ScrollState *ss, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY) {
  *xValue = objectX - SCROLL_CENTER;
  *yValue = objectY - SCROLL_CENTER;
  ss->autoScrollOverRide = FALSE;
  ss->stickyX = FALSE;
  ss->stickyY = FALSE;
  ss->targetOffsetX = 0;
  ss->targetOffsetY = 0;
  ss->currentOffsetSubX = 0;
  ss->currentOffsetSubY = 0;
  ss->subPosX = 0;
  ss->subPosY = 0;
  ss->threatLatX = 0;
  ss->threatLatY = 0;
  ss->threatLatDesiredX = 0;
  ss->threatLatDesiredY = 0;
  ss->threatLatSubX = 0;
  ss->threatLatSubY = 0;
  ss->initialized = FALSE;
}

/* Classic WinBolo autoscroll.
 *
 * When the crosshair (gunsight) touches a screen edge, push the view that
 * way by a burst of up to CLASSIC_AS_MAX_LEAD tiles at full tank speed,
 * scaled linearly with speed (burst = MAX_LEAD * speed / FULL_SPEED).
 *
 * The burst is committed up-front (a per-tick countdown in
 * ss->scrollX/scrollY) and plays out one tile per tick. This matters
 * because scrolling toward the crosshair moves it off the edge after the
 * first tile — without the latched countdown the push would stop at one
 * tile. A new burst only arms once the previous one finishes and the
 * crosshair is at an edge again. A hard clamp keeps the tank on screen.
 * Integer-tile, no sub-tile smoothing. */
#define CLASSIC_AS_FULL_SPEED 16  /* tank top speed (road) */
#define CLASSIC_AS_MAX_LEAD    5  /* max burst tiles at full speed */
static bool scrollClassicAutoScroll(ScrollState *ss, BYTE *xValue, BYTE *yValue,
                                    BYTE objectX, BYTE objectY,
                                    BYTE gunsightX, BYTE gunsightY, BYTE speed) {
  bool returnValue = FALSE;
  int burst = (CLASSIC_AS_MAX_LEAD * (int)speed) / CLASSIC_AS_FULL_SPEED;

  ss->subPosX = 0;
  ss->subPosY = 0;

  /* Arm a burst when the crosshair is at an edge and none is in flight. */
  if (ss->scrollX == 0 && ss->scrollY == 0 && burst > 0) {
    int gcol = (int)gunsightX - (int)*xValue;  /* gunsight column within view */
    int grow = (int)gunsightY - (int)*yValue;  /* gunsight row within view    */
    if (gcol >= MAIN_SCREEN_SIZE_X - 1) { ss->scrollX = (BYTE)burst; ss->xPositive = TRUE; }
    else if (gcol <= 0)                 { ss->scrollX = (BYTE)burst; ss->xPositive = FALSE; }
    if (grow >= MAIN_SCREEN_SIZE_Y - 1) { ss->scrollY = (BYTE)burst; ss->yPositive = TRUE; }
    else if (grow <= 0)                 { ss->scrollY = (BYTE)burst; ss->yPositive = FALSE; }
  }

  /* Play the burst out, one tile per tick. */
  if (ss->scrollX > 0) {
    ss->scrollX--;
    if (ss->xPositive) { (*xValue)++; } else { (*xValue)--; }
    returnValue = TRUE;
  }
  if (ss->scrollY > 0) {
    ss->scrollY--;
    if (ss->yPositive) { (*yValue)++; } else { (*yValue)--; }
    returnValue = TRUE;
  }

  /* Hard clamp: the tank must always stay on screen. */
  if ((int)objectX <= (int)*xValue) {
    *xValue = (BYTE)(objectX - 1); ss->scrollX = 0;
  } else if ((int)objectX >= (int)*xValue + MAIN_SCREEN_SIZE_X) {
    *xValue = (BYTE)(objectX - MAIN_SCREEN_SIZE_X + 1); ss->scrollX = 0;
  }
  if ((int)objectY <= (int)*yValue) {
    *yValue = (BYTE)(objectY - 1); ss->scrollY = 0;
  } else if ((int)objectY >= (int)*yValue + MAIN_SCREEN_SIZE_Y) {
    *yValue = (BYTE)(objectY - MAIN_SCREEN_SIZE_Y + 1); ss->scrollY = 0;
  }

  if (ss->mods == TRUE && returnValue == TRUE) {
    ss->mods = FALSE;
  }
  if (returnValue == TRUE) {
    ss->autoScrollOverRide = FALSE;
  }

  return returnValue;
}

/* v1WithThreats autoscroll — unmodified v1 (scrollClassicAutoScroll) with a
 * single addition that is active ONLY while moving: a view shift toward the
 * threats around you, so threats near your path stay framed instead of being
 * cropped by the forward lead.
 *
 * It never alters v1's own framing. Each tick the offset applied last tick is
 * removed, v1 runs on that recovered base exactly as it would standalone, then
 * the offset is re-applied on top and the tank re-clamped on screen. With zero
 * offset (no threats, balanced threats, or parked-and-settled) this is
 * byte-for-byte v1.
 *
 * The bias is WORLD-relative, not facing-relative: each threat within the
 * concern radius votes the sign of its world direction (sign-only, the same
 * anti-jitter choice computeTargetOffset uses) and the summed votes pick the
 * offset, normalised to at most V1THREATS_LATERAL_MAX. Because it never
 * projects onto facing, simply steering the tank does not move the camera —
 * the target only changes when a threat actually crosses to your other side.
 * The target is recomputed on the recalc debounce with a one-tile dead-zone.
 * The offset is tracked at sub-tile resolution and eased toward the target at
 * V1THREATS_EASE_SUB_PER_TICK sub-units/tick; its whole part lands in the view
 * tile and the fractional remainder goes to subPos, so the camera glides
 * smoothly rather than lurching or stepping a tile at a time. */
static bool scrollV1WithThreatsAutoScroll(ScrollState *ss, GameSim *sim,
                                          BYTE *xValue, BYTE *yValue,
                                          BYTE objectX, BYTE objectY,
                                          BYTE gunsightX, BYTE gunsightY,
                                          BYTE speed, TURNTYPE angle) {
  BYTE myPlayer = sim->viewPlayer;
  BYTE inX = *xValue, inY = *yValue;
  int  baseX, baseY;
  int  rx, ry;
  int  totalSubX, totalSubY, vx, vy;
  int  latSubX, latSubY;                /* eased sub-tile offset (trace) */
  bool clamped = FALSE;

  g_autoscrollTick++;

  /* Recover the v1 base view by removing last tick's applied lateral offset. */
  rx = (int)*xValue - (int)ss->threatLatX;
  ry = (int)*yValue - (int)ss->threatLatY;
  if (rx < 0) rx = 0; else if (rx > 255) rx = 255;
  if (ry < 0) ry = 0; else if (ry > 255) ry = 255;
  *xValue = (BYTE)rx;
  *yValue = (BYTE)ry;

  /* Run v1 unchanged on the recovered base. */
  scrollClassicAutoScroll(ss, xValue, yValue, objectX, objectY,
                          gunsightX, gunsightY, speed);
  baseX = (int)*xValue;
  baseY = (int)*yValue;

  /* Recompute the desired lateral offset (moving only, debounced). */
  if (speed == 0) {
    ss->threatLatDesiredX = 0;
    ss->threatLatDesiredY = 0;
  } else if (ss->lastRecalcTick == 0 ||
             (g_autoscrollTick - ss->lastRecalcTick) >= AUTOSCROLL_RECALC_DEBOUNCE) {
    /* World-relative threat bias: sum the sign of each threat's world
     * direction (NOT projected onto facing), so steering the tank never moves
     * the camera — the target only shifts when a threat genuinely crosses to
     * your other side. Sign-only is the same anti-jitter choice as
     * computeTargetOffset. */
    int sumX = 0, sumY = 0;
    int nVote = 0;
    int candX, candY, i;

    for (i = 0; i < MAX_TANKS; i++) {
      int tx, ty, dx, dy;
      if (!isThreatTank(sim, myPlayer, i, &tx, &ty)) continue;
      dx = tx - (int)objectX; dy = ty - (int)objectY;
      if (abs(dx) > AUTOSCROLL_CONCERN_RADIUS) continue;
      if (abs(dy) > AUTOSCROLL_CONCERN_RADIUS) continue;
      sumX += (dx > 0) - (dx < 0);
      sumY += (dy > 0) - (dy < 0);
      nVote++;
    }
    for (i = 1; i <= sim->pb->numPills; i++) {
      pillbox p;
      int dx, dy;
      if (!isThreatPill(sim, myPlayer, i, &p)) continue;
      dx = (int)p.x - (int)objectX; dy = (int)p.y - (int)objectY;
      if (abs(dx) > AUTOSCROLL_CONCERN_RADIUS) continue;
      if (abs(dy) > AUTOSCROLL_CONCERN_RADIUS) continue;
      sumX += (dx > 0) - (dx < 0);
      sumY += (dy > 0) - (dy < 0);
      nVote++;
    }

    /* Scale by how many threats agree on a direction, clamped to LATERAL_MAX,
     * rather than normalising the dominant axis to MAX. The latter sent even a
     * single far threat to a full ±MAX corner and flipped corners as the tank
     * drove past it; the clamped gain keeps a lone threat to a gentle nudge. */
    candX = sumX * V1THREATS_LATERAL_GAIN;
    candY = sumY * V1THREATS_LATERAL_GAIN;
    if (candX >  V1THREATS_LATERAL_MAX) candX =  V1THREATS_LATERAL_MAX;
    if (candX < -V1THREATS_LATERAL_MAX) candX = -V1THREATS_LATERAL_MAX;
    if (candY >  V1THREATS_LATERAL_MAX) candY =  V1THREATS_LATERAL_MAX;
    if (candY < -V1THREATS_LATERAL_MAX) candY = -V1THREATS_LATERAL_MAX;

    /* Dead-zone: only commit a target that moved more than a tile, so a
     * flickering vote (a threat hovering on an axis) doesn't keep restarting a
     * slide. */
    if (abs(candX - (int)ss->threatLatDesiredX) > AUTOSCROLL_DEAD_ZONE ||
        abs(candY - (int)ss->threatLatDesiredY) > AUTOSCROLL_DEAD_ZONE) {
      ss->threatLatDesiredX = (int8_t)candX;
      ss->threatLatDesiredY = (int8_t)candY;
    }
    ss->lastRecalcTick = g_autoscrollTick;
    /* What the recompute saw: votes, the summed world direction, the candidate
     * target, and the (dead-zoned) target the offset will now ease toward. */
    autoscrollLog("[t=%u] V1THR RECALC ang=%d votes=%d sum=(%d,%d) cand=(%d,%d) -> desired=(%d,%d)\n",
                  (unsigned)g_autoscrollTick, (int)angle, nVote, sumX, sumY,
                  candX, candY,
                  (int)ss->threatLatDesiredX, (int)ss->threatLatDesiredY);
  }

  /* Ease the sub-tile lateral offset toward the target (desired tiles × 256) at
   * a constant glide speed, so the camera slides smoothly across tile
   * boundaries instead of stepping a whole tile at a time. */
  {
    int tgtSubX = (int)ss->threatLatDesiredX * AUTOSCROLL_SUB_PER_TILE;
    int tgtSubY = (int)ss->threatLatDesiredY * AUTOSCROLL_SUB_PER_TILE;
    int dX = tgtSubX - (int)ss->threatLatSubX;
    int dY = tgtSubY - (int)ss->threatLatSubY;
    if (abs(dX) <= V1THREATS_EASE_SUB_PER_TICK) ss->threatLatSubX = (int16_t)tgtSubX;
    else ss->threatLatSubX += (int16_t)((dX > 0 ? 1 : -1) * V1THREATS_EASE_SUB_PER_TICK);
    if (abs(dY) <= V1THREATS_EASE_SUB_PER_TICK) ss->threatLatSubY = (int16_t)tgtSubY;
    else ss->threatLatSubY += (int16_t)((dY > 0 ? 1 : -1) * V1THREATS_EASE_SUB_PER_TICK);
  }
  latSubX = (int)ss->threatLatSubX;
  latSubY = (int)ss->threatLatSubY;

  /* Apply on top of the v1 base in sub-tile units, then keep the tank on screen
   * and the view on the map — BOTH clamped in sub-tile units, not whole tiles.
   * A whole-tile clamp here made the camera bounce: when the desired offset
   * pushed the tank toward an edge, the view snapped a full tile back past the
   * edge, the ease climbed to the edge again over a few ticks, and it snapped
   * back again — a ~1-tile sawtooth. Clamping to the exact sub-tile limit lets
   * the view rest against the edge instead. The whole part lands in *xValue;
   * the fractional remainder goes to subPos for the renderer to glide (snapped
   * to whole tiles when sub-tile precision is off). Store both the surviving
   * sub offset and its whole part so next tick's recovery and ease are exact. */
  totalSubX = baseX * AUTOSCROLL_SUB_PER_TILE + latSubX;
  totalSubY = baseY * AUTOSCROLL_SUB_PER_TILE + latSubY;
  {
    int loSubX  = ((int)objectX - MAIN_SCREEN_SIZE_X + 1) * AUTOSCROLL_SUB_PER_TILE;
    int hiSubX  = ((int)objectX - 1)                      * AUTOSCROLL_SUB_PER_TILE;
    int loSubY  = ((int)objectY - MAIN_SCREEN_SIZE_Y + 1) * AUTOSCROLL_SUB_PER_TILE;
    int hiSubY  = ((int)objectY - 1)                      * AUTOSCROLL_SUB_PER_TILE;
    int maxSubX = (255 - MAIN_SCREEN_SIZE_X)              * AUTOSCROLL_SUB_PER_TILE;
    int maxSubY = (255 - MAIN_SCREEN_SIZE_Y)              * AUTOSCROLL_SUB_PER_TILE;

    /* Tank on screen. */
    if      (totalSubX < loSubX) { totalSubX = loSubX; clamped = TRUE; }
    else if (totalSubX > hiSubX) { totalSubX = hiSubX; clamped = TRUE; }
    if      (totalSubY < loSubY) { totalSubY = loSubY; clamped = TRUE; }
    else if (totalSubY > hiSubY) { totalSubY = hiSubY; clamped = TRUE; }
    /* View on map (wins at map edges, exactly as the whole-tile clamp did). */
    if (totalSubX < 0) totalSubX = 0; else if (totalSubX > maxSubX) totalSubX = maxSubX;
    if (totalSubY < 0) totalSubY = 0; else if (totalSubY > maxSubY) totalSubY = maxSubY;
  }
  vx = totalSubX / AUTOSCROLL_SUB_PER_TILE;
  vy = totalSubY / AUTOSCROLL_SUB_PER_TILE;

  *xValue = (BYTE)vx;
  *yValue = (BYTE)vy;
  if (g_scrollSubTilePrecision) {
    ss->subPosX = (int16_t)(totalSubX - vx * AUTOSCROLL_SUB_PER_TILE);
    ss->subPosY = (int16_t)(totalSubY - vy * AUTOSCROLL_SUB_PER_TILE);
  } else {
    ss->subPosX = 0;
    ss->subPosY = 0;
  }

  ss->threatLatX    = (int8_t)(vx - baseX);
  ss->threatLatY    = (int8_t)(vy - baseY);
  ss->threatLatSubX = (int16_t)(totalSubX - baseX * AUTOSCROLL_SUB_PER_TILE);
  ss->threatLatSubY = (int16_t)(totalSubY - baseY * AUTOSCROLL_SUB_PER_TILE);

  /* Per-tick trace. Read left to right to see where movement comes from:
   *   in      = view handed in (carries last tick's whole-tile offset)
   *   recov   = base after removing that offset (what v1 should see)
   *   v1base  = view after running unmodified v1 on recov (v1's own move)
   *   des     = lateral target tiles for the current threats
   *   latSub  = eased sub-tile offset this tick (256 = one tile)
   *   out     = final view tile written; subPos = fractional glide remainder
   *   survLat = whole-tile offset that survived the clamp
   * A jump shows up as a big v1base step (v1's doing) or des changing (threats
   * actually moved); CLAMPED means the edge ate part of the offset. */
  autoscrollLog("[t=%u] V1THR spd=%u ang=%d in=(%u,%u) recov=(%d,%d) v1base=(%d,%d) "
                "des=(%d,%d) latSub=(%d,%d) out=(%u,%u) subPos=(%d,%d) survLat=(%d,%d)%s\n",
                (unsigned)g_autoscrollTick, (unsigned)speed, (int)angle,
                (unsigned)inX, (unsigned)inY, rx, ry, baseX, baseY,
                (int)ss->threatLatDesiredX, (int)ss->threatLatDesiredY,
                latSubX, latSubY,
                (unsigned)*xValue, (unsigned)*yValue,
                (int)ss->subPosX, (int)ss->subPosY,
                (int)ss->threatLatX, (int)ss->threatLatY,
                clamped ? " CLAMPED" : "");

  return (*xValue != inX || *yValue != inY);
}

/* Ease one view axis (tile + sub-tile remainder) toward a target tile with a
   fast deceleration. Returns TRUE when arrived. 256 sub-units = 1 tile; the
   renderer applies the remainder for smooth sub-pixel motion. */
static bool canucksSlideAxis(BYTE *tile, int16_t *sub, int targetTile) {
  int cur    = (int)(*tile) * 256 + (int)(*sub);
  int target = targetTile * 256;
  int d      = target - cur;
  int step;
  if (d == 0) { *sub = 0; return true; }
  step = d / 3;                                 /* ~1/3 of the gap per tick */
  if (step == 0) step = (d > 0) ? 64 : -64;     /* min 1/4 tile/tick near target */
  cur += step;
  if ((d > 0 && cur >= target) || (d < 0 && cur <= target)) {
    *tile = (BYTE)targetTile; *sub = 0; return true;
  }
  if (cur < 0) cur = 0;
  *tile = (BYTE)(cur / 256);
  *sub  = (int16_t)(cur % 256);
  return false;
}

/* Canuck's autoscroll — classic WinBolo v1 autoscroll with two twists:
 *  - a crosshair touching a screen edge scrolls a moderate CANUCKS_LOOKAHEAD
 *    tiles further in the aim direction (like v1's short lead, not a whole
 *    screen), and
 *  - an "alert jump": a threat that is BEHIND the tank AND off-screen, within
 *    the concern radius (same detection / 24-tile range as enhanced), makes
 *    the view fast-slide to centre on it, hold briefly, then slide back. Each
 *    threat is alerted on only once (seenThreat*) until it leaves the radius.
 * The alert jump owns the view while active; otherwise the edge-push runs. */
#define CANUCKS_LOOKAHEAD       5   /* tiles of view past the crosshair edge */
#define CANUCKS_PEEK_HOLD_TICKS 15  /* hold on the threat (~0.25s @ 60Hz) */
/* Alert only on threats close enough that the player could have spotted them
   by manually scrolling (~a screen from the tank), and never further than the
   enhanced autoscroll's concern radius. */
#define CANUCKS_ALERT_RADIUS \
  ((MAIN_SCREEN_SIZE_X - 1) < AUTOSCROLL_CONCERN_RADIUS ? (MAIN_SCREEN_SIZE_X - 1) : AUTOSCROLL_CONCERN_RADIUS)
static bool scrollCanucksAutoScroll(ScrollState *ss, GameSim *sim,
                                    BYTE *xValue, BYTE *yValue,
                                    BYTE objectX, BYTE objectY,
                                    BYTE gunsightX, BYTE gunsightY, TURNTYPE angle) {
  bool returnValue = FALSE;
  BYTE myPlayer = sim->viewPlayer;
  bool jump = FALSE;
  int  threatX = 0, threatY = 0;
  int  idx = (((int)angle + 8) >> 4) & 15;
  int  fx = kForwardX[idx], fy = kForwardY[idx];
  int  i;

  g_autoscrollTick++;

  /* Alert-jump candidate: a threat (enemy tank or pillbox; same helpers as
     enhanced) within CANUCKS_ALERT_RADIUS that is BEHIND the tank (rear
     hemisphere via the forward vector) AND not currently on screen, and that
     we haven't already alerted on. The seenThreat latch clears when a threat
     leaves the radius, so it can alert again on a fresh sneak-up. Threats
     present at startup are pre-marked seen (baseline). */
  for (i = 0; i < MAX_TANKS; i++) {
    int tx, ty, scol, srow;
    bool behind, onScreen;
    /* Tanks move, so once one leaves range we forget it — a later approach is
       a fresh sneak-up. */
    if (!isThreatTank(sim, myPlayer, i, &tx, &ty) ||
        abs(tx - (int)objectX) > CANUCKS_ALERT_RADIUS ||
        abs(ty - (int)objectY) > CANUCKS_ALERT_RADIUS) {
      ss->seenThreatTank[i] = FALSE;
      continue;
    }
    scol = tx - (int)*xValue; srow = ty - (int)*yValue;
    onScreen = (scol >= 0 && scol < MAIN_SCREEN_SIZE_X &&
                srow >= 0 && srow < MAIN_SCREEN_SIZE_Y);
    if (onScreen) { ss->seenThreatTank[i] = TRUE; continue; }  /* we can see it now */
    behind = ((tx - (int)objectX) * fx + (ty - (int)objectY) * fy) < 0;
    if (behind && !ss->seenThreatTank[i]) {
      if (!ss->initialized)      { ss->seenThreatTank[i] = TRUE; }
      else if (!jump)            { jump = TRUE; threatX = tx; threatY = ty;
                                   ss->seenThreatTank[i] = TRUE; }
    }
  }
  for (i = 1; i <= sim->pb->numPills; i++) {
    pillbox p;
    int scol, srow;
    bool behind, onScreen;
    /* A pillbox is static, so once seen it stays "known" — UNLESS it stops
       being a placed threat (picked up / captured), which is exactly what
       happens when it's rebuilt elsewhere: that re-arms it for a fresh alert
       at the new spot. Leaving the alert radius does NOT forget it. */
    if (!isThreatPill(sim, myPlayer, i, &p)) {
      ss->seenThreatPill[i - 1] = FALSE;
      continue;
    }
    if (abs((int)p.x - (int)objectX) > CANUCKS_ALERT_RADIUS ||
        abs((int)p.y - (int)objectY) > CANUCKS_ALERT_RADIUS) {
      continue;   /* out of range: keep its seen state */
    }
    scol = (int)p.x - (int)*xValue; srow = (int)p.y - (int)*yValue;
    onScreen = (scol >= 0 && scol < MAIN_SCREEN_SIZE_X &&
                srow >= 0 && srow < MAIN_SCREEN_SIZE_Y);
    if (onScreen) { ss->seenThreatPill[i - 1] = TRUE; continue; }  /* scrolled across it */
    behind = (((int)p.x - (int)objectX) * fx + ((int)p.y - (int)objectY) * fy) < 0;
    if (behind && !ss->seenThreatPill[i - 1]) {
      if (!ss->initialized)      { ss->seenThreatPill[i - 1] = TRUE; }
      else if (!jump)            { jump = TRUE; threatX = (int)p.x; threatY = (int)p.y;
                                   ss->seenThreatPill[i - 1] = TRUE; }
    }
  }
  ss->initialized = TRUE;

  /* Alert-jump overlay: while it's running it owns the view. */
  if (ss->peekPhase == 1) {                    /* sliding to threat */
    bool rx = canucksSlideAxis(xValue, &ss->subPosX, ss->peekTargetX);
    bool ry = canucksSlideAxis(yValue, &ss->subPosY, ss->peekTargetY);
    if (rx && ry) { ss->peekPhase = 2; ss->peekUntilTick = g_autoscrollTick + CANUCKS_PEEK_HOLD_TICKS; }
    return TRUE;
  }
  if (ss->peekPhase == 2) {                    /* holding */
    ss->subPosX = 0; ss->subPosY = 0;
    if (g_autoscrollTick < ss->peekUntilTick) return FALSE;
    ss->peekPhase   = 3;
    ss->peekTargetX = ss->peekReturnX;
    ss->peekTargetY = ss->peekReturnY;
    return FALSE;
  }
  if (ss->peekPhase == 3) {                    /* sliding back */
    bool rx = canucksSlideAxis(xValue, &ss->subPosX, ss->peekTargetX);
    bool ry = canucksSlideAxis(yValue, &ss->subPosY, ss->peekTargetY);
    if (rx && ry) ss->peekPhase = 0;
    return TRUE;
  }

  /* New qualifying threat: start an alert jump. Aim to centre the threat, but
     the OUR TANK must stay in the jumped view — clamp the view so the tank is
     visible (this also keeps the threat in, since both are within a screen),
     then keep it on the map. If the tank still can't fit with the threat,
     don't jump at all. Clear any in-flight edge push first. */
  if (jump) {
    int vx = threatX - MAIN_SCREEN_SIZE_X / 2;
    int vy = threatY - MAIN_SCREEN_SIZE_Y / 2;
    int tcol, trow, gcol, grow;
    /* Keep the tank in view (priority over centring the threat). */
    if (vx > (int)objectX)                        vx = (int)objectX;
    if (vx < (int)objectX - (MAIN_SCREEN_SIZE_X - 1)) vx = (int)objectX - (MAIN_SCREEN_SIZE_X - 1);
    if (vy > (int)objectY)                        vy = (int)objectY;
    if (vy < (int)objectY - (MAIN_SCREEN_SIZE_Y - 1)) vy = (int)objectY - (MAIN_SCREEN_SIZE_Y - 1);
    /* Then keep it on the map. */
    if (vx < 0) vx = 0;
    if (vx > 255 - MAIN_SCREEN_SIZE_X) vx = 255 - MAIN_SCREEN_SIZE_X;
    if (vy < 0) vy = 0;
    if (vy > 255 - MAIN_SCREEN_SIZE_Y) vy = 255 - MAIN_SCREEN_SIZE_Y;
    /* Verify both tank and threat are actually in the final view; if not (map
       edge forced the tank out), skip the jump. It's already marked seen so it
       won't keep retrying. */
    tcol = (int)objectX - vx; trow = (int)objectY - vy;
    gcol = threatX - vx;      grow = threatY - vy;
    if (tcol >= 0 && tcol < MAIN_SCREEN_SIZE_X && trow >= 0 && trow < MAIN_SCREEN_SIZE_Y &&
        gcol >= 0 && gcol < MAIN_SCREEN_SIZE_X && grow >= 0 && grow < MAIN_SCREEN_SIZE_Y) {
      ss->peekReturnX = *xValue;
      ss->peekReturnY = *yValue;
      ss->peekTargetX = (BYTE)vx;
      ss->peekTargetY = (BYTE)vy;
      ss->peekPhase   = 1;
      ss->scrollX = 0;
      ss->scrollY = 0;
      return TRUE;
    }
  }

  ss->subPosX = 0;
  ss->subPosY = 0;

  /* Arm a push when the crosshair is at an edge and none is in flight: scroll
     CANUCKS_LOOKAHEAD tiles further into the aim direction (moderate lead). */
  if (ss->scrollX == 0 && ss->scrollY == 0) {
    int gcol = (int)gunsightX - (int)*xValue;  /* crosshair column within view */
    int grow = (int)gunsightY - (int)*yValue;
    if (gcol >= MAIN_SCREEN_SIZE_X - 1) {        /* crosshair at right edge */
      int n = gcol - (MAIN_SCREEN_SIZE_X - 1 - CANUCKS_LOOKAHEAD);
      if (n > 0) { ss->scrollX = (BYTE)n; ss->xPositive = TRUE; }
    } else if (gcol <= 0) {                       /* crosshair at left edge */
      int n = CANUCKS_LOOKAHEAD - gcol;
      if (n > 0) { ss->scrollX = (BYTE)n; ss->xPositive = FALSE; }
    }
    if (grow >= MAIN_SCREEN_SIZE_Y - 1) {        /* crosshair at bottom edge */
      int n = grow - (MAIN_SCREEN_SIZE_Y - 1 - CANUCKS_LOOKAHEAD);
      if (n > 0) { ss->scrollY = (BYTE)n; ss->yPositive = TRUE; }
    } else if (grow <= 0) {                       /* crosshair at top edge */
      int n = CANUCKS_LOOKAHEAD - grow;
      if (n > 0) { ss->scrollY = (BYTE)n; ss->yPositive = FALSE; }
    }
  }

  /* Play the push out, one tile per tick. */
  if (ss->scrollX > 0) {
    ss->scrollX--;
    if (ss->xPositive) { (*xValue)++; } else { (*xValue)--; }
    returnValue = TRUE;
  }
  if (ss->scrollY > 0) {
    ss->scrollY--;
    if (ss->yPositive) { (*yValue)++; } else { (*yValue)--; }
    returnValue = TRUE;
  }

  /* Hard clamp: the tank must always stay on screen. */
  if ((int)objectX <= (int)*xValue) {
    *xValue = (BYTE)(objectX - 1); ss->scrollX = 0;
  } else if ((int)objectX >= (int)*xValue + MAIN_SCREEN_SIZE_X) {
    *xValue = (BYTE)(objectX - MAIN_SCREEN_SIZE_X + 1); ss->scrollX = 0;
  }
  if ((int)objectY <= (int)*yValue) {
    *yValue = (BYTE)(objectY - 1); ss->scrollY = 0;
  } else if ((int)objectY >= (int)*yValue + MAIN_SCREEN_SIZE_Y) {
    *yValue = (BYTE)(objectY - MAIN_SCREEN_SIZE_Y + 1); ss->scrollY = 0;
  }

  if (ss->mods == TRUE && returnValue == TRUE) {
    ss->mods = FALSE;
  }
  if (returnValue == TRUE) {
    ss->autoScrollOverRide = FALSE;
  }

  return returnValue;
}

bool scrollUpdate(ScrollState *ss, GameSim *sim, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, bool isTank, BYTE gunsightX, BYTE gunsightY, BYTE speed, BYTE armour, TURNTYPE angle, bool manual, bool tankIsDead, WORLD ownRenderX, WORLD ownRenderY) {
  bool returnValue;

  returnValue = TRUE;
  if (tankIsDead == TRUE) {
    returnValue = FALSE;
  } else if (g_scrollMechanism == SCROLL_MECH_CLASSIC_NO_AUTOSCROLL) {
    /* Classic, autoscroll disabled: manual keys + screen-edge nudge only.
     * The edge nudge only fires with forward motion. When the tank is
     * parked it must NOT fire: scrollNoAutoScroll nudges off facing angle,
     * so a still tank at the edge would shove the view back every tick and
     * fight a manual scroll (the 144<->143 jitter). Original WinBolo let
     * you scroll cleanly to the edge while parked. */
    if (manual == TRUE) {
      returnValue = scrollManual(ss, xValue, yValue, objectX, objectY, angle);
    } else if (speed > 0) {
      returnValue = scrollNoAutoScroll(ss, xValue, yValue, objectX, objectY, angle);
    } else {
      returnValue = FALSE;
    }
  } else if (g_scrollMechanism == SCROLL_MECH_CLASSIC_AUTOSCROLL) {
    /* Classic autoscroll — starts as a copy of the landed no-autoscroll
     * behaviour (manual keys + forward-motion edge nudge, parked = no
     * fight). Build the autoscroll up from scrollClassicAutoScroll. */
    if (manual == TRUE) {
      returnValue = scrollManual(ss, xValue, yValue, objectX, objectY, angle);
    } else if (speed > 0) {
      returnValue = scrollClassicAutoScroll(ss, xValue, yValue, objectX, objectY,
                                            gunsightX, gunsightY, speed);
    } else {
      returnValue = FALSE;
    }
  } else if (g_scrollMechanism == SCROLL_MECH_V1_WITH_THREATS) {
    /* v1 autoscroll plus a moving-only sideways bias toward the side with
     * threats. Manual panning drops the lateral offset so the recovered v1
     * base stays correct. Parked with no offset = does nothing (exactly v1);
     * a residual offset is allowed to unwind even while parked.
     *
     * When autoscroll is switched off in the menu (ss->autoScroll == FALSE)
     * this reverts to classic manual scrolling — manual keys plus a forward-
     * motion edge nudge, no nudge while parked — so the toggle actually
     * disables autoscroll. scrollSetScrollType has already zeroed the lateral
     * offset on the way off, so there is nothing left to unwind here. */
    if (manual == TRUE) {
      ss->threatLatX = 0;
      ss->threatLatY = 0;
      ss->threatLatDesiredX = 0;
      ss->threatLatDesiredY = 0;
      ss->threatLatSubX = 0;
      ss->threatLatSubY = 0;
      returnValue = scrollManual(ss, xValue, yValue, objectX, objectY, angle);
    } else if (ss->autoScroll == FALSE) {
      if (speed > 0) {
        returnValue = scrollNoAutoScroll(ss, xValue, yValue, objectX, objectY, angle);
      } else {
        returnValue = FALSE;
      }
    } else if (speed > 0 || ss->threatLatSubX != 0 || ss->threatLatSubY != 0) {
      returnValue = scrollV1WithThreatsAutoScroll(ss, sim, xValue, yValue, objectX, objectY,
                                                  gunsightX, gunsightY, speed, angle);
    } else {
      returnValue = FALSE;
    }
  } else if (g_scrollMechanism == SCROLL_MECH_ANDREW_ENHANCED) {
    /* Canuck's — v1 autoscroll, but a crosshair at the edge scrolls hard so
     * the tank lands 2 tiles from the opposite edge. Fires regardless of speed
     * so aiming to the edge while parked still pushes the view. */
    if (manual == TRUE) {
      returnValue = scrollManual(ss, xValue, yValue, objectX, objectY, angle);
    } else {
      returnValue = scrollCanucksAutoScroll(ss, sim, xValue, yValue, objectX, objectY,
                                            gunsightX, gunsightY, angle);
    }
  } else {
    /* SCROLL_MECH_ENHANCED — John's current sub-tile / threat-aware path. */
    if (manual == TRUE) {
      returnValue = scrollManual(ss, xValue, yValue, objectX, objectY, angle);
    } else if (ss->autoScroll == TRUE && isTank == TRUE && armour <= TANK_FULL_ARMOUR) {
      returnValue = scrollAutoScroll(ss, sim, xValue, yValue, objectX, objectY, gunsightX, gunsightY, speed, angle, ownRenderX, ownRenderY);
    } else {
      returnValue = scrollNoAutoScroll(ss, xValue, yValue, objectX, objectY, angle);
    }
  }

  return returnValue;
}

bool scrollCheck(BYTE xValue, BYTE yValue, BYTE objectX, BYTE objectY) {
  bool returnValue;

  returnValue = TRUE;
  if ((objectX - xValue) > MAIN_SCREEN_SIZE_X) {
    xValue++;
    returnValue = FALSE;
  }
  if ((objectX-1) < xValue) {
    xValue--;
    returnValue = FALSE;
  }
  if ((objectY - yValue) > MAIN_SCREEN_SIZE_Y) {
    returnValue = FALSE;
  }
  if (objectY <= yValue) {
   returnValue = FALSE;
  }

  return returnValue;
}


bool scrollManual(ScrollState *ss, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, TURNTYPE angle) {
  bool returnValue;
  bool leftPos;
  bool rightPos;
  bool upPos;
  bool downPos;

  leftPos = FALSE;
  rightPos = FALSE;
  upPos = FALSE;
  downPos = FALSE;
  returnValue = FALSE;
  ss->autoScrollOverRide = TRUE;
  ss->subPosX = 0;
  ss->subPosY = 0;

  if (angle >= BRADIANS_SSWEST && angle <= BRADIANS_NNWEST) {
    rightPos = TRUE;
  }
  if (angle >= BRADIANS_NNEAST && angle <= BRADIANS_SSEAST) {
    leftPos = TRUE;
  }
  if (angle >= BRADIANS_SEASTE  && angle <= BRADIANS_SWESTW) {
    downPos = TRUE;
  }
  if (angle <= BRADIANS_NEASTE || angle >= BRADIANS_NWESTW) {
    upPos = TRUE;
  }


  if ((objectX - (*xValue)) > MAIN_SCREEN_SIZE_X && leftPos == TRUE) {
    (*xValue)++;
    returnValue = TRUE;
  }
  if ((objectX-2) < (*xValue) && rightPos == FALSE) {
    (*xValue)--;
    returnValue = TRUE;
  }
  if ((objectY - (*yValue)) > MAIN_SCREEN_SIZE_Y && downPos == TRUE) {
    (*yValue)++;
    returnValue = TRUE;
  }
  if (objectY <= (*yValue) && upPos == TRUE) {
   (*yValue)--;
   returnValue = TRUE;
  }
  if (ss->mods == TRUE && returnValue == TRUE) {
    ss->mods = FALSE;
  }

  return returnValue;
}

bool scrollNoAutoScroll(ScrollState *ss, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, TURNTYPE angle) {
  bool returnValue;
  bool leftPos;
  bool rightPos;
  bool upPos;
  bool downPos;

  leftPos = FALSE;
  rightPos = FALSE;
  upPos = FALSE;
  downPos = FALSE;
  returnValue = FALSE;
  ss->subPosX = 0;
  ss->subPosY = 0;

  if (angle >= BRADIANS_SSWEST && angle <= BRADIANS_NNWEST) {
    rightPos = TRUE;
  }
  if (angle >= BRADIANS_NNEAST && angle <= BRADIANS_SSEAST) {
    leftPos = TRUE;
  }
  if (angle >= BRADIANS_SEASTE  && angle <= BRADIANS_SWESTW) {
    downPos = TRUE;
  }
  if (angle <= BRADIANS_NEASTE || angle >= BRADIANS_NWESTW) {
    upPos = TRUE;
  }

  if ((objectX - (*xValue)) >= (MAIN_SCREEN_SIZE_X-NO_SCROLL_EDGE) && leftPos == TRUE) {
    (*xValue)++;
    returnValue = TRUE;
  }
  if ((objectX-1) < (*xValue)+NO_SCROLL_EDGE && rightPos == TRUE) {
    (*xValue)--;
    returnValue = TRUE;
  }
  if ((objectY - (*yValue)) >= (MAIN_SCREEN_SIZE_Y-NO_SCROLL_EDGE) && downPos == TRUE) {
    (*yValue)++;
    returnValue = TRUE;
  }
  if (objectY <= (*yValue)+NO_SCROLL_EDGE && upPos == TRUE) {
   (*yValue)--;
   returnValue = TRUE;
  }
  if (ss->mods == TRUE && returnValue == TRUE) {
    ss->mods = FALSE;
  }
  if (returnValue == TRUE) {
    ss->autoScrollOverRide = FALSE;
  }

  return returnValue;
}

/* Ease one view axis (sub-tile units) toward the whole tile nearest the
 * natural tank-centred target, so a stopped view settles onto a tile boundary
 * rather than resting between tiles. */
static int scrollSettleToTile(int cur, int natural, int maxSub) {
  /* Aim at the whole tile nearest the natural target, then ease toward it
   * (sub-units per tick = remaining / DIVISOR, with a SNAP window that jumps
   * the final bit so it actually lands). `natural` is clamped to [0, maxSub]
   * by the caller, so the truncating round is well-defined. */
  int target = ((natural + AUTOSCROLL_SUB_PER_TILE / 2) / AUTOSCROLL_SUB_PER_TILE) * AUTOSCROLL_SUB_PER_TILE;
  int d;
  if (target < 0)      target = 0;
  if (target > maxSub) target = maxSub;
  d = target - cur;
  if (d <= AUTOSCROLL_SETTLE_SNAP && d >= -AUTOSCROLL_SETTLE_SNAP) return target;
  return cur + d / AUTOSCROLL_SETTLE_DIVISOR + (d > 0 ? 1 : -1);
}

bool scrollAutoScroll(ScrollState *ss, GameSim *sim, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, BYTE gunsightX, BYTE gunsightY, BYTE speed, TURNTYPE angle, WORLD ownRenderX, WORLD ownRenderY) {
  /*
   * Event-driven autoscroll with sub-tile precision. The view target is
   * tank's sub-pixel world position + currentOffsetSub. The whole-tile
   * targetOffset is only recomputed on discrete events:
   *
   *   A. gunsight just crossed outside the view (one-shot)
   *   B. a threat entered the concern radius that wasn't there last tick
   *   C. parked ≥ AUTOSCROLL_PARKED_TICKS with a threat in the rear hemisphere
   *
   * Between events targetOffset is fixed; currentOffsetSub eases toward
   * it at AUTOSCROLL_SUB_STEP sub-units per tick. The renderer reads the
   * decomposed sub-tile remainder (ss->subPosX/Y) every frame and folds
   * it into the drag offset, so the view glides continuously across
   * tile boundaries with no snap.
   *
   * Threat set = enemy tanks + enemy pills + neutral pills. Friendlies
   * and all bases are ignored (they don't shoot the viewing player).
   */
  BYTE myPlayer = sim->viewPlayer;
  BYTE inViewX = *xValue;
  BYTE inViewY = *yValue;
  bool triggered = FALSE;
  const char *reason = NULL;
  bool curThreatTank[MAX_TANKS];
  bool curThreatPill[MAX_PILLS];
  bool gunsightInside;
  int i;

  g_autoscrollTick++;

  /* Manual freelook: arrow keys (clientRenderFrame) set autoScrollOverRide
   * to TRUE after nudging *xValue/*yValue. The camera stays where the
   * player put it for as long as the tank is parked or driving safely
   * inside the view. Autoscroll re-engages automatically the moment the
   * player is driving AND the tank reaches within NO_SCROLL_EDGE tiles
   * of any screen edge: that's exactly when the held view would start to
   * lose the tank, and it matches the classic "look around while parked,
   * snap back to follow when you drive off" feel. On re-engage we clear
   * the override and re-seed the event baseline (initialized = FALSE) so
   * no spurious gunsight/threat trigger fires on the handover, then fall
   * through to the normal follow logic below which re-centres. */
  if (ss->autoScrollOverRide) {
    bool nearLeft  = ((int)objectX - 1)            <  ((int)*xValue + NO_SCROLL_EDGE);
    bool nearRight = ((int)objectX - (int)*xValue) >= (MAIN_SCREEN_SIZE_X - NO_SCROLL_EDGE);
    bool nearTop   = ((int)objectY)                <= ((int)*yValue + NO_SCROLL_EDGE);
    bool nearBot   = ((int)objectY - (int)*yValue) >= (MAIN_SCREEN_SIZE_Y - NO_SCROLL_EDGE);
    bool nearEdge  = nearLeft || nearRight || nearTop || nearBot;

    if (speed > 0 && nearEdge) {
      autoscrollLog("[t=%u] OVERRIDE_RELEASE view=(%u,%u) tank=(%u,%u) tankView=(%d,%d) speed=%u angle=%d\n",
                    (unsigned)g_autoscrollTick,
                    (unsigned)*xValue, (unsigned)*yValue,
                    (unsigned)objectX, (unsigned)objectY,
                    (int)objectX - (int)*xValue, (int)objectY - (int)*yValue,
                    (unsigned)speed, (int)angle);
      ss->autoScrollOverRide = FALSE;
      ss->initialized = FALSE;   /* re-seed baseline on handover */
      /* fall through to normal autoscroll follow below */
    } else {
      autoscrollLog("[t=%u] OVERRIDE HOLD view=(%u,%u) tank=(%u,%u) tankView=(%d,%d) speed=%u angle=%d\n",
                    (unsigned)g_autoscrollTick,
                    (unsigned)*xValue, (unsigned)*yValue,
                    (unsigned)objectX, (unsigned)objectY,
                    (int)objectX - (int)*xValue, (int)objectY - (int)*yValue,
                    (unsigned)speed, (int)angle);
      ss->subPosX = 0;
      ss->subPosY = 0;
      ss->mods    = FALSE;
      ss->scrollX = 0;
      ss->scrollY = 0;
      return FALSE;
    }
  }

  /* Event A: gunsight crossed view edge (rising-edge only). */
  gunsightInside =
      ((int)gunsightX >= (int)*xValue) &&
      ((int)gunsightX - 1 < (int)*xValue + MAIN_SCREEN_SIZE_X) &&
      ((int)gunsightY >= (int)*yValue) &&
      ((int)gunsightY - 1 < (int)*yValue + MAIN_SCREEN_SIZE_Y);
  if (gunsightInside == FALSE && ss->gunsightWasInside == TRUE) {
    triggered = TRUE;
    reason = "gunsight_edge";
  }
  ss->gunsightWasInside = gunsightInside;

  /* Event B: build "currently threatening" set, flag newly-entered. */
  for (i = 0; i < MAX_TANKS; i++) curThreatTank[i] = FALSE;
  for (i = 0; i < MAX_PILLS; i++) curThreatPill[i] = FALSE;

  for (i = 0; i < MAX_TANKS; i++) {
    int tx, ty;
    if (!isThreatTank(sim, myPlayer, i, &tx, &ty)) continue;
    if (abs(tx - (int)objectX) > AUTOSCROLL_CONCERN_RADIUS) continue;
    if (abs(ty - (int)objectY) > AUTOSCROLL_CONCERN_RADIUS) continue;
    curThreatTank[i] = TRUE;
    if (!ss->prevThreatTank[i]) {
      triggered = TRUE;
      if (reason == NULL) reason = "new_threat_tank";
    }
  }
  for (i = 1; i <= sim->pb->numPills; i++) {
    pillbox p;
    if (!isThreatPill(sim, myPlayer, i, &p)) continue;
    if (abs((int)p.x - (int)objectX) > AUTOSCROLL_CONCERN_RADIUS) continue;
    if (abs((int)p.y - (int)objectY) > AUTOSCROLL_CONCERN_RADIUS) continue;
    curThreatPill[i - 1] = TRUE;
    if (!ss->prevThreatPill[i - 1]) {
      triggered = TRUE;
      if (reason == NULL) reason = "new_threat_pill";
    }
  }

  /* Event C: parked ≥ N ticks with a threat in the rear hemisphere. */
  if (speed == 0) {
    if (ss->parkedSinceTick == 0) ss->parkedSinceTick = g_autoscrollTick;
    if (g_autoscrollTick - ss->parkedSinceTick >= AUTOSCROLL_PARKED_TICKS) {
      int idx = (((int)angle + 8) >> 4) & 15;
      int fx = kForwardX[idx], fy = kForwardY[idx];
      bool rearThreat = FALSE;
      int j;
      for (j = 0; j < MAX_TANKS && !rearThreat; j++) {
        int tx, ty;
        if (!curThreatTank[j]) continue;
        tx = (int)tankGetMX(&sim->tanks[j]);
        ty = (int)tankGetMY(&sim->tanks[j]);
        if ((tx - (int)objectX) * fx + (ty - (int)objectY) * fy < 0) rearThreat = TRUE;
      }
      for (j = 1; j <= sim->pb->numPills && !rearThreat; j++) {
        pillbox p;
        if (!curThreatPill[j - 1]) continue;
        pillsGetPill(&sim->pb, &p, j);
        if (((int)p.x - (int)objectX) * fx + ((int)p.y - (int)objectY) * fy < 0) rearThreat = TRUE;
      }
      if (rearThreat) {
        triggered = TRUE;
        if (reason == NULL) reason = "parked_rear";
        /* Reset the parked clock so this re-fires once per window, not every tick. */
        ss->parkedSinceTick = g_autoscrollTick;
      }
    }
  } else {
    ss->parkedSinceTick = 0;
  }

  /* First tick after fresh state: the threat set and gunsight state we
   * just observed are the BASELINE, not "newly entered" / "just crossed
   * the edge." Suppress any triggers detected above; from tick 2 onward
   * events fire normally based on real changes. */
  if (!ss->initialized) {
    if (triggered) {
      autoscrollLog("[t=%u p=%u] INIT_SUPPRESS reason=%s baseline_captured\n",
                    (unsigned)g_autoscrollTick, (unsigned)myPlayer,
                    reason ? reason : "?");
    }
    triggered = FALSE;
    reason = NULL;
    ss->initialized = TRUE;
  }

  /* Persist threat set for next tick's "newly entered" detection. */
  for (i = 0; i < MAX_TANKS; i++) ss->prevThreatTank[i] = curThreatTank[i];
  for (i = 0; i < MAX_PILLS; i++) ss->prevThreatPill[i] = curThreatPill[i];

  /* Recompute target offset on event (debounced, dead-zoned).
   * lastRecalcTick == 0 is the never-fired sentinel: first event always
   * passes regardless of debounce. */
  if (triggered &&
      (ss->lastRecalcTick == 0 ||
       (g_autoscrollTick - ss->lastRecalcTick) >= AUTOSCROLL_RECALC_DEBOUNCE)) {
    int8_t newOX = 0, newOY = 0;
    int threatCount = 0;
    int dox, doy;
    computeTargetOffset(sim, myPlayer, (int)objectX, (int)objectY,
                        speed, angle, &newOX, &newOY, &threatCount);
    dox = abs((int)newOX - (int)ss->targetOffsetX);
    doy = abs((int)newOY - (int)ss->targetOffsetY);
    if (dox > AUTOSCROLL_DEAD_ZONE || doy > AUTOSCROLL_DEAD_ZONE) {
      autoscrollLog("[t=%u p=%u] EVENT=%s tank=(%u,%u) speed=%u angle=%d threats=%d target=(%d,%d)->(%d,%d)\n",
                    (unsigned)g_autoscrollTick, (unsigned)myPlayer,
                    reason ? reason : "?",
                    (unsigned)objectX, (unsigned)objectY,
                    (unsigned)speed, (int)angle, threatCount,
                    (int)ss->targetOffsetX, (int)ss->targetOffsetY,
                    (int)newOX, (int)newOY);
      ss->targetOffsetX = newOX;
      ss->targetOffsetY = newOY;
      ss->lastRecalcTick = g_autoscrollTick;
    } else {
      autoscrollLog("[t=%u p=%u] EVENT=%s dead_zone target=(%d,%d) new=(%d,%d)\n",
                    (unsigned)g_autoscrollTick, (unsigned)myPlayer,
                    reason ? reason : "?",
                    (int)ss->targetOffsetX, (int)ss->targetOffsetY,
                    (int)newOX, (int)newOY);
      /* Still consumes the debounce window — "no change" is a decision. */
      ss->lastRecalcTick = g_autoscrollTick;
    }
  }

  /* Ease currentOffsetSub toward (targetOffset * SUB_PER_TILE) using a
   * proportional step (1/DIVISOR of remaining per tick). This produces
   * a continuous slide that decelerates near target and gracefully
   * redirects when a new event shifts target mid-slide — no "fast then
   * suddenly stop" lurches. */
  {
    int targetSubX = (int)ss->targetOffsetX * AUTOSCROLL_SUB_PER_TILE;
    int targetSubY = (int)ss->targetOffsetY * AUTOSCROLL_SUB_PER_TILE;
    int beforeSubX = ss->currentOffsetSubX;
    int beforeSubY = ss->currentOffsetSubY;
    int dx = targetSubX - (int)ss->currentOffsetSubX;
    int dy = targetSubY - (int)ss->currentOffsetSubY;
    /* Sub-pixel snap so the slide actually settles. */
    if (dx > 0 && dx <= AUTOSCROLL_SNAP_THRESHOLD)      ss->currentOffsetSubX = (int16_t)targetSubX;
    else if (dx < 0 && dx >= -AUTOSCROLL_SNAP_THRESHOLD) ss->currentOffsetSubX = (int16_t)targetSubX;
    else if (dx != 0)                                    ss->currentOffsetSubX += (int16_t)(dx / AUTOSCROLL_EASE_DIVISOR + (dx > 0 ? 1 : -1));
    if (dy > 0 && dy <= AUTOSCROLL_SNAP_THRESHOLD)      ss->currentOffsetSubY = (int16_t)targetSubY;
    else if (dy < 0 && dy >= -AUTOSCROLL_SNAP_THRESHOLD) ss->currentOffsetSubY = (int16_t)targetSubY;
    else if (dy != 0)                                    ss->currentOffsetSubY += (int16_t)(dy / AUTOSCROLL_EASE_DIVISOR + (dy > 0 ? 1 : -1));

    if ((beforeSubX != targetSubX || beforeSubY != targetSubY) &&
        ss->currentOffsetSubX == targetSubX &&
        ss->currentOffsetSubY == targetSubY) {
      autoscrollLog("[t=%u p=%u] SLIDE_DONE offset_sub=(%d,%d)\n",
                    (unsigned)g_autoscrollTick, (unsigned)myPlayer,
                    (int)ss->currentOffsetSubX, (int)ss->currentOffsetSubY);
    }
  }

  /* Apply: compute desired view position in sub-tile units from the
   * tank's rendered sub-tile world position. The hull sprite is drawn
   * from the same rendered source (render-error smoothing applies its
   * offset to both), so tank and camera move together — a reconciliation
   * correction slides the world under a fixed-relative tank rather than
   * shearing the hull against the camera. With the offset at zero this is
   * the raw tank position. Decompose into the tile-aligned *xValue/*yValue
   * (what engine code reads) plus subPosX/Y for the renderer to fold into
   * its sub-pixel drag offset. */
  {
    int tankSubX = (int)ownRenderX - TANK_SUBTRACT;
    int tankSubY = (int)ownRenderY - TANK_SUBTRACT;
    int desiredSubX = tankSubX - SCROLL_CENTER * AUTOSCROLL_SUB_PER_TILE + (int)ss->currentOffsetSubX;
    int desiredSubY = tankSubY - SCROLL_CENTER * AUTOSCROLL_SUB_PER_TILE + (int)ss->currentOffsetSubY;
    int   maxSubX = (255 - MAIN_SCREEN_SIZE_X) * AUTOSCROLL_SUB_PER_TILE;
    int   maxSubY = (255 - MAIN_SCREEN_SIZE_Y) * AUTOSCROLL_SUB_PER_TILE;

    if (desiredSubX < 0)       desiredSubX = 0;
    if (desiredSubY < 0)       desiredSubY = 0;
    if (desiredSubX > maxSubX) desiredSubX = maxSubX;
    if (desiredSubY > maxSubY) desiredSubY = maxSubY;

    /* While the tank moves the view tracks it exactly (the desiredSub above) —
     * 1:1, no lag, smooth via the renderer's sub-pixel drag. The moment it
     * stops, ease the view onto the whole tile nearest where it ended up, so
     * the framing settles tile-aligned instead of resting mid-tile. Because
     * the moving view has zero lag, there is no accumulated gap to unwind at
     * the stop — the settle only ever travels the sub-tile remainder (≤ half a
     * tile). Skipped when sub-tile precision is off (the view is already
     * whole-tile). cur = the view the renderer is currently showing. */
    if (speed == 0 && g_scrollSubTilePrecision) {
      int curSubX = (int)inViewX * AUTOSCROLL_SUB_PER_TILE + (int)ss->subPosX;
      int curSubY = (int)inViewY * AUTOSCROLL_SUB_PER_TILE + (int)ss->subPosY;
      desiredSubX = scrollSettleToTile(curSubX, desiredSubX, maxSubX);
      desiredSubY = scrollSettleToTile(curSubY, desiredSubY, maxSubY);
    }

    *xValue   = (BYTE)(desiredSubX / AUTOSCROLL_SUB_PER_TILE);
    *yValue   = (BYTE)(desiredSubY / AUTOSCROLL_SUB_PER_TILE);
    if (g_scrollSubTilePrecision) {
      ss->subPosX = (int16_t)(desiredSubX - (int)*xValue * AUTOSCROLL_SUB_PER_TILE);
      ss->subPosY = (int16_t)(desiredSubY - (int)*yValue * AUTOSCROLL_SUB_PER_TILE);
    } else {
      /* Sub-tile precision off → snap the view to whole tiles. */
      ss->subPosX = 0;
      ss->subPosY = 0;
    }

    autoscrollLog("[t=%u p=%u] DBG rawSub=(%d,%d) angle=%d speed=%u offSub=(%d,%d) view=(%u,%u) subPos=(%d,%d)\n",
                  (unsigned)g_autoscrollTick, (unsigned)myPlayer,
                  tankSubX, tankSubY,
                  (int)angle, (unsigned)speed,
                  (int)ss->currentOffsetSubX, (int)ss->currentOffsetSubY,
                  (unsigned)*xValue, (unsigned)*yValue,
                  (int)ss->subPosX, (int)ss->subPosY);
  }

  ss->mods = (ss->currentOffsetSubX != ss->targetOffsetX * AUTOSCROLL_SUB_PER_TILE ||
              ss->currentOffsetSubY != ss->targetOffsetY * AUTOSCROLL_SUB_PER_TILE);

  /* Legacy burst counters: the new design doesn't use them, but other
   * code reads scrollX/scrollY to detect "burst in progress" (e.g.
   * brain map invalidation). Keep them at zero so those checks don't
   * spuriously fire. */
  ss->scrollX = 0;
  ss->scrollY = 0;

  return (*xValue != inViewX || *yValue != inViewY);
}
