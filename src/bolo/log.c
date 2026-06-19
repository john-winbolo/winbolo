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
#include "log_internal.h"
#include "../winbolonet/winbolonet_core.h"

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
static BYTE logSpectatorAcc[LOG_MEMORY_BUFFER_SIZE]; /* this tick's plaintext events */
static int logSpectatorAccLen = 0;

void logSetSpectatorRing(SpectatorRing *ring, ServerSim *sim) {
  logSpectatorRing = ring;
  logSpectatorSim = sim;
  logSpectatorAccLen = 0;
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
        data[0] = LOG_NOEVENTS ^ logOldKey;
        data[1] = (BYTE) logLastEvent ^ logOldKey;
        zipWriteInFileInZip(logFile, data, 2);
      } else {
        us = htons(logLastEvent);
        data[0] = LOG_NOEVENTS_LONG ^ logOldKey;
        data[1] = (BYTE) (us >> 8) ^ logOldKey;
        data[2] = (BYTE) (us & 0xFF) ^ logOldKey;
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

  /* First call pins the owner thread. logStart runs from main() at
   * startup (sync-replay of CTRL_GAME_PHASE_LOBBY inside
   * serverDedicatedLogInstall), but every tick afterwards runs from the
   * SDL timer thread — capturing the owner at logStart would pin the
   * wrong thread and drop every subsequent logAddEvent. logWriteTick is
   * only ever called from the timer thread (serverSimLogTick /
   * simRunHalfStep), so capturing here pins the correct one. The
   * startup-thread window between logStart and the first logWriteTick
   * has no concurrent writers (worker pool isn't running yet), so the
   * log_LobbyEnter / log_PlayerJoined writes during sync-replay pass
   * through with logOwnerThread still 0. */
  if (logOwnerThread == 0) {
    logOwnerThread = SDL_GetCurrentThreadID();
  }

  if (logIsRunning == TRUE) {
    /* Spectator ring tap: record one ring tick for the registered sim, using
       the tick's accumulated events or a fresh keyframe. Independent of the
       .wbv logMem path below. */
    if (logSpectatorRing != NULL && logSpectatorSim != NULL) {
      uint32_t gameTick = serverSimGetTick(logSpectatorSim);
      if (spectatorRingNeedsKeyframe(logSpectatorRing, gameTick) == true) {
        BYTE *body = (BYTE *) malloc(LOG_SNAPSHOT_BODY_MAX);
        if (body != NULL) {
          int bodyLen = logSerializeSnapshotBody(logSpectatorSim, body,
                                                 LOG_SNAPSHOT_BODY_MAX);
          if (bodyLen >= 0) {
            spectatorRingRecordTick(logSpectatorRing, gameTick, true, body,
                                    bodyLen);
          }
          free(body);
        }
      } else {
        spectatorRingRecordTick(logSpectatorRing, gameTick, false,
                                logSpectatorAcc, logSpectatorAccLen);
      }
      logSpectatorAccLen = 0;
    }

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
      data[0] = LOG_EVENT ^ key;
      data[1] = (BYTE) logNumEvents ^ key;
      zipWriteInFileInZip(logFile, data, 2);
    } else {
      us = htons(logNumEvents);
      data[0] = LOG_EVENT_LONG ^ key;
      data[1] = (BYTE) (us >> 8) ^ key;
      data[2] = (BYTE) (us & 0xFF) ^ key;
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
  BYTE savedKey = logOldKey; /* Save the key as the old key will be overridden in WriteEmpty */

  if (logIsRunning == TRUE) {
    logWriteEmpty();
    data[0] = LOG_QUIT ^ savedKey;
    data[1] = LOG_QUIT ^ savedKey;
    zipWriteInFileInZip(logFile, data, 2);
    zipCloseFileInZip(logFile);
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
* Writes a single event's plaintext bytes (no XOR) into out: the event-code
* byte followed by that event type's payload, in the exact order, lengths and
* values logAddEvent's switch produces before the XOR. Variable-length events
* append their pascal string as words[0]+1 plaintext bytes. Returns the number
* of bytes written, or 0 for an unknown event type (nothing written). Does not
* touch logKey, logMem, logNumEvents or call logCheckTankSame.
*
*ARGUMENTS:
*  itemNum - Item number to serialize
*  opt1    - Option argument 1
*  opt2    - Option argument 2
*  opt3    - Option argument 3
*  opt4    - Option argument 4
*  short1  - Short optional argument
*  words   - Char* optional argument (pascal string)
*  out     - Destination buffer (must hold up to 262 bytes)
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
  case log_PillSetInTank:
    out[off++] = itemNum;
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
  default:
    return 0;
  }
  return off;
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
  BYTE event[262]; /* Plaintext event: 6-byte header + 256-byte pascal string */
  int eventLen; /* Bytes the serializer produced */
  int count;

  if (logOwnerThread != 0 && SDL_GetCurrentThreadID() != logOwnerThread) {
    return;
  }
  if (logLobbyMode == TRUE && logitemMutatesWorld(itemNum) == TRUE) {
    return;
  }
  if (logIsRunning == TRUE && logMem != NULL) {
    /* Bounds check: ensure we have room in the log buffer.
       Max single event is 6 bytes header + 256 bytes words data */
    if (logMemSize + 262 >= LOG_MEMORY_BUFFER_SIZE) {
      return;
    }
    /* log_PlayerLocation only emits when the tank state changed; logCheckTankSame
       updates its cached state as a side effect. When unchanged, emit nothing:
       no bytes, no event count change, no key rotation. */
    if (itemNum == log_PlayerLocation &&
        logCheckTankSame(opt1, opt2, opt3, opt4, (BYTE) short1) == TRUE) {
      return;
    }
    eventLen = logSerializeEvent(itemNum, opt1, opt2, opt3, opt4, short1, words, event);
    if (eventLen <= 0) {
      /* Unknown event type: emit nothing, no count change, no key rotation. */
      return;
    }
    /* Append the event to logMem by XOR-ing each plaintext byte with the
       current logKey, which is constant for the whole event. */
    for (count = 0; count < eventLen; count++) {
      *(logMem+logMemSize) = event[count] ^ logKey;
      logMemSize++;
    }
    /* Feed the same plaintext to the spectator ring's per-tick accumulator,
       so the tap inherits this function's emit decisions and lobby gating. */
    if (logSpectatorRing != NULL &&
        logSpectatorAccLen + eventLen <= (int)sizeof(logSpectatorAcc)) {
      memcpy(logSpectatorAcc + logSpectatorAccLen, event, (size_t)eventLen);
      logSpectatorAccLen += eventLen;
    }
    logNumEvents++;
    logKey = itemNum;
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
  int count = 0;
  while (count < len) {
    data[count] = data[count] ^ key;
    count++;
  }

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
