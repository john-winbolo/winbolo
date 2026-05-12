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
*Name:          scroll_item_list
*Filename:      scroll_item_list.c
*Author:        John Morrison
*Creation Date: 14/04/26
*Last Modified: 14/04/26
*Purpose:
*  Priority-based item list for auto-scroll viewport
*  decisions. Items are sorted by score and the viewport
*  target is adjusted per-item with conflict validation
*  against higher-priority items.
*********************************************************/

#include <math.h>
#include <stdlib.h>
#include "global.h"
#include "game_sim.h"  /* GAME_NUMGAMETICKS_SEC */
#include "scroll_item_list.h"


void scrollItemListCreate(ScrollItemList *list) {
  list->count = 0;
  list->tankAngle = 0;
  list->tankSpeed = 0;
  list->tankWX = 0;
  list->tankWY = 0;
}


bool scrollItemListAddW(ScrollItemList *list, WORLD wx, WORLD wy, int baseScore, int rangeMult, bool special, WORLD tankWX, WORLD tankWY) {
  ScrollItem *item;
  int dx, dy, range;

  if (list->count >= SCROLL_ITEM_LIST_MAX_SIZE) {
    return FALSE;
  }

  /* Calculate range from player tank in map tiles */
  range = 0;
  if (rangeMult > 0) {
    dx = abs((int)(wx >> 8) - (int)(tankWX >> 8));
    dy = abs((int)(wy >> 8) - (int)(tankWY >> 8));
    range = (dx > dy) ? dx : dy;

    /* Directional penalty: items behind the moving tank get
     * their range doubled so they lose priority to items ahead. */
    if (list->tankSpeed > 0 && range > 0) {
      float rad = list->tankAngle * (float)BRADIAN_TO_RADIAN_FACTOR;
      float facingX = (float)sin(rad);
      float facingY = -(float)cos(rad);
      float toItemX = (float)((int)wx - (int)tankWX);
      float toItemY = (float)((int)wy - (int)tankWY);
      float dot = facingX * toItemX + facingY * toItemY;
      if (dot < 0.0f) {
        range *= 2;  /* Behind the tank — double range penalty */
      }
    }

    range *= rangeMult;
  }

  item = &list->items[list->count];
  item->wx = wx;
  item->wy = wy;
  item->score = baseScore + range;
  item->ignore = FALSE;
  item->special = special;

  list->count++;
  return TRUE;
}


bool scrollItemListAddM(ScrollItemList *list, BYTE mx, BYTE my, int baseScore, int rangeMult, bool special, WORLD tankWX, WORLD tankWY) {
  WORLD wx, wy;

  /* Convert map coordinate to world coordinate (center of tile) */
  wx = ((WORLD)mx << 8) | 0x80;
  wy = ((WORLD)my << 8) | 0x80;

  return scrollItemListAddW(list, wx, wy, baseScore, rangeMult, special, tankWX, tankWY);
}


void scrollItemListSort(ScrollItemList *list) {
  int i, j;
  ScrollItem temp;

  /* Insertion sort by score ascending (lower score = higher priority) */
  for (i = 1; i < list->count; i++) {
    temp = list->items[i];
    j = i - 1;
    while (j >= 0 && list->items[j].score > temp.score) {
      list->items[j + 1] = list->items[j];
      j--;
    }
    list->items[j + 1] = temp;
  }
}


/*********************************************************
*NAME:          scrollItemIsLeft / scrollItemIsRight / scrollItemIsAbove / scrollItemIsBelow
*PURPOSE:
*  Check if an item is outside the viewport on each side.
*  margin parameter shrinks the viewport for conflict checks
*  so higher-priority items aren't pushed to the very edge.
*********************************************************/
static bool scrollItemIsLeft(WORLD itemWX, int targetX, int margin) {
  return ((int)itemWX < targetX + margin) ? TRUE : FALSE;
}

static bool scrollItemIsRight(WORLD itemWX, int targetX, int margin) {
  return ((int)itemWX > targetX + SCREEN_WORLD_W - margin) ? TRUE : FALSE;
}

static bool scrollItemIsAbove(WORLD itemWY, int targetY, int margin) {
  return ((int)itemWY < targetY + margin) ? TRUE : FALSE;
}

static bool scrollItemIsBelow(WORLD itemWY, int targetY, int margin) {
  return ((int)itemWY > targetY + SCREEN_WORLD_H - margin) ? TRUE : FALSE;
}


bool scrollItemListProcess(ScrollItemList *list, int *targetX, int *targetY) {
  int i, j;
  ScrollItem *item;
  int stashX, stashY;
  bool modified;
  bool anyModified;
  float facingX = 0.0f;
  float facingY = 0.0f;
  bool hasForwardImportant = FALSE;

  anyModified = FALSE;

  /* The directional veto activates only when the tank is moving AND
   * there is a non-special item meaningfully ahead. With nothing
   * important ahead, behind items can still pull the viewport so
   * trailing threats stay visible — matches user-stated preference. */
  if (list->tankSpeed > 0) {
    float rad = list->tankAngle * (float)BRADIAN_TO_RADIAN_FACTOR;
    facingX = (float)sin(rad);
    facingY = -(float)cos(rad);

    for (i = 0; i < list->count; i++) {
      float toX, toY, dot;
      if (list->items[i].special == TRUE) {
        continue;
      }
      toX = (float)((int)list->items[i].wx - (int)list->tankWX);
      toY = (float)((int)list->items[i].wy - (int)list->tankWY);
      dot = toX * facingX + toY * facingY;
      if (dot > (float)(1 << 8)) {  /* > 1 tile ahead */
        hasForwardImportant = TRUE;
        break;
      }
    }
  }

  for (i = 0; i < list->count; i++) {
    bool isBehind;
    bool vetoLeftAdj, vetoRightAdj, vetoUpAdj, vetoDownAdj;

    item = &list->items[i];
    stashX = *targetX;
    stashY = *targetY;

    isBehind = FALSE;
    if (list->tankSpeed > 0 && hasForwardImportant && item->special == FALSE) {
      float toX = (float)((int)item->wx - (int)list->tankWX);
      float toY = (float)((int)item->wy - (int)list->tankWY);
      float dot = toX * facingX + toY * facingY;
      if (dot < 0.0f) {
        isBehind = TRUE;
      }
    }

    /* Per-axis veto: if facing X is strongly +/-, behind items can't
     * pull the target the opposite way on that axis. Threshold of 0.3
     * skips veto for nearly-perpendicular facing where the axis isn't
     * really the tank's direction of travel. */
    vetoLeftAdj  = (isBehind && facingX >  0.3f) ? TRUE : FALSE;
    vetoRightAdj = (isBehind && facingX < -0.3f) ? TRUE : FALSE;
    vetoUpAdj    = (isBehind && facingY >  0.3f) ? TRUE : FALSE;
    vetoDownAdj  = (isBehind && facingY < -0.3f) ? TRUE : FALSE;

    if (scrollItemIsLeft(item->wx, *targetX, 0)) {
      if (vetoLeftAdj == FALSE) {
        *targetX = (int)item->wx;
      }
    } else if (scrollItemIsRight(item->wx, *targetX, 0)) {
      if (vetoRightAdj == FALSE) {
        *targetX = (int)item->wx - SCREEN_WORLD_W;
      }
    }

    if (scrollItemIsAbove(item->wy, *targetY, 0)) {
      if (vetoUpAdj == FALSE) {
        *targetY = (int)item->wy;
      }
    } else if (scrollItemIsBelow(item->wy, *targetY, 0)) {
      if (vetoDownAdj == FALSE) {
        *targetY = (int)item->wy - SCREEN_WORLD_H;
      }
    }

    if (*targetX == stashX && *targetY == stashY) {
      continue;
    }

    /* Validate: check that no higher-priority item was pushed off-screen */
    modified = TRUE;
    for (j = 0; j < i; j++) {
      int margin;
      if (list->items[j].ignore == TRUE) {
        continue;
      }
      /* Special items (gunsight) are soft — they can trigger scrolling
       * but don't block hard items from adjusting the viewport. */
      if (list->items[j].special == TRUE && item->special == FALSE) {
        continue;
      }
      /* When a special item adjusts the viewport, use a wider margin
       * against hard items to prevent the 1-tile-per-tick scroll from
       * pushing a hard item just past the trigger threshold. */
      margin = AUTO_SCROLL_MARGIN;
      if (item->special == TRUE && list->items[j].special == FALSE) {
        margin = AUTO_SCROLL_MARGIN * 2;
      }
      if (scrollItemIsLeft(list->items[j].wx, *targetX, margin) ||
          scrollItemIsRight(list->items[j].wx, *targetX, margin) ||
          scrollItemIsAbove(list->items[j].wy, *targetY, margin) ||
          scrollItemIsBelow(list->items[j].wy, *targetY, margin)) {
        item->ignore = TRUE;
        *targetX = stashX;
        *targetY = stashY;
        modified = FALSE;
        break;
      }
    }

    if (modified == TRUE) {
      anyModified = TRUE;
    }
  }

  return anyModified;
}


void calculateProjectedPosition(WORLD tankWX, WORLD tankWY, BYTE speed, TURNTYPE angle, float seconds, WORLD *projWX, WORLD *projWY) {
  float rad;
  float ticks;
  int deltaX, deltaY;
  int px, py;

  if (speed == 0) {
    *projWX = tankWX;
    *projWY = tankWY;
    return;
  }

  rad = angle * (float)BRADIAN_TO_RADIAN_FACTOR;
  ticks = seconds * (float)GAME_NUMGAMETICKS_SEC;

  deltaX = (int)((float)speed * (float)sin(rad) * ticks);
  deltaY = (int)((float)speed * -(float)cos(rad) * ticks);

  px = (int)tankWX + deltaX;
  py = (int)tankWY + deltaY;
  if (px < 0) px = 0;
  if (px > 65535) px = 65535;
  if (py < 0) py = 0;
  if (py > 65535) py = 65535;

  *projWX = (WORLD)px;
  *projWY = (WORLD)py;
}
