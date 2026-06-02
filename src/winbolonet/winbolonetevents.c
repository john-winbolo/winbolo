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
*Name:          WinBolo.net Event
*Filename:      winbolonetevents.c
*Author:        John Morrison
*Creation Date: 04/04/02
*Last Modified: 30/03/26
*Purpose:
*  Responsible for tracking winbolonetEvents
*********************************************************/

#include "global.h"
#include "winbolonetevents.h"

winbolonetEvents wbe; /* Winbolo.net Event */

/*********************************************************
*NAME:          winbolonetEventsCreate
*PURPOSE:
*  Sets up the winbolonetEvents data structure
*********************************************************/
void winbolonetEventsCreate(void) {
  wbe = NULL;
}

/*********************************************************
*NAME:          winbolonetEventsDestroy
*PURPOSE:
*  Destroys and frees memory for the winbolonetEvents
*  data structure
*********************************************************/
void winbolonetEventsDestroy(void) {
  winbolonetEvents q;

  while (!IsEmpty(wbe)) {
    q = wbe;
    wbe = winbolonetEventsTail(q);
    Dispose(q);
  }
}

/*********************************************************
*NAME:          winbolonetEventsAddItem
*PURPOSE:
*  Adds an item to the winbolonetEvents data structure.
*********************************************************/
void winbolonetEventsAddItem(BYTE itemType, const char *keyA, const char *keyB, bool aIsBot, bool bIsBot) {
  winbolonetEvents q;

  q = wbe;
  New (q);
  q->itemType = itemType;
  strncpy(q->keyA, keyA, WINBOLONET_KEY_LEN - 1);
  q->keyA[WINBOLONET_KEY_LEN - 1] = '\0';
  strncpy(q->keyB, keyB, WINBOLONET_KEY_LEN - 1);
  q->keyB[WINBOLONET_KEY_LEN - 1] = '\0';
  q->aIsBot = aIsBot;
  q->bIsBot = bIsBot;
  q->next = wbe;
  wbe = q;
}

/*********************************************************
*NAME:          winbolonetEventsGetSize
*PURPOSE:
*  Returns the number of events in the queue.
*********************************************************/
int winbolonetEventsGetSize(void) {
  int returnValue;    /* Size to return */
  winbolonetEvents q;

  q = wbe;
  returnValue = 0;
  while (NonEmpty(q)) {
    q = winbolonetEventsTail(q);
    returnValue++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          winbolonetEventsRemove
*PURPOSE:
*  Removes the oldest item from the winbolonetEvents data
*  structure. Returns itemType or WINBOLONET_EVENT_NOITEM
*  if empty.
*********************************************************/
BYTE winbolonetEventsRemove(char *keyA, char *keyB, bool *aIsBot, bool *bIsBot) {
  BYTE returnValue;       /* Return Value - Item type */
  winbolonetEvents inc;
  winbolonetEvents prev;
  winbolonetEvents prev2;
  inc = wbe;
  prev = NULL;
  prev2 = NULL;

  returnValue = WINBOLONET_EVENT_NOITEM;

  if (inc != NULL) {
    while (NonEmpty(inc)) {
      prev2 = prev;
      prev = inc;
      inc = winbolonetEventsTail(inc);
    }

    if (prev != NULL) {
      returnValue = prev->itemType;
      strncpy(keyA, prev->keyA, WINBOLONET_KEY_LEN);
      strncpy(keyB, prev->keyB, WINBOLONET_KEY_LEN);
      if (aIsBot != NULL) *aIsBot = prev->aIsBot;
      if (bIsBot != NULL) *bIsBot = prev->bIsBot;
      if (prev2 != NULL) {
        prev2->next = NULL;
      } else {
        wbe = NULL;
      }
      Dispose(prev);
    }
  }
  return returnValue;
}
