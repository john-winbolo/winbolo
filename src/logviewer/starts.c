/*
 * $Id$
 *
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


/*********************************************************
*Name:          Starts 
*Filename:      starts.c 
*Author:        John Morrison
*Creation Date: 28/10/98
*Last Modified: 28/10/98
*Purpose:
*  Provides operations on player starts 
*********************************************************/

/* Includes */
#include "lv_global.h"
#include "lv_starts.h"

/* Bytes per start record in a snapshot's start block: x, y, dir. */
#define LV_START_NET_RECORD 3

/*********************************************************
*NAME:          lv_startsCreate
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Creates and initilises the player starts structure.
*  Sets number of starts to zero
*
*ARGUMENTS:
*  value - Pointer to the starts structure 
*********************************************************/
void lv_startsCreate(starts *value) {
  New(*value);
  /* emalloc does not zero, and the live flags past the wire region are read
     from the first snapshot on, so the whole structure is cleared here the
     way startsCreate clears the sim's. Every slot starts off the map. */
  memset(*value, 0, sizeof(**value));
  ((*value)->numStarts) = 0;
}

/*********************************************************
*NAME:          lv_startsIsActive
*PURPOSE:
*  Returns whether a start number names a start that is on
*  the map. A recording can take one off mid-round
*  (log_EntityChange) and the slot and the count stay, so a
*  number in range is not on its own enough. A number out of
*  range returns FALSE.
*
*ARGUMENTS:
*  value    - Pointer to the starts structure
*  startNum - The start number, 1 based
*********************************************************/
bool lv_startsIsActive(starts *value, BYTE startNum) {
  if (value == NULL || *value == NULL) {
    return FALSE;
  }
  if (startNum == 0 || startNum > (*value)->numStarts) {
    return FALSE;
  }
  return ((*value)->active[startNum - 1] != FALSE);
}

/*********************************************************
*NAME:          lv_startsInstallItem
*PURPOSE:
*  Writes a start at the number the recording names and puts
*  that slot on the map, whatever the slot held before. A
*  number past the count raises the count to cover it and
*  leaves the slots the gap opens up off the map. Mirrors
*  startsInstallItem, which is what a live client applies the
*  same change through. Returns FALSE for number 0 or a
*  number past MAX_STARTS.
*
*ARGUMENTS:
*  value    - Pointer to the starts structure
*  item     - The start to store
*  startNum - The start number, 1 based
*********************************************************/
bool lv_startsInstallItem(starts *value, start *item, BYTE startNum) {
  BYTE slot;  /* The array index the number names */
  BYTE count; /* Looping variable */

  if (value == NULL || *value == NULL || item == NULL) {
    return FALSE;
  }
  if (startNum == 0 || startNum > MAX_STARTS) {
    return FALSE;
  }
  slot = (BYTE) (startNum - 1);
  for (count = (*value)->numStarts; count < slot; count++) {
    (*value)->active[count] = FALSE;
  }
  if (startNum > (*value)->numStarts) {
    (*value)->numStarts = startNum;
  }
  (*value)->item[slot].x = item->x;
  (*value)->item[slot].y = item->y;
  (*value)->item[slot].dir = item->dir;
  (*value)->active[slot] = TRUE;
  return TRUE;
}

/*********************************************************
*NAME:          lv_startsRemoveItem
*PURPOSE:
*  Takes a start off the map, leaving its slot, the count
*  and every start number above it alone, so the numbers the
*  rest of the recording uses keep meaning the same start.
*  Returns FALSE for a number out of range or one already
*  off the map.
*
*ARGUMENTS:
*  value    - Pointer to the starts structure
*  startNum - The start number, 1 based
*********************************************************/
bool lv_startsRemoveItem(starts *value, BYTE startNum) {
  if (value == NULL || *value == NULL) {
    return FALSE;
  }
  if (startNum == 0 || startNum > MAX_STARTS || startNum > (*value)->numStarts) {
    return FALSE;
  }
  startNum--;
  if ((*value)->active[startNum] == FALSE) {
    return FALSE;
  }
  (*value)->active[startNum] = FALSE;
  return TRUE;
}

/*********************************************************
*NAME:          lv_startsSetActive
*PURPOSE:
*  Puts a start on the map or takes it off it, leaving its
*  record and the count alone either way. This is what a
*  log_EntityMasks record writes, so a slot put back holds
*  the start it already held. Returns FALSE for a number out
*  of range. Mirrors startsSetActive.
*
*ARGUMENTS:
*  value    - Pointer to the starts structure
*  startNum - The start number, 1 based
*  onMap    - TRUE for on the map, FALSE for off it
*********************************************************/
bool lv_startsSetActive(starts *value, BYTE startNum, bool onMap) {
  if (value == NULL || *value == NULL) {
    return FALSE;
  }
  if (startNum == 0 || startNum > (*value)->numStarts) {
    return FALSE;
  }
  (*value)->active[startNum - 1] = onMap ? TRUE : FALSE;
  return TRUE;
}


/*********************************************************
*NAME:          lv_startsDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Destroys the starts data structure. Also frees memory.
*
*ARGUMENTS:
*  value - Pointer to the starts structure
*********************************************************/
void lv_startsDestroy(starts *value) {
  Dispose(*value);
}

/*********************************************************
*NAME:          lv_startsSetNumStarts
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets the number of starts in the structure 
*
*ARGUMENTS:
*  value     - Pointer to the starts structure
*  numStarts - The number of starts 
*********************************************************/
void lv_startsSetNumStarts(starts *value, BYTE numStarts) {
  BYTE count; /* Looping variable */

  if (numStarts <= MAX_STARTS) {
    (*value)->numStarts = numStarts;
    /* Nothing past the count names a start, so a shorter list must not leave
       the slots it dropped reading as on the map. */
    for (count = numStarts; count < MAX_STARTS; count++) {
      (*value)->active[count] = FALSE;
    }
  }
}


/*********************************************************
*NAME:          lv_startsGetNumStarts
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Gets the number of starts in the structure
*
*ARGUMENTS:
*  value  - Pointer to the starts structure
*********************************************************/
BYTE lv_startsGetNumStarts(starts *value) {
  return (*value)->numStarts;
}

/*********************************************************
*NAME:          lv_startsSetStart
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets a specific start with its item data
*
*ARGUMENTS:
*  value    - Pointer to the starts structure
*  item     - Pointer to a player start 
*  startNum - The start number
*********************************************************/
void lv_startsSetStart(starts *value, start *item, BYTE startNum) {
  if (startNum > 0 && startNum  <= (*value)->numStarts) {
    startNum--;
    (((*value)->item[startNum]).x) = item->x;
    (((*value)->item[startNum]).y) = item->y;
    (((*value)->item[startNum]).dir) = item->dir;
    /* Only ever called to put a real start in the slot, so the slot is on the
       map whatever it held before. */
    (*value)->active[startNum] = TRUE;
  }
}


/*********************************************************
*NAME:          lv_startsGetStartStruct
*AUTHOR:        John Morrison
*CREATION DATE:   9/2/98
*LAST MODIFIED: 11/11/00
*PURPOSE:
*  Gets a specific start.
*
*ARGUMENTS:
*  value    - Pointer to the starts structure
*  item     - Pointer to a player start 
*  startNum - The start number
*********************************************************/
void lv_startsGetStartStruct(starts *value, start *item, BYTE startNum) {
  if (startNum > 0 && startNum  <= (*value)->numStarts) {
    startNum--;
    item->x = ((*value)->item[startNum]).x;
    item->y = ((*value)->item[startNum]).y;
    item->dir = ((*value)->item[startNum]).dir;
  }
}


void lv_startsDeleteStart(starts *value, BYTE x, BYTE y) {
  BYTE count = 0;
  BYTE count2;
  
  while (count < (*value)->numStarts) {
    if ((*value)->item[count].x == x && (*value)->item[count].y == y) {
      count2 = count;
      while (count2 < (*value)->numStarts - 1) {
        (*value)->item[count2].x = (*value)->item[count2+1].x;
        (*value)->item[count2].y = (*value)->item[count2+1].y;
        (*value)->item[count2].dir = (*value)->item[count2+1].dir;
        count2++;
      }
      (*value)->numStarts--;
    }
    count++;
  }
}

bool lv_startsExistPos(starts *value, BYTE xValue, BYTE yValue) {
  bool returnValue; /* Value to return */
  BYTE count;       /* Looping Variable */
  returnValue = FALSE;
  count = 0;

  while (returnValue == FALSE && count < ((*value)->numStarts)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue) {
      returnValue = TRUE;
    }
    count++;
  }
  return returnValue;
}

/*********************************************************
*NAME:          lv_startsSetStartNetData
*AUTHOR:        John Morrison
*CREATION DATE: 27/02/99
*LAST MODIFIED: 24/07/04
*PURPOSE:
* Sets the starts data to buff from the network.
*
*ARGUMENTS:
*  value   - Pointer to the starts structure
*  buff    - Buffer of data to set starts structure to
*  dataLen - Length of the data
*********************************************************/
void lv_startsSetStartNetData(starts *value, BYTE *buff, BYTE dataLen) {
  BYTE count = 0;
  BYTE len = 1;
  BYTE known = (*value)->numStarts; /* How many numbers this list already had */
  BYTE live;                        /* Numbers the blob actually describes */
  BYTE avail;                       /* Records the block's length can hold */

  /* The count is a byte read off the recording and item[] holds MAX_STARTS,
     so a count past that, or past what the block's own length can describe,
     is a damaged or hostile file. Clamp it before it is stored or walked;
     nothing below indexes by the raw byte. */
  live = buff[0];
  avail = (BYTE)(dataLen >= 1 ? (dataLen - 1) / LV_START_NET_RECORD : 0);
  if (live > MAX_STARTS) {
    live = MAX_STARTS;
  }
  if (live > avail) {
    live = avail;
  }
  (*value)->numStarts = live;
  /* The blob is the map: a count and a record per start, with no room for
     which of them are on it — the live flags sit past the wire region on both
     sides. So a snapshot restates the records and leaves the flags to the
     stream: a number this list already knew keeps the flag the records have
     given it, a number the blob has grown past the old count arrives on the
     map, and a number the blob no longer reaches is off it. Without that a
     start a log_EntityChange had taken away would come back at the next
     snapshot and stay. */
  if (known > MAX_STARTS) {
    known = MAX_STARTS;
  }
  for (count = known; count < live; count++) {
    (*value)->active[count] = TRUE;
  }
  for (count = live; count < MAX_STARTS; count++) {
    (*value)->active[count] = FALSE;
  }

  count = 0;
  while (count < live) {
    (*value)->item[count].x = buff[len];
    len++;
    (*value)->item[count].y = buff[len];
    len++;
    (*value)->item[count].dir = buff[len];
    len++;
    count++;
  }
}
