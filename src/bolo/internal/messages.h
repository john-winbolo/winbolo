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

#include <stdio.h>
#include "global.h"
#include "wire_limits.h"   /* PACKET_MAX_CHAT_MESSAGE — sizes brain inbox slot */

#ifndef MESSAGESTATE_TYPEDEF
#define MESSAGESTATE_TYPEDEF
typedef struct MessageState MessageState;
#endif

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
 * sized for ~150 typical messages of scroll-in (10240 cells * 8 bytes/pair =
 * 80 KB per stream). Sized to absorb bursts where many parallel bots emit
 * newswire events in the same tick — the queue drains at ~12.5 cells/sec
 * (one per MESSAGE_SCROLL_TIME × game tick), so a smaller cap overflows
 * during base-capture floods and drops chars mid-message. When full, the
 * oldest pending cells are dropped so producers never block. */
#define MESSAGE_QUEUE_CAP 10240

/* Brain inbox dimensions. Capacity 32 covers the worst-case
 * full-roster broadcast (16 bots all chatting on the same tick) with
 * 2× headroom for concurrent human chat. Per-slot text fits a
 * Pascal-stringified PACKET_MAX_CHAT_MESSAGE (length byte + body +
 * NUL); the host enforces PACKET_MAX_CHAT_MESSAGE on the wire so any
 * legit chat fits in this slot. */
#define BRAIN_INBOX_CAP     32
#define BRAIN_INBOX_MSG_LEN (PACKET_MAX_CHAT_MESSAGE + 2)

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
struct MessageState {
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
  /* Legacy single-slot newMessage / newMessageFrom: kept as a back-compat
   * alias for the messageIsNewMessage / messageGetNewMessage API. These
   * now drain the head of the brain inbox below — non-brain consumers
   * that still call the legacy pair see exactly one (oldest) message per
   * call, the same shape as before this change. Brain consumers should
   * use messageInboxCount / messageInboxPeek instead so a tick with
   * multiple incoming chats from N allies isn't silently truncated to
   * the most recent.
   * (Pre-change behavior: each playerNMessage arrival overwrote
   *  newMessage in place, so 15 simultaneous allies → brain sees 1.
   *  See bolo/internal/messages.h history for context.) */
  char    newMessage[FILENAME_MAX];
  BYTE    newMessageFrom;

  /* Brain inbox: per-tick queue of incoming chat messages addressed to
   * this ClientSim. Sized to comfortably absorb a full 16-bot game
   * where every ally broadcasts on the same tick, plus chat from humans.
   * Strings are stored as Pascal strings (byte 0 = length, bytes 1..N =
   * text) to match the wire format used by utilCtoPString. When the
   * ring fills, the OLDEST entry is dropped — chat history is more
   * useful than the oldest sample when we're saturated.
   *
   * Drained by brainDataMakeInfo (which copies into BrainInfo.messages
   * and resets the inbox). Other consumers can peek non-destructively
   * via messageInboxPeek; that path is mainly for tests / future UI. */
  char    inboxText[BRAIN_INBOX_CAP][BRAIN_INBOX_MSG_LEN];
  BYTE    inboxFrom[BRAIN_INBOX_CAP];
  int     inboxHead;     /* index of oldest entry */
  int     inboxTail;     /* one past the newest */
  int     inboxCount;    /* 0..BRAIN_INBOX_CAP */
  bool    showNewswire;
  bool    showAssistant;
  bool    showAI;
  bool    showNetwork;
  bool    showNetStat;
  BYTE    messageTime;
  BYTE    lastMessage;
};

/* Prototypes */

void messageCreate(MessageState *ms);
void messageDestroy(MessageState *ms);
/* Clear queued/displayed messages and the brain inbox without disturbing
 * the show* visibility toggles. Used at game start so a ClientSim reused
 * across a lobby cycle does not carry the previous game's pending chat. */
void messageReset(MessageState *ms);
void clientMessageAdd(MessageState *ms, messageType msgType, char *top, char *bottom);
struct ServerSim;
void serverMessageAdd(struct ServerSim *sim, messageType msgType, char *top, char *bottom);
void messageAddItem(MessageState *ms, char *top, char *bottom);
struct ClientSim;
/* cs is forwarded to frontEndMessages so the active-cs gate suppresses
 * messages from non-active ClientSims (bots/bg_game/gym). */
void messageUpdate(struct ClientSim *cs, MessageState *ms);
void messageGetMessage(MessageState *ms, char *top, char *bottom);
void messageSetNewswire(MessageState *ms, bool isShown);
void messageSetAssistant(MessageState *ms, bool isShown);
void messageSetAI(MessageState *ms, bool isShown);
void messageSetNetwork(MessageState *ms, bool isShown);
void messageSetNetStatus(MessageState *ms, bool isShown);
bool messageIsNewMessage(MessageState *ms);
BYTE messageGetNewMessage(MessageState *ms, char *dest, uint32_t **playerBitmap);

/*********************************************************
*NAME:          Brain inbox helpers
*PURPOSE:
*  Multi-message queue used by brains to read every chat
*  arrival in the current tick, not just the most recent.
*  Without this, two allies broadcasting on the same tick
*  see one of their messages silently overwritten before
*  the brain ever reads it — the root cause of why
*  aIndy-style ally coordination is fragile under load.
*
*  Producers (clientMessageAdd's playerNMessage paths on
*  both builds, and botManagerDeliverInternalMessage) all
*  go through messageInboxPushLine, which is the one place
*  a line is clamped and Pascal-stringified; the brain
*  drains the entire ring
*  each tick via messageInboxCount + messageInboxPeek (or
*  brainDataMakeInfo which does the drain centrally and
*  populates BrainInfo.messages).
*
*  Strings are stored as Pascal strings (byte 0 = length,
*  bytes 1..N = text), matching utilCtoPString output and
*  the legacy newMessage buffer convention.
*********************************************************/

/* Every body below lives in src/bolo/message_inbox.c, which every build
 * links — including the dedicated server, which cannot link messages.c
 * because that file draws the message HUD. Before the split the server's
 * stub file carried a hand-copied second ring and a third copy of the push,
 * which is the asymmetric-runtime bug class docs/ARCHITECTURE.md exists to
 * prevent; it had already bitten once, when the stubs were no-ops and every
 * hosted bot's inbox silently stayed empty. */

/* Append one (sender, Pascal-string) pair to the inbox. When the
 * ring is full, drops the OLDEST entry. text must already be in
 * Pascal-string form (byte 0 = length). */
void messageInboxPush(MessageState *ms, BYTE from, const char *pascalText);

/* THE one way a line of ordinary text becomes an inbox entry: clamps it to
 * what a slot holds, puts the length byte on the front, and pushes. Every
 * producer goes through this — clientMessageAdd's playerNMessage arms in
 * messages.c, the same arm in the dedicated server's stub file, and
 * botManagerDeliverInternalMessage, which used to Pascal-stringify by hand.
 * `from` is the SENDER's slot, which is what a brain's team check reads. */
void messageInboxPushLine(MessageState *ms, BYTE from, const char *text);

/* Returns current population (0..BRAIN_INBOX_CAP). */
int  messageInboxCount(const MessageState *ms);

/* Read the inbox entry at logical index i (0 = oldest). Returns
 * the sender byte and copies the Pascal-stringified text into
 * dest (caller-provided, must be at least BRAIN_INBOX_MSG_LEN
 * bytes). Returns 0 and writes an empty Pascal string when i is
 * out of range. Non-destructive. */
BYTE messageInboxPeek(const MessageState *ms, int i, char *dest);

/* Drop every queued message. Used by brainDataMakeInfo after
 * draining into the BrainInfo.messages array so the next tick's
 * arrivals start with an empty queue. */
void messageInboxClear(MessageState *ms);

#endif /* MESSAGE_H */
