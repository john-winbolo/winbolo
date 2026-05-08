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
#include "screen.h"
#include "scroll.h"
#include "scroll_item_list.h"
#include "tank.h"
#include "players.h"
#include "pillbox.h"
#include "shells.h"
#include "bases.h"


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
  leadDX = 0;
  leadDY = 0;
  if (speed > 0 && bestShell == NULL) {
    WORLD leadWX, leadWY;
    int len2;
    const int leadMaxWorld = 3 << 8;  /* cap lead at 3 tiles */
    calculateProjectedPosition(tankWX, tankWY, speed, angle, 1.0f, &leadWX, &leadWY);
    leadDX = (int)leadWX - (int)tankWX;
    leadDY = (int)leadWY - (int)tankWY;
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

  /* Phase 2: build the priority item list */
  scrollItemListCreate(&ss->itemList);
  ss->itemList.tankAngle = angle;
  ss->itemList.tankSpeed = speed;
  ss->itemList.tankWX = tankWX;
  ss->itemList.tankWY = tankWY;

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

  /* Phase 5: execute scroll (at most 1 tile per tick).
   * Tile-align so deltas come out on tile boundaries. */
  targetX &= ~0xFF;
  targetY &= ~0xFF;
  xmove = targetX - viewRefX;
  ymove = targetY - viewRefY;

  if (xmove != 0 || ymove != 0) {
    if (xmove > 0) {
      (*xValue)++;
    } else if (xmove < 0) {
      if (*xValue > 0) (*xValue)--;
    }
    if (ymove > 0) {
      (*yValue)++;
    } else if (ymove < 0) {
      if (*yValue > 0) (*yValue)--;
    }

    if (*xValue > 255 - MAIN_SCREEN_SIZE_X) *xValue = 255 - MAIN_SCREEN_SIZE_X;
    if (*yValue > 255 - MAIN_SCREEN_SIZE_Y) *yValue = 255 - MAIN_SCREEN_SIZE_Y;

    return TRUE;
  }

  return FALSE;
}
