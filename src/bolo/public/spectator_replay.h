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
 *Filename:      spectator_replay.h
 *Author:        John Morrison
 *Purpose:
 *  A pure-data translator that turns the spectator ring's
 *  keyframe / event-tick record payloads into the
 *  LOG_*-marked plaintext byte stream the log-viewer decoder
 *  consumes, and synthesizes the v2 header the decoder reads
 *  before the first snapshot.
 *
 *  The module is side-effect-free: it holds no state, owns no
 *  buffers, and knows nothing of the ring, the sim, the wire
 *  transport or the log viewer. It operates on already-extracted
 *  record payloads, writing into caller-provided buffers.
 *
 *  Dependency-free public seam: its only include is platform_types.h
 *  (also public), so it is includable from any tier — the sim, the
 *  server, and the log viewer / client spectator host (which feeds the
 *  captured records into the decoder), as well as tests/unit.
 *********************************************************/

#ifndef SPECTATOR_REPLAY_H
#define SPECTATOR_REPLAY_H

#include "platform_types.h" /* BYTE */

typedef struct {
  const char *mapName;        /* <= 255 chars */
  BYTE gameType, allowHiddenMines, ai, usePassword, maxPlayers;
  BYTE versionMajor, versionMinor, versionRevision;
} SpecReplayHeaderInfo;

/* Write the v2 .wbv-style header the decoder reads before the first snapshot.
   addr(4)/port(2)/createTime(4)/wbnKey(32) are zeroed (v2 holds the block key at
   0, so createTime is display-only). Returns bytes written, or -1 on overflow. */
int specReplayWriteHeader(const SpecReplayHeaderInfo *info, BYTE *out, int cap);

/* Translate a ring keyframe payload [u32 bodyLen BE][body][u32 ctrlLen BE][ctrl]
   into [LOG_EVENT_SNAPSHOT][body]. Sets *ctrl/*ctrlLen to the control-snapshot
   slice within payload (NOT written to out — the caller hands it to the HUD).
   Returns bytes written to out, or -1 on malformed input / overflow. */
int specReplayTranslateKeyframe(const BYTE *payload, int payloadLen,
                                BYTE *out, int cap,
                                const BYTE **ctrl, int *ctrlLen);

/* Translate a ring event-tick payload (concatenated [type][u16 BE len][body]
   events, or empty) into LOG_* bytes: empty -> [LOG_NOEVENTS][1]; else
   [LOG_EVENT][count] (count < LOG_SIZE_LONG_DIFF) or [LOG_EVENT_LONG][htons count]
   followed by the payload verbatim. Returns bytes written, or -1 on malformed
   framing / overflow. */
int specReplayTranslateEvents(const BYTE *payload, int payloadLen,
                              BYTE *out, int cap);

#endif /* SPECTATOR_REPLAY_H */
