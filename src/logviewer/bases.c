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
*Name:          Bases 
*Filename:      bases.c 
*Author:        John Morrison
*Creation Date: 28/10/98
*Last Modified: 11/11/00
*Purpose:
*  Provides operations on bases 
*********************************************************/

/* Includes */
#include "lv_global.h"
#include "lv_bases.h"
#include "backend.h"
#include "lv_players.h"
#include "lv_messages.h"
#include "../gui/lang.h"


/*********************************************************
*NAME:         lv_basesCreate 
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Creates and initilises the bases structure.
*  Sets number of bases to zero
*
*ARGUMENTS:
*  value - Pointer to the bases structure 
*********************************************************/
void lv_basesCreate(bases *value) {
  New(*value);
  (*value)->numBases = 0;
}

/*********************************************************
*NAME:          lv_basesDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Destroys the bases data structure. Also frees memory.
*
*ARGUMENTS:
*  value - Pointer to the bases structure
*********************************************************/
void lv_basesDestroy(bases *value) {
  Dispose(*value);
}

/*********************************************************
*NAME:          lv_basesSetNumBases
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets the number of bases in the structure 
*
*ARGUMENTS:
*  value     - Pointer to the bases structure
*  numBases - The number of bases  
*********************************************************/
void lv_basesSetNumBases(bases *value, BYTE numBases) {
  if (numBases <= MAX_BASES) {
    (*value)->numBases = numBases;
  }
}


/*********************************************************
*NAME:          lv_basesGetNumBases 
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Gets the number of bases in the structure
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*********************************************************/
BYTE lv_basesGetNumBases(bases *value) {
  return (*value)->numBases;
}

/*********************************************************
*NAME:          lv_basesSetBase
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets a specific base with its item data
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  item    - Pointer to a base item 
*  baseNum - The base number
*********************************************************/
void lv_basesSetBase(bases *value, base *item, BYTE baseNum) {
  if (baseNum > 0 && baseNum <= (*value)->numBases) {
    baseNum--;
    (((*value)->item[baseNum]).x) = item->x;
    (((*value)->item[baseNum]).y) = item->y;
    (((*value)->item[baseNum]).owner) = item->owner;
    (((*value)->item[baseNum]).armour) = item->armour;
    (((*value)->item[baseNum]).shells) = item->shells;
    (((*value)->item[baseNum]).mines) = item->mines;
  }
}

/*********************************************************
*NAME:          lv_basesExistPos
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Returns whether a base exist at a specific location
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
bool lv_basesExistPos(bases *value, BYTE xValue, BYTE yValue) {
  bool returnValue; /* Value to return */
  BYTE count;       /* Looping Variable */

  returnValue = FALSE;
  count = 0;
  while (returnValue == FALSE && count < ((*value)->numBases)) {
    if (((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue) {
      returnValue = TRUE;
    }
    count++;
  }

  return returnValue;
}

// Assumes that the base exists otherwise returns 0
BYTE lv_basesItemNumAt(bases *value, BYTE xValue, BYTE yValue) {
  BYTE count;       /* Looping Variable */

  count = 0;
  while (count < ((*value)->numBases)) {
    if (((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue) {
      return count;
    }
    count++;
  }

  return 0;
}


/*********************************************************
*NAME:          lv_basesGetBase
*AUTHOR:        John Morrison
*CREATION DATE:   9/2/98
*LAST MODIFIED: 11/11/00
*PURPOSE:
*  Gets a specific base
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  item    - Pointer to a base item 
*  baseNum - The base number
*********************************************************/
void lv_basesGetBase(bases *value, base *item, BYTE baseNum) {
  if (baseNum > 0 && baseNum <= (*value)->numBases) {
    baseNum--;
    item->x = ((*value)->item[baseNum]).x;
    item->y = ((*value)->item[baseNum]).y;
    item->owner = ((*value)->item[baseNum]).owner;
    item->armour = ((*value)->item[baseNum]).armour;
    item->shells = ((*value)->item[baseNum]).shells;
    item->mines = ((*value)->item[baseNum]).mines;
  }
}

void lv_basesDeleteBase(bases *value, BYTE x, BYTE y) {
  BYTE count = 0;
  BYTE count2;

  while (count < (*value)->numBases) {
    if ((*value)->item[count].x == x && (*value)->item[count].y == y) {
      count2 = count;
      while (count2 < (*value)->numBases - 1) {
        (*value)->item[count2].x = (*value)->item[count2+1].x;
        (*value)->item[count2].y = (*value)->item[count2+1].y;
        (*value)->item[count2].owner = (*value)->item[count2+1].owner;
        (*value)->item[count2].armour = (*value)->item[count2+1].armour;
        (*value)->item[count2].shells = (*value)->item[count2+1].shells;
        (*value)->item[count2].mines = (*value)->item[count2+1].mines;
	count2++;
      }
      (*value)->numBases--;
    }
    count++;
  }
}

/*********************************************************
*NAME:          lv_basesAmOwner
*AUTHOR:        John Morrison
*CREATION DATE: 10/1/99
*LAST MODIFIED: 31/10/99
*PURPOSE:
*  Returns whether the bases at the particular location
*  is owned by the player
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  owner  - Owner to check with
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
bool lv_basesAmOwner(bases *value, BYTE owner, BYTE xValue, BYTE yValue) {
  bool returnValue;         /* Value to return */
  BYTE self;                /* Our player number */
  bool done;                /* Finished looping */
  BYTE count;               /* Looping Variable */

  returnValue = FALSE;
  count = 0;
  done = FALSE;
  //FIXME: This is redundent.
  self = owner;
  while (done == FALSE && count < ((*value)->numBases)) {
    if (((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue) {
      if ((*value)->item[count].owner == self || (lv_playersIsAllie((*value)->item[count].owner, self) == TRUE)) {
        returnValue = TRUE;
      }
      done = TRUE;
    }
    count++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          lv_basesGetAlliancePos
*AUTHOR:        John Morrison
*CREATION DATE: 21/12/98
*LAST MODIFIED: 21/12/98
*PURPOSE:
*  Returns the type of a base (good, neutral, evil) 
*  depending on the map position. Returns base neutral if
*  base not found.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
baseAlliance lv_basesGetAlliancePos(bases *value, BYTE xValue, BYTE yValue) {
  baseAlliance returnValue; /* Value to return */
  bool done;                /* Finished looping */
  BYTE count;               /* Looping Variable */

  returnValue = baseNeutral;
  count = 0;
  done = FALSE;
  while (done == FALSE && count < ((*value)->numBases)) {
    if (((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue) {
     if ((*value)->item[count].armour == 0) {
        returnValue = baseDead;
      } else if ((*value)->item[count].owner == NEUTRAL) {
        returnValue = baseNeutral;
      } else if ((*value)->item[count].owner == lv_playersGetSelf()) {
        returnValue = baseOwnGood;
      } else if (lv_playersIsAllie((*value)->item[count].owner, lv_playersGetSelf()) == TRUE) {
        returnValue = baseAllieGood;
      } else {
        returnValue = baseEvil;
      }
      done = TRUE;
    }
    count++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          lv_basesSetOwner
*AUTHOR:        John Morrison
*CREATION DATE: 10/1/99
*LAST MODIFIED: 2/11/99
*PURPOSE:
* Sets the base to be owned by paremeter passed.
* Returns the previous owner. If it was not neutral we
* assume then it was "stolen" and subsequently remove
* all its possessions. If migrate is set to TRUE then
* it has migrated from a alliance when a player left 
* and we shouldn't make a message
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  baseNum - Base Number
*  owner   - Who owns it
*  migrate - TRUE if it has migrated from an alliance
*********************************************************/
BYTE lv_basesSetOwner(bases *value, BYTE baseNum, BYTE owner, BYTE migrate) {
  BYTE returnValue;         /* Value to return */
  char ownerName[FILENAME_MAX];
  char oldOwner[FILENAME_MAX];

  ownerName[0] = '\0';
  oldOwner[0] = '\0';

  returnValue = (*value)->item[baseNum].owner;
  if (migrate == TRUE) {
    (*value)->item[baseNum].owner = owner;
  } else if (owner == NEUTRAL) {
    (*value)->item[baseNum].owner = owner;
  } else if ((*value)->item[baseNum].owner != owner) {
    lv_playersMakeMessageName(owner, ownerName);
    MessageArgs args = {0};
    snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, ownerName);
    if (returnValue != NEUTRAL) {
      lv_playersGetPlayerName(returnValue, oldOwner, sizeof(oldOwner));
      snprintf(args.otherName, sizeof(args.otherName), "%.*s", (int)sizeof(args.otherName) - 1, oldOwner);
      lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_STOLE_BASE, &args);
    } else {
      /* Wording normalizes to bolo's MESSAGE_CAPTURE_BASE
       * ("Neutral Base"); the legacy lower-case "neutral base"
       * variant is gone. */
      lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_CAPTURE_BASE, &args);
    }
    (*value)->item[baseNum].owner = owner;
  }
  return returnValue;
}

void lv_basesSetStock(bases *value, BYTE baseNum, BYTE s, BYTE m, BYTE a) {
  (*value)->item[baseNum].shells = s;
  (*value)->item[baseNum].mines = m;
  (*value)->item[baseNum].armour = a;
}

bool lv_basesChooseView(bases *value, int x, int y) {
  BYTE count = 0;
  while (count < (*value)->numBases) {
    if ((*value)->item[count].x == x && (*value)->item[count].y == y && (*value)->item[count].owner != NEUTRAL) {
      lv_playersSetSelf((*value)->item[count].owner);
      return TRUE;
    }
    count++;
  }
  return FALSE;
}

/*********************************************************
*NAME:          lv_basesSetBaseNetData
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED: 27/2/99
*PURPOSE:
* Sets the base data to buff.
*
*ARGUMENTS:
*  value - Pointer to the bases structure
*  buff  - Buffer of data to set base structure to
*  len   - Length of the data
*********************************************************/
void lv_basesSetBaseNetData(bases *value, BYTE *buff, int len)  {
  BYTE returnValue = 1;
  BYTE count = 0;
  unsigned short us;

  (*value)->numBases = buff[0];
  if ((*value)->numBases > 16) {
    count = (*value)->numBases;
  }
  while (count < (*value)->numBases) {
    (*value)->item[count].x = buff[returnValue];
    returnValue++;
    (*value)->item[count].y = buff[returnValue];
    returnValue++;
    (*value)->item[count].owner = buff[returnValue];
    returnValue++;
    (*value)->item[count].armour = buff[returnValue];
    returnValue++;
    (*value)->item[count].shells = buff[returnValue];
    returnValue++;
    (*value)->item[count].mines = buff[returnValue];
    returnValue++;
/*    (*value)->item[count].refuelTime = buff[returnValue]; */
    returnValue++;
    us = buff[returnValue];
    us += (buff[returnValue+1] << 8);
/*    (*value)->item[count].baseTime = ntohs(us); */
    returnValue += 2;
/*    (*value)->item[count].justStopped = buff[returnValue]; */
    returnValue++;
    count++;
  }
}
