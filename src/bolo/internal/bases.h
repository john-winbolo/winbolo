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
*Name:          Bases 
*Filename:      bases.h 
*Author:        John Morrison
*Creation Date: 28/10/98
*Last Modified: 19/11/03
*Purpose:
*  Provides operations on bases
*********************************************************/

#ifndef BASES_H
#define BASES_H


/* Includes */
#include "global.h"
#include "alliance_enums.h"
#include "types.h"

struct GameSim;
struct ClientSim;

/* The time between base get new units of armour/shells/mines */
#define BASE_ADD_TIME  1670 /* Old time was 3340 in 1.09 */

/* Defines how much the bases can hold */
#define BASE_FULL_ARMOUR 90
#define BASE_FULL_SHELLS 90
#define BASE_FULL_MINES  90

#define BASE_NOT_FOUND 254

#define BASE_TICKS_BETWEEN_REFUEL 1000
/* A base is dead if it has 9 armour */
#define BASE_DEAD 9

/* Base will soak shots until it has this much armor */
#define BASE_MIN_CAN_HIT 4

/* This is the units at which winbolo will display */
#define BASE_DISPLAY_X 4

/* Minium amount of stocks a base can have of each item */
#define BASE_MIN_SHELLS 0
/* Base will keep 10 units (2 armor) for itself to defend itself against attack */
#define BASE_MIN_ARMOUR 10
#define BASE_MIN_MINES 0

/* The amount given to a tank each time */
#define BASE_SHELLS_GIVE 1
#define BASE_ARMOUR_GIVE 5
#define BASE_MINES_GIVE 1

#define BASE_REFUEL_ARMOUR 46 /* 125 */
#define BASE_REFUEL_SHELLS 7.5 /* 30 */
#define BASE_REFUEL_MINES 7.5 /* 30 */

/* Minimum armour required for a capture */
#define MIN_ARMOUR_CAPTURE 9

/* Range something has to be in to be shown on the display */
/* 7 map squares or 1792 WORLD units */
#define BASE_STATUS_RANGE 1792

/* This is for the half tick calculator these numbers are simply unique identifiers*/

#define BASES_HALFTICK_TYPE_SHELL  1
#define BASES_HALFTICK_TYPE_MINE   2
#define BASES_HALFTICK_TYPE_ARMOUR 3

/* Bases Brain stuff */
#define BASES_BRAIN_FRIENDLY 0
#define BASES_BRAIN_NEUTRAL 2
#define BASES_BRAIN_HOSTILE 1
#define BASES_BRAIN_OBJECT_TYPE 3

/* Prototypes */
void basesUpdateTimer(struct GameSim *sim, int playerNumber);
void basesRemoveTimer(struct GameSim *sim, int playerNumber);
/*********************************************************
*NAME:         basesCreate 
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Creates and initilises the bases structure.
*  Sets number of bases to zero
*
*ARGUMENTS:
*  value - Pointer to the bases structure 
*********************************************************/
void basesCreate(bases *value);

/*********************************************************
*NAME:          basesDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Destroys the bases data structure. Also frees memory.
*
*ARGUMENTS:
*  value - Pointer to the bases structure
*********************************************************/
void basesDestroy(bases *value);

/*********************************************************
*NAME:          basesSetNumBases
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets the number of bases in the structure 
*
*ARGUMENTS:
*  value     - Pointer to the bases structure
*  numBases - The number of bases  
*********************************************************/
void basesSetNumBases(bases *value, BYTE numBases);

/*********************************************************
*NAME:          basesGetNumBases 
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Gets the number of bases in the structure
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*********************************************************/
BYTE basesGetNumBases(bases *value);

/*********************************************************
*NAME:          basesSetBase
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets a specific base with its item data
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  item    - Pointer to a base item 
*  baseNum - The base number
*********************************************************/
void basesSetBase(bases *value, base *item, BYTE bsaeNum);

/*********************************************************
*NAME:          basesGetBase
*AUTHOR:        John Morrison
*CREATION DATE: 9/2/98
*LAST MODIFIED: 9/2/98
*PURPOSE:
*  Gets a specific base
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  item    - Pointer to a base item 
*  baseNum - The base number
*********************************************************/
void basesGetBase(bases *value, base *item, BYTE baseNum);

/*********************************************************
*NAME:          basesExistPos
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Returns whether a base exist at a specific location
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
bool basesExistPos(bases *value, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          basesGetAlliancePos
*AUTHOR:        John Morrison
*CREATION DATE: 21/12/98
*LAST MODIFIED: 21/12/98
*PURPOSE:
*  Returns the type of a base (good, neutral, evil) 
*  depending on the map position. Returns base dead if
*  base not found.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
baseAlliance basesGetAlliancePos(struct GameSim *sim, BYTE xValue, BYTE yValue, BYTE viewPlayer);

/*********************************************************
*NAME:          basesGetStatusNum
*AUTHOR:        John Morrison
*CREATION DATE: 21/12/98
*LAST MODIFIED: 11/1/99
*PURPOSE:
*  Returns the type of a base (good, neutral, dead, evil) 
*  depending on the baseNum. Returns base neutral if
*  base not found.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
baseAlliance basesGetStatusNum(struct GameSim *sim, BYTE baseNum);

/*********************************************************
*NAME:          basesUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 10/1/99
*LAST MODIFIED: 19/2/99
*PURPOSE:
*  Called once every Bolo tick to update stocks, give
*  stuff to tanks etc.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  tnk    - Pointer to the tank structure
*********************************************************/
void basesUpdate(struct GameSim *sim, tank *tnk);

/*********************************************************
*NAME:          basesEnqueueCaptureMessage
*PURPOSE:
*  Client-side: emits or debounces a STOLE_BASE/CAPTURE_BASE
*  newswire message in response to EVENT_BASE_CAPTURED.
*  STOLE_BASE messages are debounced per (newOwner, prevOwner)
*  pair (leading + trailing edge) so two players flipping a
*  base back and forth produces one message at the start and
*  one once the burst settles, instead of hundreds.
*  CAPTURE_BASE (from neutral) is never debounced.
*
*ARGUMENTS:
*  sim       - Pointer to the client's GameSim
*  cs        - Pointer to the ClientSim (for player-name resolution)
*  newOwner  - Player who took the base
*  prevOwner - Previous owner (NEUTRAL for capture-from-neutral)
*********************************************************/
void basesEnqueueCaptureMessage(struct GameSim *sim, struct ClientSim *cs,
                                BYTE newOwner, BYTE prevOwner);

/*********************************************************
*NAME:          basesTickMessageQueue
*PURPOSE:
*  Client-side per-tick maintenance for the steal-message
*  debounce table. Ages slot counters and flushes any pending
*  message whose quiet period has elapsed. Call once per
*  client frame.
*
*ARGUMENTS:
*  sim - Pointer to the client's GameSim
*  cs  - Pointer to the ClientSim (for player-name resolution)
*********************************************************/
void basesTickMessageQueue(struct GameSim *sim, struct ClientSim *cs);

/*********************************************************
*NAME:          basesUpdateStock
*AUTHOR:        John Morrison
*CREATION DATE: 10/1/99
*LAST MODIFIED: 19/2/99
*PURPOSE:
*  Called when a stock update is needed
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  baseNum - The base to update
*********************************************************/
void basesUpdateStock(struct GameSim *sim, BYTE baseNum);

/*********************************************************
*NAME:          basesAmOwner
*AUTHOR:        John Morrison
*CREATION DATE: 10/1/99
*LAST MODIFIED: 31/10/99
*PURPOSE:
*  Returns whether the bases at the particular location
*  is owned by the player
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  owner  - Owner to check with
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
bool basesAmOwner(struct GameSim *sim, BYTE owner, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          basesSetBaseOwner
*AUTHOR:        John Morrison
*CREATION DATE: 10/1/99
*LAST MODIFIED: 2/11/99
*PURPOSE:
* Sets the base to be owned by paremeter passed.
* Returns the previous owner. If it was not neutral we
* assume then it was "stolen" and subsequently remove
* all its possessions. If migrate is set to TRUE then
* it has migrated from a alliance when a player left 
* and we shouldn't make a message
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  baseNum - Base number to set
*  owner   - Who owns it
*  migrate - TRUE if it has migrated from an alliance
*********************************************************/
BYTE basesSetBaseOwner(struct GameSim *sim, BYTE baseNum, BYTE owner, BYTE migrate);

/*********************************************************
*NAME:          basesSetOwner
*AUTHOR:        John Morrison
*CREATION DATE: 10/1/99
*LAST MODIFIED: 2/11/99
*PURPOSE:
* Sets the base to be owned by paremeter passed.
* Returns the previous owner. If it was not neutral we
* assume then it was "stolen" and subsequently remove
* all its possessions. If migrate is set to TRUE then
* it has migrated from a alliance when a player left 
* and we shouldn't make a message
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  xValue  - X Location
*  yValue  - Y Location
*  owner   - Who owns it
*  migrate - TRUE if it has migrated from an alliance
*********************************************************/
BYTE basesSetOwner(struct GameSim *sim, BYTE xValue, BYTE yValue, BYTE owner, BYTE migrate);

/*********************************************************
*NAME:          basesGetBaseNum
*AUTHOR:        John Morrison
*CREATION DATE: 10/1/99
*LAST MODIFIED: 10/1/99
*PURPOSE:
* Returns the bases number.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
BYTE basesGetBaseNum(bases *value, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          basesRefueling
*AUTHOR:        John Morrison
*CREATION DATE: 10/1/99
*LAST MODIFIED: 10/1/99
*PURPOSE:
* The baseNum is being occupied by a tank. Refuel it
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
void basesRefueling(struct GameSim *sim, tank *tnk, BYTE baseNum);

/*********************************************************
*NAME:          basesGetClosest
*AUTHOR:        John Morrison
*CREATION DATE: 11/1/99
*LAST MODIFIED: 16/2/99
*PURPOSE:
* Returns the base Number of the base closest and is 
* either neutral or allied to the tank and inside the 
* range. If no base is inside the range it returns 
* BASE_NOT_FOUND.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Map Location of the tank
*  yValue - Y Map Location of the tank
*********************************************************/
BYTE basesGetClosest(struct GameSim *sim, WORLD tankX, WORLD tankY);

/* Explicit-player variant of basesGetClosest (evaluates for the given player
 * rather than sim->viewPlayer). `range` is the selection ceiling in world
 * units: only a base strictly nearer than `range` is returned. The client
 * display path passes BASE_STATUS_RANGE; the server stock-send gating passes a
 * slightly wider ceiling so a base's stock is revealed before the client's
 * display logic switches to it. */
BYTE basesGetClosestForPlayer(struct GameSim *sim, BYTE player, WORLD tankX, WORLD tankY, WORLD range);

/* Per-base counterpart of basesGetClosestForPlayer's filter + distance test:
 * returns true iff baseIdx is a real base owned NEUTRAL or allied to `player`
 * and its centre lies strictly within `range` world units of (tankX, tankY).
 * The server stock cull gates each base's shells/mines on this so every
 * neutral/allied base within send range keeps real ammo, not just the closest. */
bool basesBaseInStockRange(struct GameSim *sim, BYTE player, WORLD tankX, WORLD tankY,
                           BYTE baseIdx, WORLD range);

/*********************************************************
*NAME:          basesGetStats
*AUTHOR:        John Morrison
*CREATION DATE: 12/1/99
*LAST MODIFIED: 12/1/99
*PURPOSE:
* Gets the base statistics (armour, shells, mines) for
* a given base number
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  baseNum - The base number to get
*  shellsAmount  - Pointer to hold the shells amount
*  mines   - Pointer to hold the mines amount
*  armour  - Pointer to hold the armour amount
*********************************************************/
void basesGetStats(bases *value, BYTE baseNum, BYTE *shellsAmount, BYTE *mines, BYTE *armour);

/*********************************************************
*NAME:          basesDamagePos
*AUTHOR:        John Morrison
*CREATION DATE: 16/2/99
*LAST MODIFIED: 16/2/99
*PURPOSE:
* This base has been hit. Do some damage to it.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
void basesDamagePos(struct GameSim *sim, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          basesCanHit
*AUTHOR:        John Morrison
*CREATION DATE: 16/2/99
*LAST MODIFIED: 16/2/99
*PURPOSE:
*  Returns whether the bases at the particular location
*  can be hit by a shell from a particular owner
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*  hitBy  - Person who fired the shell
*********************************************************/
bool basesCanHit(struct GameSim *sim, BYTE xValue, BYTE yValue, BYTE hitBy);

/*********************************************************
*NAME:          basesCantDrive
*AUTHOR:        Minhiriath
*CREATION DATE: 29/12/2008
*LAST MODIFIED: 29/12/2008
*PURPOSE:
*  Returns whether the bases at the particular location
*  can be driven over by an object owned by the player lgm, or tank.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*  hitBy  - Person who fired the shell
*********************************************************/
bool basesCantDrive(struct GameSim *sim, BYTE xValue, BYTE yValue, BYTE hitBy);

/*********************************************************
*NAME:          basesGetBaseOwner
*AUTHOR:        John Morrison
*CREATION DATE: 16/2/99
*LAST MODIFIED: 16/2/99
*PURPOSE:
* Returns the owner of a base 
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  baseNum - The base number to check
*********************************************************/
BYTE basesGetBaseOwner(bases *value, BYTE baseNum);

/*********************************************************
*NAME:          basesGetOwnerPos
*AUTHOR:        John Morrison
*CREATION DATE: 16/2/99
*LAST MODIFIED: 16/2/99
*PURPOSE:
* Returns the owner of a base at the position.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
BYTE basesGetOwnerPos(bases *value, BYTE xValue, BYTE yValue);


/*********************************************************
*NAME:          basesGetNumNeutral
*AUTHOR:        John Morrison
*CREATION DATE: 21/2/99
*LAST MODIFIED: 21/2/99
*PURPOSE:
* Returns the number of neutral bases
*
*ARGUMENTS:
*  value - Pointer to the bases structure
*********************************************************/
BYTE basesGetNumNeutral(bases *value);

/*********************************************************
*NAME:          basesSetBaseNetData
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED: 27/2/99
*PURPOSE:
* Sets the base data to buff.
*
*ARGUMENTS:
*  value - Pointer to the bases structure
*  buff  - Buffer of data to set base structure to
*  len   - Length of the data
*********************************************************/
void basesSetBaseNetData(bases *value, BYTE *buff, int len);

/*********************************************************
*NAME:          basesSetBaseNetData
*AUTHOR:        John Morrison
*CREATION DATE: 27/02/99
*LAST MODIFIED: 24/07/04
*PURPOSE:
* Gets a copy of the base data and copies it to buff.
* Returns the data length
*
*ARGUMENTS:
*  value - Pointer to the bases structure
*  buff  - Buffer to hold copy of bases data
*********************************************************/
BYTE basesGetBaseNetData(bases *value, BYTE *buff);

/*********************************************************
*NAME:          basesNetGiveArmour
*AUTHOR:        John Morrison
*CREATION DATE: 9/3/99
*LAST MODIFIED: 9/3/99
*PURPOSE:
* A network call has been made that some one is refueling
* armour from a base. Remove it and update the screen here
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  baseNum - Basenum it is happening to
*********************************************************/
void basesNetGiveArmour(bases *bs, BYTE baseNum);

/*********************************************************
*NAME:          basesNetGiveShells
*AUTHOR:        John Morrison
*CREATION DATE: 9/3/99
*LAST MODIFIED: 9/3/99
*PURPOSE:
* A network call has been made that some one is refueling
* shells from a base. Remove it and update the screen here
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  baseNum - Basenum it is happening to
*********************************************************/
void basesNetGiveShells(bases *value, BYTE baseNum);

/*********************************************************
*NAME:          basesNetGiveMines
*AUTHOR:        John Morrison
*CREATION DATE: 9/3/99
*LAST MODIFIED: 9/3/99
*PURPOSE:
* A network call has been made that some one is refueling
* mines from a base. Remove it and update the screen here
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  baseNum - Basenum it is happening to
*********************************************************/
void basesNetGiveMines(bases *value, BYTE baseNum);

/*********************************************************
*NAME:          basesSetNeutralOwner
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED: 1/11/99
*PURPOSE:
*  Changes all bases of owner owner back to neutral
*
*ARGUMENTS:
*  value - Pointer to the bases structure
*  owner - Owner to set back to neutral
*********************************************************/
void basesSetNeutralOwner(struct GameSim *sim, BYTE owner);

/*********************************************************
*NAME:          basesMigrate
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED: 1/11/99
*PURPOSE:
*  Causes all bases owned by owner  to migrate to a new 
* owner because its old owner left the game.
*
*ARGUMENTS:
*  value    - Pointer to the bases structure
*  oldOwner - Old Owner to remove
*  newOwner - Owner to replace with
*********************************************************/
void basesMigrate(struct GameSim *sim, BYTE oldOwner, BYTE newOwner);

/*********************************************************
*NAME:          basesServerRefuel
*AUTHOR:        John Morrison
*CREATION DATE: 9/11/99
*LAST MODIFIED: 9/11/99
*PURPOSE:
*  The server has issued a refuel command to a base
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  baseNum - The base number to refuel
*  addAmount - Amount of items to add
*********************************************************/
void basesServerRefuel(struct GameSim *sim, BYTE baseNum, BYTE addAmount);

/*********************************************************
*NAME:          baseIsCapturable
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Returns whether a base can be captured at a specific
*  location. Returns FALSE if it doesn't exist at location
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
bool baseIsCapturable(bases *value, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          basesGetBrainBaseItem
*AUTHOR:        John Morrison
*CREATION DATE: 25/11/99
*LAST MODIFIED: 25/11/99
*PURPOSE:
*  Returns the base info required by a brain for a 
*  specific base.
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  baseNum - Base Number to get info for
*  wx      - Bases world X Location
*  wy      - Bases world Y Location
*  info    - Bases alliance
*  shells  - Amount of shells in the base
*  mines   - Amount of mines in the base
*  armour  - Amount of armour in the base
*********************************************************/
void basesGetBrainBaseItem(struct GameSim *sim, BYTE baseNum, WORLD *wx, WORLD *wy, BYTE *info, BYTE *shell, BYTE *mines, BYTE *armour);

/*********************************************************
*NAME:          basesGetBrainBaseInRect
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/99
*LAST MODIFIED: 28/11/99
*PURPOSE:
*  Makes the brain base info for each base inside the
*  rectangle formed by the function parameters
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  leftPos   - Left position of rectangle
*  rightPos  - Right position of rectangle
*  topPos    - Top position of rectangle
*  bottomPos - Bottom position of rectangle
*********************************************************/
void basesGetBrainBaseInRect(struct ClientSim *cs, struct GameSim *sim, BYTE leftPos, BYTE rightPos, BYTE topPos, BYTE bottomPos);

/*********************************************************
*NAME:          basesGetMaxs
*AUTHOR:        John Morrison
*CREATION DATE: 13/6/00
*LAST MODIFIED: 13/6/00
*PURPOSE:
*  Gets the maximum positions values of all the bases.
*  eg left most, right most etc.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  left   - Pointer to hold the left most value
*  right  - Pointer to hold the right most value
*  top    - Pointer to hold the top most value
*  bottom - Pointer to hold the bottom most value
*********************************************************/
void basesGetMaxs(bases *value, int *leftPos, int *rightPos, int *topPos, int *bottomPos);

/*********************************************************
*NAME:          basesMoveAll
*AUTHOR:        John Morrison
*CREATION DATE: 13/6/00
*LAST MODIFIED: 13/6/00
*PURPOSE:
*  Repositions the bases by moveX, moveY
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  moveX  - The X move amount 
*  moveY  - The Y move amount 
*********************************************************/
void basesMoveAll(bases *value, int moveX, int moveY);

/*********************************************************
*NAME:          basesGetOwnerBitMask
*AUTHOR:        John Morrison
*CREATION DATE: 22/6/00
*LAST MODIFIED: 22/6/00
*PURPOSE:
* Returns the owner bitmask for all the bases own by a 
* player.
*
*ARGUMENTS:
*  value - Pointer to the pills structure
*  owner - Owner to return pills owned for
*  moveY  - The Y move amount 
*********************************************************/
PlayerBitMap basesGetOwnerBitMask(bases *value, BYTE owner);

/*********************************************************
*NAME:          basesClearMines
*AUTHOR:        John Morrison
*CREATION DATE: 31/7/00
*LAST MODIFIED: 31/7/00
*PURPOSE:
* Removes mines from under bases.
*
*ARGUMENTS:
*  value  - Pointer to the pills structure
*  mp     - Pointer to the map structure
*********************************************************/
void basesClearMines(struct GameSim *sim);

/*********************************************************
*NAME:          basesGetNumberOwnedByPlayer
*AUTHOR:        John Morrison
*CREATION DATE: 19/11/03
*LAST MODIFIED: 19/11/03
*PURPOSE:
* Returns the number of bases owned by this player
*
*ARGUMENTS:
*  value     - Pointer to the bases structure
*  playerNum - Player number for bases
*********************************************************/
BYTE basesGetNumberOwnedByPlayer(bases *value, BYTE playerNum);

/*********************************************************
*NAME:          basesHalfTickCalulator
*AUTHOR:        Minhiriath
*CREATION DATE: 1/3/2009
*LAST MODIFIED: 3/3/2009
*PURPOSE:
* calculate what to return to average out half ticks
*
*ARGUMENTS:
*  typeSelector - tells us what type of number to return.
*********************************************************/
int basesHalfTickCalulator(int typeSelector);

void basesSetBaseCompressData(bases *value, BYTE *buff, int dataLen);

#endif /* BASES_H */
