/*
 * $Id$
 *
 * Copyright (c) 1998-2008 John Morrison.
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

bool scrollUpdate(ScrollState *ss, GameSim *sim, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, bool isTank, BYTE gunsightX, BYTE gunsightY, BYTE speed, BYTE armour, TURNTYPE angle, bool manual, bool tankIsDead) {
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
  } else {
    /* SCROLL_MECH_ENHANCED — John's current sub-tile / threat-aware path. */
    if (manual == TRUE) {
      returnValue = scrollManual(ss, xValue, yValue, objectX, objectY, angle);
    } else if (ss->autoScroll == TRUE && isTank == TRUE && armour <= TANK_FULL_ARMOUR) {
      returnValue = scrollAutoScroll(ss, sim, xValue, yValue, objectX, objectY, gunsightX, gunsightY, speed, angle);
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

bool scrollAutoScroll(ScrollState *ss, GameSim *sim, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, BYTE gunsightX, BYTE gunsightY, BYTE speed, TURNTYPE angle) {
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
   * tank's raw sub-tile world position. The tank sprite is drawn from
   * the same raw source, so tank and camera move together — bouncing
   * in tank.x (predict/reconcile ~25Hz) shows as a small world-jitter
   * with the tank fixed relative to the view, not as a tank shake.
   * Decompose into the tile-aligned *xValue/*yValue (what engine code
   * reads) plus subPosX/Y for the renderer to fold into its sub-pixel
   * drag offset. */
  {
    int tankSubX = (int)(sim->tanks[myPlayer])->x - TANK_SUBTRACT;
    int tankSubY = (int)(sim->tanks[myPlayer])->y - TANK_SUBTRACT;
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
