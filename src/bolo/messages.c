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
*Creation Date: 03/01/99
*Last Modified: 20/01/02
*Purpose:
*  Responsable for scrolling messages.
*********************************************************/


#include <string.h>
#include "global.h"
#include "frontend.h"
#include "util.h"
#include "messages.h"


void messageCreate(MessageState *ms) {
  BYTE count;

  ms->msg = NULL;
  ms->showNewswire = TRUE;
  ms->showAssistant = TRUE;
  ms->showAI = FALSE;
  ms->showNetwork = FALSE;
  ms->showNetStat = TRUE;
  ms->messageTime = 0;
  ms->lastMessage = globalMessage;
  ms->newMessage[0] = '\0';
  ms->newMessageFrom = 0;
  for (count=0;count<MESSAGE_WIDTH;count++) {
    ms->topLine[count] = MESSAGE_BLANK;
    ms->bottomLine[count] = MESSAGE_BLANK;
  }
  ms->topLine[MESSAGE_WIDTH-1] = END_OF_STRING;
  ms->bottomLine[MESSAGE_WIDTH-1] = END_OF_STRING;
}

void messageDestroy(MessageState *ms) {
  message q;

  while (!IsEmpty(ms->msg)) {
    q = ms->msg;
    ms->msg = MessageTail(q);
    Dispose(q);
  }
}


void clientMessageAdd(MessageState *ms, messageType msgType, char *top, char *bottom) {
  switch (msgType) {
  case newsWireMessage:
    if (ms->showNewswire == TRUE) {
      if (ms->lastMessage != newsWireMessage) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = newsWireMessage;
    }
    break;
  case assistantMessage:
    if (ms->showAssistant == TRUE) {
      if (ms->lastMessage != assistantMessage) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = assistantMessage;
    }
    break;
  case AIMessage:
    if (ms->showAI == TRUE) {
      if (ms->lastMessage != AIMessage) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = AIMessage;
    }
    break;
  case networkMessage:
    if (ms->showNetwork == TRUE) {
      if (ms->lastMessage != networkMessage) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = networkMessage;
    }
    break;
    case player0Message:
      ms->newMessageFrom = BASE_0;
      utilCtoPString(bottom, ms->newMessage);
      if (ms->lastMessage != player0Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player0Message;
      break;
    case player1Message:
      ms->newMessageFrom = BASE_1;
      utilCtoPString(bottom, ms->newMessage);
      if (ms->lastMessage != player1Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player1Message;
      break;
    case player2Message:
      ms->newMessageFrom = BASE_2;
      utilCtoPString(bottom, ms->newMessage);
      if (ms->lastMessage != player2Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player2Message;
      break;
    case player3Message:
      ms->newMessageFrom = BASE_3;
      utilCtoPString(bottom, ms->newMessage);
      if (ms->lastMessage != player3Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player3Message;
      break;
    case player4Message:
      ms->newMessageFrom = BASE_4;
      utilCtoPString(bottom, ms->newMessage);
      if (ms->lastMessage != player4Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player4Message;
      break;
    case player5Message:
      ms->newMessageFrom = BASE_5;
      utilCtoPString(bottom, ms->newMessage);
      if (ms->lastMessage != player5Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player5Message;
      break;
    case player6Message:
      ms->newMessageFrom = BASE_6;
      utilCtoPString(bottom, ms->newMessage);
      if (ms->lastMessage != player6Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player6Message;
      break;
    case player7Message:
      ms->newMessageFrom = BASE_7;
      utilCtoPString(bottom, ms->newMessage);
      if (ms->lastMessage != player7Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player7Message;
      break;
    case player8Message:
      ms->newMessageFrom = BASE_8;
      utilCtoPString(bottom, ms->newMessage);
      if (ms->lastMessage != player8Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player8Message;
      break;
    case player9Message:
      ms->newMessageFrom = BASE_9;
      utilCtoPString(bottom, ms->newMessage);
      if (ms->lastMessage != player9Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player9Message;
      break;
    case player10Message:
      ms->newMessageFrom = BASE_10;
      utilCtoPString(bottom, ms->newMessage);
      if (ms->lastMessage != player10Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player10Message;
      break;
    case player11Message:
      ms->newMessageFrom = BASE_11;
      utilCtoPString(bottom, ms->newMessage);
      if (ms->lastMessage != player11Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player11Message;
      break;
    case player12Message:
      ms->newMessageFrom = BASE_12;
      utilCtoPString(bottom, ms->newMessage);
      if (ms->lastMessage != player12Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player12Message;
      break;
    case player13Message:
      ms->newMessageFrom = BASE_13;
      utilCtoPString(bottom, ms->newMessage);
      if (ms->lastMessage != player13Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player13Message;
      break;
    case player14Message:
      ms->newMessageFrom = BASE_14;
      utilCtoPString(bottom, ms->newMessage);
      if (ms->lastMessage != player14Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player14Message;
      break;
    case player15Message:
      ms->newMessageFrom = BASE_15;
      utilCtoPString(bottom, ms->newMessage);
      if (ms->lastMessage != player15Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player15Message;
      break;
    case networkStatus:
      if (ms->showNetStat == TRUE) {
        ms->newMessageFrom = networkStatus;
        if (ms->lastMessage != networkStatus) {
          messageAddItem(ms, top, bottom);
        } else {
          messageAddItem(ms, (char *) MESSAGE_EMPTY, bottom);
        }
        ms->lastMessage = networkStatus;
      }
      break;
    case globalMessage:
    default:
      messageAddItem(ms, top,bottom);
  }

}


void messageAddItem(MessageState *ms, char *top, char *bottom) {
  message q;
  message prev;
  message add;
  int lenTop;
  int lenBottom;
  int count;
  int longest;
  bool newQ;

  newQ = FALSE;

  if (IsEmpty(ms->msg)) {
    newQ = TRUE;
    New(ms->msg);
    ms->msg->next = NULL;
  }

  prev = q = ms->msg;
  while (NonEmpty(q)) {
    prev = q;
    q = MessageTail(q);
  }
  q = prev;

  lenTop = (int) strlen(top);
  lenBottom = (int) strlen(bottom);
  if (lenTop > lenBottom) {
    longest = lenTop;
  } else {
    longest = lenBottom;
  }

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
    q = ms->msg;
    ms->msg = MessageTail(q);
    Dispose(q);
  }
}

void messageUpdate(MessageState *ms) {
  message q;
  BYTE count;

  if (NonEmpty(ms->msg)) {
    ms->topLine[MESSAGE_WIDTH-1] = MessageHeadTop(ms->msg);
    ms->bottomLine[MESSAGE_WIDTH-1] = MessageHeadBottom(ms->msg);
    q = ms->msg;
    ms->msg = MessageTail(q);
    Dispose(q);

    count = 0;
    while (count < (MESSAGE_WIDTH-1)) {
      ms->topLine[count] = ms->topLine[count+1];
      ms->bottomLine[count] = ms->bottomLine[count+1];
      count++;
    }
    ms->topLine[MESSAGE_WIDTH-1] = END_OF_STRING;
    ms->bottomLine[MESSAGE_WIDTH-1] = END_OF_STRING;
    frontEndMessages(ms->topLine, ms->bottomLine);
  }
}

void messageGetMessage(MessageState *ms, char *top, char *bottom) {
  strcpy(top, ms->topLine);
  strcpy(bottom, ms->bottomLine);
}

void messageSetNewswire(MessageState *ms, bool isShown) {
  ms->showNewswire = isShown;
}

void messageSetAssistant(MessageState *ms, bool isShown) {
  ms->showAssistant = isShown;
}

void messageSetAI(MessageState *ms, bool isShown) {
  ms->showAI = isShown;
}

void messageSetNetwork(MessageState *ms, bool isShown) {
  ms->showNetwork = isShown;
}

void messageSetNetStatus(MessageState *ms, bool isShown) {
  ms->showNetStat = isShown;
}

bool messageIsNewMessage(MessageState *ms) {
  bool returnValue;

  returnValue = TRUE;
  if (ms->newMessage[0] == '\0') {
    returnValue = FALSE;
  }
  return returnValue;
}

BYTE messageGetNewMessage(MessageState *ms, char *dest, uint32_t **playerBitmap) {
  strcpy(dest, ms->newMessage);
  (void)playerBitmap;
  ms->newMessage[0] = '\0';
  return ms->newMessageFrom;
}
