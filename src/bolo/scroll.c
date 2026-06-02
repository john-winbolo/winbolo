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


#include <stdlib.h>
#include "global.h"
#include "scroll.h"
#include "tank.h"
#include "players.h"
#include "pillbox.h"
#include "bases.h"
#include "game_sim.h"

/* Per-direction lockout for the secondary edge-keeper.
 *
 * Any view-move on an axis (primary burst or secondary nudge) resets
 * the matching direction's counter to zero. The OPPOSITE direction's
 * secondary is then suppressed until OPPOSITE_LOCKOUT_TICKS have
 * elapsed. This prevents short-term ping-pong between primary and
 * secondary right after a burst. */
#define OPPOSITE_LOCKOUT_TICKS 30
static BYTE g_ticksSinceMoveE = 255;
static BYTE g_ticksSinceMoveW = 255;
static BYTE g_ticksSinceMoveN = 255;
static BYTE g_ticksSinceMoveS = 255;

/* Facing-aware forward vector lookup (16 directions, sin/cos * 256).
 * Used to skip items that sit strictly *behind* the tank when the
 * player is driving — those items naturally fall off the trailing
 * edge and shouldn't be allowed to drag the camera backward. The
 * cooldown above kills short-term reversals; this kills long-term
 * "drift back to items behind me" dips. */
static const int kForwardX[16] = {
     0,   98,  181,  237,  256,  237,  181,   98,
     0,  -98, -181, -237, -256, -237, -181,  -98
};
static const int kForwardY[16] = {
  -256, -237, -181,  -98,    0,   98,  181,  237,
   256,  237,  181,   98,    0,  -98, -181, -237
};


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
   * Gunsight-edge-driven autoscroll, with a secondary edge-keeper for
   * nearby objects of interest.
   *
   * Primary: when the gunsight reaches a screen edge, kick off a
   * speed-proportional scroll burst toward it (original Bolo
   * behaviour). The burst counters in ScrollState rate-limit the next
   * trigger naturally.
   *
   * Secondary: when no burst is active and the gunsight isn't on an
   * edge, allow a single 1-tile nudge if an interesting item (other
   * tank, live pillbox, live base) sits on the very edge of the view.
   * This keeps "something sneaking up behind you" visible without
   * continuously chasing every object on screen.
   */
  bool returnValue;
  BYTE myPlayer;
  int count;
  BYTE inViewX = *xValue;
  BYTE inViewY = *yValue;

  returnValue = FALSE;
  ss->autoScrollOverRide = FALSE;
  myPlayer = sim->viewPlayer;

  if (ss->scrollX == 0 && ss->scrollY == 0) {
    bool triggered = FALSE;

    /* Primary: gunsight reaching a screen edge. */
    if (((gunsightX - 1) - (*xValue)) >= MAIN_SCREEN_SIZE_X) {
      ss->scrollX = (BYTE)(speed / SCROLL_DIVIDE);
      if (ss->scrollX == 0) ss->scrollX = 1;
      ss->xPositive = TRUE;
      ss->mods = TRUE;
      triggered = TRUE;
    }
    if (gunsightX < (*xValue)) {
      ss->scrollX = (BYTE)(speed / SCROLL_DIVIDE);
      if (ss->scrollX == 0) ss->scrollX = 1;
      ss->xPositive = FALSE;
      ss->mods = TRUE;
      triggered = TRUE;
    }
    if (((gunsightY - 1) - (*yValue)) >= MAIN_SCREEN_SIZE_Y) {
      ss->scrollY = (BYTE)(speed / SCROLL_DIVIDE);
      if (ss->scrollY == 0) ss->scrollY = 1;
      ss->yPositive = TRUE;
      ss->mods = TRUE;
      triggered = TRUE;
    }
    if (gunsightY < (*yValue)) {
      ss->scrollY = (BYTE)(speed / SCROLL_DIVIDE);
      if (ss->scrollY == 0) ss->scrollY = 1;
      ss->yPositive = FALSE;
      ss->mods = TRUE;
      triggered = TRUE;
    }

    /* Secondary: bias the view toward unopposed threats at an edge.
     * Each direction is suppressed while the OPPOSITE direction has
     * fired recently, so a finishing primary burst can't be undone
     * by a stale secondary pull from the trailing edge. */
    if (triggered == FALSE) {
      bool eastOK  = (g_ticksSinceMoveW >= OPPOSITE_LOCKOUT_TICKS);
      bool westOK  = (g_ticksSinceMoveE >= OPPOSITE_LOCKOUT_TICKS);
      bool northOK = (g_ticksSinceMoveS >= OPPOSITE_LOCKOUT_TICKS);
      bool southOK = (g_ticksSinceMoveN >= OPPOSITE_LOCKOUT_TICKS);
      /* When driving, only count items that are NOT behind the tank
       * (dot(item-tank, facing) >= 0). Behind items are drifting off
       * the trailing edge by design — they shouldn't bias the camera
       * back. Stationary players see in all directions equally. */
      bool useFacingFilter = (speed > 0);
      int fx = 0, fy = 0;
      if (useFacingFilter) {
        int idx = (((int)angle + 8) >> 4) & 15;
        fx = kForwardX[idx];
        fy = kForwardY[idx];
      }

      {
        BYTE viewX = *xValue;
        BYTE viewY = *yValue;
        int rightCol  = (int)viewX + MAIN_SCREEN_SIZE_X - 1;
        int bottomRow = (int)viewY + MAIN_SCREEN_SIZE_Y - 1;
        int eastCount = 0, westCount = 0;
        int northCount = 0, southCount = 0;

        /* Count items sitting at each edge (on the edge column/row or
         * one tile off it). Edges are mutually exclusive per axis: an
         * item can only be at the east OR west edge, not both. While
         * the tank is driving, items strictly behind the facing
         * direction are skipped — they would otherwise pull the camera
         * back into the trailing edge. */
        for (count = 0; count < MAX_TANKS; count++) {
          int tx, ty;
          if (sim->tanks[count] == NULL) continue;
          if (count == myPlayer) continue;
          tx = (int)tankGetMX(&sim->tanks[count]);
          ty = (int)tankGetMY(&sim->tanks[count]);
          if (tx == 0 && ty == 0) continue;
          if (useFacingFilter) {
            int dot = (tx - (int)objectX) * fx + (ty - (int)objectY) * fy;
            if (dot < 0) continue;
          }
          if (tx >= rightCol && tx <= rightCol + 1) eastCount++;
          else if (tx >= (int)viewX - 1 && tx <= (int)viewX) westCount++;
          if (ty >= bottomRow && ty <= bottomRow + 1) southCount++;
          else if (ty >= (int)viewY - 1 && ty <= (int)viewY) northCount++;
        }

        for (count = 1; count <= sim->pb->numPills; count++) {
          pillbox pill;
          pillsGetPill(&sim->pb, &pill, count);
          if (pill.armour == 0) continue;
          if (useFacingFilter) {
            int dot = ((int)pill.x - (int)objectX) * fx + ((int)pill.y - (int)objectY) * fy;
            if (dot < 0) continue;
          }
          if ((int)pill.x >= rightCol && (int)pill.x <= rightCol + 1) eastCount++;
          else if ((int)pill.x >= (int)viewX - 1 && (int)pill.x <= (int)viewX) westCount++;
          if ((int)pill.y >= bottomRow && (int)pill.y <= bottomRow + 1) southCount++;
          else if ((int)pill.y >= (int)viewY - 1 && (int)pill.y <= (int)viewY) northCount++;
        }

        for (count = 1; count <= sim->bs->numBases; count++) {
          base b;
          basesGetBase(&sim->bs, &b, count);
          if (b.armour <= BASE_DEAD) continue;
          if (useFacingFilter) {
            int dot = ((int)b.x - (int)objectX) * fx + ((int)b.y - (int)objectY) * fy;
            if (dot < 0) continue;
          }
          if ((int)b.x >= rightCol && (int)b.x <= rightCol + 1) eastCount++;
          else if ((int)b.x >= (int)viewX - 1 && (int)b.x <= (int)viewX) westCount++;
          if ((int)b.y >= bottomRow && (int)b.y <= bottomRow + 1) southCount++;
          else if ((int)b.y >= (int)viewY - 1 && (int)b.y <= (int)viewY) northCount++;
        }

        /* Fire only when one side is empty AND the chosen direction
         * isn't blocked by recent opposite-axis camera momentum. */
        if (eastOK && eastCount > 0 && westCount == 0) {
          ss->scrollX = 1;
          ss->xPositive = TRUE;
          ss->mods = TRUE;
        } else if (westOK && westCount > 0 && eastCount == 0) {
          ss->scrollX = 1;
          ss->xPositive = FALSE;
          ss->mods = TRUE;
        }
        if (southOK && southCount > 0 && northCount == 0) {
          ss->scrollY = 1;
          ss->yPositive = TRUE;
          ss->mods = TRUE;
        } else if (northOK && northCount > 0 && southCount == 0) {
          ss->scrollY = 1;
          ss->yPositive = FALSE;
          ss->mods = TRUE;
        }
      }
    }
  }

  /* Apply the pending burst (one tile per tick). */
  if (ss->scrollX > 0) {
    ss->scrollX--;
    if (ss->xPositive == TRUE) {
      (*xValue)++;
    } else if (*xValue > 0) {
      (*xValue)--;
    }
    returnValue = TRUE;
  }
  if (ss->scrollY > 0) {
    ss->scrollY--;
    if (ss->yPositive == TRUE) {
      (*yValue)++;
    } else if (*yValue > 0) {
      (*yValue)--;
    }
    returnValue = TRUE;
  }
  if (ss->scrollX == 0 && ss->scrollY == 0) {
    ss->mods = FALSE;
  }

  if (returnValue == TRUE) {
    if (*xValue > 255 - MAIN_SCREEN_SIZE_X) *xValue = 255 - MAIN_SCREEN_SIZE_X;
    if (*yValue > 255 - MAIN_SCREEN_SIZE_Y) *yValue = 255 - MAIN_SCREEN_SIZE_Y;
  }

  {
    int dvx = (int)*xValue - (int)inViewX;
    int dvy = (int)*yValue - (int)inViewY;

    /* Age all four cooldown counters one tick (saturating at 255), then
     * reset the directions we just moved in. The next tick's secondary
     * read sees the freshly-reset opposite direction blocked. */
    if (g_ticksSinceMoveE < 255) g_ticksSinceMoveE++;
    if (g_ticksSinceMoveW < 255) g_ticksSinceMoveW++;
    if (g_ticksSinceMoveN < 255) g_ticksSinceMoveN++;
    if (g_ticksSinceMoveS < 255) g_ticksSinceMoveS++;
    if (dvx > 0) g_ticksSinceMoveE = 0;
    else if (dvx < 0) g_ticksSinceMoveW = 0;
    if (dvy > 0) g_ticksSinceMoveS = 0;
    else if (dvy < 0) g_ticksSinceMoveN = 0;
  }

  return returnValue;
}
