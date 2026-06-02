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


#include <math.h>
#include <stdlib.h>
#include "global.h"
#include "scroll.h"
#include "scroll_item_list.h"
#include "tank.h"
#include "players.h"
#include "pillbox.h"
#include "shells.h"
#include "bases.h"
#include "game_sim.h"


/* Autoscroll rate cap. Game runs at ~20 ticks/sec; this caps the camera
 * at one 1-tile move per N ticks on each axis. Picks (at N=5) → 4 tiles/sec
 * so an 8-tile target jump glides over ~2 seconds instead of whipping
 * across in 400 ms. Tune here and rebuild.
 *   N = 5 → 4 tiles/sec  (default)
 *   N = 4 → 5 tiles/sec
 *   N = 3 → ~6.7 tiles/sec
 *   N = 1 → 20 tiles/sec (no cap — original behaviour) */
#define AUTOSCROLL_TICKS_PER_TILE 5

/* Saturating counters: ticks since last 1-tile autoscroll fired on each
 * axis. AUTOSCROLL_TICKS_PER_TILE gates the next move. Module-level
 * because ScrollState is off-limits for this change; single viewer
 * assumption holds for normal client play. */
static BYTE g_ticksSinceScrollX = AUTOSCROLL_TICKS_PER_TILE;
static BYTE g_ticksSinceScrollY = AUTOSCROLL_TICKS_PER_TILE;

/* Smoothed facing angle for the priority/veto math (NOT the lead).
 *
 * Raw angle reorients instantly with the tank; if the item list
 * scores off the raw value, a sharp turn flips every behind item's
 * directional penalty in one frame and floods the priority list.
 * Even with the +4 flat penalty, a hard reversal can promote a
 * previously-suppressed far item enough to swing the target.
 *
 * We low-pass filter the bradian angle (wrap-aware so 240° → 30°
 * takes the short way) at SMOOTH_ANGLE_ALPHA per tick. With 0.2 the
 * 63% catchup is ~5 ticks (250 ms) — enough that a turn-and-back
 * within that window barely budges the priorities.
 *
 * Reset to current angle whenever the tank is stationary (speed 0)
 * so a stop-then-go in a new direction doesn't carry stale lag.
 *
 * The lead vector still uses raw `angle` so the camera doesn't
 * briefly aim backwards during a turn. */
#define SMOOTH_ANGLE_ALPHA 0.2f
static float g_smoothAngleBR        = 0.0f;
static BYTE  g_smoothAngleLastSpeed = 0;

/* Forward-important hysteresis.
 *
 * The directional veto in scrollItemListProcess engages when any
 * non-special item is more than 1 tile ahead of the tank in the
 * facing direction. That's a binary threshold: a small angle change
 * can drop a near-threshold item below 1 tile of forward component
 * and disengage the veto, which lets a previously-vetoed behind item
 * suddenly pull the camera by several tiles. The user observed this
 * as a stationary-ish turn producing small up/down/up bumps.
 *
 * To bridge the wobble, once the natural check has fired TRUE we keep
 * the effective flag TRUE for FORWARD_IMP_STICKY_TICKS more ticks
 * even if the natural check briefly flips off. A real direction
 * change (sustained absence of items ahead) lets the counter expire
 * and the natural FALSE through. */
#define FORWARD_IMP_STICKY_TICKS 15
static BYTE g_forwardImpStickyTicks = 0;

/* Observation-based lead vector.
 *
 * The old lead computed where the tank "would be" in 1 second from
 * its engine `speed` and `angle`. Two problems:
 *  1. If the tank is wedged against a wall, speed > 0 but the tank
 *     isn't actually moving — yet the lead still pushes the target
 *     forward, drifting the camera off a stationary tank.
 *  2. On a sharp turn the lead instantly snaps to the new facing
 *     direction even though the tank's real velocity is still
 *     rotating in; the lead-driven target jump is the "wonky on
 *     turn" feeling.
 *
 * Replace it with a smoothed per-tick measured delta: lead reflects
 * what the tank actually did, not what speed claims. Stuck → zero
 * lead. Mid-turn → lead direction lags the angle by a few ticks,
 * matching how the tank's velocity vector is actually rotating. */
#define SMOOTH_MOTION_ALPHA 0.2f
/* Maximum lead distance in world units (256 wu = 1 tile).
 * Lower = camera commits less to direction-of-motion; reduces visible
 * shifting when the tank reverses or weaves. 384 = 1.5 tiles, half
 * the prior 3-tile lead. Bump to 256 (1 tile) for even less commit. */
#define AUTOSCROLL_MAX_LEAD_WORLD 384
static WORLD g_prevTankWX     = 0;
static WORLD g_prevTankWY     = 0;
static float g_smoothMotionDX = 0.0f;
static float g_smoothMotionDY = 0.0f;
static bool  g_motionInit     = FALSE;


void scrollCreate(ScrollState *ss) {
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
  scrollItemListCreate(&ss->itemList);
}


void scrollSetScrollType(ScrollState *ss, bool isAuto) {
  ss->autoScroll = isAuto;
  ss->autoScrollOverRide = FALSE;
  if (isAuto == FALSE) {
    ss->scrollX = 0;
    ss->scrollY = 0;
    ss->mods = FALSE;
    ss->stickyX = FALSE;
    ss->stickyY = FALSE;
  }
}

void scrollCenterObject(ScrollState *ss, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY) {
  *xValue = objectX - SCROLL_CENTER;
  *yValue = objectY - SCROLL_CENTER;
  ss->autoScrollOverRide = FALSE;
  ss->stickyX = FALSE;
  ss->stickyY = FALSE;
}

bool scrollUpdate(ScrollState *ss, GameSim *sim, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, bool isTank, BYTE gunsightX, BYTE gunsightY, BYTE speed, BYTE armour, TURNTYPE angle, bool manual, bool tankIsDead) {
  bool returnValue;

  returnValue = TRUE;
  if (tankIsDead == TRUE) {
    returnValue = FALSE;
  } else if (manual == TRUE) {
    returnValue = scrollManual(ss, xValue, yValue, objectX, objectY, angle);
  } else if (ss->autoScroll == TRUE && isTank == TRUE && armour <= TANK_FULL_ARMOUR) {
    returnValue = scrollAutoScroll(ss, sim, xValue, yValue, objectX, objectY, gunsightX, gunsightY, speed, angle);
  } else {
    returnValue = scrollNoAutoScroll(ss, xValue, yValue, objectX, objectY, angle);
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

bool scrollAutoScroll(ScrollState *ss, GameSim *sim, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, BYTE gunsightX, BYTE gunsightY, BYTE speed, TURNTYPE angle) {
  /*
   * Priority-based viewport scrolling.
   *
   * The viewport's natural resting position leads the tank in its
   * facing direction (capped at LEAD_MAX_TILES). Items pull the target
   * back toward themselves, sorted by score; conflict-validation keeps
   * higher-priority items on screen.
   *
   * The directional veto lives in scrollItemListProcess: when the tank
   * is moving and there's an important item ahead, behind items can't
   * pull the target against the facing direction. That kills the
   * leading-edge-vs-trailing-edge oscillation at its source — items
   * behind fall off the back rather than fighting the lead.
   *
   * Scrolls at most 1 tile per tick.
   */
  int targetX, targetY;
  int viewRefX, viewRefY;
  int xmove, ymove;
  int leadDX, leadDY;
  bool scrolled;
  bool xAllowed, yAllowed;
  BYTE myPlayer;
  WORLD tankWX, tankWY;
  WORLD gunsightWX, gunsightWY;
  int count;
  shells current;
  shells bestShell;

  myPlayer = sim->viewPlayer;
  tankGetWorld(&sim->tanks[myPlayer], &tankWX, &tankWY);

  /* Find the player's longest-lived live shell (the "fireball" the
   * camera follows). When present, the screen tracks the shell instead
   * of leading the tank. */
  bestShell = NULL;
  current = sim->shs;
  while (current != NULL) {
    if (current->owner == myPlayer && current->shellDead == FALSE) {
      if (bestShell == NULL || current->length > bestShell->length) {
        bestShell = current;
      }
    }
    current = current->next;
  }

  /* Phase 1: initial target leads the tank in its facing direction.
   * The +1 tile inset matches viewRefX so a tank-centered, no-lead
   * viewport produces zero scroll delta. */
  /* Update the smoothed motion estimate from the per-tick tank delta.
   * Reset on init or on a teleport-sized jump (e.g. respawn). */
  {
    int dx = (int)tankWX - (int)g_prevTankWX;
    int dy = (int)tankWY - (int)g_prevTankWY;
    if (g_motionInit == FALSE ||
        abs(dx) > (4 << 8) || abs(dy) > (4 << 8)) {
      g_smoothMotionDX = 0.0f;
      g_smoothMotionDY = 0.0f;
      g_motionInit = TRUE;
    } else {
      g_smoothMotionDX += SMOOTH_MOTION_ALPHA * ((float)dx - g_smoothMotionDX);
      g_smoothMotionDY += SMOOTH_MOTION_ALPHA * ((float)dy - g_smoothMotionDY);
    }
    g_prevTankWX = tankWX;
    g_prevTankWY = tankWY;
  }

  /* Lead = smoothed per-tick motion projected forward. The 50× factor
   * matches the existing calculateProjectedPosition convention so the
   * resulting magnitudes are comparable to the previous behaviour;
   * 3-tile clamp unchanged. No lead while the screen tracks a shell. */
  leadDX = 0;
  leadDY = 0;
  if (bestShell == NULL) {
    int len2;
    const int leadMaxWorld = AUTOSCROLL_MAX_LEAD_WORLD;
    leadDX = (int)(g_smoothMotionDX * 50.0f);
    leadDY = (int)(g_smoothMotionDY * 50.0f);
    len2 = leadDX * leadDX + leadDY * leadDY;
    if (len2 > leadMaxWorld * leadMaxWorld) {
      double len = sqrt((double)len2);
      leadDX = (int)((double)leadDX * (double)leadMaxWorld / len);
      leadDY = (int)((double)leadDY * (double)leadMaxWorld / len);
    }
  }
  targetX = (int)tankWX + leadDX - SCREEN_WORLD_W / 2 + (1 << 8);
  targetY = (int)tankWY + leadDY - SCREEN_WORLD_H / 2 + (1 << 8);

  viewRefX = ((int)*xValue + 1) << 8;
  viewRefY = ((int)*yValue + 1) << 8;

  /* scrollAutoScroll is only called when manual==FALSE, so clear any
   * leftover manual override. */
  ss->autoScrollOverRide = FALSE;

  /* Update smoothed angle for priority/veto. Snap (no lag) on any
   * stationary frame so a stop-then-restart doesn't carry over. */
  {
    float currentBR = (float)angle;
    if (g_smoothAngleLastSpeed == 0 || speed == 0) {
      g_smoothAngleBR = currentBR;
    } else {
      float delta = currentBR - g_smoothAngleBR;
      while (delta >  128.0f) delta -= 256.0f;
      while (delta < -128.0f) delta += 256.0f;
      g_smoothAngleBR += SMOOTH_ANGLE_ALPHA * delta;
      while (g_smoothAngleBR <    0.0f) g_smoothAngleBR += 256.0f;
      while (g_smoothAngleBR >= 256.0f) g_smoothAngleBR -= 256.0f;
    }
    g_smoothAngleLastSpeed = speed;
  }

  /* Phase 2: build the priority item list */
  scrollItemListCreate(&ss->itemList);
  ss->itemList.tankAngle = (TURNTYPE)g_smoothAngleBR;
  ss->itemList.tankSpeed = speed;
  ss->itemList.tankWX = tankWX;
  ss->itemList.tankWY = tankWY;
  /* Carry forward the directional veto across short natural-FALSE
   * blips while the sticky counter is alive — see
   * FORWARD_IMP_STICKY_TICKS comment. */
  ss->itemList.forceForwardImportant = (g_forwardImpStickyTicks > 0) ? TRUE : FALSE;

  /* Always add own tank (score 0 — must stay on screen) */
  scrollItemListAddW(&ss->itemList, tankWX, tankWY, 0, 0, FALSE, tankWX, tankWY);

  if (bestShell != NULL) {
    /* Shell active: track it exclusively at score 0. */
    scrollItemListAddW(&ss->itemList, bestShell->x, bestShell->y, 0, 0, FALSE, tankWX, tankWY);
  } else {
    pillbox pill;
    pillAlliance pa;
    BYTE tx, ty;
    WORLD otherWX, otherWY;
    int tankMX = (int)(tankWX >> 8);
    int tankMY = (int)(tankWY >> 8);

    /* Other tanks (enemy or ally) */
    for (count = 0; count < MAX_TANKS; count++) {
      int baseScore;
      if (sim->tanks[count] == NULL) continue;
      if (count == myPlayer) continue;
      tx = tankGetMX(&sim->tanks[count]);
      ty = tankGetMY(&sim->tanks[count]);
      if (tx == 0 && ty == 0) continue;
      if (abs((int)tx - tankMX) > MAIN_SCREEN_SIZE_X ||
          abs((int)ty - tankMY) > MAIN_SCREEN_SIZE_Y) continue;
      if (playersIsAllie(&sim->plyrs, myPlayer, (BYTE)count) == TRUE) {
        baseScore = 2;
      } else {
        baseScore = 1;
      }
      tankGetWorld(&sim->tanks[count], &otherWX, &otherWY);
      if (tankGetSpeed(&sim->tanks[count]) > 0) {
        WORLD projOX, projOY;
        calculateProjectedPosition(otherWX, otherWY, tankGetSpeed(&sim->tanks[count]),
                                   tankGetAngle(&sim->tanks[count]), 1.0f, &projOX, &projOY);
        scrollItemListAddW(&ss->itemList, projOX, projOY, baseScore, 1, FALSE, tankWX, tankWY);
      } else {
        scrollItemListAddW(&ss->itemList, otherWX, otherWY, baseScore, 1, FALSE, tankWX, tankWY);
      }
    }

    /* Pillboxes (1-indexed API) */
    for (count = 1; count <= sim->pb->numPills; count++) {
      int baseScore;
      pillsGetPill(&sim->pb, &pill, count);
      if (pill.armour == 0) {
        continue;
      }
      if (abs((int)pill.x - tankMX) > MAIN_SCREEN_SIZE_X ||
          abs((int)pill.y - tankMY) > MAIN_SCREEN_SIZE_Y) {
        continue;
      }
      pa = pillsGetAllianceNum(sim, &sim->pb, count);
      if (pa == pillEvil || pa == pillNeutral) {
        baseScore = 1;
      } else {
        baseScore = 2;
      }
      scrollItemListAddM(&ss->itemList, pill.x, pill.y, baseScore, 1, FALSE, tankWX, tankWY);
    }

    /* Bases (1-indexed API) */
    for (count = 1; count <= sim->bs->numBases; count++) {
      base b;
      basesGetBase(&sim->bs, &b, count);
      if (b.armour <= BASE_DEAD) {
        continue;
      }
      if (abs((int)b.x - tankMX) > MAIN_SCREEN_SIZE_X ||
          abs((int)b.y - tankMY) > MAIN_SCREEN_SIZE_Y) {
        continue;
      }
      scrollItemListAddM(&ss->itemList, b.x, b.y, 3, 2, FALSE, tankWX, tankWY);
    }

    /* Gunsight crosshairs (score 4, special — exempt from veto so the
     * player can always see what they're aiming at, even if behind). */
    gunsightWX = ((WORLD)gunsightX << 8) | 0x80;
    gunsightWY = ((WORLD)gunsightY << 8) | 0x80;
    scrollItemListAddW(&ss->itemList, gunsightWX, gunsightWY, 4, 0, TRUE, tankWX, tankWY);
  }

  /* Phase 3: sort by score */
  scrollItemListSort(&ss->itemList);

  /* Phase 4: process — adjust target, validate conflicts, apply veto */
  scrollItemListProcess(&ss->itemList, &targetX, &targetY);

  /* Refresh / decay the forward-important sticky counter based on
   * what the natural check decided this tick. */
  if (ss->itemList.lastNaturalForwardImportant == TRUE) {
    g_forwardImpStickyTicks = FORWARD_IMP_STICKY_TICKS;
  } else if (g_forwardImpStickyTicks > 0) {
    g_forwardImpStickyTicks--;
  }

  /* Phase 5: execute scroll (at most 1 tile per tick).
   *
   * Deadband hysteresis: only scroll when target is at least 1 full
   * tile from viewRef. A previous floor-align approach
   * (targetX &= ~0xFF; xmove = target - viewRef) used the *same*
   * threshold for "scroll forward" and "scroll back" at each tile
   * boundary, which made the viewport bounce whenever target wobbled
   * sub-pixel across that boundary — from sub-pixel tank motion,
   * integer speed-step lead recomputation, or tank-turn lead
   * reorientation. With a 1-tile deadband, scroll-forward fires at
   * target=viewRef+1 and scroll-back at target=viewRef-1, leaving a
   * 2-tile stable band centred on viewRef. */
  scrolled = FALSE;
  {
    int deltaX = targetX - viewRefX;
    int deltaY = targetY - viewRefY;
    const int DEADBAND = (1 << 8);  /* 1 tile in world units */
    xmove = (deltaX >=  DEADBAND) ?  DEADBAND
          : (deltaX <= -DEADBAND) ? -DEADBAND : 0;
    ymove = (deltaY >=  DEADBAND) ?  DEADBAND
          : (deltaY <= -DEADBAND) ? -DEADBAND : 0;
  }

  /* Rate cap: independently gate each axis on its own cooldown so a
   * diagonal move doesn't lock the slower-recovering axis. Counters
   * advance every autoscroll tick regardless of whether a scroll
   * fires, so a long quiet period leaves both axes ready to move. */
  if (g_ticksSinceScrollX < 255) g_ticksSinceScrollX++;
  if (g_ticksSinceScrollY < 255) g_ticksSinceScrollY++;
  xAllowed = (g_ticksSinceScrollX >= AUTOSCROLL_TICKS_PER_TILE) ? TRUE : FALSE;
  yAllowed = (g_ticksSinceScrollY >= AUTOSCROLL_TICKS_PER_TILE) ? TRUE : FALSE;

  if (xmove != 0 && xAllowed == TRUE) {
    if (xmove > 0) {
      (*xValue)++;
    } else if (*xValue > 0) {
      (*xValue)--;
    }
    g_ticksSinceScrollX = 0;
    scrolled = TRUE;
  }
  if (ymove != 0 && yAllowed == TRUE) {
    if (ymove > 0) {
      (*yValue)++;
    } else if (*yValue > 0) {
      (*yValue)--;
    }
    g_ticksSinceScrollY = 0;
    scrolled = TRUE;
  }

  if (scrolled == TRUE) {
    if (*xValue > 255 - MAIN_SCREEN_SIZE_X) *xValue = 255 - MAIN_SCREEN_SIZE_X;
    if (*yValue > 255 - MAIN_SCREEN_SIZE_Y) *yValue = 255 - MAIN_SCREEN_SIZE_Y;
  }

  return scrolled;
}
