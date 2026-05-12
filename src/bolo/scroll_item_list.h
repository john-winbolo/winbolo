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
*Filename:      scroll_item_list.h
*Author:        John Morrison
*Creation Date: 14/04/26
*Last Modified: 14/04/26
*Purpose:
*  Priority-based item list for auto-scroll viewport
*  decisions. Items are sorted by score and the viewport
*  target is adjusted per-item with conflict validation
*  against higher-priority items.
*********************************************************/

#ifndef SCROLL_ITEM_LIST_H
#define SCROLL_ITEM_LIST_H

#include "global.h"
#include "viewport_types.h"  /* MAIN_SCREEN_SIZE_X/Y */

/* Forward declarations */
struct ScrollState;
struct GameSim;

/* Maximum number of items in the scroll list */
#define SCROLL_ITEM_LIST_MAX_SIZE 64

/* Margin in world coordinates to keep items away from viewport edge */
#define AUTO_SCROLL_MARGIN 0x100

/* Screen size in world coordinates (15 tiles * 256) */
#define SCREEN_WORLD_W ((MAIN_SCREEN_SIZE_X) << 8)
#define SCREEN_WORLD_H ((MAIN_SCREEN_SIZE_Y) << 8)

/* A single item that may influence viewport scrolling */
typedef struct {
  WORLD wx;         /* World X coordinate */
  WORLD wy;         /* World Y coordinate */
  int score;        /* Priority score: lower = higher priority */
  bool ignore;      /* TRUE if scrolling for this conflicts with a higher-priority item */
  bool special;     /* TRUE if scrolling for this item enables drivescroll mode */
} ScrollItem;

/* Fixed-size list of scroll items */
typedef struct {
  ScrollItem items[SCROLL_ITEM_LIST_MAX_SIZE];
  int count;
  TURNTYPE tankAngle;  /* Tank facing direction for directional scoring/veto */
  BYTE tankSpeed;      /* Tank speed — 0 disables directional logic */
  WORLD tankWX;        /* Tank world X — used by the directional veto in process */
  WORLD tankWY;        /* Tank world Y */
} ScrollItemList;

/*********************************************************
*NAME:          scrollItemListCreate
*PURPOSE:
*  Initialise a scroll item list. Sets count to zero.
*
*ARGUMENTS:
*  list - Pointer to ScrollItemList structure
*********************************************************/
void scrollItemListCreate(ScrollItemList *list);

/*********************************************************
*NAME:          scrollItemListAddW
*PURPOSE:
*  Add an item using world coordinates. Score is computed
*  as baseScore + distance_from_tank * rangeMult.
*
*ARGUMENTS:
*  list      - Pointer to ScrollItemList structure
*  wx        - World X coordinate
*  wy        - World Y coordinate
*  baseScore - Base priority (0=tank, 1=hostile, 2=friendly, 3=base, 4=low)
*  rangeMult - Multiplier for range component of score (0=no range, 1=normal, 2=double)
*  special   - TRUE to enable drivescroll when scrolled for
*  tankWX    - Player tank world X (for range calculation)
*  tankWY    - Player tank world Y (for range calculation)
*RETURNS:
*  TRUE if added successfully, FALSE if list is full
*********************************************************/
bool scrollItemListAddW(ScrollItemList *list, WORLD wx, WORLD wy, int baseScore, int rangeMult, bool special, WORLD tankWX, WORLD tankWY);

/*********************************************************
*NAME:          scrollItemListAddM
*PURPOSE:
*  Add an item using map coordinates. Converts to world
*  coordinates (center of tile) then calls scrollItemListAddW.
*
*ARGUMENTS:
*  list      - Pointer to ScrollItemList structure
*  mx        - Map X coordinate
*  my        - Map Y coordinate
*  baseScore - Base priority
*  rangeMult - Range multiplier
*  special   - Drivescroll flag
*  tankWX    - Player tank world X
*  tankWY    - Player tank world Y
*RETURNS:
*  TRUE if added successfully, FALSE if list is full
*********************************************************/
bool scrollItemListAddM(ScrollItemList *list, BYTE mx, BYTE my, int baseScore, int rangeMult, bool special, WORLD tankWX, WORLD tankWY);

/*********************************************************
*NAME:          scrollItemListSort
*PURPOSE:
*  Sort the list by score ascending (lower score = higher
*  priority = processed first). Uses insertion sort.
*
*ARGUMENTS:
*  list - Pointer to ScrollItemList structure
*********************************************************/
void scrollItemListSort(ScrollItemList *list);

/*********************************************************
*NAME:          scrollItemListProcess
*PURPOSE:
*  Iterate through sorted items and adjust the target
*  viewport position. For each off-screen item, moves the
*  target to bring it on screen, then validates that no
*  higher-priority item was pushed off-screen. If conflict,
*  reverts and marks the item as ignored.
*
*  Directional veto: when the tank is moving and there is
*  an important non-special item ahead, items *behind* the
*  tank cannot pull the target *against* the facing
*  direction on a given axis. This prevents trailing items
*  from fighting the lead bias on that axis.
*
*ARGUMENTS:
*  list       - Pointer to sorted ScrollItemList
*  targetX    - In/out: target viewport X (world coords)
*  targetY    - In/out: target viewport Y (world coords)
*RETURNS:
*  TRUE if targetX or targetY was modified
*********************************************************/
bool scrollItemListProcess(ScrollItemList *list, int *targetX, int *targetY);

/*********************************************************
*NAME:          calculateProjectedPosition
*PURPOSE:
*  Calculate projected tank position with WORLD precision
*  for drivescroll look-ahead.
*
*ARGUMENTS:
*  tankWX  - Current tank world X
*  tankWY  - Current tank world Y
*  speed   - Current speed (world units per tick)
*  angle   - Current direction (bradians)
*  seconds - Look-ahead time in seconds
*  projWX  - Output: projected world X
*  projWY  - Output: projected world Y
*********************************************************/
void calculateProjectedPosition(WORLD tankWX, WORLD tankWY, BYTE speed, TURNTYPE angle, float seconds, WORLD *projWX, WORLD *projWY);

#endif /* SCROLL_ITEM_LIST_H */
