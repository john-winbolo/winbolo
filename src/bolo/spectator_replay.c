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
 *Name:          Spectator Replay
 *Filename:      spectator_replay.c
 *Author:        John Morrison
 *Purpose:
 *  Translate spectator-ring records into the LOG_*-marked
 *  plaintext byte stream the log-viewer decoder consumes, and
 *  synthesize the v2 header it reads before the first snapshot.
 *
 *  Pure data: no globals, no allocation, no sim / ring /
 *  transport / log-viewer dependency. Every write is bounds-
 *  checked against the caller's buffer.
 *********************************************************/

#include <stdint.h>
#include <string.h>

#include "spectator_replay.h"
#include "log.h"          /* LOG_* markers, LOG_HEADER, LOG_VERSION, LOG_SIZE_LONG_DIFF */
#include "platform_net.h" /* htons / htonl */

/* Read a big-endian u32 from p[0..3]. */
static uint32_t specReplayReadU32BE(const BYTE *p) {
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
         ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

int specReplayWriteHeader(const SpecReplayHeaderInfo *info, BYTE *out, int cap) {
  int pos = 0;
  int mapLen;
  unsigned short port;
  int32_t startTime;

  if (info == NULL || out == NULL || info->mapName == NULL || cap < 0) {
    return -1;
  }
  mapLen = (int) strlen(info->mapName);
  if (mapLen > 255) {
    return -1;
  }

  /* "WBOLOMOV" id tag, 8 bytes, no NUL. (LENGTH_ID in internal/bolo_map.h is
     this same 8; the literal avoids pulling that header in for one constant.) */
  if (pos + 8 > cap) return -1;
  memcpy(out + pos, LOG_HEADER, 8);
  pos += 8;

  /* Version byte. A seed is built here and read straight away, so it
     states the current version: plaintext stream, block key 0, framed
     events, and the pill health record in its two-byte form. */
  if (pos + 1 > cap) return -1;
  out[pos++] = (BYTE) LOG_VERSION;

  /* Map name: [len][bytes]. */
  if (pos + 1 + mapLen > cap) return -1;
  out[pos++] = (BYTE) mapLen;
  memcpy(out + pos, info->mapName, (size_t) mapLen);
  pos += mapLen;

  /* Game type, allow-hidden-mines, AI, password, max players, version triple. */
  if (pos + 8 > cap) return -1;
  out[pos++] = info->gameType;
  out[pos++] = info->allowHiddenMines;
  out[pos++] = info->ai;
  out[pos++] = info->usePassword;
  out[pos++] = info->maxPlayers;
  out[pos++] = info->versionMajor;
  out[pos++] = info->versionMinor;
  out[pos++] = info->versionRevision;

  /* Server address: 4 zero bytes. */
  if (pos + 4 > cap) return -1;
  memset(out + pos, 0, 4);
  pos += 4;

  /* Port: htons(0), written as the raw 2-byte value the writer emits. */
  if (pos + (int) sizeof(unsigned short) > cap) return -1;
  port = htons(0);
  memcpy(out + pos, &port, sizeof(unsigned short));
  pos += (int) sizeof(unsigned short);

  /* Create time: htonl(0). Display-only from v2 on (it no longer seeds a
     key). */
  if (pos + (int) sizeof(int32_t) > cap) return -1;
  startTime = (int32_t) htonl(0);
  memcpy(out + pos, &startTime, sizeof(int32_t));
  pos += (int) sizeof(int32_t);

  /* WBN key: 32 zero bytes. */
  if (pos + 32 > cap) return -1;
  memset(out + pos, 0, 32);
  pos += 32;

  return pos;
}

int specReplayTranslateKeyframe(const BYTE *payload, int payloadLen,
                                BYTE *out, int cap,
                                const BYTE **ctrl, int *ctrlLen) {
  uint32_t bodyLen;
  uint32_t cLen;

  /* Leave the control out-params well-defined on every error return. */
  if (ctrl != NULL) *ctrl = NULL;
  if (ctrlLen != NULL) *ctrlLen = 0;

  if (payload == NULL || out == NULL || cap < 0 || payloadLen < 8) {
    return -1;
  }

  /* [u32 bodyLen BE][body][u32 ctrlLen BE][ctrl]. Validate each length against
     the bytes actually present, with no signed/u32 overflow in the arithmetic. */
  bodyLen = specReplayReadU32BE(payload);
  if (bodyLen > (uint32_t) (payloadLen - 8)) {
    return -1;
  }
  cLen = specReplayReadU32BE(payload + 4 + bodyLen);
  if (cLen > (uint32_t) payloadLen - 8u - bodyLen) {
    return -1;
  }

  /* Emit [LOG_EVENT_SNAPSHOT][body]; the body is the bytes the decoder reads
     straight after the snapshot marker. */
  if ((uint32_t) cap < bodyLen + 1u) {
    return -1;
  }
  out[0] = LOG_EVENT_SNAPSHOT;
  memcpy(out + 1, payload + 4, (size_t) bodyLen);

  /* Expose the control-snapshot slice; the caller hands it to the HUD. */
  if (ctrl != NULL) *ctrl = payload + 8 + bodyLen;
  if (ctrlLen != NULL) *ctrlLen = (int) cLen;

  return 1 + (int) bodyLen;
}

int specReplayTranslateEvents(const BYTE *payload, int payloadLen,
                              BYTE *out, int cap) {
  int pos;
  int count;
  unsigned short us;

  if (out == NULL || cap < 0 || payloadLen < 0) {
    return -1;
  }
  if (payloadLen > 0 && payload == NULL) {
    return -1;
  }

  if (payloadLen == 0) {
    if (cap < 2) return -1;
    out[0] = LOG_NOEVENTS;
    out[1] = 1;
    return 2;
  }

  /* Walk the concatenated [type][u16 BE len][body] events: count them and
     require the framing to land exactly on payloadLen. */
  pos = 0;
  count = 0;
  while (pos < payloadLen) {
    int evLen;
    if (pos + 3 > payloadLen) {
      return -1;
    }
    evLen = ((int) payload[pos + 1] << 8) | (int) payload[pos + 2];
    if (pos + 3 + evLen > payloadLen) {
      return -1;
    }
    pos += 3 + evLen;
    count++;
  }

  /* [LOG_EVENT][count] for a small count, else [LOG_EVENT_LONG][htons count] —
     mirroring logWriteEvents — then the payload verbatim. */
  if (count < LOG_SIZE_LONG_DIFF) {
    if (cap < 2 + payloadLen) return -1;
    out[0] = LOG_EVENT;
    out[1] = (BYTE) count;
    memcpy(out + 2, payload, (size_t) payloadLen);
    return 2 + payloadLen;
  }
  if (cap < 3 + payloadLen) return -1;
  us = htons((unsigned short) count);
  out[0] = LOG_EVENT_LONG;
  out[1] = (BYTE) (us >> 8);
  out[2] = (BYTE) (us & 0xFF);
  memcpy(out + 3, payload, (size_t) payloadLen);
  return 3 + payloadLen;
}
