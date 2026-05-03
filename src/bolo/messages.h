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

/* Message String Macros */
#define MESSAGE_QUOTES "\""

/* Width of the screen */
#define MESSAGE_WIDTH 68
/* End of string marker */
#define END_OF_STRING '\0'
/* Blank space marker */
#define MESSAGE_BLANK ' '

/* Sentinel cell value for the second visual column of a wide (East Asian
 * full-width) codepoint. Each cell in the marquee represents one visual
 * column; a wide codepoint occupies its leading cell + a CONT cell so
 * cell counts match between top and bottom rows even when one row has
 * narrow Latin and the other has wide CJK. The encoder skips CONT cells
 * (the codepoint in the leading cell already renders 2 cols at its
 * natural advance). 0xFFFFFFFE is outside any valid Unicode codepoint. */
#define MESSAGE_CELL_CONT 0xFFFFFFFEu

/* An Empty Message */
#define MESSAGE_EMPTY " \0"

/* Time between screen updates */
#define MESSAGE_SCROLL_TIME 4 /* Was 5 prior to 1.09 */

/* Pending codepoint pairs awaiting the next messageUpdate tick. Ring buffer
 * sized for ~15 typical messages of scroll-in (1024 cells * 8 bytes/pair =
 * 8 KB per stream). When full, the oldest pending cells are dropped so
 * producers never block. */
#define MESSAGE_QUEUE_CAP 1024

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

/* UTF-8 byte buffer sized for MESSAGE_WIDTH codepoints (max 4 bytes each)
 * plus NUL terminator. The frontend receives this rendered byte string;
 * the codepoint cells below are the canonical state. */
#define MESSAGE_LINE_BYTES (MESSAGE_WIDTH * 4 + 1)

/* Per-instance message state (moved from module-level globals) */
typedef struct MessageState {
  /* Canonical state: one Unicode codepoint per visible cell. */
  uint32_t topCells[MESSAGE_WIDTH];
  uint32_t bottomCells[MESSAGE_WIDTH];
  /* UTF-8 rendering of topCells/bottomCells, refreshed each tick and
   * passed to frontEndMessages(). NUL-terminated. */
  char    topLine[MESSAGE_LINE_BYTES];
  char    bottomLine[MESSAGE_LINE_BYTES];
  /* Ring buffer of pending codepoint pairs. messageAddItem writes pairs
   * at queueTail; messageUpdate consumes one pair from queueHead per tick.
   * queueCount tracks population (0..MESSAGE_QUEUE_CAP). All inline — no
   * heap allocations after construction. */
  uint32_t queueTop[MESSAGE_QUEUE_CAP];
  uint32_t queueBottom[MESSAGE_QUEUE_CAP];
  int     queueHead;
  int     queueTail;
  int     queueCount;
  char    newMessage[FILENAME_MAX];
  BYTE    newMessageFrom;
  bool    showNewswire;
  bool    showAssistant;
  bool    showAI;
  bool    showNetwork;
  bool    showNetStat;
  BYTE    messageTime;
  BYTE    lastMessage;
} MessageState;

/* Prototypes */

void messageCreate(MessageState *ms);
void messageDestroy(MessageState *ms);
void clientMessageAdd(MessageState *ms, messageType msgType, const char *top, const char *bottom);
struct ServerSim;
void serverMessageAdd(struct ServerSim *sim, messageType msgType, char *top, char *bottom);
void messageAddItem(MessageState *ms, const char *top, const char *bottom);
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
