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
*Name:          Messages
*Filename:      messages.h
*Author:        John Morrison
*Creation Date: 3/1/99
*Last Modified: 1/6/00
*Purpose:
*  Responsable for scrolling messages.
*********************************************************/

#ifndef MESSAGE_H
#define MESSAGE_H

#include "lv_global.h"
#include "../gui/lang.h"

#define IsEmpty(list) ((list) ==NULL)
#define NonEmpty(list) (!IsEmpty(list))
#define MessageTail(list) ((list)->next);
#define MessageHeadTop(list) ((list)->topLine);
#define MessageHeadBottom(list) ((list)->bottomLine);

/* Message String Macros */
#define MESSAGE_QUOTES "\""

/* Width of the screen */
#define MESSAGE_WIDTH 68
/* End of string marker */
#define END_OF_STRING '\0'
/* Blank space marker */
#define MESSAGE_BLANK ' '

/* An Empty Message */
#define MESSAGE_EMPTY " \0"

/* Time between screen updates */
#define MESSAGE_SCROLL_TIME 4 /* Was 5 prior to 1.09 */

/* Type structure */

typedef struct messageObj *message;
struct messageObj {
  message next;
  char topLine;
  char bottomLine;
};

/* Offset to a player message */
#define PLAYER_MESSAGE_OFFSET 5

typedef enum {
  newsWireMessage, /* Differnt Message types */
  assistantMessage,
  AIMessage,
  networkMessage,
  networkStatus,
  player0Message,  /* Player Number messages */
  player1Message,
  player2Message,
  player3Message,
  player4Message,
  player5Message,
  player6Message,
  player7Message,
  player8Message,
  player9Message,
  player10Message,
  player11Message,
  player12Message,
  player13Message,
  player14Message,
  player15Message,
  globalMessage     /* Message must be printed */
} messageType;

/* Prototypes */

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
void lv_messageCreate(void);

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
void lv_messageDestroy(void);

/*********************************************************
*NAME:          clientMessageAdd
*AUTHOR:        John Morrison
*CREATION DATE:  3/1/99
*LAST MODIFIED:  4/7/00
*PURPOSE:
*  Functions call this to display a message. They must
*  pass the message type so that it can be determined
*  whether the header should be printed etc.
*
*ARGUMENTS:
*  msgType - The type of the message
*  top     - The message to print in the top line
*  bottom  - The message to print in the bottom line
*********************************************************/
void clientMessageAdd(messageType msgType, char *top, char *bottom);

/*********************************************************
*NAME:          serverMessageAdd
*AUTHOR:        John Morrison
*CREATION DATE:  3/1/99
*LAST MODIFIED:  4/7/00
*PURPOSE:
*  Functions call this to display a message. They must
*  pass the message type so that it can be determined
*  whether the header should be printed etc.
*
*ARGUMENTS:
*  msgType - The type of the message
*  top     - The message to print in the top line
*  bottom  - The message to print in the bottom line
*********************************************************/
void serverMessageAdd(messageType msgType, char *top, char *bottom);

/*********************************************************
*NAME:          lv_messageAdd
*PURPOSE:
*  Sim-replay sites call this to log a localized message.
*  Mirrors the bolo csCallbackMessageAdd signature: the
*  caller passes the lang IDs and substitution args; this
*  renders via langGetText/langGetTextFmt and forwards the
*  rendered body to the events panel via lv_windowAddEvent.
*
*ARGUMENTS:
*  msgType - The type of the message (newswire, assistant, ...)
*  topId   - Lang ID for the "channel" header (currently unused
*            by the events panel; reserved for parity with
*            the bolo callback)
*  bodyId  - Lang ID for the message body
*  args    - Substitution args, or NULL
*********************************************************/
void lv_messageAdd(messageType msgType, langid topId, langid bodyId,
                   const MessageArgs *args);

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
void lv_messageAddItem(char *top, char *bottom);

/*********************************************************
*NAME:          lv_messageUpdate
*AUTHOR:        John Morrison
*CREATION DATE:  3/1/99
*LAST MODIFIED:  3/1/99
*PURPOSE:
*  Updates the scrolling message
*
*ARGUMENTS:
*
*********************************************************/
void lv_messageUpdate(void);

/*********************************************************
*NAME:          lv_messageDrainQueue
*PURPOSE:
*  Pops every pending cell straight into the visible row,
*  leaving the marquee with the queue empty and the visible
*  cells holding the tail of whatever was queued. Used by
*  the seek path so jumping forward doesn't dump tens of
*  seconds of accumulated text to scroll past at wall-clock
*  pace — instead the most recent message lands in the
*  visible cells and the next live message starts scrolling
*  in from the right normally.
*
*ARGUMENTS:
*
*********************************************************/
void lv_messageDrainQueue(void);

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
void lv_messageGetMessage(char *top, char *bottom);

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
void lv_messageSetNewswire(bool isShown);

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
void lv_messageSetAssistant(bool isShown);

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
void lv_messageSetAI(bool isShown);

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
void lv_messageSetNetwork(bool isShown);

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
void lv_messageSetNetStatus(bool isShown);

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
bool lv_messageIsNewMessage();

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
BYTE lv_messageGetNewMessage(char *dest, unsigned long **playerBitmap);

#endif /* MESSAGE_H */
