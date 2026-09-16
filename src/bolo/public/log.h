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
*Creation Date: 05/05/01
*Last Modified: 24/01/05
*Purpose:
*  Responsable for creating WinBolo log files
*********************************************************/

#ifndef _LOG_H
#define _LOG_H

#include <stdio.h>
#include <string.h>
#include "global.h"
#include "scenario_panel.h" /* SCN_PANEL_MAX — the longest payload a record carries */
#include "server_sim.h"

/* Log items */
#define LOG_QUIT 0
#define LOG_NOEVENTS 1
#define LOG_NOEVENTS_LONG 2
#define LOG_EVENT 3
#define LOG_EVENT_LONG 4
#define LOG_EVENT_SNAPSHOT 5

/* The difference between big log and items */
#define LOG_SIZE_LONG_DIFF 256

/* Log header and version information. Version 2 drops the XOR
 * obfuscation (plaintext stream) and frames each event record as
 * [type][u16 big-endian payload length][payload]. Version 3 keeps that
 * framing and gives log_PillSetHealth two payload bytes — the pillbox
 * index and its armour — where every earlier version packed the pair
 * into one byte's nibbles.
 *
 * The same ladder is mirrored in src/logviewer/lv_log.h, which is what
 * reads the files; keep the two in step. */
#define LOG_HEADER "WBOLOMOV"
#define LOG_VERSION_V0 0
#define LOG_VERSION_V1 1
#define LOG_VERSION_V2 2
#define LOG_VERSION_V3 3
#define LOG_VERSION LOG_VERSION_V3

/* Memory buffer for writing events */
#define LOG_MEMORY_BUFFER_SIZE (64 *1024)

/* The most bytes one serialized event can occupy, framing included. Every
 * buffer a single event is written into is this size, and the memory
 * buffer keeps this much room free before it takes another one.
 *
 * The longest record is log_ScnPanel: the framing spends the type byte and
 * the two-byte payload length, and its payload spends the panel id, the two
 * destination bytes and the list's own two-byte length ahead of a list
 * capped at SCN_PANEL_MAX:
 *   1 + 2 + 1 + 1 + 1 + 2 + 1017 = 1025
 * Every other record is shorter. The widest of them spends six header bytes
 * and a 255-byte pascal string with its length byte, which is 264. */
#define LOG_EVENT_MAX_BYTES (3 + 5 + SCN_PANEL_MAX)

/* The events we record in our log file */
typedef enum {
log_PlayerJoined=1, // 0 must remain an invalid/canary value
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
log_TankSetModifiers,// opt1=player, then a 6-byte pascal blob: speed, accel, turn, reload, dealt, taken
log_EntityChange,    // opt1=kind (ENTITY_KIND_*), opt2=index (0 based), opt3=on the map, then the item's record as a pascal blob (layout in docs/replay-format.md)
log_EntityMasks,     // which indices are on the map, as three big-endian u16: pills in opt1/opt2, bases in opt3/opt4, starts in short1. Written after every snapshot (layout in docs/replay-format.md)
log_ServerText,      // a server line a scenario wrote: opt1=destTeam (0 = everyone), opt2=destPlayer (0xFF = everyone), then the text as a pascal string
log_GameTimeSet,     // the round's game time after a scenario changed it, as a big-endian int32 of ticks across opt1..opt4
log_RuleSet,         // one simulation rule a scenario changed: short1=rule index, then the value the field ended up holding as an 8-byte pascal blob (layout in docs/replay-format.md)
log_ScnPanel,        // one scenario panel's display list: opt1=panel id, opt2=destTeam (0 = everyone), opt3=destPlayer (0xFF = everyone), short1=the list's byte length, then that many bytes (layout in docs/replay-format.md)
log_ScnScore,        // a scenario's score row: opt1=kind, opt2=target, then the score as a big-endian int32 and the label as a pascal string (layout in docs/replay-format.md)
log_ScnAnnounce,     // a centre-screen line a scenario put up: opt1=destTeam, opt2=destPlayer, short1=ticks it stays up, then the text as a pascal string
log_ScnMarker        // a scenario map marker: opt1=id, opt2=kind, opt3=destTeam, opt4=destPlayer, then x, y, slot and colour as a four-byte pascal blob
} logitem;

typedef struct {
  BYTE mx;
  BYTE my;
  BYTE pxy;
  BYTE opt;
} logTank;

typedef struct {
  logTank item[MAX_TANKS];
} logTanks;
  
 
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
void logWriteEvents(BYTE key);

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
*NAME:          logStart
*AUTHOR:        John Morrison
*CREATION DATE: 05/05/01
*LAST MODIFIED: 25/07/04
*PURPOSE:
* Starts logging, returns success
*
*ARGUMENTS:
* fileName    - FileName and path of the file to open
* ssim        - ServerSim (contains GameSim plus server-specific fields)
* ai          - Games AI type
* maxPlayers  - Maximum number of players allowed in the
*                game
* usePassword - Is the game password protected
*********************************************************/
bool logStart(char *fileName, ServerSim *ssim, BYTE ai, BYTE maxPlayers, bool usePassword);

/*********************************************************
*NAME:          logWriteSnapshot
*AUTHOR:        John Morrison
*CREATION DATE: 25/07/04
*LAST MODIFIED: 25/07/04
*PURPOSE:
* Writes a snapshot. Returns success
*
*ARGUMENTS:
* ssim  - ServerSim (contains GameSim plus server-specific fields)
* check - Whether to check if running or not
*********************************************************/
bool logWriteSnapshot(ServerSim *ssim, bool check);

/*********************************************************
*NAME:          logSetLobbyMode
*PURPOSE:
* Toggles lobby recording mode. While enabled, logAddEvent
* drops world-mutation opcodes (map/pill/base/shell/sound/
* tank/lgm position) and logWriteSnapshot emits an empty
* world (no pills/bases/starts, all deep sea, no tanks).
* Lobby roster events (PlayerJoined/Quit/Leaving, TeamSet,
* Ready/Unready, CountdownStart/Cancel, BalanceApplied,
* MapSkip*, MessageServer/All/Players, ChangeName, votes)
* pass through unchanged.
*
*ARGUMENTS:
*  enabled - TRUE to enter lobby mode, FALSE to leave it
*********************************************************/
void logSetLobbyMode(bool enabled);

/*********************************************************
*NAME:          logSetPreTickHook
*PURPOSE:
* Registers a function to run at the top of every
* logWriteTick, before the tick's event accounting.
* logAddEvent drops writes from any thread but the one
* logWriteTick pinned as the log's owner, so a caller that
* produces events on another thread queues them and emits
* them from here instead. Events emitted by the hook land
* in the tick's LOG_EVENT frame. Pass NULL to clear.
* Install-only: logCreate does not reset it.
*
*ARGUMENTS:
*  fn - Function to run each tick, or NULL for none
*********************************************************/
void logSetPreTickHook(void (*fn)(void));

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
bool logCheckTankSame(BYTE playerNum, BYTE mx, BYTE my, BYTE pxy, BYTE opt);

#endif /* _LOG_H */
