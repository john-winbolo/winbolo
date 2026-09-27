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
*Name:          Pillbox
*Filename:      pillbox.h
*Author:        John Morrison
*Creation Date: 28/10/98
*Last Modified: 11/11/00
*Purpose:
*  Provides operations on pillbox and pillboxes
*********************************************************/

#ifndef PILLBOX_H
#define PILLBOX_H


/* Includes */
#include "types.h"
#include "lv_global.h"


/* Defines */
#define MAX_PILLS 16

typedef enum {
  pillDead,
  pillAllie,
  pillGood,
  pillNeutral,
  pillEvil,
  pillTankGood,
  pillTankAllie,
  pillTankEvil
} pillAlliance;


/* Typedefs - pillboxes is already defined in bolo/types.h */

/* Prototypes */

/*********************************************************
*NAME:          lv_pillsCreate
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Creates and initilises the pillboxes structure.
*  Sets number of pillboxes to zero
*
*ARGUMENTS:
*  value - Pointer to the map file
*********************************************************/
void lv_pillsCreate(pillboxes *value);

/*********************************************************
*NAME:          lv_pillsDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Destroys the pills data structure. Also frees memory.
*
*ARGUMENTS:
*  value - Pointer to the pills structure
*********************************************************/
void lv_pillsDestroy(pillboxes *value);

/*********************************************************
*NAME:          lv_pillsSetNumPills
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets the number of pills in the structure 
*
*ARGUMENTS:
*  value    - Pointer to the pillbox structure
*  numPills - The number of pills
*********************************************************/
void lv_pillsSetNumPills(pillboxes *value, BYTE numPills);

/*********************************************************
*NAME:          lv_pillsGetNumPills
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Gets the number of pills in the structure
*
*ARGUMENTS:
*  value    - Pointer to the pillbox structure
*********************************************************/
BYTE lv_pillsGetNumPills(pillboxes *value);

/*********************************************************
*NAME:          lv_pillsSetPill
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets a specific pill with its item data
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  item    - Pointer to a pillbox
*  pillNum - The pillbox number
*********************************************************/
void lv_pillsSetPill(pillboxes *value, pillbox *item, BYTE pillNum);

/*********************************************************
*NAME:          lv_pillsExistPos
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Returns whether a pillbox exist at a specific location
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
bool lv_pillsExistPos(pillboxes *value, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          lv_pillsGetPill
*AUTHOR:        John Morrison
*CREATION DATE:   9/2/99
*LAST MODIFIED: 11/11/00
*PURPOSE:
*  Gets a specific pill.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  item    - Pointer to a pillbox
*  pillNum - The pillbox number
*********************************************************/
void lv_pillsGetPill(pillboxes *value, pillbox *item, BYTE pillNum);

void lv_pillsDeletePill(pillboxes *value, BYTE x, BYTE y);

/*********************************************************
*NAME:          lv_pillsGetScreenHealth
*AUTHOR:        John Morrison
*CREATION DATE: 30/10/98
*LAST MODIFIED: 30/10/98
*PURPOSE:
*  Returns the health of a pillbox as a screen define.
*
*ARGUMENTS:
*  value     - Pointer to the pillbox structure
*  xValue    - X Location
*  yValue    - Y Location
*  maxArmour - The round's pill_max_armour at the playhead
*********************************************************/
BYTE lv_pillsGetScreenHealth(pillboxes *value, BYTE xValue, BYTE yValue,
                             int maxArmour);

/*********************************************************
*NAME:          lv_pillsArmourLevel
*PURPOSE:
*  Which of the sixteen pillbox pictures, 0 (empty) to 15
*  (intact), an armour value draws as against a cap of
*  maxArmour. The client's rule: armour at or above the cap
*  is 15, anything below it is armour * 15 / maxArmour
*  rounded down, and a cap below 1 is read as the classic 15.
*
*ARGUMENTS:
*  armour    - The pillbox's armour
*  maxArmour - The round's pill_max_armour
*********************************************************/
BYTE lv_pillsArmourLevel(BYTE armour, int maxArmour);

void lv_pillsSetHealth(pillboxes *value, BYTE pillNum, BYTE health);

void lv_pillsSetInTank(pillboxes *value, BYTE pillNum, bool inTank);

void lv_pillsSetPos(pillboxes *value, BYTE pillNum, BYTE mx, BYTE my);


/*********************************************************
*NAME:          lv_pillsSetPillOwner
*AUTHOR:        John Morrison
*CREATION DATE: 15/1/99
*LAST MODIFIED: 16/2/99
*PURPOSE:
* Sets the pillbox pillNum to owner. Returns the previous
* owner. If migrate is set to TRUE then it has migrated 
* from a alliance when a player left and we shouldn't 
* make a message
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number to apply it to
*  owner   - The new owner
*  migrate - TRUE if it is migrating.
*********************************************************/
BYTE lv_pillsSetPillOwner(pillboxes *value, BYTE pillNum, BYTE owner, bool migrate);

bool lv_pillsChooseView(pillboxes *value, int x, int y);

/*********************************************************
*NAME:          lv_pillsSetPillNetData
*AUTHOR:        John Morrison
*CREATION DATE: 27/02/99
*LAST MODIFIED: 27/07/04
*PURPOSE:
* Sets the pills data to buff.
*
*ARGUMENTS:
*  value   - Pointer to the pills structure
*  buff    - Buffer of data to set pills structure to
*  dataLen - Length of the data
*********************************************************/
void lv_pillsSetPillNetData(pillboxes *value, BYTE *buff, BYTE dataLen);

// Assumes that the pillbox exists otherwise returns 0
BYTE lv_pillsItemNumAt(pillboxes *value, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          lv_pillsGetAllianceNum
*PURPOSE:
*  Returns the alliance type of a pillbox by index for
*  status-panel rendering. Mirrors bolo's pillsGetAllianceNum
*  but resolves "self" via lv_playersGetSelf() instead of a
*  GameSim handle the logviewer doesn't have.
*
*ARGUMENTS:
*  value   - Pointer to the pillboxes structure
*  pillNum - 1-based pillbox index
*********************************************************/
pillAlliance lv_pillsGetAllianceNum(pillboxes *value, BYTE pillNum);

/*********************************************************
*NAME:          lv_pillsIsActive
*PURPOSE:
*  Returns whether a pillbox number names a pillbox that is
*  on the map. A log_EntityChange can take one off mid-round
*  and the slot and the count stay, so a number in range is
*  not on its own enough.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number, 1 based
*********************************************************/
bool lv_pillsIsActive(pillboxes *value, BYTE pillNum);

/*********************************************************
*NAME:          lv_pillsInstallItem
*PURPOSE:
*  Writes a pillbox at the number a log_EntityChange names
*  and puts that slot on the map. A number past the count
*  raises the count and leaves the gap off the map. Mirrors
*  pillsInstallItem on the sim side.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  item    - The pillbox to store
*  pillNum - The pillbox number, 1 based
*********************************************************/
bool lv_pillsInstallItem(pillboxes *value, pillbox *item, BYTE pillNum);

/*********************************************************
*NAME:          lv_pillsRemoveItem
*PURPOSE:
*  Takes a pillbox off the map and keeps its slot, its
*  count and every number above it. Mirrors pillsRemoveItem
*  on the sim side.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number, 1 based
*********************************************************/
bool lv_pillsRemoveItem(pillboxes *value, BYTE pillNum);

/*********************************************************
*NAME:          lv_pillsSetActive
*PURPOSE:
*  Puts a pillbox on the map or takes it off it, leaving
*  its record and the count alone. What a log_EntityMasks
*  record writes. Mirrors pillsSetActive on the sim side.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number, 1 based
*  onMap   - TRUE for on the map, FALSE for off it
*********************************************************/
bool lv_pillsSetActive(pillboxes *value, BYTE pillNum, bool onMap);

#endif /* PILLBOX_H */
