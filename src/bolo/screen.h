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

#endif /* SCREEN_H */
