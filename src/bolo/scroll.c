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
#include "global.h"
#include "screen.h"
#include "scroll.h"
#include "tank.h"
#include "players.h"
#include "pillbox.h"

/* Is it auto scrolling or not */
bool autoScroll = FALSE;
BYTE scrollX = 0;
BYTE scrollY = 0;
BYTE xPositive = TRUE;
BYTE yPositive = TRUE;
bool autoScrollOverRide = FALSE;
bool mods = FALSE;

/* Sticky state: remembers enemy-awareness scroll direction so the
 * gunsight edge-detection does not immediately undo it. */
static bool stickyX = FALSE;
static bool stickyXDir = FALSE;
static bool stickyY = FALSE;
static bool stickyYDir = FALSE;


/*********************************************************
*NAME:          scrollSetScrollType
*AUTHOR:        John Morrison
*CREATION DATE: 16/1/99
*LAST MODIFIED: 16/1/99
*PURPOSE:
*  Modifies state of autoscrolling
*
*ARGUMENTS:
*  isAuto - Set to on or off?
*********************************************************/
void scrollSetScrollType(bool isAuto) {
  autoScroll = isAuto;
  autoScrollOverRide = FALSE;
  if (isAuto == FALSE) {
    scrollX = 0;
    scrollY = 0;
    mods = FALSE;
    stickyX = FALSE;
    stickyY = FALSE;
  }
}

/*********************************************************
*NAME:          scrollCenterObject
*AUTHOR:        John Morrison
*CREATION DATE: 16/1/99
*LAST MODIFIED: 16/1/99
*PURPOSE:
*  Centres the screen on the object
*
*ARGUMENTS:
*  xValue - Pointer to hold new X co-ordinate
*  yValue - Pointer to hold new Y co-ordinate
*  objectX - Object to centre on X co-ordinate
*  objectY - Object to centre on Y co-ordinate
*********************************************************/
void scrollCenterObject(BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY) {
  *xValue = objectX - SCROLL_CENTER;
  *yValue = objectY - SCROLL_CENTER;
  autoScrollOverRide = FALSE;
  stickyX = FALSE;
  stickyY = FALSE;
}

/*********************************************************
*NAME:          scrollUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 16/1/99
*LAST MODIFIED: 19/11/99
*PURPOSE:
*  Called every game tick. Checks to see if the screen
*  is required to be moved because the tank has moved
*  etc.
*  If the object is not a tank the last 3 parameters
*  are ignored. It returns if a recalculation is needed
*
*ARGUMENTS:
*  sim       - Pointer to the game simulation
*  xValue    - Pointer to hold new X co-ordinate
*  yValue    - Pointer to hold new Y co-ordinate
*  objectX   - Object to centre on X co-ordinate
*  objectY   - Object to centre on Y co-ordinate
*  isTank    - Is the object a tank
*  gunsightX - The gunsights X position
*  gunsightY - The gunishgts Y position
*  speed     - The speed of the tank
*  armour    - Amount of armour on the tank
*  angle     - Tank travelling angle
*  manual    - Is it manual move (ie by keys, not tank)
*********************************************************/
bool scrollUpdate(GameSim *sim, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, bool isTank, BYTE gunsightX, BYTE gunsightY, BYTE speed, BYTE armour, TURNTYPE angle, bool manual, bool tankIsDead) {
  bool returnValue; /* Value to return */

  returnValue = TRUE;
  if (tankIsDead == TRUE) {
    returnValue = FALSE;
  } else if (manual == TRUE) {
    returnValue = scrollManual(xValue, yValue, objectX, objectY, angle);
  } else if (autoScroll == TRUE && isTank == TRUE && armour <= TANK_FULL_ARMOUR && autoScrollOverRide == FALSE) {
    /* Calculate using the autoscroll function */
    returnValue = scrollAutoScroll(sim, xValue, yValue, objectX, objectY, gunsightX, gunsightY, speed, angle);
  } else {
   /* calculate not using auto scroll functions */
    returnValue = scrollNoAutoScroll(xValue, yValue, objectX, objectY, angle);
  }

  return returnValue;

}

/*********************************************************
*NAME:          scrollCheck
*AUTHOR:        John Morrison
*CREATION DATE: 19/11/99
*LAST MODIFIED: 19/11/99
*PURPOSE:
*  Returns whether an item is on screen or not
*
*ARGUMENTS:
*  xValue    - Current X co-ordinate
*  yValue    - Current Y co-ordinate
*  objectX   - Objects X co-ordinate
*  objectY   - Objects Y co-ordinate
*********************************************************/
bool scrollCheck(BYTE xValue, BYTE yValue, BYTE objectX, BYTE objectY) {
  bool returnValue; /* Used internally */

  returnValue = TRUE;
  /* Check to see if moving towards screen edge */
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


/*********************************************************
*NAME:          scrollManual
*AUTHOR:        John Morrison
*CREATION DATE: 16/1/99
*LAST MODIFIED: 10/06/01
*PURPOSE:
*  Movement scroll keys have been pressed. Returns if a
*  movement occurs
*
*ARGUMENTS:
*  xValue    - Pointer to hold new X co-ordinate
*  yValue    - Pointer to hold new Y co-ordinate
*  objectX   - Objects X co-ordinate
*  objectY   - Objects Y co-ordinate
*  angle     - Items Angle
*********************************************************/
bool scrollManual(BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, TURNTYPE angle) {
  bool returnValue; /* Used internally */
  bool leftPos;
  bool rightPos;
  bool upPos;
  bool downPos;

  leftPos = FALSE;
  rightPos = FALSE;
  upPos = FALSE;
  downPos = FALSE;
  returnValue = FALSE;
  autoScrollOverRide = TRUE;

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


  /* Check to see if moving towards screen edge */
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
  if (mods == TRUE && returnValue == TRUE) {
    mods = FALSE;
  }

  return returnValue;
}

/*********************************************************
*NAME:          scrollNoAutoScroll
*AUTHOR:        John Morrison
*CREATION DATE: 16/1/99
*LAST MODIFIED: 19/11/99
*PURPOSE:
*  Checks to see if the screen is required to be moved
*  because the object is moving off screen. Doesn't
*  use autoscrolling features. Returns if a recalculation
*  of the screen is needed
*
*ARGUMENTS:
*  xValue    - Pointer to hold new X co-ordinate
*  yValue    - Pointer to hold new Y co-ordinate
*  objectX   - Object to centre on X co-ordinate
*  objectY   - Object to centre on Y co-ordinate
*  angle     - Turntype angle
*********************************************************/
bool scrollNoAutoScroll(BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, TURNTYPE angle) {
  bool returnValue; /* Value to return */
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

  /* Check to see if moving towards screen edge */
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
  if (mods == TRUE && returnValue == TRUE) {
    mods = FALSE;
  }
  if (returnValue == TRUE) {
    autoScrollOverRide = FALSE;
  }

  return returnValue;
}

/*********************************************************
*NAME:          scrollEnemyAwareness
*PURPOSE:
*  Biases the viewport toward nearby hostile pillboxes and
*  enemy tanks so the player can see threats.
*
*  Called after gunsight edge-detection in scrollAutoScroll.
*  Only acts on scroll axes the gunsight did NOT already
*  claim, so it supplements the gunsight rather than
*  fighting it.
*
*  How it works:
*  1. Scans for hostile pillboxes (evil/neutral with armour)
*     and enemy tanks within a search region around the
*     current viewport (viewport + 4 tile margin).
*  2. Each enemy gets a directional weight based on the
*     dot product of the tank's movement direction and the
*     direction to the enemy:
*       - Heading toward:  high weight (up to 2.0)
*       - Perpendicular:   medium (1.0)
*       - Heading away:    near zero, unless within 4 tiles
*     Enemy tanks are weighted 2x vs pillboxes.
*  3. Computes a weighted enemy centroid, then picks an
*     ideal viewport that centers the midpoint of the tank
*     and that centroid. The ideal is clamped so the tank
*     stays at least 2 tiles from the viewport edge.
*  4. On each free axis, initiates a scroll burst toward
*     the ideal, but:
*       - The burst is capped to the actual distance needed
*         (prevents overshoot oscillation).
*       - The burst is blocked if it would oppose the tank's
*         movement direction on that axis (prevents fighting
*         the gunsight, which will want that axis soon).
*
*  A separate hard clamp in scrollAutoScroll guarantees the
*  tank never leaves the screen regardless of what this
*  function does.
*
*ARGUMENTS:
*  sim       - Pointer to the game simulation
*  viewX     - Current viewport X
*  viewY     - Current viewport Y
*  tankX     - Player tank map X
*  tankY     - Player tank map Y
*  speed     - Tank speed
*  angle     - Tank travel angle in bradians
*********************************************************/
static void scrollEnemyAwareness(GameSim *sim, BYTE viewX, BYTE viewY, BYTE tankX, BYTE tankY, BYTE speed, TURNTYPE angle) {
  BYTE myPlayer;
  float enemySumX = 0.0f;
  float enemySumY = 0.0f;
  float totalWeight = 0.0f;
  int searchMargin = 4;
  int searchMinX, searchMinY, searchMaxX, searchMaxY;
  int count;
  pillbox pill;
  pillAlliance pa;
  float moveDirX, moveDirY, rad, weight;
  bool isMoving;
  float dx, dy, dist, dot;
  float centroidX, centroidY;
  int idealX, idealY;
  bool wantPositive;
  bool blocked;
  int distNeeded;
  BYTE burst;

  isMoving = (speed > 0) ? TRUE : FALSE;
  if (isMoving == TRUE) {
    rad = angle * (float)BRADIAN_TO_RADIAN_FACTOR;
    moveDirX = (float)sin(rad);
    moveDirY = -(float)cos(rad);
  } else {
    moveDirX = 0.0f;
    moveDirY = 0.0f;
  }

  myPlayer = playersGetSelf(&sim->plyrs);

  searchMinX = (int)viewX - searchMargin;
  searchMinY = (int)viewY - searchMargin;
  searchMaxX = (int)viewX + MAIN_SCREEN_SIZE_X + searchMargin;
  searchMaxY = (int)viewY + MAIN_SCREEN_SIZE_Y + searchMargin;
  if (searchMinX < 0) searchMinX = 0;
  if (searchMinY < 0) searchMinY = 0;
  if (searchMaxX > 255) searchMaxX = 255;
  if (searchMaxY > 255) searchMaxY = 255;

  /* Enemy pillboxes */
  for (count = 0; count < sim->pb->numPills; count++) {
    pillsGetPill(&sim->pb, &pill, count);
    pa = pillsGetAllianceNum(sim, &sim->pb, count);
    if (pa != pillEvil && pa != pillNeutral) {
      continue;
    }
    if (pill.armour == 0) {
      continue;
    }
    if ((int)pill.x < searchMinX || (int)pill.x > searchMaxX ||
        (int)pill.y < searchMinY || (int)pill.y > searchMaxY) {
      continue;
    }
    dx = (float)((int)pill.x - (int)tankX);
    dy = (float)((int)pill.y - (int)tankY);
    dist = (float)sqrt(dx * dx + dy * dy);
    if (isMoving == TRUE && dist > 0.5f) {
      dot = (dx / dist) * moveDirX + (dy / dist) * moveDirY;
      weight = dot + 1.0f;
      if (dist < 4.0f && weight < 0.5f) {
        weight = 0.5f;
      }
    } else {
      weight = 1.0f;
    }
    if (weight > 0.1f) {
      enemySumX += (float)pill.x * weight;
      enemySumY += (float)pill.y * weight;
      totalWeight += weight;
    }
  }

  /* Enemy tanks */
  for (count = 0; count < MAX_TANKS; count++) {
    BYTE tx, ty;
    if (sim->tanks[count] == NULL) continue;
    if (count == myPlayer) continue;
    if (playersIsAllie(&sim->plyrs, myPlayer, (BYTE)count) == TRUE) continue;
    tx = tankGetMX(&sim->tanks[count]);
    ty = tankGetMY(&sim->tanks[count]);
    if (tx == 0 && ty == 0) continue;
    if ((int)tx < searchMinX || (int)tx > searchMaxX ||
        (int)ty < searchMinY || (int)ty > searchMaxY) {
      continue;
    }
    dx = (float)((int)tx - (int)tankX);
    dy = (float)((int)ty - (int)tankY);
    dist = (float)sqrt(dx * dx + dy * dy);
    if (isMoving == TRUE && dist > 0.5f) {
      dot = (dx / dist) * moveDirX + (dy / dist) * moveDirY;
      weight = dot + 1.0f;
      if (dist < 4.0f && weight < 0.5f) {
        weight = 0.5f;
      }
    } else {
      weight = 1.0f;
    }
    weight *= 2.0f;
    if (weight > 0.1f) {
      enemySumX += (float)tx * weight;
      enemySumY += (float)ty * weight;
      totalWeight += weight;
    }
  }

  if (totalWeight < 0.1f) {
    stickyX = FALSE;
    stickyY = FALSE;
    return;
  }

  centroidX = enemySumX / totalWeight;
  centroidY = enemySumY / totalWeight;

  /* Ideal viewport: center the midpoint of tank and enemy centroid */
  idealX = (int)(((float)tankX + centroidX) / 2.0f) - MAIN_SCREEN_SIZE_X / 2;
  idealY = (int)(((float)tankY + centroidY) / 2.0f) - MAIN_SCREEN_SIZE_Y / 2;

  /* Clamp so tank stays at least 2 tiles from viewport edge */
  if (idealX > (int)tankX - 2) idealX = (int)tankX - 2;
  if (idealX < (int)tankX - MAIN_SCREEN_SIZE_X + 2) idealX = (int)tankX - MAIN_SCREEN_SIZE_X + 2;
  if (idealY > (int)tankY - 2) idealY = (int)tankY - 2;
  if (idealY < (int)tankY - MAIN_SCREEN_SIZE_Y + 2) idealY = (int)tankY - MAIN_SCREEN_SIZE_Y + 2;
  if (idealX < 0) idealX = 0;
  if (idealY < 0) idealY = 0;
  if (idealX > 255 - MAIN_SCREEN_SIZE_X) idealX = 255 - MAIN_SCREEN_SIZE_X;
  if (idealY > 255 - MAIN_SCREEN_SIZE_Y) idealY = 255 - MAIN_SCREEN_SIZE_Y;

  /* Update sticky state so gunsight doesn't undo enemy-driven offsets */
  if (idealX != (int)viewX) {
    stickyX = TRUE;
    stickyXDir = (idealX > (int)viewX) ? TRUE : FALSE;
  }
  if (idealY != (int)viewY) {
    stickyY = TRUE;
    stickyYDir = (idealY > (int)viewY) ? TRUE : FALSE;
  }

  /* Only trigger scroll on axes the gunsight didn't already claim,
   * and NEVER oppose the tank's movement direction. If the tank is
   * moving along an axis, the gunsight will eventually want to scroll
   * that way — scrolling opposite causes oscillation.
   * Cap burst size to the actual distance needed to prevent overshoot. */
  if (scrollX == 0 && idealX != (int)viewX) {
    wantPositive = (idealX > (int)viewX) ? TRUE : FALSE;
    blocked = FALSE;
    distNeeded = abs(idealX - (int)viewX);
    if (isMoving == TRUE) {
      if (moveDirX > 0.3f && wantPositive == FALSE) blocked = TRUE;
      if (moveDirX < -0.3f && wantPositive == TRUE) blocked = TRUE;
    }
    if (blocked == FALSE) {
      burst = (BYTE)(speed / SCROLL_DIVIDE);
      if (burst == 0) burst = 1;
      if (burst > distNeeded) burst = (BYTE)distNeeded;
      scrollX = burst;
      xPositive = wantPositive;
      mods = TRUE;
    }
  }
  if (scrollY == 0 && idealY != (int)viewY) {
    wantPositive = (idealY > (int)viewY) ? TRUE : FALSE;
    blocked = FALSE;
    distNeeded = abs(idealY - (int)viewY);
    if (isMoving == TRUE) {
      if (moveDirY > 0.3f && wantPositive == FALSE) blocked = TRUE;
      if (moveDirY < -0.3f && wantPositive == TRUE) blocked = TRUE;
    }
    if (blocked == FALSE) {
      burst = (BYTE)(speed / SCROLL_DIVIDE);
      if (burst == 0) burst = 1;
      if (burst > distNeeded) burst = (BYTE)distNeeded;
      scrollY = burst;
      yPositive = wantPositive;
      mods = TRUE;
    }
  }
}


/*********************************************************
*NAME:          scrollAutoScroll
*AUTHOR:        John Morrison
*CREATION DATE: 16/1/99
*LAST MODIFIED: 16/1/99
*PURPOSE:
*  Checks to see if the screen is required to be moved
*  because the object is moving off screen. Uses the
*  autoscrolling features. After gunsight edge-detection,
*  enemy awareness can trigger scrolling on axes the
*  gunsight didn't claim. Returns if a recalculation
*  of the screen is needed
*
*ARGUMENTS:
*  sim       - Pointer to the game simulation
*  xValue    - Pointer to hold new X co-ordinate
*  yValue    - Pointer to hold new Y co-ordinate
*  objectX   - Object to centre on X co-ordinate
*  objectY   - Object to centre on Y co-ordinate
*  gunsightX - The gunsights X position
*  gunsightY - The gunishgts Y position
*  speed     - The speed of the tank
*  angle     - Angle of the tank
*********************************************************/
bool scrollAutoScroll(GameSim *sim, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, BYTE gunsightX, BYTE gunsightY, BYTE speed, TURNTYPE angle) {
  bool returnValue;        /* Value to return */

  returnValue = FALSE;

  if (scrollX == 0 && scrollY == 0) {
    /* Check to see if moving towards screen edge.
     * Suppress gunsight scrolling that opposes an active enemy-awareness
     * sticky offset — the hard safety clamp below guarantees the tank
     * stays on screen regardless. */
    if (((gunsightX-1) - (*xValue)) >= (MAIN_SCREEN_SIZE_X)) {
      if (!(stickyX == TRUE && stickyXDir == FALSE)) {
        scrollX = (BYTE) (speed / SCROLL_DIVIDE);
        if (scrollX == 0) {
          scrollX = 1;
        }
        xPositive = TRUE;
        mods = TRUE;
      }
    }
    if ((gunsightX) < (*xValue)) {
      if (!(stickyX == TRUE && stickyXDir == TRUE)) {
        scrollX = (BYTE) (speed / SCROLL_DIVIDE);
        if (scrollX == 0) {
          scrollX = 1;
        }
        xPositive = FALSE;
        mods = TRUE;
      }
    }
    if (((gunsightY-1)- (*yValue)) >= (MAIN_SCREEN_SIZE_Y)) {
      if (!(stickyY == TRUE && stickyYDir == FALSE)) {
        scrollY = (BYTE) (speed / SCROLL_DIVIDE);
        if (scrollY == 0) {
          scrollY = 1;
        }
        yPositive = TRUE;
        mods = TRUE;
      }
    }
    if (gunsightY < (*yValue)) {
      if (!(stickyY == TRUE && stickyYDir == TRUE)) {
        scrollY = (BYTE) (speed / SCROLL_DIVIDE);
        if (scrollY == 0) {
          scrollY = 1;
        }
        yPositive = FALSE;
        mods = TRUE;
      }
    }

    /* Enemy awareness: fill in any axis the gunsight didn't trigger */
    scrollEnemyAwareness(sim, *xValue, *yValue, objectX, objectY, speed, angle);
  }


  if (scrollX > 0) {
    returnValue = TRUE;
    scrollX--;
    if (xPositive == TRUE) {
      (*xValue)++;
    } else {
      (*xValue)--;
    }
  }
  if (scrollY > 0) {
    scrollY--;
    returnValue = TRUE;
    if (yPositive == TRUE) {
      (*yValue)++;
    } else {
      (*yValue)--;
    }
  }

  /* Hard safety: tank must always be on screen. If a scroll step pushed
   * the tank off the viewport, clamp the viewport back and cancel the
   * remaining burst on that axis to prevent further drift. */
  if (objectX <= (*xValue)) {
    *xValue = objectX - 1;
    if (xPositive == FALSE) scrollX = 0;
  } else if ((int)objectX >= (int)(*xValue) + MAIN_SCREEN_SIZE_X) {
    *xValue = objectX - MAIN_SCREEN_SIZE_X + 1;
    if (xPositive == TRUE) scrollX = 0;
  }
  if (objectY <= (*yValue)) {
    *yValue = objectY - 1;
    if (yPositive == FALSE) scrollY = 0;
  } else if ((int)objectY >= (int)(*yValue) + MAIN_SCREEN_SIZE_Y) {
    *yValue = objectY - MAIN_SCREEN_SIZE_Y + 1;
    if (yPositive == TRUE) scrollY = 0;
  }

  if (scrollX == 0 && scrollY == 0) {
    mods = FALSE;
  }

  return returnValue;
}
