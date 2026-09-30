/*
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
 *Name:          Message inbox
 *Filename:      message_inbox.c
 *Purpose:
 *  The brain inbox ring inside MessageState, and the one way a line
 *  of chat is put into it.
 *
 *  ITS OWN TRANSLATION UNIT ON PURPOSE. The ring lived in messages.c,
 *  which draws the message HUD and so pulls in frontend.h — a file no
 *  dedicated server has. WinBoloDS could not link messages.c, so
 *  src/server/server_frontend_stubs.c carried a SECOND copy of the ring
 *  and a THIRD copy of the push that feeds it, each with a comment
 *  asking the next reader to keep them in step by hand.
 *
 *  That is the bug class docs/ARCHITECTURE.md exists to prevent:
 *  "features that work on one client (e.g. the desktop GUI) and silently
 *  break on another (server, headless, gym, brain test, Android, WASM)
 *  because the broken client did not run the same code path"
 *  (docs/ARCHITECTURE.md, "Patterns to avoid"). A hand-copied ring is
 *  that divergence waiting to happen — and it had already happened once,
 *  when the server's stubs were no-ops and every hosted bot's inbox
 *  silently stayed empty.
 *
 *  Nothing here draws, so every build links this file and there is one
 *  copy of the ring again. messages.c keeps the message HUD; the
 *  server's stub file keeps only stubs.
 *********************************************************/

#include <string.h>

#include "global.h"
#include "messages.h"

/* Brain inbox ring (see internal/messages.h). Drop-oldest on full, so a
 * producer never blocks and never fails. text is Pascal-stringified
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

void messageInboxPushLine(MessageState *ms, BYTE from, const char *text) {
  char pbuf[BRAIN_INBOX_MSG_LEN];
  size_t len;

  if (ms == NULL || text == NULL) {
    return;
  }
  /* The clamp is written here rather than left to utilCtoPString, which
   * takes no size and would run off the end of a line longer than a chat
   * line. Every caller is inside the cap; the clamp says so. */
  len = strlen(text);
  if (len > BRAIN_INBOX_MSG_LEN - 2) {
    len = BRAIN_INBOX_MSG_LEN - 2;
  }
  pbuf[0] = (char)len;
  memcpy(pbuf + 1, text, len);
  pbuf[len + 1] = '\0';
  messageInboxPush(ms, from, pbuf);
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
