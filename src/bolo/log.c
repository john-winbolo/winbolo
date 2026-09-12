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
*Name:          Log
*Filename:      log.c
*Author:        John Morrison
*CREATION DATE: 05/05/01
*LAST MODIFIED: 25/07/04
*Purpose:
*  Responsable for creating WinBolo log files
*********************************************************/

#include <stdio.h>
#include <string.h>
//#include <winsock2.h>
#include <SDL3/SDL.h>
#include "global.h"
#include "util.h"
#include "bolo_map.h"
#include "starts.h"
#include "pillbox.h"
#include "bases.h"
#include "log.h"
#include "game_sim.h"
#include "netpacks.h"
#include "zip.h"
#include "server_sim.h"
#include "control_event.h"  /* ControlEvent — serverSimFillEntitySyncEvent's out-parameter */
#include "attribution_track.h"
#include "log_internal.h"
#include "../winbolonet/winbolonet_core.h"
#include "../common/wb_log.h"

zipFile logFile;               /* File to log to */
unsigned short logLastEvent; /* Last event logged. Increments each time there are no events */
unsigned short logNumEvents; /* Last event logged. Increments each time there are no events */
bool  logIsRunning;          /* Are we saving a log */
BYTE *logMem = NULL;
unsigned short logMemSize;   /* How much memory are we using */
BYTE logKey; /* Current log encryption key */
BYTE logOldKey; /* Old key needed for writing state */
bool logLastEmpty; /* Was the last log empty? */

logTanks logCheckTanks;

/* Last stock values written per tank, so log_TankSetStock only goes out when
 * one of the four changed. Same shape and lifetime as logCheckTanks: cleared
 * in logStart, and when a spectator ring attaches with no .wbv recording. */
typedef struct {
  BYTE shells;
  BYTE mines;
  BYTE armour;
  BYTE trees;
} logTankStock;
static logTankStock logCheckTankStocks[MAX_TANKS];

/* Thread that owns the log writer. Captured at logStart. Every mutating
 * entry point bails if called from any other thread.
 *
 * Why: log.c keeps its writer state (logMem / logKey / logNumEvents / ...)
 * in file-static globals with no synchronisation. The bot worker pool
 * reaches logAddEvent via clientSimSyncFromSnapshot -> mapSetPos on each
 * bot's own ClientSim, and concurrent writers interleave bytes in logMem
 * and desync the XOR-key chain — the resulting .wbv opens, plays for
 * ~60s, then trips the viewer's lv-corrupt diagnostic. The owner check
 * makes those off-thread calls drop silently. */
static SDL_ThreadID logOwnerThread = 0;

/* While TRUE, the writer is recording a lobby segment: world-mutation
 * events queued via logAddEvent are dropped, and logWriteSnapshot emits
 * an empty world (no pills/bases/starts, all deep sea, no tanks). The
 * lobby roster — joins, leaves, team/ready/countdown, chat, votes —
 * passes through unchanged. handleLobbyEnter sets it on before the
 * opening snapshot; handleGameStart clears it before the rewriting
 * snapshot that establishes the real world for the running segment. */
static bool logLobbyMode = FALSE;

/* Opcodes that touch world state and must be suppressed during a lobby
 * segment. serverSimResetGameWorld and the lobby-time mapSetPos burst
 * would otherwise flood the lobby segment with map-cell deltas and
 * stale base/pill ownership churn — none of which makes sense against
 * the deep-sea lobby snapshot. */
static bool logitemMutatesWorld(logitem itemNum) {
  switch (itemNum) {
    case log_PlayerLocation:
    case log_LgmLocation:
    case log_MapChange:
    case log_Shell:
    case log_SoundBuild:
    case log_SoundFarm:
    case log_SoundShoot:
    case log_SoundHitTank:
    case log_SoundHitTree:
    case log_SoundHitWall:
    case log_SoundMineLay:
    case log_SoundMineExplode:
    case log_SoundExplosion:
    case log_SoundBigExplosion:
    case log_SoundManDie:
    case log_BaseSetOwner:
    case log_BaseSetStock:
    case log_PillSetOwner:
    case log_PillSetHealth:
    case log_PillSetPlace:
    case log_PillSetInTank:
    case log_LostMan:
    case log_KillPlayer:
    case log_PlayerDied:
    case log_PlayerRejoin:
    case log_TankSetStock:
    case log_TankSetModifiers:
    case log_EntityChange:
      return TRUE;
    default:
      return FALSE;
  }
}

void logSetLobbyMode(bool enabled) {
  logLobbyMode = enabled ? TRUE : FALSE;
}

/* Spectator ring tap. When logSpectatorRing is non-NULL, logAddEvent appends
 * each emitted event's plaintext to logSpectatorAcc and logWriteTick records
 * one ring tick per call (a keyframe snapshot body, or the accumulated event
 * bytes). All off by default — NULL ring means the tap costs nothing. */
static SpectatorRing *logSpectatorRing = NULL;
static ServerSim *logSpectatorSim = NULL;

/* The sim whose per-round attribution track logStop serializes into the .wbv.
 * Captured in logStart; NULL means no track member is written. */
static ServerSim *logSsim = NULL;
static BYTE logSpectatorAcc[LOG_MEMORY_BUFFER_SIZE]; /* this tick's plaintext events */
static int logSpectatorAccLen = 0;

void logSetSpectatorRing(SpectatorRing *ring, ServerSim *sim) {
  logSpectatorRing = ring;
  logSpectatorSim = sim;
  logSpectatorAccLen = 0;
  /* Prime the per-tank change-gating baseline so the ring's first
     log_PlayerLocation events emit against a clean slate — the same reset
     logStart does for the .wbv path. Only when no .wbv is recording: an active
     .wbv already owns and maintains logCheckTanks, and the ring shares that
     same gating, so wiping it mid-stream would corrupt the .wbv's location
     deltas. The owner-thread pin is left to logWriteTick's first-call capture
     (the tick thread), exactly as the .wbv path relies on. */
  if (ring != NULL && logIsRunning == FALSE) {
    int count;
    for (count = 0; count < MAX_TANKS; count++) {
      logCheckTanks.item[count].mx = 0;
      logCheckTanks.item[count].my = 0;
      logCheckTanks.item[count].pxy = 0;
      logCheckTanks.item[count].opt = 0;
      logCheckTankStocks[count].shells = 0;
      logCheckTankStocks[count].mines = 0;
      logCheckTankStocks[count].armour = 0;
      logCheckTankStocks[count].trees = 0;
    }
  }
}

bool logHasSpectatorRing(void) {
  return logSpectatorRing != NULL;
}

/* Run at the top of every logWriteTick, before this tick's accounting. Gives a
 * caller that produced log events off the recording thread somewhere to emit
 * them from: logAddEvent drops writes from any thread but the one logWriteTick
 * pinned, so an event queued here lands in this tick's LOG_EVENT frame instead
 * of being discarded. Deliberately not cleared by logCreate — that runs from
 * serverSimCreate, so a background-menu sim created after the hook was
 * installed would disarm it. The registered function lives in a module that is
 * never unloaded, and it guards itself when there is nothing to do. */
static void (*logPreTickHook)(void) = NULL;

void logSetPreTickHook(void (*fn)(void)) {
  logPreTickHook = fn;
}

/*********************************************************
*NAME:          logCreate
*AUTHOR:        John Morrison
*CREATION DATE: 5/5/01
*LAST MODIFIED: 5/5/01
*PURPOSE:
* Creates the log subsystem
*
*ARGUMENTS:
*  
*********************************************************/
void logCreate() {
  logFile = NULL;
  logLastEvent = 0;
  logIsRunning = FALSE;
  logNumEvents = 0;
  logMem = malloc(LOG_MEMORY_BUFFER_SIZE);
  logMemSize = 0;
  logKey = 0;
  logOldKey = 0;
  logLobbyMode = FALSE;
}

/*********************************************************
*NAME:          logWriteEmpty
*AUTHOR:        John Morrison
*CREATION DATE: 05/05/01
*LAST MODIFIED: 25/07/04
*PURPOSE:
* Writes the nothing happened for X ticks to the log file
*
*ARGUMENTS:
*  
*********************************************************/
void logWriteEmpty() {
  BYTE data[3];
  unsigned short us;
  if (logIsRunning == TRUE) {
    if (logLastEvent > 0) {
      if (logLastEvent < LOG_SIZE_LONG_DIFF) {
        data[0] = LOG_NOEVENTS;
        data[1] = (BYTE) logLastEvent;
        zipWriteInFileInZip(logFile, data, 2);
      } else {
        us = htons(logLastEvent);
        data[0] = LOG_NOEVENTS_LONG;
        data[1] = (BYTE) (us >> 8);
        data[2] = (BYTE) (us & 0xFF);
        zipWriteInFileInZip(logFile, data, 3);
      }
    }
    logOldKey = logKey;
    logLastEvent = 0;
  }
}

/*********************************************************
*NAME:          logWriteTick
*AUTHOR:        John Morrison
*CREATION DATE: 05/05/01
*LAST MODIFIED: 25/07/04
*PURPOSE:
* Called every tick. Logs stuff if required
*
*ARGUMENTS:
*  
*********************************************************/
void logWriteTick() {
  BYTE savedKey = logOldKey;

  /* First call pins the owner thread; logStart clears the pin so the next
   * logWriteTick re-pins it per log. logWriteTick is only ever called from
   * the SDL timer thread (serverSimLogTick / simRunHalfStep), so capturing
   * here pins the thread that does the writing — capturing at logStart
   * would pin whichever thread happened to open the log and drop every
   * subsequent logAddEvent.
   *
   * The lobby log opens from the pre-tick hook below
   * (serverDedicatedLogDrain -> handleLobbyEnter), i.e. from inside this
   * call: logStart clears the pin, so the log_LobbyEnter /
   * log_PlayerJoined writes that follow run with logOwnerThread back at 0
   * for the remainder of this tick, on the very timer thread that pinned
   * it a moment earlier and will re-pin it next tick. The no-lobby path is
   * the one that still opens its log off the timer thread — logStart from
   * main() during the sync-replay of CTRL_GAME_PHASE_RUNNING inside
   * serverDedicatedLogInstall — but that call only writes the header, and
   * the worker pool isn't running yet, so that window has no concurrent
   * writers either. */
  if (logOwnerThread == 0) {
    logOwnerThread = SDL_GetCurrentThreadID();
  }

  /* Owner is pinned, nothing is written yet: the point where a deferred
     emission can queue events that this tick's accounting will frame. */
  if (logPreTickHook != NULL) {
    logPreTickHook();
  }

  /* Spectator ring tap: record one ring tick for the registered sim, using the
     tick's accumulated events or a fresh keyframe. Fires whenever a ring is
     registered, independent of .wbv recording — a normal (non-recording) server
     still feeds connecting spectators. The .wbv logMem path below stays gated on
     logIsRunning. */
  {
    if (logSpectatorRing != NULL && logSpectatorSim != NULL) {
      uint32_t gameTick = serverSimGetTick(logSpectatorSim);
      if (spectatorRingNeedsKeyframe(logSpectatorRing, gameTick) == true) {
        /* Ring keyframe = [u32 bodyLen][world snapshot body][u32 ctrlLen]
           [control snapshot] (big-endian lengths). The world body is the same
           plaintext logSerializeSnapshotBody writes to the .wbv; the control
           snapshot is the serverSimSyncSubscriber-equivalent roster / score /
           team / phase / lobby / vote / balance state a delayed joiner needs.
           Both serializers write straight into their final slots to avoid a
           second copy. Ring-only — the .wbv path below is untouched. */
        BYTE *combined = (BYTE *) malloc(LOG_SNAPSHOT_BODY_MAX +
                                         LOG_CONTROL_SNAPSHOT_MAX + 8);
        if (combined != NULL) {
          int bodyLen = logSerializeSnapshotBody(logSpectatorSim, combined + 4,
                                                 LOG_SNAPSHOT_BODY_MAX);
          int ctrlLen = -1;
          if (bodyLen >= 0) {
            ctrlLen = serverSimSerializeControlSnapshot(
                logSpectatorSim, combined + 8 + bodyLen,
                LOG_CONTROL_SNAPSHOT_MAX);
          }
          /* Skip the keyframe entirely on either failure rather than record a
             truncated one (mirrors the original bodyLen >= 0 guard). */
          if (bodyLen >= 0 && ctrlLen >= 0) {
            int cpos = 4 + bodyLen;
            combined[0] = (BYTE) (((uint32_t) bodyLen >> 24) & 0xFF);
            combined[1] = (BYTE) (((uint32_t) bodyLen >> 16) & 0xFF);
            combined[2] = (BYTE) (((uint32_t) bodyLen >> 8) & 0xFF);
            combined[3] = (BYTE) ((uint32_t) bodyLen & 0xFF);
            combined[cpos + 0] = (BYTE) (((uint32_t) ctrlLen >> 24) & 0xFF);
            combined[cpos + 1] = (BYTE) (((uint32_t) ctrlLen >> 16) & 0xFF);
            combined[cpos + 2] = (BYTE) (((uint32_t) ctrlLen >> 8) & 0xFF);
            combined[cpos + 3] = (BYTE) ((uint32_t) ctrlLen & 0xFF);
            spectatorRingRecordTick(logSpectatorRing, gameTick, true, combined,
                                    cpos + 4 + ctrlLen);
          }
          free(combined);
        }
      } else {
        spectatorRingRecordTick(logSpectatorRing, gameTick, false,
                                logSpectatorAcc, logSpectatorAccLen);
      }
      logSpectatorAccLen = 0;
    }
  }

  if (logIsRunning == TRUE) {
    if (logNumEvents > 0) {
      logWriteEmpty();
      logWriteEvents(savedKey);
      logLastEmpty = FALSE;
    } else {
      logLastEvent++;
      if (logLastEvent == 63000) {
        logLastEmpty = FALSE;
        logWriteEmpty();
      }
    }
    logOldKey = logKey;
  }
}

/*********************************************************
*NAME:          logWriteEvents
*AUTHOR:        John Morrison
*CREATION DATE: 05/05/01
*LAST MODIFIED: 25/07/04
*PURPOSE:
* Writes any memory written events to the log file
*
*ARGUMENTS:
*  key - Key to use to write events header
*********************************************************/
void logWriteEvents(BYTE key) {
  BYTE data[3];
  unsigned short us;

  if (logNumEvents > 0) {
    if (logNumEvents < LOG_SIZE_LONG_DIFF) {
      data[0] = LOG_EVENT;
      data[1] = (BYTE) logNumEvents;
      zipWriteInFileInZip(logFile, data, 2);
    } else {
      us = htons(logNumEvents);
      data[0] = LOG_EVENT_LONG;
      data[1] = (BYTE) (us >> 8);
      data[2] = (BYTE) (us & 0xFF);
      zipWriteInFileInZip(logFile, data, 3);
    }
    zipWriteInFileInZip(logFile, logMem, logMemSize);
    logMemSize = 0;
    logNumEvents = 0;
  }
}

/*********************************************************
*NAME:          logStop
*AUTHOR:        John Morrison
*CREATION DATE: 5/5/01
*LAST MODIFIED: 5/5/01
*PURPOSE:
* Stops logging if we are
*
*ARGUMENTS:
*  
*********************************************************/
void logStop() {
  BYTE data[2];

  if (logIsRunning == TRUE) {
    logWriteEmpty();
    data[0] = LOG_QUIT;
    data[1] = LOG_QUIT;
    zipWriteInFileInZip(logFile, data, 2);
    zipCloseFileInZip(logFile);            /* closes log.dat */
    if (logSsim != NULL) {
      /* Second member: the round's attribution track (header + record stream),
       * DEFLATE-compressed alongside log.dat. Written even when the record
       * stream is empty so every logged round carries a track member. */
      size_t tlen = 0; uint32_t trec = 0; bool ttrunc = false;
      const uint8_t *tbuf = serverSimGetTrackBuffer(logSsim, &tlen, &trec, &ttrunc);
      const AttrSlotIdentity *tids = serverSimGetTrackIdentity(logSsim);
      AttrTrackHeader hdr;
      memset(&hdr, 0, sizeof hdr);
      memcpy(hdr.magic, ATTRIBUTION_TRACK_MAGIC, 4);   /* 4 bytes, no NUL */
      hdr.version     = ATTRIBUTION_TRACK_VERSION;
      hdr.truncated   = ttrunc ? 1 : 0;
      hdr.slotCount   = MAX_TANKS;
      memcpy(hdr.slots, tids, sizeof hdr.slots);
      hdr.recordCount = trec;
      zip_fileinfo zi;
      memset(&zi, 0, sizeof zi);
      if (zipOpenNewFileInZip(logFile, ATTRIBUTION_TRACK_MEMBER, &zi,
                              NULL, 0, NULL, 0, "",
                              Z_DEFLATED, Z_DEFAULT_COMPRESSION) == Z_OK) {
        zipWriteInFileInZip(logFile, &hdr, (unsigned)sizeof hdr);
        if (tbuf != NULL && tlen > 0) {
          zipWriteInFileInZip(logFile, tbuf, (unsigned)tlen);
        }
        zipCloseFileInZip(logFile);
        WB_LOG_INFO(WB_LOG_CAT_SERVER,
                    "attribution track: recordCount=%u bytes=%zu truncated=%d",
                    trec, (size_t)(sizeof hdr + tlen), (int)ttrunc);
      }
    }
    zipClose(logFile, "WinBolo Log File");
  }
  logIsRunning = FALSE;
  /* Intentionally do NOT touch logLobbyMode here. logStart calls
   * logStop at its top, so resetting the flag would wipe out an
   * immediately-preceding logSetLobbyMode(TRUE) before the opening
   * snapshot ever reads it. The flag's lifecycle is owned by the
   * explicit logSetLobbyMode callers (handleLobbyEnter sets TRUE
   * before logStart; handleGameStart clears it before the rewriting
   * snapshot). Process-level state stays clean because logCreate
   * initialises it FALSE. */
}

/*********************************************************
*NAME:          logIsRecording
*AUTHOR:        John Morrison
*CREATION DATE: 5/5/01
*LAST MODIFIED: 5/5/01
*PURPOSE:
* Returns if we are reocrding or not
*
*ARGUMENTS:
*  
*********************************************************/
bool logIsRecording() {
  return logIsRunning;
}


/*********************************************************
*NAME:          logAddToMemory
*AUTHOR:        John Morrison
*CREATION DATE: 5/5/01
*LAST MODIFIED: 5/5/01
*PURPOSE:
*
*
*ARGUMENTS:
*	memPos -
*	data   -
*	dataLen -
*********************************************************/
void logAddToMemory(BYTE *memPos, const void *dataIn, BYTE dataLen) {
  const BYTE *data = (const BYTE *)dataIn;
  BYTE count = 0;

  while (count < dataLen) {
    *(memPos+count) = *(data+count) ^ logKey;
    count++;
  }
}

/*********************************************************
*NAME:          logSerializeEvent
*AUTHOR:        John Morrison
*PURPOSE:
* Writes a single event's plaintext bytes (no XOR) into out, framed as
* [type][u16 big-endian payload length][payload]: the event-code byte, then
* the payload byte count as a big-endian u16, then that event type's payload
* in the exact order, lengths and values logAddEvent's switch produces.
* Variable-length events append their pascal string as words[0]+1 plaintext
* bytes. Returns the number of bytes written (3 + payload length), or 0 for an
* unknown event type (nothing written, no framing). Does not touch logKey,
* logMem, logNumEvents or call logCheckTankSame.
*
*ARGUMENTS:
*  itemNum - Item number to serialize
*  opt1    - Option argument 1
*  opt2    - Option argument 2
*  opt3    - Option argument 3
*  opt4    - Option argument 4
*  short1  - Short optional argument
*  words   - Char* optional argument (pascal string)
*  out     - Destination buffer (must hold up to 264 bytes)
*********************************************************/
static int logSerializeEvent(logitem itemNum, BYTE opt1, BYTE opt2, BYTE opt3, BYTE opt4, unsigned short short1, const char *words, BYTE *out) {
  int off = 0; /* Bytes written so far */
  unsigned short wordsLen; /* Safe length for words data */

  switch (itemNum) {
  case log_BaseSetOwner:
    out[off++] = log_BaseSetOwner;
    out[off++] = opt1;
    out[off++] = opt2;
    out[off++] = opt3;
    break;
  case log_BaseSetStock:
    out[off++] = log_BaseSetStock;
    out[off++] = opt1;
    out[off++] = opt2;
    out[off++] = opt3;
    out[off++] = opt4;
    break;
  case log_PlayerJoined:
    out[off++] = log_PlayerJoined;
    out[off++] = opt1;
    out[off++] = opt2;
    out[off++] = opt3;
    out[off++] = opt4;
    out[off++] = (BYTE) short1;
    wordsLen = (unsigned short)((BYTE)words[0]) + 1;
    memcpy(out + off, words, wordsLen);
    off += wordsLen;
    break;
  case log_PlayerQuit:
    out[off++] = log_PlayerQuit;
    out[off++] = opt1;
    break;
  case log_LostMan:
    out[off++] = log_LostMan;
    out[off++] = opt1;
    break;
  case log_MapChange:
    out[off++] = log_MapChange;
    out[off++] = opt1;
    out[off++] = opt2;
    out[off++] = opt3;
    break;
  case log_ChangeName:
    out[off++] = log_ChangeName;
    out[off++] = opt1;
    wordsLen = (unsigned short)((BYTE)words[0]) + 1;
    memcpy(out + off, words, wordsLen);
    off += wordsLen;
    break;
  case log_AllyRequest:
    out[off++] = log_AllyRequest;
    out[off++] = opt1;
    out[off++] = opt2;
    break;
  case log_AllyAccept:
    out[off++] = log_AllyAccept;
    out[off++] = opt1;
    out[off++] = opt2;
    break;
  case log_AllyLeave:
    out[off++] = log_AllyLeave;
    out[off++] = opt1;
    break;
  case log_PillSetOwner:
    out[off++] = log_PillSetOwner;
    out[off++] = opt1;
    out[off++] = opt2;
    out[off++] = opt3;
    break;
  case log_PillSetPlace:
    out[off++] = log_PillSetPlace;
    out[off++] = opt1;
    out[off++] = opt2;
    out[off++] = opt3;
    break;
  case log_PillSetHealth:
    /* The index and the armour in a byte each: armour outgrew a nibble.
       This is what LOG_VERSION 3 says about a file — up to version 2 the
       pair shared one byte, so a reader has to take the version's word
       for the length rather than this writer's. */
    out[off++] = log_PillSetHealth;
    out[off++] = opt1;
    out[off++] = opt2;
    break;
  case log_PillSetInTank:
    out[off++] = log_PillSetInTank;
    out[off++] = opt1;
    break;
  case log_SoundBuild:
  case log_SoundFarm:
  case log_SoundShoot:
  case log_SoundHitWall:
  case log_SoundHitTank:
  case log_SoundHitTree:
  case log_SoundMineLay:
  case log_SoundMineExplode:
  case log_SoundExplosion:
  case log_SoundBigExplosion:
  case log_SoundManDie:
    out[off++] = itemNum;
    out[off++] = opt1;
    out[off++] = opt2;
    break;
  case log_PlayerLocation:
  case log_TankSetStock:
    /* Five bytes either way: position (player, mx, my, pixel nibbles, dir/boat
       nibbles) or stocks (player, shells, mines, armour, trees). */
    out[off++] = itemNum;
    out[off++] = opt1;
    out[off++] = opt2;
    out[off++] = opt3;
    out[off++] = opt4;
    out[off++] = (BYTE) short1;
    break;
  case log_Shell:
  case log_LgmLocation:
    out[off++] = itemNum;
    out[off++] = opt1;
    out[off++] = opt2;
    out[off++] = opt3;
    out[off++] = opt4;
    break;
  case log_KillPlayer:
    out[off++] = itemNum;
    out[off++] = opt1;
    out[off++] = opt2;
    break;
  case log_MessagePlayers:
    out[off++] = log_MessagePlayers;
    out[off++] = opt1;
    out[off++] = opt2;
    wordsLen = (unsigned short)((BYTE)words[0]) + 1;
    memcpy(out + off, words, wordsLen);
    off += wordsLen;
    break;
  case log_MessageAll:
    out[off++] = log_MessageAll;
    out[off++] = opt1;
    //FIXTHIS
    wordsLen = (unsigned short)((BYTE)words[0]) + 1;
    memcpy(out + off, words, wordsLen);
    off += wordsLen;
    break;
  case log_MessageServer:
    out[off++] = log_MessageServer;
    wordsLen = (unsigned short)((BYTE)words[0]) + 1;
    memcpy(out + off, words, wordsLen);
    off += wordsLen;
    break;
  case log_PlayerRejoin:
    out[off++] = log_PlayerRejoin;
    out[off++] = opt1;
    break;
  case log_PlayerLeaving:
    out[off++] = log_PlayerLeaving;
    out[off++] = opt1;
    break;
  case log_PlayerDied:
    out[off++] = log_PlayerDied;
    out[off++] = opt1;
    break;
  case log_LobbyEnter:
  case log_LobbyExit:
    out[off++] = itemNum;
    break;
  case log_PlayerReady:
  case log_PlayerUnready:
  case log_MapSkipVote:
    /* event code + player number */
    out[off++] = itemNum;
    out[off++] = opt1;
    break;
  case log_TeamSet:
    /* event code + player number + team number */
    out[off++] = itemNum;
    out[off++] = opt1;
    out[off++] = opt2;
    break;
  case log_CountdownStart:
  case log_CountdownCancel:
  case log_BalanceApplied:
    /* event code only, no payload */
    out[off++] = itemNum;
    break;
  case log_MapSkipApplied:
    /* event code + pascal string map name */
    out[off++] = itemNum;
    wordsLen = (unsigned short)((BYTE)words[0]) + 1;
    memcpy(out + off, words, wordsLen);
    off += wordsLen;
    break;
  case log_GameSettings:
    /* event code + length-prefixed settings blob. The blob is binary, not
       text: it carries 0x00 bytes, and only the leading length byte decides
       how much is copied. */
    out[off++] = itemNum;
    wordsLen = (unsigned short)((BYTE)words[0]) + 1;
    memcpy(out + off, words, wordsLen);
    off += wordsLen;
    break;
  case log_GameVoteStart:
    /* event code + kind + initiator player + team (0 = global) */
    out[off++] = itemNum;
    out[off++] = opt1;
    out[off++] = opt2;
    out[off++] = opt3;
    break;
  case log_GameVoteCast:
    /* event code + kind + player + voteYes (0/1) */
    out[off++] = itemNum;
    out[off++] = opt1;
    out[off++] = opt2;
    out[off++] = opt3;
    break;
  case log_GameVoteEnd:
    /* event code + kind + result (0=failed,1=passed) */
    out[off++] = itemNum;
    out[off++] = opt1;
    out[off++] = opt2;
    break;
  case log_SpectatorJoined:
    /* spectator slot + country[0] + country[1] + wbnFlags + reserved
       + pascal-string viewer name. Mirrors log_PlayerJoined's shape. */
    out[off++] = log_SpectatorJoined;
    out[off++] = opt1;
    out[off++] = opt2;
    out[off++] = opt3;
    out[off++] = opt4;
    out[off++] = (BYTE) short1;
    wordsLen = (unsigned short)((BYTE)words[0]) + 1;
    memcpy(out + off, words, wordsLen);
    off += wordsLen;
    break;
  case log_SpectatorLeft:
    /* spectator slot + pascal-string viewer name so the leaver is named
       unambiguously even after the slot is reused. */
    out[off++] = log_SpectatorLeft;
    out[off++] = opt1;
    wordsLen = (unsigned short)((BYTE)words[0]) + 1;
    memcpy(out + off, words, wordsLen);
    off += wordsLen;
    break;
  case log_Ping:
    /* sender + kind + worldX (big-endian u16) + worldY (big-endian u16).
       The two coordinate halves ride opt3/opt4 and short1 respectively, so
       the ping lands in the replay at the same sub-tile point the sender
       clicked rather than snapped to a map square. */
    out[off++] = log_Ping;
    out[off++] = opt1;
    out[off++] = opt2;
    out[off++] = opt3;
    out[off++] = opt4;
    out[off++] = (BYTE)((short1 >> 8) & 0xFF);
    out[off++] = (BYTE)(short1 & 0xFF);
    break;
  case log_SpectatorChat:
    /* Format-reserved (no emitter yet): sender spectator slot + pascal-string
       message. Mirrors log_MessageAll so the on-disk shape is locked now. */
    out[off++] = log_SpectatorChat;
    out[off++] = opt1;
    wordsLen = (unsigned short)((BYTE)words[0]) + 1;
    memcpy(out + off, words, wordsLen);
    off += wordsLen;
    break;
  case log_TankSetModifiers:
    /* player + a length-prefixed blob of the six modifier bytes. The six do
       not fit the four opt bytes and the short, so they travel as a binary
       pascal blob the way log_GameSettings carries its settings. */
    out[off++] = log_TankSetModifiers;
    out[off++] = opt1;
    wordsLen = (unsigned short)((BYTE)words[0]) + 1;
    memcpy(out + off, words, wordsLen);
    off += wordsLen;
    break;
  case log_EntityChange:
    /* kind + index + on-the-map flag + a length-prefixed copy of the item's
       map record. The record is six bytes for a pillbox or a base and three
       for a start, so it travels as a binary pascal blob rather than padded
       to one size; the leading length byte decides how much is copied, the
       way log_GameSettings carries its settings. */
    out[off++] = log_EntityChange;
    out[off++] = opt1;
    out[off++] = opt2;
    out[off++] = opt3;
    wordsLen = (unsigned short)((BYTE)words[0]) + 1;
    memcpy(out + off, words, wordsLen);
    off += wordsLen;
    break;
  case log_EntityMasks:
    /* Three 16-bit masks, big-endian, one per list: bit i set means index i,
       counting from zero, holds an item that is on the map. Six bytes, which
       is exactly the four opt bytes and the short, so there is no pascal blob
       here. Same three masks CTRL_ENTITY_SYNC carries to a live client. */
    out[off++] = log_EntityMasks;
    out[off++] = opt1;
    out[off++] = opt2;
    out[off++] = opt3;
    out[off++] = opt4;
    out[off++] = (BYTE)((short1 >> 8) & 0xFF);
    out[off++] = (BYTE)(short1 & 0xFF);
    break;
  case log_ServerText:
    /* The destination the line was published with, then the line: destTeam
       (0 = everyone), destPlayer (0xFF = everyone), pascal text. Both bytes
       are recorded because the viewer has no other way to know a line went to
       one team or one player rather than to the whole game. */
    out[off++] = log_ServerText;
    out[off++] = opt1;
    out[off++] = opt2;
    wordsLen = (unsigned short)((BYTE)words[0]) + 1;
    memcpy(out + off, words, wordsLen);
    off += wordsLen;
    break;
  case log_GameTimeSet:
    /* The round's game time after the change, as a big-endian int32 of ticks
       across the four opt bytes. The settings blob states the length once at
       the head of a round and never restates it, so this is the only thing
       that tells a replay the round's clock moved. */
    out[off++] = log_GameTimeSet;
    out[off++] = opt1;
    out[off++] = opt2;
    out[off++] = opt3;
    out[off++] = opt4;
    break;
  case log_RuleSet:
    /* Which rule changed, as a big-endian u16 index into the simulation's
       rules table, then the value the field ended up holding as a
       length-prefixed blob of eight bytes. The value travels as a blob
       because it does not fit the four opt bytes, the way
       log_TankSetModifiers carries its six; its layout is in
       docs/replay-format.md. */
    out[off++] = log_RuleSet;
    out[off++] = (BYTE)((short1 >> 8) & 0xFF);
    out[off++] = (BYTE)(short1 & 0xFF);
    wordsLen = (unsigned short)((BYTE)words[0]) + 1;
    memcpy(out + off, words, wordsLen);
    off += wordsLen;
    break;
  default:
    return 0;
  }
  /* Frame the [type][payload] the switch produced as [type][u16 BE len]
     [payload] by shifting the payload right two bytes and inserting the
     big-endian payload length after the type byte. */
  {
    int payloadLen = off - 1; /* bytes after the type byte */
    memmove(out + 3, out + 1, (size_t)payloadLen);
    out[1] = (BYTE)((payloadLen >> 8) & 0xFF);
    out[2] = (BYTE)(payloadLen & 0xFF);
    return off + 2;
  }
}

/*********************************************************
*NAME:          logCheckTankStockSame
*PURPOSE:
* Checks whether a tank's four stock values are the same as the last ones
* written for it, updating the cache when they are not. The per-tick emit
* pass offers a record for every connected tank, so this is what keeps
* log_TankSetStock down to one record per tank per change.
*
*ARGUMENTS:
* playerNum - Player number to check
* shells    - Tank shells
* mines     - Tank mines
* armour    - Tank armour
* trees     - Tank trees
*********************************************************/
static bool logCheckTankStockSame(BYTE playerNum, BYTE shells, BYTE mines, BYTE armour, BYTE trees) {
  if (playerNum >= MAX_TANKS) {
    return FALSE;
  }
  if (logCheckTankStocks[playerNum].shells != shells ||
      logCheckTankStocks[playerNum].mines != mines ||
      logCheckTankStocks[playerNum].armour != armour ||
      logCheckTankStocks[playerNum].trees != trees) {
    logCheckTankStocks[playerNum].shells = shells;
    logCheckTankStocks[playerNum].mines = mines;
    logCheckTankStocks[playerNum].armour = armour;
    logCheckTankStocks[playerNum].trees = trees;
    return FALSE;
  }
  return TRUE;
}

/*********************************************************
*NAME:          logAddEvent
*AUTHOR:        John Morrison
*CREATION DATE: 5/5/01
*LAST MODIFIED: 5/5/01
*PURPOSE:
* Adds a event to be logged
*
*ARGUMENTS:
*  itemNum - Item number to add
*  opt1    - Option argument 1
*  opt2    - Option argument 2
*  opt3    - Option argument 3
*  opt4    - Option argument 4
*  short1  - Short optional argument
*  words   - Char* optional argument
*********************************************************/
void logAddEvent(logitem itemNum, BYTE opt1, BYTE opt2, BYTE opt3, BYTE opt4, unsigned short short1, char *words) {
  BYTE event[264]; /* Plaintext event: type + u16 len + 6-byte header + 256-byte pascal string */
  int eventLen; /* Bytes the serializer produced */
  int count;
  bool wbvActive; /* Is a .wbv log buffer the destination this call */

  if (logOwnerThread != 0 && SDL_GetCurrentThreadID() != logOwnerThread) {
    return;
  }
  if (logLobbyMode == TRUE && logitemMutatesWorld(itemNum) == TRUE) {
    return;
  }
  /* Record the event once into the .wbv buffer and/or the spectator ring. The
     change-gating (logCheckTankSame) and serialize run a single time and feed
     both, so a registered ring needs no .wbv log and a location event is never
     double-counted. The .wbv byte stream is unchanged: every step that touches
     logMem / logNumEvents / logKey stays guarded on wbvActive in the original
     order, so an inactive ring leaves the .wbv path identical. */
  wbvActive = (logIsRunning == TRUE && logMem != NULL);
  if (wbvActive == FALSE && logSpectatorRing == NULL) {
    return;
  }
  /* Bounds check applies only to the .wbv buffer (the ring has its own bound
     below). Preserves the original drop-the-event-with-no-side-effect semantics
     when the .wbv buffer is full. Max single event is type + u16 len + 6 bytes
     header + 256 bytes words data. */
  if (wbvActive == TRUE && logMemSize + 264 >= LOG_MEMORY_BUFFER_SIZE) {
    return;
  }
  /* log_PlayerLocation only emits when the tank state changed; logCheckTankSame
     updates its cached state as a side effect. When unchanged, emit nothing:
     no bytes, no event count change, no key rotation. */
  if (itemNum == log_PlayerLocation &&
      logCheckTankSame(opt1, opt2, opt3, opt4, (BYTE) short1) == TRUE) {
    return;
  }
  /* Same rule for the tank's stocks: the tick pass offers one for every
     connected tank, and only a change is worth a record. */
  if (itemNum == log_TankSetStock &&
      logCheckTankStockSame(opt1, opt2, opt3, opt4, (BYTE) short1) == TRUE) {
    return;
  }
  eventLen = logSerializeEvent(itemNum, opt1, opt2, opt3, opt4, short1, words, event);
  if (eventLen <= 0) {
    /* Unknown event type: emit nothing, no count change, no key rotation. */
    return;
  }
  if (wbvActive == TRUE) {
    /* Append the event's plaintext bytes to logMem. */
    for (count = 0; count < eventLen; count++) {
      *(logMem+logMemSize) = event[count];
      logMemSize++;
    }
    logNumEvents++;
    logKey = itemNum;
  }
  /* Feed the same plaintext to the spectator ring's per-tick accumulator, so the
     tap inherits this function's emit decisions and lobby gating. */
  if (logSpectatorRing != NULL &&
      logSpectatorAccLen + eventLen <= (int)sizeof(logSpectatorAcc)) {
    memcpy(logSpectatorAcc + logSpectatorAccLen, event, (size_t)eventLen);
    logSpectatorAccLen += eventLen;
  }
}

/*********************************************************
*NAME:          logDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 5/5/01
*LAST MODIFIED: 5/5/01
*PURPOSE:
* Shuts down the log subsystem
*
*ARGUMENTS:
*  
*********************************************************/
void logDestroy() {
  logStop();
  logIsRunning = FALSE;
  logFile = NULL;  
  if (logMem != NULL) {
    free(logMem);
    logMem = NULL;
  }
}

int writeData(BYTE *data, int len, BYTE key) {
  /* Plaintext stream (v2): no XOR. key is retained in the signature so the
     existing call sites need no churn; it is intentionally unused. */
  (void) key;
  return zipWriteInFileInZip(logFile, data, len);
}

/*********************************************************
*NAME:          logSerializeSnapshotBody
*AUTHOR:        John Morrison
*PURPOSE:
* Writes the snapshot body (everything the recorder emits after the
* LOG_EVENT_SNAPSHOT marker, starting at startDelay) as plaintext into out,
* returning the number of bytes written, or -1 if cap is too small. The bytes,
* their order and their lengths match exactly what logWriteSnapshot feeds
* writeData before the XOR; logWriteSnapshot re-applies the XOR and the zip
* write as a post-step.
*
*ARGUMENTS:
* ssim - ServerSim (contains GameSim plus server-specific fields)
* out  - Destination buffer for the plaintext body
* cap  - Capacity of out in bytes
*********************************************************/
int logSerializeSnapshotBody(ServerSim *ssim, BYTE *out, int cap) {
  GameSim *gs = serverSimGetGameSim(ssim);
  BYTE scratch[512];
  BYTE dataLen;
  int32_t length;
  BYTE count;
  int off = 0;

#define LOG_SNAP_PUT(src, n)                                                   \
  do {                                                                         \
    if (off + (int)(n) > cap) return -1;                                       \
    memcpy(out + off, (src), (size_t)(n));                                     \
    off += (int)(n);                                                           \
  } while (0)

  /* Start delay and time left */
  length = htonl(serverSimGetStartDelay(ssim));
  LOG_SNAP_PUT(&length, sizeof(int32_t));
  length = htonl(serverSimGetGameLength(ssim));
  LOG_SNAP_PUT(&length, sizeof(int32_t));

  if (logLobbyMode == TRUE) {
    /* Lobby snapshot — empty world: count=0 pill/base/start blocks, a single
     * all-deep-sea terminator run, and a "not in use" stub per player slot. */
    BYTE block[2];
    BYTE terminator[4];
    BYTE stub[3];

    block[0] = 1;
    block[1] = 0;
    LOG_SNAP_PUT(block, 2); /* pills */
    LOG_SNAP_PUT(block, 2); /* bases */
    LOG_SNAP_PUT(block, 2); /* starts */

    terminator[0] = 4;
    terminator[1] = 0xFF;
    terminator[2] = 0xFF;
    terminator[3] = 0xFF;
    LOG_SNAP_PUT(terminator, 4);

    for (count = 0; count < MAX_TANKS; count++) {
      stub[0] = 2; /* dataLen */
      stub[1] = count;
      stub[2] = FALSE;
      LOG_SNAP_PUT(stub, 3);
    }
  } else {
    bmapRun run;
    BYTE xPos;
    BYTE yPos;
    int len;

    /* Pill locations */
    dataLen = pillsGetPillNetData(&gs->pb, scratch);
    LOG_SNAP_PUT(&dataLen, 1);
    LOG_SNAP_PUT(scratch, dataLen);

    /* Base locations */
    dataLen = basesGetBaseNetData(&gs->bs, scratch);
    LOG_SNAP_PUT(&dataLen, 1);
    LOG_SNAP_PUT(scratch, dataLen);

    /* Start locations */
    dataLen = startsGetStartNetData(&gs->ss, scratch);
    LOG_SNAP_PUT(&dataLen, 1);
    LOG_SNAP_PUT(scratch, dataLen);

    /* The map itself, as RLE runs */
    xPos = 0;
    yPos = 0;
    while (yPos < 0xFF) {
      len = mapPrepareRun(&gs->mp, &run, &xPos, &yPos);
      LOG_SNAP_PUT(&run, len);
    }

    /* Each player */
    for (count = 0; count < MAX_TANKS; count++) {
      playersPrepareLogSnapshotForPlayer(gs, &gs->plyrs, count, scratch, &dataLen);
      LOG_SNAP_PUT(&dataLen, 1);
      LOG_SNAP_PUT(scratch, dataLen);
    }
  }

#undef LOG_SNAP_PUT
  return off;
}

/*********************************************************
*NAME:          logWriteEntityMasks
*AUTHOR:        John Morrison
*PURPOSE:
* Writes the log_EntityMasks record that belongs after a snapshot, as a
* LOG_EVENT block holding that one event. The snapshot body carries a count
* and a record for every pillbox, base and start but has nowhere to say which
* of them are on the map — the live flags sit past each list's wire region —
* so a reader that started at this snapshot would put every item back. The
* masks are what CTRL_ENTITY_SYNC tells a live client for the same reason,
* and serverSimFillEntitySyncEvent builds them here too, so the rule cannot
* drift between the two.
*
* That function reports false when every index within every count is on the
* map, which is what loading a map produces and what a reader's own load
* assumes, so nothing is written and a round that never takes an item off the
* map records not one extra byte.
*
* Written with writeData rather than queued through logAddEvent: the opening
* snapshot is written from logStart before logIsRunning is set, and
* logAddEvent drops everything until then. Writing the bytes here also keeps
* the record out of logLobbyMode's reach, which matters because the snapshot
* this follows is the one a round opens with.
*
*ARGUMENTS:
* ssim - ServerSim (contains GameSim plus server-specific fields)
*********************************************************/
static bool logWriteEntityMasks(ServerSim *ssim) {
  ControlEvent evt;
  BYTE block[2];
  BYTE event[264];
  int eventLen;

  if (ssim == NULL) {
    return TRUE;
  }
  if (!serverSimFillEntitySyncEvent(ssim, &evt)) {
    return TRUE;
  }

  eventLen = logSerializeEvent(log_EntityMasks,
                               (BYTE)((evt.u.entitySync.pills >> 8) & 0xFF),
                               (BYTE)(evt.u.entitySync.pills & 0xFF),
                               (BYTE)((evt.u.entitySync.bases >> 8) & 0xFF),
                               (BYTE)(evt.u.entitySync.bases & 0xFF),
                               evt.u.entitySync.starts, NULL, event);
  if (eventLen <= 0) {
    return TRUE;
  }

  /* One event in this block, framed the way logWriteEvents frames a queued
     one. */
  block[0] = LOG_EVENT;
  block[1] = 1;
  if (writeData(block, 2, logOldKey) != Z_OK) {
    return FALSE;
  }
  if (writeData(event, eventLen, logOldKey) != Z_OK) {
    return FALSE;
  }
  /* The key rotation a queued event would have left behind. Inert on a v2
     stream, which is plaintext, and kept so the two paths agree. */
  logKey = log_EntityMasks;
  logOldKey = logKey;
  return TRUE;
}

/*********************************************************
*NAME:          logWriteSnapshot
*AUTHOR:        John Morrison
*CREATION DATE: 25/07/04
*LAST MODIFIED: 25/07/04
*PURPOSE:
* ssim  - ServerSim (contains GameSim plus server-specific fields)
* check - Whether to check if running or not
*********************************************************/
bool logWriteSnapshot(ServerSim *ssim, bool check) {
  bool returnValue = TRUE; /* Value to return */
  BYTE data[512];
  int ret;

  if (logIsRunning == FALSE && check == TRUE) {
    return TRUE;
  }

  
//  printf("snapshotting %d - ", logOldKey);

  if (logNumEvents > 0) {
    logWriteEvents(logOldKey);
    /* logWriteEvents flushes the queued events but leaves logOldKey
     * stale at the pre-tick value, while logKey has advanced to the
     * last event's code. The reader's blockKey after a LOG_EVENT block
     * equals that last event code, so without re-syncing here the
     * snapshot marker we write next would be XOR'd with the wrong key
     * and the whole stream desyncs. The empty branch below gets this
     * for free via logWriteEmpty's own logOldKey = logKey tail. */
    logOldKey = logKey;
  } else {
    logWriteEmpty();
    if (logLastEmpty == TRUE) {
      return TRUE;
    }
    logLastEmpty = TRUE;
  }

  data[0] = LOG_EVENT_SNAPSHOT;
  ret = writeData(data, 1, logOldKey);
  if (ret != Z_OK) {
    returnValue = FALSE;
  }

  /* Serialize the snapshot body as plaintext, then emit it with a single
   * writeData so it is XOR'd with logOldKey and zip-written in one pass. The
   * body uses the same key throughout, so concatenating the sections produces
   * the same on-disk bytes the per-section writes did. */
  if (returnValue == TRUE) {
    BYTE *body = (BYTE *) malloc(LOG_SNAPSHOT_BODY_MAX);
    if (body == NULL) {
      returnValue = FALSE;
    } else {
      int bodyLen = logSerializeSnapshotBody(ssim, body, LOG_SNAPSHOT_BODY_MAX);
      if (bodyLen < 0) {
        returnValue = FALSE;
      } else {
        ret = writeData(body, bodyLen, logOldKey);
        if (ret != Z_OK) {
          returnValue = FALSE;
        }
      }
      free(body);
    }
  }
  logOldKey = logKey;

  /* The part of the world the body has no room for: which items are on the
     map. Written after it, so a reader that starts here has the records from
     the snapshot and the flags from this. */
  if (returnValue == TRUE) {
    returnValue = logWriteEntityMasks(ssim);
  }

  return returnValue;
}

/*********************************************************
*NAME:          logStart
*AUTHOR:        John Morrison
*CREATION DATE: 05/05/01
*LAST MODIFIED: 25/07/04
*PURPOSE:
* Starts logging. Return success
*
*ARGUMENTS:
* fileName    - FileName and path of the file to open
* ssim        - ServerSim (contains GameSim plus server-specific fields)
* ai          - Games AI type
* maxPlayers  - Maximum number of players allowed in the
*                game
* usePassword - Is the game password protected
*********************************************************/
bool logStart(char *fileName, ServerSim *ssim, BYTE ai, BYTE maxPlayers, bool usePassword) {
  bool returnValue; /* Value to return */
  int ret;            /* Function return value */
  zip_fileinfo zi;
  BYTE data[512];
  int32_t start;
  unsigned short port;
  BYTE count;

  returnValue = TRUE;
  logStop(); /* Stop the current log if it is running */
  logSsim = ssim; /* sim whose attribution track logStop serializes at round end */
  logLastEmpty = FALSE;
  /* Reset owner-thread capture so the next logWriteTick re-pins it.
   * Necessary across round boundaries: handleLobbyEnter for a new round
   * calls logStart on whichever thread published CTRL_GAME_PHASE_LOBBY
   * (timer thread), so the previous round's owner would still be the
   * timer thread and the check would pass — but clearing it lets the
   * capture stay correctly scoped per-log. */
  logOwnerThread = 0;

  count = 0;
  while (count < MAX_TANKS) {
    logCheckTanks.item[count].mx = 0;
    logCheckTanks.item[count].my = 0;
    logCheckTanks.item[count].pxy = 0;
    logCheckTanks.item[count].opt = 0;
    logCheckTankStocks[count].shells = 0;
    logCheckTankStocks[count].mines = 0;
    logCheckTankStocks[count].armour = 0;
    logCheckTankStocks[count].trees = 0;
    count++;
  }


  logMemSize = 0;
  logNumEvents = 0;
  logFile = zipOpen(fileName, 0);
  
  zi.tmz_date.tm_sec = zi.tmz_date.tm_min = zi.tmz_date.tm_hour =
  zi.tmz_date.tm_mday = zi.tmz_date.tm_mon = zi.tmz_date.tm_year = 0;
  zi.dosDate = 0;
  zi.internal_fa = 0;
  zi.external_fa = 0;

  
  if (logFile == NULL) {
    returnValue = FALSE;
    ret = Z_OK;
  } else {
    ret = zipOpenNewFileInZip(logFile, "log.dat", &zi, NULL, 0, NULL, 0, "", Z_DEFLATED, Z_DEFAULT_COMPRESSION);
  }

  if (ret != Z_OK) {
    returnValue = FALSE;
  } else {
    strcpy((char *)data, LOG_HEADER);
    ret = zipWriteInFileInZip(logFile, data, (unsigned int) strlen((char *)data));
    if (ret != Z_OK) {
      returnValue = FALSE;
    }
  }

  
  /* Write log version */
  if (returnValue == TRUE) {
    data[0] = LOG_VERSION;
    ret = zipWriteInFileInZip(logFile, data, 1);
    if (ret != Z_OK) {
      returnValue = FALSE;
    }
  }

  /* Write Map Name */
  if (returnValue == TRUE) {
    strcpy((char *)(data+1), serverSimGetMapName(ssim));
    data[0] = (BYTE) strlen((char *)(data+1));
    ret = zipWriteInFileInZip(logFile, data, data[0]+1);
    if (ret != Z_OK) {
      returnValue = FALSE;
    }
  }

  /* Write Game Type, Allow mines, AI type, password */
  if (returnValue == TRUE) {
    data[0] = gameTypeGet(&serverSimGetGameSim(ssim)->game);
    data[1] = minesGetAllowHiddenMines(&serverSimGetGameSim(ssim)->mns);
    data[2] = ai;
    data[3] = usePassword;
    data[4] = maxPlayers;
    data[5] = BOLO_VERSION_MAJOR;
    data[6] = BOLO_VERSION_MINOR;
    data[7] = BOLO_VERSION_REVISION;
    ret = zipWriteInFileInZip(logFile, data, 8);
    if (ret != Z_OK) {
      returnValue = FALSE;
    }
  }

  /* Write server address and port */
  memset(data, 0, 4);
  port = 0;
  port = htons(port);
  if (returnValue == TRUE) {
    ret = zipWriteInFileInZip(logFile, data, 4);
    if (ret != Z_OK) {
      returnValue = FALSE;
    } else {
      ret = zipWriteInFileInZip(logFile, &port, sizeof(unsigned short));
      if (ret != Z_OK) {
        returnValue = FALSE;
      }
    }
  }

  /* Start time */
  if (returnValue == TRUE) {
    start = htonl((int32_t)serverSimGetTimeCreated(ssim));
    ret = zipWriteInFileInZip(logFile, &start, sizeof(int32_t));
    if (ret != Z_OK) {
      returnValue = FALSE;
    }
  }

  /* Write WBN Key */
  if (returnValue == TRUE) {
    char wbnKey[WINBOLONET_KEY_LEN];
    winboloNetGetServerKey(wbnKey);
    ret = zipWriteInFileInZip(logFile, wbnKey, 32);
    if (ret != Z_OK) {
      returnValue = FALSE;
    }
  }

  logKey = logOldKey = (BYTE) (serverSimGetTimeCreated(ssim) & 0xFF);
  /* Write Snapshot */
  if (returnValue == TRUE) {
    returnValue = logWriteSnapshot(ssim, FALSE);
  }

  if (returnValue == TRUE) {
    logIsRunning = TRUE;
  } else if(logFile != NULL) {
    zipCloseFileInZip(logFile);
    zipClose(logFile, "");
    logFile = NULL;
  }

  return returnValue;
}

/*********************************************************
*NAME:          logCheckTankSame
*AUTHOR:        John Morrison
*CREATION DATE: 24/01/05
*LAST MODIFIED: 24/01/05
*PURPOSE:
* Checks if the tanks position is the same from last
* update.
*
*ARGUMENTS:
* playerNum - Player number to check
* mx        - Tank MX
* my        - Tank MY
* pxy       - Tank PXY
* opt       - Misc tank data
*********************************************************/
bool logCheckTankSame(BYTE playerNum, BYTE mx, BYTE my, BYTE pxy, BYTE opt) {

  if (my == 0) {
    my =0;
  }

  if (playerNum >= MAX_TANKS) {
    return FALSE;
  } else  if (logCheckTanks.item[playerNum].mx != mx || logCheckTanks.item[playerNum].my != my || logCheckTanks.item[playerNum].pxy != pxy || logCheckTanks.item[playerNum].opt != opt) {
    logCheckTanks.item[playerNum].mx = mx;
    logCheckTanks.item[playerNum].my = my;
    logCheckTanks.item[playerNum].pxy = pxy;
    logCheckTanks.item[playerNum].opt = opt;
    return FALSE;
  }
  return TRUE;
}
