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
*Filename:      winbolonetevents.h
*Author:        John Morrison
*Creation Date: 04/04/02
*Last Modified: 30/03/26
*Purpose:
*  Responsible for tracking winbolonetEvents
*********************************************************/


#ifndef __WINBOLONET_EVENT
#define __WINBOLONET_EVENT


#include "global.h"
#include "winbolonet.h"


/* Defines */
#define WINBOLONET_EVENT_NOITEM 255 /* We have no more items to send */
#define IsEmpty(list) ((list) ==NULL)
#define NonEmpty(list) (!IsEmpty(list))
#define winbolonetEventsTail(list) ((list)->next);


/* Types */

typedef struct winboloNetObj *winbolonetEvents;
struct winboloNetObj {
  winbolonetEvents next; /* Next item */
  BYTE itemType;
  char keyA[WINBOLONET_KEY_LEN];
  char keyB[WINBOLONET_KEY_LEN];
};


/*********************************************************
*NAME:          winbolonetEventsCreate
*PURPOSE:
*  Sets up the winbolonetEvents data structure
*********************************************************/
void winbolonetEventsCreate(void);

/*********************************************************
*NAME:          winbolonetEventsDestroy
*PURPOSE:
*  Destroys and frees memory for the winbolonetEvents
*  data structure
*********************************************************/
void winbolonetEventsDestroy(void);

/*********************************************************
*NAME:          winbolonetEventsAddItem
*PURPOSE:
*  Adds an item to the winbolonetEvents data structure.
*
*ARGUMENTS:
*  itemType - The WinBolo.net Item Event Type
*  keyA     - Key string of user A
*  keyB     - Key string of user B
*********************************************************/
void winbolonetEventsAddItem(BYTE itemType, const char *keyA, const char *keyB);

/*********************************************************
*NAME:          winbolonetEventsGetSize
*PURPOSE:
*  Returns the number of events in the queue.
*********************************************************/
int winbolonetEventsGetSize(void);

/*********************************************************
*NAME:          winbolonetEventsRemove
*PURPOSE:
*  Removes the oldest item from the winbolonetEvents data
*  structure. Returns itemType or WINBOLONET_EVENT_NOITEM
*  if empty.
*
*ARGUMENTS:
*  keyA - Buffer to copy key of user A (WINBOLONET_KEY_LEN)
*  keyB - Buffer to copy key of user B (WINBOLONET_KEY_LEN)
*********************************************************/
BYTE winbolonetEventsRemove(char *keyA, char *keyB);

#endif /* __WINBOLONET_EVENT */
