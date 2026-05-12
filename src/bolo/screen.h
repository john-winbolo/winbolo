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
*Name:          Screen
*Filename:      screen.h
*Author:        John Morrison
*Creation Date: 28/10/98
*Last Modified: 17/12/03
*Purpose:
*  Provides Interfaces with the front end
*********************************************************/

#ifndef SCREEN_H
#define SCREEN_H


/* Includes */
#include "global.h"
#include "viewport_types.h"
#include "client_enums.h"
#include "client_render.h"
#include "gametype.h"
#include "screenbullet.h"
#include "screentank.h"
#include "lgm.h"
#include "brain.h"
#include "players.h"
#include "screenlgm.h"
#include "building.h"
#include "explosions.h"
#include "floodfill.h"
#include "grass.h"
#include "mines.h"
#include "minesexp.h"
#include "rubble.h"
#include "swamp.h"
#include "tankexp.h"
#include "input_packet.h"

/* Forward declarations */
struct ServerSim;
struct GameSim;

/* Button Pressed - These are the valid items
   that should be passed to this module*/

#ifndef _AITYPE_ENUM
#define _AITYPE_ENUM

typedef enum {
  aiNone,
  aiYes,
  aiYesAdvantage,
  aiFull
} aiType;

#endif

/* Prototypes */

bool screenIsItemInTrees(struct GameSim *sim, tank viewerTank, WORLD bmx, WORLD bmy);
void screenGetSubMapSquareOffset(int *xPos, int *yPos);
void screenAddBrainObject(struct ClientSim *cs, unsigned short object, WORLD wx, WORLD wy, unsigned short idNum, BYTE dir, BYTE info, BYTE speed);
void screenMakeBrainViewDataCS(struct ClientSim *cs, BYTE *buff, BYTE leftPos, BYTE rightPos, BYTE topPos, BYTE bottomPos);
void clientCenterTankCS(struct ClientSim *csPtr);
void screenNetStatusMessage(struct ClientSim *csPtr, char *messageStr);
bool screenExtractPNBData(BYTE *buff, BYTE dataLen, bool isTcp);
bool screenExtractMNTData(BYTE *buff, BYTE dataLen, bool isTcp);


/* -------------------------------------------------------
 * Content migrated from backend.h during Phase 7 cleanup.
 * backend.h was a "kitchen sink" header; these definitions
 * now live here as their canonical location.
 * ------------------------------------------------------- */

/* The game timer is 20 milliseconds between events the game_tick_length is half this */
#define GAME_TICK_LENGTH 10
#define GAME_NUMTOTALTICKS_SEC (1000 / GAME_TICK_LENGTH)
#define GAME_NUMGAMETICKS_SEC (1000 / 20)

/* Messages status on/off */
#define MSG_NEWSWIRE 0
#define MSG_ASSISTANT 1
#define MSG_AI 2
#define MSG_NETWORK 3
#define MSG_NETSTATUS 4

/* Flag to indicate no gunsight is to be drawn */
#define NO_GUNSIGHT -1

/* Number of squares a tank must be in to see it in the forests */
#define MIN_SIGHT_DISTANCE_LEFT -1
#define MIN_SIGHT_DISTANCE_RIGHT 1

#ifndef BASES_H
typedef enum {
  baseDead,
  baseOwnGood,
  baseAllieGood,
  baseNeutral,
  baseEvil
} baseAlliance;
#endif

#ifndef PILLBOX_H
typedef enum {
  pillDead,
  pillAllie,
  pillGood,
  pillNeutral,
  pillEvil,
  pillTankGood,
  pillTankAllie,
  pillTankEvil
} pillAlliance;
#endif

#ifndef _BUILDSELECT_ENUM
#define _BUILDSELECT_ENUM

/* The type of building operation currently being selected */
typedef enum {
  BsTrees,
  BsRoad,
  BsBuilding,
  BsPillbox,
  BsMine
} buildSelect;
#endif

#ifndef _PLAYERNUMBERS_ENUM
#define _PLAYERNUMBERS_ENUM
/* Player Numbers */
typedef enum {
  player01,
  player02,
  player03,
  player04,
  player05,
  player06,
  player07,
  player08,
  player09,
  player10,
  player11,
  player12,
  player13,
  player14,
  player15,
  player16
} playerNumbers;
#endif

void screenGetMessages(struct ClientSim *csPtr, char *top, char *bottom);
void screenShowMessages(struct ClientSim *csPtr, BYTE msgType, bool isShown);
void screenSetAutoScroll(struct ClientSim *csPtr, bool isAuto);
void screenSetLabelOwnTank(struct ClientSim *csPtr, bool value);
void screenSetMesageLabelLen(struct ClientSim *csPtr, labelLen value);
void screenSetTankLabelLen(struct ClientSim *csPtr, labelLen value);

/* Forward declaration for ClientSim-parameterized functions */
struct ClientSim;

void screenMakeBrainInfoCS(struct ClientSim *cs, BrainInfo *value, bool first, aiType aiMode);
void screenExtractBrainInfoCS(struct ClientSim *cs, BrainInfo *value);
void screenBuildInputPacketCS(struct ClientSim *cs, InputPacket *pkt, tankButton tb, bool isShoot, bool isMine, bool isBrain, bool isGameTick, BYTE playerNum, uint32_t tick);
void screenSyncFromSnapshotCS(struct ClientSim *cs,
                              const SnapshotHeader *hdr,
                              const TankSnapshot *tanks, int tankCount,
                              const ShellSnapshot *shellSnaps, int shellCount,
                              const TkExplosionSnapshot *tkExplSnaps, int tkExplosionCount,
                              const BaseSnapshot *baseSnaps, int baseCount,
                              const PillSnapshot *pillSnaps, int pillCount,
                              const GameEvent *events, int eventCount,
                              BYTE playerNum);
BYTE screenCalcSquareCS(struct ClientSim *csPtr, BYTE xValue, BYTE yValue, BYTE scrX, BYTE scrY);
tankButton screenTranslateBrainButtonsCS(struct ClientSim *csPtr, bool *isShoot, bool isGameTick);
void screenGunsightRangeCS(struct ClientSim *csPtr, bool increase);
void screenManMoveCS(struct ClientSim *csPtr, buildSelect buildS);
void screenLgmDropPillCS(struct ClientSim *csPtr, BYTE mx, BYTE my, BYTE owner, BYTE pillNum);
void screenTankLayMineCS(struct ClientSim *csPtr);
void screenCheckTankMineDamageCS(struct ClientSim *csPtr, BYTE mx, BYTE my);
void screenSetAllowHiddenMinesCS(struct ClientSim *csPtr, bool hidden);
int32_t screenGetGameStartDelayCS(struct ClientSim *csPtr);
tankAlliance screenTankAllianceCS(struct ClientSim *csPtr, BYTE playerNum);
BYTE screenGetNumNeutralPillsCS(struct ClientSim *csPtr);
int32_t screenGetTimeGameCreatedCS(struct ClientSim *csPtr);
void screenSetTimeGameCreatedCS(struct ClientSim *csPtr, int32_t value);
void screenSetMapNameCS(struct ClientSim *csPtr, char *name);
void screenSetTimeLengthsCS(struct ClientSim *csPtr, int srtDelay, int32_t gmeLen);
void screenSetGameTypeCS(struct ClientSim *csPtr, gameType gt);
void screenNetSetupTankCS(struct ClientSim *csPtr, bool isInStart);
void screenNetSetupTankGoCS(struct ClientSim *csPtr);
void screenSetBaseNetDataCS(struct ClientSim *csPtr, BYTE *buff, int length);
void screenSetPillNetDataCS(struct ClientSim *csPtr, BYTE *buff, BYTE dataLen);
void screenSetStartsNetDataCS(struct ClientSim *csPtr, BYTE *buff, BYTE dataLen);
void screenChangeOwnershipCS(struct ClientSim *csPtr, BYTE oldOwner);
void screenGetLgmStatusCS(struct ClientSim *csPtr, bool *isOut, bool *isDead, TURNTYPE *angle);
BYTE screenMakeShellDataCS(struct ClientSim *csPtr, BYTE *buff);
void screenExtractShellDataCS(struct ClientSim *csPtr, BYTE *buff, BYTE dataLen);
void screenIncomingMessageCS(struct ClientSim *csPtr, BYTE playerNum, char *messageStr);
void screenSetCursorPosCS(struct ClientSim *csPtr, BYTE posX, BYTE posY);
void screenTankStopCarryingPillCS(struct ClientSim *csPtr, BYTE itemNum);
void screenNetLgmReturnCS(struct ClientSim *csPtr, BYTE numTrees, BYTE numMines, BYTE pillNum);
void screenNetManWorkingCS(struct ClientSim *csPtr, BYTE mapX, BYTE mapY, BYTE numMines, BYTE pillNum, BYTE numTrees);
void screenSetTankStartPositionCS(struct ClientSim *csPtr, BYTE xValue, BYTE yValue, TURNTYPE angle, BYTE numShells, BYTE numMines);
void screenSetPlayersMenuCS(struct ClientSim *csPtr);
#endif /* SCREEN_H */
