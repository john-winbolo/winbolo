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

/* Step one UTF-8 codepoint at *p, advancing *p past the bytes consumed.
 * Returns 0xFFFD on malformed sequences (and advances at least one byte so
 * callers can keep walking). Returns 0 only when *p hits the NUL terminator;
 * callers should check for empty input first. */
static uint32_t utf8StepCodepoint(const char **p) {
  const unsigned char *s = (const unsigned char *)*p;
  if (*s == 0) {
    return 0;
  }
  uint32_t cp;
  int extra;
  if (*s < 0x80) {
    cp = *s;
    extra = 0;
  } else if ((*s & 0xE0) == 0xC0) {
    cp = *s & 0x1F;
    extra = 1;
  } else if ((*s & 0xF0) == 0xE0) {
    cp = *s & 0x0F;
    extra = 2;
  } else if ((*s & 0xF8) == 0xF0) {
    cp = *s & 0x07;
    extra = 3;
  } else {
    *p = (const char *)(s + 1);
    return 0xFFFD;
  }
  s++;
  for (int i = 0; i < extra; i++) {
    if ((*s & 0xC0) != 0x80) {
      *p = (const char *)s;
      return 0xFFFD;
    }
    cp = (cp << 6) | (*s & 0x3F);
    s++;
  }
  *p = (const char *)s;
  return cp;
}

/* Visual width of a codepoint in monospace cells. Returns 2 for East
 * Asian full-width / Wide ranges (CJK ideographs, kana, hangul, fullwidth
 * forms, common emoji blocks) and 1 otherwise. Conservative: codepoints
 * not explicitly listed are treated as narrow. Combining marks (which
 * a stricter implementation would treat as W=0) are rare in chat and
 * rendering them as W=1 doesn't harm column alignment in monospace. */
static int cellWidth(uint32_t cp) {
  if (cp == MESSAGE_CELL_CONT) return 0;
  if (cp < 0x1100) return 1;
  /* Hangul Jamo */
  if (cp >= 0x1100 && cp <= 0x115F) return 2;
  /* CJK Radicals + Kangxi + CJK Symbols */
  if (cp >= 0x2E80 && cp <= 0x303E) return 2;
  /* Hiragana, Katakana, CJK Letters, Bopomofo, Hangul Compat, Kanbun,
   * CJK Strokes, Katakana Phonetic, Enclosed CJK, CJK Compatibility */
  if (cp >= 0x3040 && cp <= 0x33FF) return 2;
  /* CJK Unified Ideographs Extension A */
  if (cp >= 0x3400 && cp <= 0x4DBF) return 2;
  /* CJK Unified Ideographs */
  if (cp >= 0x4E00 && cp <= 0x9FFF) return 2;
  /* Yi Syllables + Radicals */
  if (cp >= 0xA000 && cp <= 0xA4CF) return 2;
  /* Hangul Syllables */
  if (cp >= 0xAC00 && cp <= 0xD7A3) return 2;
  /* CJK Compatibility Ideographs */
  if (cp >= 0xF900 && cp <= 0xFAFF) return 2;
  /* Vertical Forms + CJK Compat Forms + Small Form Variants */
  if (cp >= 0xFE10 && cp <= 0xFE6F) return 2;
  /* Fullwidth Forms */
  if (cp >= 0xFF00 && cp <= 0xFF60) return 2;
  /* Fullwidth signs */
  if (cp >= 0xFFE0 && cp <= 0xFFE6) return 2;
  /* Misc Symbols + Emoticons + Transport/Map + Supplemental Symbols */
  if (cp >= 0x1F300 && cp <= 0x1FAFF) return 2;
  /* CJK Unified Ideographs Extension B-F (supplementary plane) */
  if (cp >= 0x20000 && cp <= 0x2FFFD) return 2;
  /* CJK Unified Ideographs Extension G+ */
  if (cp >= 0x30000 && cp <= 0x3FFFD) return 2;
  return 1;
}

/* Encode an array of codepoints to a NUL-terminated UTF-8 byte buffer.
 * MESSAGE_CELL_CONT cells are skipped — the codepoint in the previous
 * cell already renders at its natural full-width advance, so the CONT
 * cell only exists to keep the cell count in lockstep between top/bot.
 * Truncates cleanly (without splitting a codepoint) if outBytes is too small. */
static void encodeCellsToUtf8(const uint32_t *cells, size_t numCells,
                              char *out, size_t outBytes) {
  size_t pos = 0;
  for (size_t i = 0; i < numCells; i++) {
    uint32_t cp = cells[i];
    if (cp == MESSAGE_CELL_CONT) continue;
    char buf[4];
    size_t n;
    if (cp < 0x80) {
      buf[0] = (char)cp;
      n = 1;
    } else if (cp < 0x800) {
      buf[0] = (char)(0xC0 | (cp >> 6));
      buf[1] = (char)(0x80 | (cp & 0x3F));
      n = 2;
    } else if (cp < 0x10000) {
      buf[0] = (char)(0xE0 | (cp >> 12));
      buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
      buf[2] = (char)(0x80 | (cp & 0x3F));
      n = 3;
    } else if (cp <= 0x10FFFF) {
      buf[0] = (char)(0xF0 | (cp >> 18));
      buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
      buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
      buf[3] = (char)(0x80 | (cp & 0x3F));
      n = 4;
    } else {
      continue;
    }
    if (pos + n + 1 > outBytes) break;
    memcpy(out + pos, buf, n);
    pos += n;
  }
  if (pos < outBytes) {
    out[pos] = '\0';
  } else if (outBytes > 0) {
    out[outBytes - 1] = '\0';
  }
}


void messageCreate(MessageState *ms) {
  BYTE count;

  ms->showNewswire = TRUE;
  ms->showAssistant = TRUE;
  ms->showAI = FALSE;
  ms->showNetwork = FALSE;
  ms->showNetStat = TRUE;
  ms->messageTime = 0;
  ms->lastMessage = globalMessage;
  ms->newMessage[0] = '\0';
  ms->newMessageFrom = 0;
  ms->queueHead = 0;
  ms->queueTail = 0;
  ms->queueCount = 0;
  ms->inboxHead = 0;
  ms->inboxTail = 0;
  ms->inboxCount = 0;
  for (count = 0; count < MESSAGE_WIDTH; count++) {
    ms->topCells[count] = MESSAGE_BLANK;
    ms->bottomCells[count] = MESSAGE_BLANK;
  }
  /* Visible row is cells[0..WIDTH-2]; the last cell is the staging slot
   * filled by messageUpdate before each shift. */
  encodeCellsToUtf8(ms->topCells,    MESSAGE_WIDTH - 1, ms->topLine,    sizeof(ms->topLine));
  encodeCellsToUtf8(ms->bottomCells, MESSAGE_WIDTH - 1, ms->bottomLine, sizeof(ms->bottomLine));
}

void messageDestroy(MessageState *ms) {
  /* Ring buffer is inline in MessageState; nothing to free. Reset indices
   * so a destroyed-then-reused state behaves like a fresh one. */
  ms->queueHead = 0;
  ms->queueTail = 0;
  ms->queueCount = 0;
  ms->inboxHead = 0;
  ms->inboxTail = 0;
  ms->inboxCount = 0;
}

void messageReset(MessageState *ms) {
  BYTE count;

  if (ms == NULL) {
    return;
  }
  /* Drop everything a finished game leaves behind so it does not bleed
   * into the next one on a ClientSim that survives the lobby cycle: the
   * pending scroll queue (codepoint pairs accepted but not yet shifted
   * onto the visible row — these would otherwise scroll in the moment
   * the new game's ticks resume), the brain inbox, and the currently
   * displayed cells. The show* visibility toggles are user preferences,
   * so they are deliberately left untouched. */
  ms->messageTime = 0;
  ms->lastMessage = globalMessage;
  ms->newMessage[0] = '\0';
  ms->newMessageFrom = 0;
  ms->queueHead = 0;
  ms->queueTail = 0;
  ms->queueCount = 0;
  ms->inboxHead = 0;
  ms->inboxTail = 0;
  ms->inboxCount = 0;
  for (count = 0; count < MESSAGE_WIDTH; count++) {
    ms->topCells[count] = MESSAGE_BLANK;
    ms->bottomCells[count] = MESSAGE_BLANK;
  }
  encodeCellsToUtf8(ms->topCells,    MESSAGE_WIDTH - 1, ms->topLine,    sizeof(ms->topLine));
  encodeCellsToUtf8(ms->bottomCells, MESSAGE_WIDTH - 1, ms->bottomLine, sizeof(ms->bottomLine));
}

/* Push one codepoint pair onto the ring. If the ring is full, drops the
 * oldest pending pair (advances head) so producers never block. */
static void messageQueuePush(MessageState *ms, uint32_t topCp, uint32_t botCp) {
  if (ms->queueCount >= MESSAGE_QUEUE_CAP) {
    ms->queueHead = (ms->queueHead + 1) % MESSAGE_QUEUE_CAP;
    ms->queueCount--;
  }
  ms->queueTop[ms->queueTail]    = topCp;
  ms->queueBottom[ms->queueTail] = botCp;
  ms->queueTail = (ms->queueTail + 1) % MESSAGE_QUEUE_CAP;
  ms->queueCount++;
}

/* Brain inbox ring (see internal/messages.h). Same drop-oldest-on-full
 * policy as the codepoint queue above. text is Pascal-stringified
 * already (byte 0 = length); we copy length+1 bytes plus a NUL guard. */
void messageInboxPush(MessageState *ms, BYTE from, const char *pascalText) {
  size_t plen;
  size_t copyLen;

  if (ms == NULL || pascalText == NULL) {
    return;
  }
  if (ms->inboxCount >= BRAIN_INBOX_CAP) {
    ms->inboxHead = (ms->inboxHead + 1) % BRAIN_INBOX_CAP;
    ms->inboxCount--;
  }
  /* Pascal string length is in byte 0. Clamp to the per-slot
   * buffer to be defensive against malformed inputs — the wire
   * already caps at PACKET_MAX_CHAT_MESSAGE but local callers
   * could in theory pass longer. */
  plen = (size_t)((unsigned char)pascalText[0]);
  if (plen + 2 > BRAIN_INBOX_MSG_LEN) {
    plen = BRAIN_INBOX_MSG_LEN - 2;
  }
  copyLen = plen + 1;  /* length byte + body bytes */
  memcpy(ms->inboxText[ms->inboxTail], pascalText, copyLen);
  ms->inboxText[ms->inboxTail][copyLen] = '\0';
  ms->inboxText[ms->inboxTail][0]       = (char)plen;  /* enforce clamp */
  ms->inboxFrom[ms->inboxTail]          = from;
  ms->inboxTail = (ms->inboxTail + 1) % BRAIN_INBOX_CAP;
  ms->inboxCount++;
}

int messageInboxCount(const MessageState *ms) {
  return (ms != NULL) ? ms->inboxCount : 0;
}

BYTE messageInboxPeek(const MessageState *ms, int i, char *dest) {
  int slot;
  size_t plen;

  if (dest == NULL) {
    return 0;
  }
  if (ms == NULL || i < 0 || i >= ms->inboxCount) {
    dest[0] = '\0';
    return 0;
  }
  slot = (ms->inboxHead + i) % BRAIN_INBOX_CAP;
  plen = (size_t)((unsigned char)ms->inboxText[slot][0]);
  if (plen + 2 > BRAIN_INBOX_MSG_LEN) {
    plen = BRAIN_INBOX_MSG_LEN - 2;
  }
  memcpy(dest, ms->inboxText[slot], plen + 1);
  dest[plen + 1] = '\0';
  return ms->inboxFrom[slot];
}

void messageInboxClear(MessageState *ms) {
  if (ms == NULL) return;
  ms->inboxHead = 0;
  ms->inboxTail = 0;
  ms->inboxCount = 0;
}


/* Helper: stringifies bottom into a Pascal string and pushes it onto
 * the brain inbox for sender `from`. Used by every playerNMessage
 * case below in place of the pre-change pattern that overwrote
 * newMessage/newMessageFrom in place (and silently dropped the
 * previous chat in the same tick). */
static void inboxPushFromBottom(MessageState *ms, BYTE from, const char *bottom) {
  char pbuf[BRAIN_INBOX_MSG_LEN];
  utilCtoPString((char *)bottom, pbuf);
  messageInboxPush(ms, from, pbuf);
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
      inboxPushFromBottom(ms, BASE_0, bottom);
      if (ms->lastMessage != player0Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player0Message;
      break;
    case player1Message:
      inboxPushFromBottom(ms, BASE_1, bottom);
      if (ms->lastMessage != player1Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player1Message;
      break;
    case player2Message:
      inboxPushFromBottom(ms, BASE_2, bottom);
      if (ms->lastMessage != player2Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player2Message;
      break;
    case player3Message:
      inboxPushFromBottom(ms, BASE_3, bottom);
      if (ms->lastMessage != player3Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player3Message;
      break;
    case player4Message:
      inboxPushFromBottom(ms, BASE_4, bottom);
      if (ms->lastMessage != player4Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player4Message;
      break;
    case player5Message:
      inboxPushFromBottom(ms, BASE_5, bottom);
      if (ms->lastMessage != player5Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player5Message;
      break;
    case player6Message:
      inboxPushFromBottom(ms, BASE_6, bottom);
      if (ms->lastMessage != player6Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player6Message;
      break;
    case player7Message:
      inboxPushFromBottom(ms, BASE_7, bottom);
      if (ms->lastMessage != player7Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player7Message;
      break;
    case player8Message:
      inboxPushFromBottom(ms, BASE_8, bottom);
      if (ms->lastMessage != player8Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player8Message;
      break;
    case player9Message:
      inboxPushFromBottom(ms, BASE_9, bottom);
      if (ms->lastMessage != player9Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player9Message;
      break;
    case player10Message:
      inboxPushFromBottom(ms, BASE_10, bottom);
      if (ms->lastMessage != player10Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player10Message;
      break;
    case player11Message:
      inboxPushFromBottom(ms, BASE_11, bottom);
      if (ms->lastMessage != player11Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player11Message;
      break;
    case player12Message:
      inboxPushFromBottom(ms, BASE_12, bottom);
      if (ms->lastMessage != player12Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player12Message;
      break;
    case player13Message:
      inboxPushFromBottom(ms, BASE_13, bottom);
      if (ms->lastMessage != player13Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player13Message;
      break;
    case player14Message:
      inboxPushFromBottom(ms, BASE_14, bottom);
      if (ms->lastMessage != player14Message) {
        messageAddItem(ms, top,bottom);
      } else {
        messageAddItem(ms, (char *) MESSAGE_EMPTY,bottom);
      }
      ms->lastMessage = player14Message;
      break;
    case player15Message:
      inboxPushFromBottom(ms, BASE_15, bottom);
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
  /* Walk top and bottom in lockstep, but step by *visual columns* not
   * codepoints. A wide (full-width CJK) codepoint emits its leading
   * cell + a MESSAGE_CELL_CONT cell on its row's next iteration, so
   * cell counts and visual widths stay matched between rows even when
   * one is Latin and the other is CJK. The shorter stream is padded
   * with MESSAGE_BLANK once it ends. */
  const char *tp = top;
  const char *bp = bottom;
  int topPendingCont = 0;  /* CONT cells still owed for top's last wide cp */
  int botPendingCont = 0;

  while (*tp != '\0' || *bp != '\0' || topPendingCont > 0 || botPendingCont > 0) {
    uint32_t topCp;
    if (topPendingCont > 0) {
      topCp = MESSAGE_CELL_CONT;
      topPendingCont--;
    } else if (*tp != '\0') {
      topCp = utf8StepCodepoint(&tp);
      topPendingCont = cellWidth(topCp) - 1;  /* 0 for narrow, 1 for wide */
    } else {
      topCp = MESSAGE_BLANK;
    }

    uint32_t botCp;
    if (botPendingCont > 0) {
      botCp = MESSAGE_CELL_CONT;
      botPendingCont--;
    } else if (*bp != '\0') {
      botCp = utf8StepCodepoint(&bp);
      botPendingCont = cellWidth(botCp) - 1;
    } else {
      botCp = MESSAGE_BLANK;
    }

    messageQueuePush(ms, topCp, botCp);
  }

  /* Trailing blank cell, matching the legacy `count <= longest` overshoot. */
  messageQueuePush(ms, MESSAGE_BLANK, MESSAGE_BLANK);
}

void messageUpdate(struct ClientSim *cs, MessageState *ms) {
  BYTE count;

  if (ms->queueCount > 0) {
    /* Pop the next codepoint pair into the staging slot at the right edge,
     * shift cells left, blank the staging slot. */
    ms->topCells[MESSAGE_WIDTH-1]    = ms->queueTop[ms->queueHead];
    ms->bottomCells[MESSAGE_WIDTH-1] = ms->queueBottom[ms->queueHead];
    ms->queueHead = (ms->queueHead + 1) % MESSAGE_QUEUE_CAP;
    ms->queueCount--;

    count = 0;
    while (count < (MESSAGE_WIDTH-1)) {
      ms->topCells[count]    = ms->topCells[count+1];
      ms->bottomCells[count] = ms->bottomCells[count+1];
      count++;
    }
    ms->topCells[MESSAGE_WIDTH-1]    = MESSAGE_BLANK;
    ms->bottomCells[MESSAGE_WIDTH-1] = MESSAGE_BLANK;

    /* Re-encode visible cells (everything except the staging slot) to UTF-8
     * for the renderer. Buffers are sized to fit MESSAGE_WIDTH * 4 + 1. */
    encodeCellsToUtf8(ms->topCells,    MESSAGE_WIDTH - 1, ms->topLine,    sizeof(ms->topLine));
    encodeCellsToUtf8(ms->bottomCells, MESSAGE_WIDTH - 1, ms->bottomLine, sizeof(ms->bottomLine));
    frontEndMessages(cs, ms->topLine, ms->bottomLine);
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
  /* Drains from the inbox now — the legacy single-slot newMessage
   * buffer was overwriting on every arrival, so two ally bots
   * chatting in the same tick silently dropped one. */
  return messageInboxCount(ms) > 0;
}

BYTE messageGetNewMessage(MessageState *ms, char *dest, uint32_t **playerBitmap) {
  /* Legacy one-message-per-call API. Pops the OLDEST entry from the
   * inbox (FIFO) so back-compat consumers see arrivals in the order
   * they came in, not last-overwrites-first like the pre-change
   * behavior. Multi-message brain consumers should use
   * messageInboxCount/Peek directly. */
  size_t plen;
  int slot;
  BYTE from;

  (void)playerBitmap;
  if (ms == NULL || ms->inboxCount == 0) {
    if (dest != NULL) dest[0] = '\0';
    return 0;
  }
  slot = ms->inboxHead;
  plen = (size_t)((unsigned char)ms->inboxText[slot][0]);
  /* Legacy callers receive the C string (not Pascal-stringified) so
   * the existing `strcpy(dest, ms->newMessage)` contract was actually
   * Pascal-with-NUL — meaning the first byte was the length. Keep
   * that shape: copy length byte + body + NUL. */
  if (dest != NULL) {
    memcpy(dest, ms->inboxText[slot], plen + 1);
    dest[plen + 1] = '\0';
  }
  from = ms->inboxFrom[slot];
  ms->inboxHead = (ms->inboxHead + 1) % BRAIN_INBOX_CAP;
  ms->inboxCount--;
  return from;
}
