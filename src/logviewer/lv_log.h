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
*Filename:      log.h
*Author:        John Morrison
*Creation Date: 5/5/01
*Last Modified: 5/5/01
*Purpose:
*  Responsable for creating WinBolo log files
*********************************************************/

#ifndef _LOG_H
#define _LOG_H

#include <stdio.h>
#include <string.h>
#include "lv_global.h"
#include "lv_bolo_map.h"
#include "lv_starts.h"
#include "lv_pillbox.h"
#include "lv_bases.h"

/* Log items */
#define LOG_QUIT 0
#define LOG_NOEVENTS 1
#define LOG_NOEVENTS_LONG 2
#define LOG_EVENT 3
#define LOG_EVENT_LONG 4
#define LOG_SNAPSHOT 5

/* The difference between big log and items */
#define LOG_SIZE_LONG_DIFF 256


#define LOG_HEADER "WBOLOMOV"
#define LOG_VERSION_V0 0  /* Original: IP address octets in player join events */
#define LOG_VERSION_V1 1  /* Country codes replace IP addresses; opt4 carries accountFlags (bit 0=WBN, bit 1=Steam, bit 5=bot) */
#define LOG_VERSION_V2 2  /* Plaintext (no XOR); each event framed [type][u16 BE payload-length][payload]. Join payload semantics unchanged from V1 */
#define LOG_VERSION LOG_VERSION_V1

/* The events we record in our log file */
typedef enum {
log_PlayerJoined=1,
log_PlayerQuit,
log_PlayerLocation,
log_LgmLocation,
log_MapChange,
log_Shell,
log_SoundBuild,
log_SoundFarm,
log_SoundShoot,
log_SoundHitTank,
log_SoundHitTree,
log_SoundHitWall,
log_SoundMineLay,
log_SoundMineExplode,
log_SoundExplosion,
log_SoundBigExplosion,
log_SoundManDie,
log_MessageServer,
log_MessageAll,
log_MessagePlayers,
log_ChangeName,
log_AllyRequest,
log_AllyAccept,
log_AllyLeave,
log_BaseSetOwner,
log_BaseSetStock,
log_PillSetOwner,
log_PillSetHealth,
log_PillSetPlace,
log_PillSetInTank,
log_SaveMap,
log_LostMan,
log_KillPlayer,
log_PlayerRejoin,
log_PlayerLeaving,
log_PlayerDied,
log_LobbyEnter,
log_LobbyExit,
log_PlayerReady,
log_PlayerUnready,
log_TeamSet,
log_CountdownStart,
log_CountdownCancel,
log_MapSkipVote,
log_MapSkipApplied,
log_BalanceApplied,
log_GameVoteStart,   // opt1=kind, opt2=initiator, opt3=team (0 = global)
log_GameVoteCast,    // opt1=kind, opt2=player,    opt3=voteYes
log_GameVoteEnd,     // opt1=kind, opt2=result (0=failed,1=passed)
log_SpectatorJoined, // opt1=spectator slot, opt2/opt3=country[0]/[1], opt4=wbnFlags, reserved byte, then name pstr
log_SpectatorLeft,   // opt1=spectator slot, then name pstr (names the leaver across slot reuse)
log_SpectatorChat,   // format-reserved: opt1=sender spectator slot + message pstr (no emitter yet)
log_GameSettings,    // pascal-string blob of every lobby setting (layout in docs/replay-format.md)
log_Ping,            // opt1=sender, opt2=kind, then worldX/worldY as two big-endian u16 (layout in docs/replay-format.md)
log_TankSetStock,    // opt1=player, opt2=shells, opt3=mines, opt4=armour, short1=trees
log_TankSetModifiers // opt1=player, then a 6-byte pascal blob: speed, accel, turn, reload, dealt, taken
} logitem;

/* Vote-kind values inside the log_GameVote* events. Mirrored from
 * src/bolo/internal/netpacks.h; that header is server-only, but the
 * logviewer needs to distinguish surrender from back-to-lobby to pick
 * the right rendered message. Keep these in lock-step. */
#define GAME_VOTE_KIND_BACK_TO_LOBBY  1
#define GAME_VOTE_KIND_SURRENDER      2

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
void logCreate();

/*********************************************************
*NAME:          logWriteEmpty
*AUTHOR:        John Morrison
*CREATION DATE: 5/5/01
*LAST MODIFIED: 5/5/01
*PURPOSE:
* Writes the nothing happened for X ticks to the log file
*
*ARGUMENTS:
*  
*********************************************************/
void logWriteEmpty();

/*********************************************************
*NAME:          logWriteTick
*AUTHOR:        John Morrison
*CREATION DATE: 5/5/01
*LAST MODIFIED: 5/5/01
*PURPOSE:
* Called every tick. Logs stuff if required
*
*ARGUMENTS:
*  
*********************************************************/
void logWriteTick();

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
void logStop();

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
bool logIsRecording();

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
void logAddEvent(logitem itemNum, BYTE opt1, BYTE opt2, BYTE opt3, BYTE opt4, unsigned short short1, char *words);

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
void logDestroy();

/*********************************************************
*NAME:          logStop
*AUTHOR:        John Morrison
*CREATION DATE: 5/5/01
*LAST MODIFIED: 5/5/01
*PURPOSE:
* Stops logging if we are
*
*ARGUMENTS:
*  fileName - FileName and path of the file to open
*********************************************************/
bool logStart(char *fileName, map *mp, bases *bs, pillboxes *pb, starts *ss);

#endif /* _LOG_H */