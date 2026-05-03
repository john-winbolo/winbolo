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
*Name:          Messages
*Filename:      messages.c
*Author:        John Morrison
*Creation Date: 3/1/99
*Last Modified: 1/6/00
*Purpose:
*  Responsable for scrolling messages.
*********************************************************/


#include <string.h>
#include "global.h"
//#include "frontend.h"
#include "util.h"
#include "messages.h"
#include "backend.h"

/* Module Variables */

/* Messages */
static char topLine[MESSAGE_WIDTH];
static char bottomLine[MESSAGE_WIDTH];

static char newMessage[FILENAME_MAX]; /* A new message */
static BYTE newMessageFrom;           /* Where it came from */

/* What types to show */
static bool showNewswire = TRUE;
static bool showAssistant = TRUE;
static bool showAI = FALSE;
static bool showNetwork = FALSE;
static bool showNetStat = TRUE;

/* Queue DS for waiting messages */
static message msg;

/*********************************************************
*NAME:          lv_messageCreate
*AUTHOR:        John Morrison
*CREATION DATE:  3/1/99
*LAST MODIFIED:  3/1/99
*PURPOSE:
*  Sets up the messages data structure
*
*ARGUMENTS:
*
*********************************************************/
void lv_messageCreate(void) {
  BYTE count; /* Looping variable */
  
  msg = NULL;
  showNewswire = TRUE;
  showAssistant = TRUE;
  showAI = FALSE;
  showNetwork = FALSE;
  showNetStat = TRUE;
  for (count=0;count<MESSAGE_WIDTH;count++) {
    topLine[count] = MESSAGE_BLANK;
    bottomLine[count] = MESSAGE_BLANK;
  }
  topLine[MESSAGE_WIDTH-1] = END_OF_STRING;
  bottomLine[MESSAGE_WIDTH-1] = END_OF_STRING;
}

/*********************************************************
*NAME:          lv_messageDestroy
*AUTHOR:        John Morrison
*CREATION DATE:  3/1/99
*LAST MODIFIED:  3/1/99
*PURPOSE:
*  Destroys and frees memory for the message data 
*  structure
*
*ARGUMENTS:
*
*********************************************************/
void lv_messageDestroy(void) {
  message q;

  while (!IsEmpty(msg)) {
    q = msg;
    msg = MessageTail(q);
    Dispose(q);
  }
}


/* lv_messageAdd lives in screen.c — it forwards rendered text to the
 * events panel via lv_windowAddEvent and tail-calls lv_messageAddItem
 * to feed the scrolling-marquee queue used by the trailer game view
 * (game_view.c sets up/tears down the queue and ticks lv_messageUpdate
 * each frame). Outside of game-view mode the queue is populated but
 * never drained — cheap, and it keeps lv_messageAdd's call sites the
 * same in both modes. */


/*********************************************************
*NAME:          lv_messageAddItem
*AUTHOR:        John Morrison
*CREATION DATE:  3/1/99
*LAST MODIFIED:  3/1/99
*PURPOSE:
*  Adds an item to the message data structure. 
*
*ARGUMENTS:
*  top    - The message to print in the top line
*  bottom - The message to print in the bottom line
*********************************************************/
void lv_messageAddItem(char *top, char *bottom) {
  message q;     /* temp Pointer */
  message prev;  /* temp pointer */
  message add;   /* Item to add */
  unsigned int lenTop;    /* Lengths of the item */
  unsigned int lenBottom; /* Length of the bottom string */
  unsigned int count;     /* Looping variable */
  unsigned int longest;   /* Longest item */
  bool newQ;     /* Denotes a new queue */

  /* Get the location to add it to */
  
  newQ = FALSE;
  
  if (IsEmpty(msg)) {
    newQ = TRUE;
    New(msg);
    msg->next = NULL;
  }

  prev = q = msg;
  while (NonEmpty(q)) {
    prev = q;
    q = MessageTail(q);
  }
  q = prev;

  /* Get the longest of the two strings */
  lenTop = (unsigned int)strlen(top);
  lenBottom = (unsigned int)strlen(bottom);
  if (lenTop > lenBottom) {
    longest = lenTop;
  } else {
    longest = lenBottom;
  }

  /* Add the items to the data structure */
  count = 0;
  while (count <= (longest)) {
    New(add);
    if (count < lenTop) {
      add->topLine = top[count];
    } else {
      add->topLine = MESSAGE_BLANK;
    }
    if (count < lenBottom) {
      add->bottomLine = bottom[count];
    } else {
      add->bottomLine = MESSAGE_BLANK;
    }
    add->next = NULL;
    q->next = add;
    q = MessageTail(q);
    count++;
  }


  if (newQ == TRUE) {
    q = msg;
    msg = MessageTail(q);
    Dispose(q);
  }
}

/*********************************************************
*NAME:          lv_messageUpdate
*AUTHOR:        John Morrison
*CREATION DATE:  3/1/99
*LAST MODIFIED: 26/11/99
*PURPOSE:
*  Updates the scrolling message — pops one queued char into
*  the right-edge staging slot, shifts the visible cells one
*  position left, and blanks the staging slot. The visible
*  region is positions 0..MESSAGE_WIDTH-2 (read via
*  lv_messageGetMessage); position MESSAGE_WIDTH-1 is the
*  staging slot and ends each call as a string terminator so
*  topLine/bottomLine stay valid as C strings.
*
*ARGUMENTS:
*
*********************************************************/
void lv_messageUpdate(void) {
  message q;  /* temp Pointer */
  BYTE count; /* Looping variable */

  newMessage[0] = '\0';
  /* Only want to do something if the message needs to be scrolled */
  if (NonEmpty(msg)) {
    /* Get the next charectors */
    topLine[MESSAGE_WIDTH-1] = MessageHeadTop(msg);
    bottomLine[MESSAGE_WIDTH-1] = MessageHeadBottom(msg);
    /* Delete the item */
    q = msg;
    msg = MessageTail(q);
    Dispose(q);

    /* Move the message */
    count = 0;
    while (count < (MESSAGE_WIDTH-1)) {
      topLine[count] = topLine[count+1];
      bottomLine[count] = bottomLine[count+1];
      count++;
    }
    topLine[MESSAGE_WIDTH-1] = END_OF_STRING;
    bottomLine[MESSAGE_WIDTH-1] = END_OF_STRING;
  }
}

/*********************************************************
*NAME:          lv_messageGetMessage
*AUTHOR:        John Morrison
*CREATION DATE: 1/1/98
*LAST MODIFIED: 1/1/98
*PURPOSE:
*  Copys the messages on screen into the given variables
*
*ARGUMENTS:
*  top    - The message to print in the top line
*  bottom - The message to print in the bottom line
*********************************************************/
void lv_messageGetMessage(char *top, char *bottom) {
  strncpy(top, topLine, MESSAGE_WIDTH - 1);
  top[MESSAGE_WIDTH - 1] = '\0';
  strncpy(bottom, bottomLine, MESSAGE_WIDTH - 1);
  bottom[MESSAGE_WIDTH - 1] = '\0';
}

/*********************************************************
*NAME:          lv_messageSetNewswire
*AUTHOR:        John Morrison
*CREATION DATE: 8/1/98
*LAST MODIFIED: 8/1/98
*PURPOSE:
*  Sets the state of newswire messages
*
*ARGUMENTS:
*  isShown - Is this type of message shown
*********************************************************/
void lv_messageSetNewswire(bool isShown) {
  showNewswire = isShown;
}

/*********************************************************
*NAME:          lv_messageSetAssistant
*AUTHOR:        John Morrison
*CREATION DATE: 8/1/98
*LAST MODIFIED: 8/1/98
*PURPOSE:
*  Sets the state of assistant messages
*
*ARGUMENTS:
*  isShown - Is this type of message shown
*********************************************************/
void lv_messageSetAssistant(bool isShown) {
  showAssistant = isShown;
}

/*********************************************************
*NAME:          lv_messageSetAI
*AUTHOR:        John Morrison
*CREATION DATE: 8/1/98
*LAST MODIFIED: 8/1/98
*PURPOSE:
*  Sets the state of AI messages
*
*ARGUMENTS:
*  isShown - Is this type of message shown
*********************************************************/
void lv_messageSetAI(bool isShown) {
  showAI = isShown;
}

/*********************************************************
*NAME:          lv_messageSetNetwork
*AUTHOR:        John Morrison
*CREATION DATE: 8/1/98
*LAST MODIFIED: 8/1/98
*PURPOSE:
*  Sets the state of network messages
*
*ARGUMENTS:
*  isShown - Is this type of message shown
*********************************************************/
void lv_messageSetNetwork(bool isShown) {
  showNetwork = isShown;
}

/*********************************************************
*NAME:          lv_messageSetNetStatus
*AUTHOR:        John Morrison
*CREATION DATE: 1/6/00
*LAST MODIFIED: 1/6/00
*PURPOSE:
*  Sets the state of network status messages
*
*ARGUMENTS:
*  isShown - Is this type of message shown
*********************************************************/
void lv_messageSetNetStatus(bool isShown) {
  showNetStat = isShown;
}


/*********************************************************
*NAME:          lv_messageIsNewMessage
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/99
*LAST MODIFIED: 26/11/99
*PURPOSE:
*  Returns whether a new message has arrived or not.
*
*ARGUMENTS:
*
*********************************************************/
bool lv_messageIsNewMessage() {
  bool returnValue; /* Value to return */

  returnValue = TRUE;
  if (newMessage[0] == '\0') {
    returnValue = FALSE;
  }
  return returnValue;
}

/*********************************************************
*NAME:          lv_messageGetNewMessage
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/99
*LAST MODIFIED: 26/11/99
*PURPOSE:
*  Gets the new message. Returns where the message 
*  originated from. NOTE: PlayerBitmap presently unused
*
*ARGUMENTS:
*  dest         - Destination for the message
*  playerBitmap - Bitmap of players that recieved it
*********************************************************/
BYTE lv_messageGetNewMessage(char *dest, unsigned long **playerBitmap) {
  strncpy(dest, newMessage, FILENAME_MAX - 1);
  dest[FILENAME_MAX - 1] = '\0';
  (void)playerBitmap; /* Presently unused */

  return newMessageFrom;
}
