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
  bool changeKey = TRUE; /* Whether to change the encryption key or not */
  unsigned short wordsLen; /* Safe length for words data */

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
    switch (itemNum) {
    case log_BaseSetOwner:
      *(logMem+logMemSize) = log_BaseSetOwner ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt2 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt3 ^ logKey;
      logMemSize++;
      break;
    case log_BaseSetStock:
      *(logMem+logMemSize) = log_BaseSetStock ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt2 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt3 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt4 ^ logKey;
      logMemSize++;
      break;
    case log_PlayerJoined:
      *(logMem+logMemSize) = log_PlayerJoined ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt2 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt3 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt4 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = (BYTE) short1 ^ logKey;
      logMemSize++;
      wordsLen = (unsigned short)((BYTE)words[0]) + 1;
      logAddToMemory((logMem+logMemSize), words, (BYTE) wordsLen);
      logMemSize += wordsLen;
      break;
    case log_PlayerQuit:
      *(logMem+logMemSize) = log_PlayerQuit ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      break;
    case log_LostMan:
      *(logMem+logMemSize) = log_LostMan ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      break;
    case log_MapChange:
      *(logMem+logMemSize) =  log_MapChange ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt2 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt3 ^ logKey;
      logMemSize++;
      break;
    case log_ChangeName:
      *(logMem+logMemSize) =  log_ChangeName ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      wordsLen = (unsigned short)((BYTE)words[0]) + 1;
      logAddToMemory((logMem+logMemSize), words, (BYTE) wordsLen);
      logMemSize += wordsLen;
      break;
    case log_AllyRequest:
      *(logMem+logMemSize) =  log_AllyRequest ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt2 ^ logKey;
      logMemSize++;
      break;
    case log_AllyAccept:
      *(logMem+logMemSize) =  log_AllyAccept ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt2 ^ logKey;
      logMemSize++;
      break;
    case log_AllyLeave:
      *(logMem+logMemSize) =  log_AllyLeave ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      break;
    case log_PillSetOwner:
      *(logMem+logMemSize) =  log_PillSetOwner ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt2 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt3 ^ logKey;
      logMemSize++;
      break;
    case log_PillSetPlace:
      *(logMem+logMemSize) =  log_PillSetPlace ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt2 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt3 ^ logKey;
      logMemSize++;
      break;
    case log_PillSetHealth:
    case log_PillSetInTank:
      *(logMem+logMemSize) =  itemNum ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
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
      *(logMem+logMemSize) = itemNum ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt2 ^ logKey;
      logMemSize++;
      break;
    case log_PlayerLocation:
      if (logCheckTankSame(opt1, opt2, opt3, opt4, (BYTE) short1) == TRUE) {
        changeKey = FALSE;
        logNumEvents--;
      } else {
        *(logMem+logMemSize) = itemNum ^ logKey;
        logMemSize++;
        *(logMem+logMemSize) = opt1 ^ logKey;
        logMemSize++;
        *(logMem+logMemSize) = opt2 ^ logKey;
        logMemSize++;
        *(logMem+logMemSize) = opt3 ^ logKey;
        logMemSize++;
        *(logMem+logMemSize) = opt4 ^ logKey;
        logMemSize++;
        *(logMem+logMemSize) = (BYTE) short1 ^ logKey;
        logMemSize++;
      }
      break;
    case log_Shell:
    case log_LgmLocation:
      *(logMem+logMemSize) = itemNum ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt2 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt3 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt4 ^ logKey;
      logMemSize++;
      break;
    case log_KillPlayer:
      *(logMem+logMemSize) = itemNum ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt2 ^ logKey;
      logMemSize++;
      break;
    case log_MessagePlayers:
      *(logMem+logMemSize) = log_MessagePlayers^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt2 ^ logKey;
      logMemSize++;
      wordsLen = (unsigned short)((BYTE)words[0]) + 1;
      logAddToMemory((logMem+logMemSize), words, (BYTE) wordsLen);
      logMemSize += wordsLen;
      break;
    case log_MessageAll:
      *(logMem+logMemSize) = log_MessageAll ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      //FIXTHIS
      wordsLen = (unsigned short)((BYTE)words[0]) + 1;
      logAddToMemory((logMem+logMemSize), words, (BYTE) wordsLen);
      logMemSize += wordsLen;
      break;
    case log_MessageServer:
      *(logMem+logMemSize) = log_MessageServer ^ logKey;
      logMemSize++;
      wordsLen = (unsigned short)((BYTE)words[0]) + 1;
      logAddToMemory((logMem+logMemSize), words, (BYTE) wordsLen);
      logMemSize += wordsLen;
      break;
    case log_PlayerRejoin:
      *(logMem+logMemSize) = log_PlayerRejoin ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      break;
    case log_PlayerLeaving:
      *(logMem+logMemSize) = log_PlayerLeaving ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      break;
    case log_PlayerDied:
      *(logMem+logMemSize) = log_PlayerDied ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      break;
    case log_LobbyEnter:
    case log_LobbyExit:
      *(logMem+logMemSize) = itemNum ^ logKey;
      logMemSize++;
      break;
    case log_PlayerReady:
    case log_PlayerUnready:
    case log_MapSkipVote:
      /* event code + player number */
      *(logMem+logMemSize) = itemNum ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      break;
    case log_TeamSet:
      /* event code + player number + team number */
      *(logMem+logMemSize) = itemNum ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt2 ^ logKey;
      logMemSize++;
      break;
    case log_CountdownStart:
    case log_CountdownCancel:
    case log_BalanceApplied:
      /* event code only, no payload */
      *(logMem+logMemSize) = itemNum ^ logKey;
      logMemSize++;
      break;
    case log_MapSkipApplied:
      /* event code + pascal string map name */
      *(logMem+logMemSize) = itemNum ^ logKey;
      logMemSize++;
      wordsLen = (unsigned short)((BYTE)words[0]) + 1;
      logAddToMemory((logMem+logMemSize), words, (BYTE) wordsLen);
      logMemSize += wordsLen;
      break;
    case log_GameVoteStart:
      /* event code + kind + initiator player + team (0 = global) */
      *(logMem+logMemSize) = itemNum ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt2 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt3 ^ logKey;
      logMemSize++;
      break;
    case log_GameVoteCast:
      /* event code + kind + player + voteYes (0/1) */
      *(logMem+logMemSize) = itemNum ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt2 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt3 ^ logKey;
      logMemSize++;
      break;
    case log_GameVoteEnd:
      /* event code + kind + result (0=failed,1=passed) */
      *(logMem+logMemSize) = itemNum ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt1 ^ logKey;
      logMemSize++;
      *(logMem+logMemSize) = opt2 ^ logKey;
      logMemSize++;
      break;
    default:
      changeKey = FALSE;
      logNumEvents--;
      break;
    }
    logNumEvents++;
    if (changeKey == TRUE) {
      logKey = itemNum;
    }
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
*NAME:          logWriteSnapshot
*AUTHOR:        John Morrison
*CREATION DATE: 25/07/04
*LAST MODIFIED: 25/07/04
*PURPOSE:
* ssim  - ServerSim (contains GameSim plus server-specific fields)
* check - Whether to check if running or not
*********************************************************/
bool logWriteSnapshot(ServerSim *ssim, bool check) {
  GameSim *gs = serverSimGetGameSim(ssim);
  bool returnValue = TRUE; /* Value to return */
  BYTE dataLen;
  BYTE savedDataLen;       /* Non XOR'd datalength */
  BYTE data[512];
  int ret;
  BYTE count = 0;
  bmapRun run;             /* Used to write the runs */
  BYTE xPos;                /* Current position on the map */
  BYTE yPos;
  int len;                 /* Length of the run to write */
  int32_t length;

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

  /* Write start delay and time left */
  if (returnValue == TRUE) {
    length = htonl(serverSimGetStartDelay(ssim));
    ret = writeData((BYTE *) &length, sizeof(int32_t), logOldKey);
    if (ret != Z_OK) {
      returnValue = FALSE;
    }
  }
  if (returnValue == TRUE) {
    length = htonl(serverSimGetGameLength(ssim));
    ret = writeData((BYTE *) &length, sizeof(int32_t), logOldKey);
    if (ret != Z_OK) {
      returnValue = FALSE;
    }
  }

  if (logLobbyMode == TRUE) {
    /* Lobby snapshot — empty world. Each of pills/bases/starts is a
     * 1-byte length-prefixed payload with count=0 (matching the
     * net-data getters' 0-entry shape). The map is a single all-
     * deep-sea terminator run (datalen=4, y=sx=ex=0xFF), which the
     * viewer's run reader at bolo_map.c:371 treats as end-of-map.
     * Each player slot is the 2-byte "not in use" stub the viewer
     * already handles at screen.c:1356.
     *
     * writeData XORs its buffer in place, so we re-initialise the
     * scratch bytes ahead of every call rather than reusing them
     * across calls. */
    BYTE lenByte;
    BYTE payload;
    BYTE terminator[4];
    BYTE stub[2];
    if (returnValue == TRUE) {
      lenByte = 1;
      payload = 0;
      ret = writeData(&lenByte, 1, logOldKey);
      if (ret == Z_OK) ret = writeData(&payload, 1, logOldKey);
      if (ret != Z_OK) returnValue = FALSE;
    }
    if (returnValue == TRUE && logFile) {
      lenByte = 1;
      payload = 0;
      ret = writeData(&lenByte, 1, logOldKey);
      if (ret == Z_OK) ret = writeData(&payload, 1, logOldKey);
      if (ret != Z_OK) returnValue = FALSE;
    }
    if (returnValue == TRUE && logFile) {
      lenByte = 1;
      payload = 0;
      ret = writeData(&lenByte, 1, logOldKey);
      if (ret == Z_OK) ret = writeData(&payload, 1, logOldKey);
      if (ret != Z_OK) returnValue = FALSE;
    }
    if (returnValue == TRUE && logFile) {
      terminator[0] = 4;
      terminator[1] = 0xFF;
      terminator[2] = 0xFF;
      terminator[3] = 0xFF;
      ret = writeData(terminator, 4, logOldKey);
      if (ret != Z_OK) returnValue = FALSE;
    }
    while (count < MAX_TANKS && returnValue == TRUE) {
      dataLen   = 2;
      stub[0]   = count;
      stub[1]   = FALSE;
      ret = writeData(&dataLen, 1, logOldKey);
      if (ret == Z_OK) ret = writeData(stub, 2, logOldKey);
      if (ret != Z_OK) returnValue = FALSE;
      count++;
    }
  } else {
    /* Write pill locations */
    if (returnValue == TRUE) {
      dataLen = pillsGetPillNetData(&gs->pb, data);
      savedDataLen = dataLen;
      ret = writeData(&dataLen, 1, logOldKey);
      ret = writeData(data, savedDataLen, logOldKey);
      if (ret != Z_OK) {
        returnValue = FALSE;
      }
    }
    /* Write bases locations */
    if (returnValue == TRUE && logFile) {
      dataLen = basesGetBaseNetData(&gs->bs, data);
      savedDataLen = dataLen;
      ret = writeData(&dataLen, 1, logOldKey);
      ret = writeData(data, savedDataLen, logOldKey);
      if (ret != Z_OK) {
        returnValue = FALSE;
      }
    }
    /* Write starts locations */
    if (returnValue == TRUE && logFile) {
      dataLen = startsGetStartNetData(&gs->ss, data);
      savedDataLen = dataLen;
      ret = writeData(&dataLen, 1, logOldKey);
      ret = writeData(data, savedDataLen, logOldKey);
      if (ret != Z_OK) {
        returnValue = FALSE;
      }
    }

    /* Write the map itself */
    if (returnValue == TRUE && logFile) {
      xPos = 0;
      yPos = 0;
      while (yPos < 0xFF && returnValue == TRUE) {
        /* Process runs */
        len = mapPrepareRun(&gs->mp, &run, &xPos, &yPos);
        /* Write the run out */
        ret = writeData((BYTE *) &run, len, logOldKey);
        if (ret != Z_OK) {
          returnValue = FALSE;
        }
      }
    }

    /* Write each player */
    while (count < MAX_TANKS && returnValue == TRUE) {
      playersPrepareLogSnapshotForPlayer(gs, &gs->plyrs, count, data, &dataLen);
      savedDataLen = dataLen;
      ret = writeData((BYTE *) &dataLen, 1, logOldKey);
      ret = writeData(data, savedDataLen, logOldKey);

      if (ret != Z_OK) {
        returnValue = FALSE;
      }
      count++;
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
