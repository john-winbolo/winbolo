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
*Filename:      pillbox.c
*Author:        John Morrison
*Creation Date: 28/10/98
*Last Modified: 11/11/00
*Purpose:
*  Provides operations on pillbox and pillboxes
*********************************************************/

/* Includes */
#include "lv_global.h"
#include "lv_tilenum.h"
#include "lv_pillbox.h"
#include "backend.h"
#include "lv_players.h"
#include "lv_messages.h"
#include "../gui/lang.h"

/* Bytes per pillbox record in a snapshot's pill block: x, y, armour, owner,
   speed, inTank, reload, justSeen, coolDown. */
#define LV_PILL_NET_RECORD 9

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
void lv_pillsCreate(pillboxes *value) {
  New(*value);
  /* emalloc does not zero, and the live flags past the wire region are read
     from the first snapshot on, so the whole structure is cleared here the
     way pillsCreate clears the sim's. Every slot starts off the map. */
  memset(*value, 0, sizeof(**value));
  ((*value)->numPills) = 0;
}

/*********************************************************
*NAME:          lv_pillsIsActive
*PURPOSE:
*  Returns whether a pillbox number names a pillbox that is
*  on the map. A recording can take one off mid-round
*  (log_EntityChange) and the slot and the count stay, so a
*  number in range is not on its own enough. A number out of
*  range returns FALSE.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number, 1 based
*********************************************************/
bool lv_pillsIsActive(pillboxes *value, BYTE pillNum) {
  if (value == NULL || *value == NULL) {
    return FALSE;
  }
  if (pillNum == 0 || pillNum > (*value)->numPills) {
    return FALSE;
  }
  return ((*value)->active[pillNum - 1] != FALSE);
}

/*********************************************************
*NAME:          lv_pillsInstallItem
*PURPOSE:
*  Writes a pillbox at the number the recording names and
*  puts that slot on the map, whatever the slot held before.
*  A number past the count raises the count to cover it and
*  leaves the slots the gap opens up off the map. Mirrors
*  pillsInstallItem, which is what a live client applies the
*  same change through. Returns FALSE for number 0 or a
*  number past MAX_PILLS.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  item    - The pillbox to store
*  pillNum - The pillbox number, 1 based
*********************************************************/
bool lv_pillsInstallItem(pillboxes *value, pillbox *item, BYTE pillNum) {
  BYTE slot;  /* The array index the number names */
  BYTE count; /* Looping variable */

  if (value == NULL || *value == NULL || item == NULL) {
    return FALSE;
  }
  if (pillNum == 0 || pillNum > MAX_PILLS) {
    return FALSE;
  }
  slot = (BYTE) (pillNum - 1);
  for (count = (*value)->numPills; count < slot; count++) {
    (*value)->active[count] = FALSE;
  }
  if (pillNum > (*value)->numPills) {
    (*value)->numPills = pillNum;
  }
  (*value)->item[slot].x = item->x;
  (*value)->item[slot].y = item->y;
  (*value)->item[slot].owner = item->owner;
  (*value)->item[slot].armour = item->armour;
  (*value)->item[slot].speed = item->speed;
  (*value)->item[slot].inTank = item->inTank;
  (*value)->active[slot] = TRUE;
  return TRUE;
}

/*********************************************************
*NAME:          lv_pillsRemoveItem
*PURPOSE:
*  Takes a pillbox off the map, leaving its slot, the count
*  and every pillbox number above it alone, so the numbers
*  the rest of the recording uses keep meaning the same
*  pillbox. Returns FALSE for a number out of range or one
*  already off the map.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number, 1 based
*********************************************************/
bool lv_pillsRemoveItem(pillboxes *value, BYTE pillNum) {
  if (value == NULL || *value == NULL) {
    return FALSE;
  }
  if (pillNum == 0 || pillNum > MAX_PILLS || pillNum > (*value)->numPills) {
    return FALSE;
  }
  pillNum--;
  if ((*value)->active[pillNum] == FALSE) {
    return FALSE;
  }
  (*value)->active[pillNum] = FALSE;
  return TRUE;
}

/*********************************************************
*NAME:          lv_pillsSetActive
*PURPOSE:
*  Puts a pillbox on the map or takes it off it, leaving its
*  record and the count alone either way. This is what a
*  log_EntityMasks record writes, so a slot put back holds
*  the pillbox it already held. Returns FALSE for a number
*  out of range. Mirrors pillsSetActive.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number, 1 based
*  onMap   - TRUE for on the map, FALSE for off it
*********************************************************/
bool lv_pillsSetActive(pillboxes *value, BYTE pillNum, bool onMap) {
  if (value == NULL || *value == NULL) {
    return FALSE;
  }
  if (pillNum == 0 || pillNum > (*value)->numPills) {
    return FALSE;
  }
  (*value)->active[pillNum - 1] = onMap ? TRUE : FALSE;
  return TRUE;
}


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
void lv_pillsDestroy(pillboxes *value) {
  Dispose(*value);
}

/*********************************************************
*NAME:          lv_pillsSetNumPills
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets a specific pill with its item data
*
*ARGUMENTS:
*  value    - Pointer to the pillbox structure
*  numPills - The number of pills
*********************************************************/
void lv_pillsSetNumPills(pillboxes *value, BYTE numPills) {
  BYTE count; /* Looping variable */

  if (numPills > 0 && numPills <= MAX_PILLS) {
    (*value)->numPills = numPills;
    /* Nothing past the count names a pillbox, so a shorter list must not
       leave the slots it dropped reading as on the map. */
    for (count = numPills; count < MAX_PILLS; count++) {
      (*value)->active[count] = FALSE;
    }
  }
}

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
BYTE lv_pillsGetNumPills(pillboxes *value) {
  return (*value)->numPills;
}

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
void lv_pillsSetPill(pillboxes *value, pillbox *item, BYTE pillNum) {
  if (pillNum > 0 && pillNum  <= (*value)->numPills) {
    pillNum--;
    (((*value)->item[pillNum]).x) = item->x;
    (((*value)->item[pillNum]).y) = item->y;
    (((*value)->item[pillNum]).owner) = item->owner;
    (((*value)->item[pillNum]).armour) = item->armour;
    (((*value)->item[pillNum]).speed) = item->speed;
    (((*value)->item[pillNum]).inTank) = item->inTank;
    /* Only ever called to put a real pillbox in the slot, so the slot is on
       the map whatever it held before. */
    (*value)->active[pillNum] = TRUE;
  }
}

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
bool lv_pillsExistPos(pillboxes *value, BYTE xValue, BYTE yValue) {
  bool returnValue; /* Value to return */
  BYTE count;       /* Looping Variable */

  returnValue = FALSE;
  count = 0;
  while (returnValue == FALSE && count < ((*value)->numPills)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue && (*value)->item[count].inTank == FALSE) {
      returnValue = TRUE;
    }
    count++;
  }

  return returnValue;
}


// Assumes that the pillbox exists otherwise returns 0
BYTE lv_pillsItemNumAt(pillboxes *value, BYTE xValue, BYTE yValue) {
  BYTE count;       /* Looping Variable */

  count = 0;
  while (count < ((*value)->numPills)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue && (*value)->item[count].inTank == FALSE) {
      return count;
    }
    count++;
  }

  return 0;
}

pillAlliance lv_pillsGetAllianceNum(pillboxes *value, BYTE pillNum) {
  pillAlliance returnValue = pillNeutral;
  BYTE self;

  if (*value == NULL) return returnValue;
  if (pillNum < 1 || pillNum > (*value)->numPills) return returnValue;
  /* A pillbox the recording has taken off the map has no owner to report:
     its record is the stale one the slot was carrying when it went. The
     status panel skips such a number rather than drawing this default. */
  if (lv_pillsIsActive(value, pillNum) == FALSE) return returnValue;
  pillNum--;
  self = lv_playersGetSelf();

  if ((*value)->item[pillNum].armour == 0 && (*value)->item[pillNum].inTank == FALSE) {
    return pillDead;
  }
  if ((*value)->item[pillNum].owner == self) {
    return (*value)->item[pillNum].inTank ? pillTankGood : pillGood;
  }
  if (lv_playersIsAllie((*value)->item[pillNum].owner, self) == TRUE) {
    return (*value)->item[pillNum].inTank ? pillTankAllie : pillAllie;
  }
  if ((*value)->item[pillNum].owner != NEUTRAL) {
    return (*value)->item[pillNum].inTank ? pillTankEvil : pillEvil;
  }
  return returnValue;
}


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
void lv_pillsGetPill(pillboxes *value, pillbox *item, BYTE pillNum) {
  if (pillNum > 0 && pillNum  <= (*value)->numPills) {
    pillNum--;
    item->x = ((*value)->item[pillNum]).x;
    item->y = ((*value)->item[pillNum]).y;
    item->owner = ((*value)->item[pillNum]).owner;
    item->armour = ((*value)->item[pillNum]).armour;
    item->speed = ((*value)->item[pillNum]).speed;
  }
}


void lv_pillsDeletePill(pillboxes *value, BYTE x, BYTE y) {
  BYTE count = 0;
  BYTE count2;
  
  while (count < (*value)->numPills) {
    if ((*value)->item[count].x == x && (*value)->item[count].y == y) {
      count2 = count;
      while (count2 < (*value)->numPills - 1) {
        (*value)->item[count2].x = (*value)->item[count2+1].x;
        (*value)->item[count2].y = (*value)->item[count2+1].y;
        (*value)->item[count2].speed = (*value)->item[count2+1].speed;
        (*value)->item[count2].owner = (*value)->item[count2+1].owner;
        (*value)->item[count2].armour = (*value)->item[count2+1].armour;
        count2++;
      }
      (*value)->numPills--;
    }
    count++;
  }
}

/*********************************************************
*NAME:          lv_pillsGetScreenHealth
*AUTHOR:        John Morrison
*CREATION DATE: 30/10/98
*LAST MODIFIED: 30/10/98
*PURPOSE:
*  Returns the health of a pillbox as a screen define.
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
BYTE lv_pillsGetScreenHealth(pillboxes *value, BYTE xValue, BYTE yValue) {
  bool done;        /* Finished searching */
  BYTE returnValue; /* Value to return */
  BYTE count;       /* Looping Variable */

  done = FALSE;
  count = 0;
  returnValue = PILL_EVIL_15;

  while (done == FALSE && count < ((*value)->numPills)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue && (*value)->item[count].inTank == FALSE) {
      /* Pillbox has been Hit */
      done = TRUE;
      
      if (lv_playersIsAllie((*value)->item[count].owner, lv_playersGetSelf() ) == FALSE) {
        switch((*value)->item[count].armour) {
        case PILLBOX_15:
          returnValue = PILL_EVIL_15;
          break;
        case PILLBOX_14:
          returnValue = PILL_EVIL_14;
          break;
        case PILLBOX_13:
          returnValue = PILL_EVIL_13;
          break;
        case PILLBOX_12:
          returnValue = PILL_EVIL_12;
          break;
        case PILLBOX_11:
          returnValue = PILL_EVIL_11;
          break;
        case PILLBOX_10:
          returnValue = PILL_EVIL_10;
          break;
        case PILLBOX_9:
          returnValue = PILL_EVIL_9;
          break;
        case PILLBOX_8:
          returnValue = PILL_EVIL_8;
          break;
        case PILLBOX_7:
          returnValue = PILL_EVIL_7;
          break;
        case PILLBOX_6:
          returnValue = PILL_EVIL_6;
          break;
        case PILLBOX_5:
          returnValue = PILL_EVIL_5;
          break;
        case PILLBOX_4:
          returnValue = PILL_EVIL_4;
          break;
        case PILLBOX_3:
          returnValue = PILL_EVIL_3;
          break;
        case PILLBOX_2:
          returnValue = PILL_EVIL_2;
          break;
        case PILLBOX_1:
          returnValue = PILL_EVIL_1;
          break;
        case PILLBOX_0:
          returnValue = PILL_EVIL_0;
          break;
        }
      } else {
        switch((*value)->item[count].armour) {
        case PILLBOX_15:
          returnValue = PILL_GOOD_15;
          break;
        case PILLBOX_14:
          returnValue = PILL_GOOD_14;
          break;
        case PILLBOX_13:
          returnValue = PILL_GOOD_13;
          break;
        case PILLBOX_12:
          returnValue = PILL_GOOD_12;
          break;
        case PILLBOX_11:
          returnValue = PILL_GOOD_11;
          break;
        case PILLBOX_10:
          returnValue = PILL_GOOD_10;
          break;
        case PILLBOX_9:
          returnValue = PILL_GOOD_9;
          break;
        case PILLBOX_8:
          returnValue = PILL_GOOD_8;
          break;
        case PILLBOX_7:
          returnValue = PILL_GOOD_7;
          break;
        case PILLBOX_6:
          returnValue = PILL_GOOD_6;
          break;
        case PILLBOX_5:
          returnValue = PILL_GOOD_5;
          break;
        case PILLBOX_4:
          returnValue = PILL_GOOD_4;
          break;
        case PILLBOX_3:
          returnValue = PILL_GOOD_3;
          break;
        case PILLBOX_2:
          returnValue = PILL_GOOD_2;
          break;
        case PILLBOX_1:
          returnValue = PILL_GOOD_1;
          break;
        case PILLBOX_0:
          returnValue = PILL_GOOD_0;
          break;
        }
      }

    }
    count++;
  }

  return returnValue;
}

void lv_pillsSetHealth(pillboxes *value, BYTE pillNum, BYTE health) {
  (*value)->item[pillNum].armour = health;
}

void lv_pillsSetInTank(pillboxes *value, BYTE pillNum, bool inTank) {
  (*value)->item[pillNum].inTank = inTank;
}

void lv_pillsSetPos(pillboxes *value, BYTE pillNum, BYTE mx, BYTE my) {
  (*value)->item[pillNum].x = mx;
  (*value)->item[pillNum].y = my;
}

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
BYTE lv_pillsSetPillOwner(pillboxes *value, BYTE pillNum, BYTE owner, bool migrate) {
  char ownerName[FILENAME_MAX];
  char oldOwner[FILENAME_MAX];
  BYTE returnValue; /* Value to return */

  ownerName[0] = '\0';
  oldOwner[0] = '\0';

  returnValue = NEUTRAL;
  returnValue = (*value)->item[pillNum].owner;
  (*value)->item[pillNum].owner = owner;
  /* Make the message if required */
  if (returnValue == NEUTRAL && migrate == FALSE) {
    /* Neutral pill — normalized to MESSAGE_CAPTURE_PILL ("Neutral Pillbox"). */
    MessageArgs args = {0};
    lv_playersMakeMessageName(owner, ownerName);
    snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, ownerName);
    lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_CAPTURE_PILL, &args);
  } else if (owner == NEUTRAL) {
    /* Do nothing */
  } else if (lv_playersIsAllie(returnValue, owner) == FALSE && migrate == FALSE) {
    /* Stole pill */
    MessageArgs args = {0};
    lv_playersMakeMessageName(owner, ownerName);
    snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, ownerName);
    lv_playersGetPlayerName(returnValue, oldOwner, sizeof(oldOwner));
    snprintf(args.otherName, sizeof(args.otherName), "%.*s", (int)sizeof(args.otherName) - 1, oldOwner);
    lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_STOLE_PILL, &args);
  }
  (*value)->item[pillNum].owner = owner;
//    frontEndStatusPillbox(pillNum, (pillsGetAllianceNum(value, pillNum)));
  return returnValue;
}


bool lv_pillsChooseView(pillboxes *value, int x, int y) {
  BYTE count = 0;
  while (count < (*value)->numPills) {
    if ((*value)->active[count] != FALSE && (*value)->item[count].inTank == FALSE && (*value)->item[count].x == x && (*value)->item[count].y == y && (*value)->item[count].owner != NEUTRAL) {
      lv_playersSetSelf((*value)->item[count].owner);
      return TRUE;
    }
    count++;
  }
  return FALSE;
}

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
void lv_pillsSetPillNetData(pillboxes *value, BYTE *buff, BYTE dataLen) {
  BYTE count = 0;
  BYTE len = 1;
  BYTE known = (*value)->numPills; /* How many numbers this list already had */
  BYTE live;                       /* Numbers the blob actually describes */
  BYTE avail;                      /* Records the block's length can hold */

  /* The count is a byte read off the recording and item[] holds MAX_PILLS,
     so a count past that, or past what the block's own length can describe,
     is a damaged or hostile file. Clamp it before it is stored or walked;
     nothing below indexes by the raw byte. */
  live = buff[0];
  avail = (BYTE)(dataLen >= 1 ? (dataLen - 1) / LV_PILL_NET_RECORD : 0);
  if (live > MAX_PILLS) {
    live = MAX_PILLS;
  }
  if (live > avail) {
    live = avail;
  }
  (*value)->numPills = live;
  /* The blob is the map: a count and a record per pillbox, with no room for
     which of them are on it — the live flags sit past the wire region on both
     sides. So a snapshot restates the records and leaves the flags to the
     stream: a number this list already knew keeps the flag the records have
     given it, a number the blob has grown past the old count arrives on the
     map, and a number the blob no longer reaches is off it. Without that a
     pillbox a log_EntityChange had taken away would come back at the next
     snapshot and stay. */
  if (known > MAX_PILLS) {
    known = MAX_PILLS;
  }
  for (count = known; count < live; count++) {
    (*value)->active[count] = TRUE;
  }
  for (count = live; count < MAX_PILLS; count++) {
    (*value)->active[count] = FALSE;
  }

  count = 0;
  while (count < live) {
    (*value)->item[count].x = buff[len];
    len++;
    (*value)->item[count].y = buff[len];
    len++;
    (*value)->item[count].armour = buff[len];
    len++;
    (*value)->item[count].owner = buff[len];
    len++;
    (*value)->item[count].speed = buff[len];
    len++;
    (*value)->item[count].inTank = buff[len];
    len++;
/*    (*value)->item[count].reload = buff[len]; */
    len++;
/*    (*value)->item[count].justSeen = buff[len]; */
    len++;
/*    (*value)->item[count].coolDown = buff[len]; */
    len++;
    count++;
  }
}
