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
#ifdef USE_SCROLL_ITEM_LIST
  scrollItemListCreate(&ss->itemList);
  ss->driveScroll = FALSE;
  ss->lastScrollDirX = 0;
  ss->lastScrollDirY = 0;
#endif
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

#ifndef USE_SCROLL_ITEM_LIST
/*********************************************************
*NAME:          scrollEnemyAwareness
*PURPOSE:
*  Biases the viewport toward nearby hostile pillboxes and
*  enemy tanks so the player can see threats.
*********************************************************/
static void scrollEnemyAwareness(ScrollState *ss, GameSim *sim, BYTE viewX, BYTE viewY, BYTE tankX, BYTE tankY, BYTE speed, TURNTYPE angle) {
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

  myPlayer = sim->viewPlayer;

  searchMinX = (int)viewX - searchMargin;
  searchMinY = (int)viewY - searchMargin;
  searchMaxX = (int)viewX + MAIN_SCREEN_SIZE_X + searchMargin;
  searchMaxY = (int)viewY + MAIN_SCREEN_SIZE_Y + searchMargin;
  if (searchMinX < 0) searchMinX = 0;
  if (searchMinY < 0) searchMinY = 0;
  if (searchMaxX > 255) searchMaxX = 255;
  if (searchMaxY > 255) searchMaxY = 255;

  /* Enemy pillboxes (1-indexed API) */
  for (count = 1; count <= sim->pb->numPills; count++) {
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
    ss->stickyX = FALSE;
    ss->stickyY = FALSE;
    return;
  }

  centroidX = enemySumX / totalWeight;
  centroidY = enemySumY / totalWeight;

  idealX = (int)(((float)tankX + centroidX) / 2.0f) - MAIN_SCREEN_SIZE_X / 2;
  idealY = (int)(((float)tankY + centroidY) / 2.0f) - MAIN_SCREEN_SIZE_Y / 2;

  if (idealX > (int)tankX - 2) idealX = (int)tankX - 2;
  if (idealX < (int)tankX - MAIN_SCREEN_SIZE_X + 2) idealX = (int)tankX - MAIN_SCREEN_SIZE_X + 2;
  if (idealY > (int)tankY - 2) idealY = (int)tankY - 2;
  if (idealY < (int)tankY - MAIN_SCREEN_SIZE_Y + 2) idealY = (int)tankY - MAIN_SCREEN_SIZE_Y + 2;
  if (idealX < 0) idealX = 0;
  if (idealY < 0) idealY = 0;
  if (idealX > 255 - MAIN_SCREEN_SIZE_X) idealX = 255 - MAIN_SCREEN_SIZE_X;
  if (idealY > 255 - MAIN_SCREEN_SIZE_Y) idealY = 255 - MAIN_SCREEN_SIZE_Y;

  if (idealX != (int)viewX) {
    ss->stickyX = TRUE;
    ss->stickyXDir = (idealX > (int)viewX) ? TRUE : FALSE;
  }
  if (idealY != (int)viewY) {
    ss->stickyY = TRUE;
    ss->stickyYDir = (idealY > (int)viewY) ? TRUE : FALSE;
  }

  if (ss->scrollX == 0 && idealX != (int)viewX) {
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
      ss->scrollX = burst;
      ss->xPositive = wantPositive;
      ss->mods = TRUE;
    }
  }
  if (ss->scrollY == 0 && idealY != (int)viewY) {
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
      ss->scrollY = burst;
      ss->yPositive = wantPositive;
      ss->mods = TRUE;
    }
  }
}
#endif /* !USE_SCROLL_ITEM_LIST */


bool scrollAutoScroll(ScrollState *ss, GameSim *sim, BYTE *xValue, BYTE *yValue, BYTE objectX, BYTE objectY, BYTE gunsightX, BYTE gunsightY, BYTE speed, TURNTYPE angle) {
#ifdef USE_SCROLL_ITEM_LIST
  /*
   * Priority-based viewport scrolling.
   * Build a scored item list, sort by score, then adjust the viewport
   * target position for each item in priority order. Validate that
   * adjusting for a lower-priority item does not push a higher-priority
   * item off-screen. Scroll at most 1 tile per tick.
   */
  int targetX, targetY;
  int viewRefX, viewRefY;
  int xmove, ymove;
  BYTE myPlayer;
  WORLD tankWX, tankWY;
  WORLD gunsightWX, gunsightWY;
  int count;
  shells current;
  shells bestShell;
  bool newDriveScroll;

  myPlayer = sim->viewPlayer;
  tankGetWorld(&sim->tanks[myPlayer], &tankWX, &tankWY);

  /* Phase 1: Initialize target viewport position */
  viewRefX = ((int)*xValue + 1) << 8;
  viewRefY = ((int)*yValue + 1) << 8;
  targetX = viewRefX;
  targetY = viewRefY;

  /* scrollAutoScroll is only called when manual==FALSE, so clear any
   * leftover manual override. The pseudocode treats scroll_override as
   * transient — it only persists while cursor keys are held. */
  ss->autoScrollOverRide = FALSE;

  /* Phase 2: Build priority item list */
  scrollItemListCreate(&ss->itemList);
  ss->itemList.tankAngle = angle;
  ss->itemList.tankSpeed = speed;

  /* Always add own tank (score 0) */
  scrollItemListAddW(&ss->itemList, tankWX, tankWY, 0, 0, FALSE, tankWX, tankWY);

  /* If moving, add position 4 tiles ahead (score 0) */
  if (speed > 0) {
    WORLD aheadWX, aheadWY;
    calculateProjectedPosition(tankWX, tankWY, speed, angle, 1.6f, &aheadWX, &aheadWY);
    scrollItemListAddW(&ss->itemList, aheadWX, aheadWY, 0, 0, FALSE, tankWX, tankWY);
  }

  /* Check for active shell (fireball) */
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

  if (bestShell != NULL) {
    /* Shell active — track it at score 0, skip autoscroll items */
    scrollItemListAddW(&ss->itemList, bestShell->x, bestShell->y, 0, 0, FALSE, tankWX, tankWY);
  } else if (ss->autoScrollOverRide == FALSE) {
    /* No shell — add all autoscroll items */
    pillbox pill;
    pillAlliance pa;
    BYTE tx, ty;
    WORLD otherWX, otherWY;
    int tankMX = (int)(tankWX >> 8);
    int tankMY = (int)(tankWY >> 8);

    /* Look-ahead position 6 tiles ahead (score 4, special) */
    if (speed > 0) {
      WORLD lookWX, lookWY;
      calculateProjectedPosition(tankWX, tankWY, speed, angle, 2.4f, &lookWX, &lookWY);
      scrollItemListAddW(&ss->itemList, lookWX, lookWY, 4, 0, TRUE, tankWX, tankWY);
    }

    /* Enemy and friendly tanks */
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
      /* Use projected position for other tanks */
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

    /* Enemy and friendly pillboxes (1-indexed API) */
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

    /* Crosshairs (score 4, special) */
    gunsightWX = ((WORLD)gunsightX << 8) | 0x80;
    gunsightWY = ((WORLD)gunsightY << 8) | 0x80;
    scrollItemListAddW(&ss->itemList, gunsightWX, gunsightWY, 4, 0, TRUE, tankWX, tankWY);

    /* Drive scroll: if previously active, add predictive positions 2-4 seconds ahead */
    if (ss->driveScroll == TRUE && speed > 0) {
      WORLD dsWX, dsWY;
      calculateProjectedPosition(tankWX, tankWY, speed, angle, 2.0f, &dsWX, &dsWY);
      scrollItemListAddW(&ss->itemList, dsWX, dsWY, 4, 0, TRUE, tankWX, tankWY);
      calculateProjectedPosition(tankWX, tankWY, speed, angle, 3.0f, &dsWX, &dsWY);
      scrollItemListAddW(&ss->itemList, dsWX, dsWY, 4, 0, TRUE, tankWX, tankWY);
      calculateProjectedPosition(tankWX, tankWY, speed, angle, 4.0f, &dsWX, &dsWY);
      scrollItemListAddW(&ss->itemList, dsWX, dsWY, 4, 0, TRUE, tankWX, tankWY);
    }
  }

  /* Phase 3: Sort by score */
  scrollItemListSort(&ss->itemList);

  /* Phase 4: Process — adjust target viewport, validate conflicts */
  newDriveScroll = FALSE;
  scrollItemListProcess(&ss->itemList, &targetX, &targetY, &newDriveScroll);
  ss->driveScroll = newDriveScroll;

  /* Phase 5: Execute scroll (at most 1 tile per tick).
   * Tile-align the target after conflict checking so we get
   * clean tile-boundary deltas (no sub-tile oscillation). */
  targetX &= ~0xFF;
  targetY &= ~0xFF;
  xmove = targetX - viewRefX;
  ymove = targetY - viewRefY;

  /* Hysteresis: suppress direction reversals unless the delta is large
   * enough (> 2 tiles) to justify it. This prevents the viewport from
   * oscillating when two items on opposite sides compete for priority. */
  if (xmove > 0 && ss->lastScrollDirX < 0 && xmove < (3 << 8)) {
    xmove = 0;
  } else if (xmove < 0 && ss->lastScrollDirX > 0 && xmove > -(3 << 8)) {
    xmove = 0;
  }
  if (ymove > 0 && ss->lastScrollDirY < 0 && ymove < (3 << 8)) {
    ymove = 0;
  } else if (ymove < 0 && ss->lastScrollDirY > 0 && ymove > -(3 << 8)) {
    ymove = 0;
  }

  if (xmove != 0 || ymove != 0) {
    ss->autoScrollOverRide = FALSE;
    if (xmove > 0) {
      (*xValue)++;
      ss->lastScrollDirX = 1;
    } else if (xmove < 0) {
      if (*xValue > 0) (*xValue)--;
      ss->lastScrollDirX = -1;
    }
    if (ymove > 0) {
      (*yValue)++;
      ss->lastScrollDirY = 1;
    } else if (ymove < 0) {
      if (*yValue > 0) (*yValue)--;
      ss->lastScrollDirY = -1;
    }

    /* Clamp viewport to map boundaries */
    if (*xValue > 255 - MAIN_SCREEN_SIZE_X) *xValue = 255 - MAIN_SCREEN_SIZE_X;
    if (*yValue > 255 - MAIN_SCREEN_SIZE_Y) *yValue = 255 - MAIN_SCREEN_SIZE_Y;

    return TRUE;
  }

  ss->lastScrollDirX = 0;
  ss->lastScrollDirY = 0;
  return FALSE;

#else
  /* Original burst-based scrolling with scrollEnemyAwareness */
  bool returnValue;

  returnValue = FALSE;

  if (ss->scrollX == 0 && ss->scrollY == 0) {
    if (((gunsightX-1) - (*xValue)) >= (MAIN_SCREEN_SIZE_X)) {
      if (!(ss->stickyX == TRUE && ss->stickyXDir == FALSE)) {
        ss->scrollX = (BYTE) (speed / SCROLL_DIVIDE);
        if (ss->scrollX == 0) {
          ss->scrollX = 1;
        }
        ss->xPositive = TRUE;
        ss->mods = TRUE;
      }
    }
    if ((gunsightX) < (*xValue)) {
      if (!(ss->stickyX == TRUE && ss->stickyXDir == TRUE)) {
        ss->scrollX = (BYTE) (speed / SCROLL_DIVIDE);
        if (ss->scrollX == 0) {
          ss->scrollX = 1;
        }
        ss->xPositive = FALSE;
        ss->mods = TRUE;
      }
    }
    if (((gunsightY-1)- (*yValue)) >= (MAIN_SCREEN_SIZE_Y)) {
      if (!(ss->stickyY == TRUE && ss->stickyYDir == FALSE)) {
        ss->scrollY = (BYTE) (speed / SCROLL_DIVIDE);
        if (ss->scrollY == 0) {
          ss->scrollY = 1;
        }
        ss->yPositive = TRUE;
        ss->mods = TRUE;
      }
    }
    if (gunsightY < (*yValue)) {
      if (!(ss->stickyY == TRUE && ss->stickyYDir == TRUE)) {
        ss->scrollY = (BYTE) (speed / SCROLL_DIVIDE);
        if (ss->scrollY == 0) {
          ss->scrollY = 1;
        }
        ss->yPositive = FALSE;
        ss->mods = TRUE;
      }
    }

    scrollEnemyAwareness(ss, sim, *xValue, *yValue, objectX, objectY, speed, angle);
  }


  if (ss->scrollX > 0) {
    returnValue = TRUE;
    ss->scrollX--;
    if (ss->xPositive == TRUE) {
      (*xValue)++;
    } else {
      if (*xValue > 0) (*xValue)--;
    }
  }
  if (ss->scrollY > 0) {
    ss->scrollY--;
    returnValue = TRUE;
    if (ss->yPositive == TRUE) {
      (*yValue)++;
    } else {
      if (*yValue > 0) (*yValue)--;
    }
  }

  /* Clamp viewport to map boundaries after burst */
  if (*xValue > 255 - MAIN_SCREEN_SIZE_X) *xValue = 255 - MAIN_SCREEN_SIZE_X;
  if (*yValue > 255 - MAIN_SCREEN_SIZE_Y) *yValue = 255 - MAIN_SCREEN_SIZE_Y;

  /* Hard safety: tank must always be on screen */
  if (objectX <= (*xValue)) {
    if (objectX > 0) {
      *xValue = objectX - 1;
    } else {
      *xValue = 0;
    }
    if (ss->xPositive == FALSE) ss->scrollX = 0;
  } else if ((int)objectX >= (int)(*xValue) + MAIN_SCREEN_SIZE_X) {
    *xValue = objectX - MAIN_SCREEN_SIZE_X + 1;
    if (ss->xPositive == TRUE) ss->scrollX = 0;
  }
  if (objectY <= (*yValue)) {
    if (objectY > 0) {
      *yValue = objectY - 1;
    } else {
      *yValue = 0;
    }
    if (ss->yPositive == FALSE) ss->scrollY = 0;
  } else if ((int)objectY >= (int)(*yValue) + MAIN_SCREEN_SIZE_Y) {
    *yValue = objectY - MAIN_SCREEN_SIZE_Y + 1;
    if (ss->yPositive == TRUE) ss->scrollY = 0;
  }

  if (ss->scrollX == 0 && ss->scrollY == 0) {
    ss->mods = FALSE;
  }

  return returnValue;
#endif
}
