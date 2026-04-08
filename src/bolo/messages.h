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
*Filename:      messages.h
*Author:        John Morrison
*Creation Date: 3/1/99
*Last Modified: 1/6/00
*Purpose:
*  Responsable for scrolling messages.
*********************************************************/

#ifndef MESSAGE_H
#define MESSAGE_H

#include <stdio.h>
#include "global.h"

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

/* Per-instance message state (moved from module-level globals) */
typedef struct MessageState {
  char    topLine[MESSAGE_WIDTH];
  char    bottomLine[MESSAGE_WIDTH];
  char    newMessage[FILENAME_MAX];
  BYTE    newMessageFrom;
  bool    showNewswire;
  bool    showAssistant;
  bool    showAI;
  bool    showNetwork;
  bool    showNetStat;
  message msg;
  BYTE    messageTime;
  BYTE    lastMessage;
} MessageState;

/* Prototypes */

void messageCreate(MessageState *ms);
void messageDestroy(MessageState *ms);
void clientMessageAdd(MessageState *ms, messageType msgType, char *top, char *bottom);
struct ServerSim;
void serverMessageAdd(struct ServerSim *sim, messageType msgType, char *top, char *bottom);
void messageAddItem(MessageState *ms, char *top, char *bottom);
void messageUpdate(MessageState *ms);
void messageGetMessage(MessageState *ms, char *top, char *bottom);
void messageSetNewswire(MessageState *ms, bool isShown);
void messageSetAssistant(MessageState *ms, bool isShown);
void messageSetAI(MessageState *ms, bool isShown);
void messageSetNetwork(MessageState *ms, bool isShown);
void messageSetNetStatus(MessageState *ms, bool isShown);
bool messageIsNewMessage(MessageState *ms);
BYTE messageGetNewMessage(MessageState *ms, char *dest, uint32_t **playerBitmap);

#endif /* MESSAGE_H */
