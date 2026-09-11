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
*Name:          Pillbox
*Filename:      pillbox.h
*Author:        John Morrison
*Creation Date: 28/10/98
*Last Modified: 27/07/04
*Purpose:
*  Provides operations on pillbox and pillboxes
*********************************************************/

#ifndef PILLBOX_H
#define PILLBOX_H


/* Includes */
#include <assert.h>

#include "global.h"
#include "alliance_enums.h"
#include "types.h"
#include "tank.h"
#include "shells.h"

/* Forward declaration for players pointer type (defined in players.h) */
struct playersObj;
struct GameSim;
struct ClientSim;

#define PILLBOX_ATTACK_NORMAL 100
#define PILLBOX_COOLDOWN_TIME 32
#define PILLBOX_MAX_FIRERATE 6 /* 6 */

#define PILLS_MAX_ARMOUR 15

/* Pill armour (0..PILLS_MAX_ARMOUR) and the inTank flag share one wire byte:
 * armour in the low nibble, inTank in bit 4. */
static inline uint8_t pillPackArmourInTank(uint8_t armour, bool inTank) {
    assert(armour <= PILLS_MAX_ARMOUR);
    return (uint8_t)((armour & 0x0F) | (inTank ? 0x10 : 0x00));
}
static inline uint8_t pillArmourFromByte(uint8_t b) { return (uint8_t)(b & 0x0F); }
static inline bool    pillInTankFromByte(uint8_t b) { return (b & 0x10) != 0; }

/* Bit 5 of the same byte: the x/y sent with this pill are its square right
 * now. Clear means the recipient was not told — the square is the last one it
 * was given, which may be where the pill used to be. */
#define PILL_POS_CURRENT 0x20
static inline uint8_t pillSetPosCurrent(uint8_t b, bool current) {
    return (uint8_t)(current ? (b | PILL_POS_CURRENT)
                             : (b & (uint8_t)~PILL_POS_CURRENT));
}
static inline bool    pillPosCurrentFromByte(uint8_t b) {
    return (b & PILL_POS_CURRENT) != 0;
}

/* Values in pillsObj::posStale. Confirmed means the server has just told us
 * this is where the pill is. Remembered means it has not, but the square is
 * still the last one we were given and a pill that has not been picked up
 * cannot have left it. Moved means we watched it go into a tank and come out
 * again without ever being told where, so the square means nothing. */
#define PILL_SQUARE_CONFIRMED  0
#define PILL_SQUARE_REMEMBERED 1
#define PILL_SQUARE_MOVED      2

/* A pillbox range is 8 map squares or 2048 world units */
#define PILLBOX_RANGE 2048

/* A pillbox fires 9 map squares this value is 8.5 becuase winbolo targets from the center of the pillbox*/
#define PILLBOX_FIRE_DISTANCE 8.5

/* Pillbox not found return Value */
#define PILL_NOT_FOUND 254

/* Maximum amount of health a pillbox can have */
#define PILL_MAX_HEALTH 15

/* A pillbox has to be within 9 squares of a base to get angry if it is shot */
#define PILL_BASE_HIT_LEFT -9
#define PILL_BASE_HIT_RIGHT 9
#define PILL_BASE_HIT_TOP -9
#define PILL_BASE_HIT_BOTTOM 9

/* Amount of damage each tree unit repairs */
#define PILL_REPAIR_AMOUNT 4

/* The maximum amount of time we will iterate to aim a shell to hit a tank */
/* Added to fix the rare tank run away bug so the shell will never hit the tank */
#define MAX_AIM_ITERATE 200

/* Brain stuff */
/* Bases Brain stuff */
#define PILLS_BRAIN_FRIENDLY 0
#define PILLS_BRAIN_NEUTRAL 2
#define PILLS_BRAIN_HOSTILE 1
#define PILLS_BRAIN_OBJECT_TYPE 2


/* Typedefs */

/* Prototypes */

/*********************************************************
*NAME:          pillsCreate
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Creates and initilises the pillboxes structure.
*  Sets number of pillboxes to zero
*
*ARGUMENTS:
*  value - Pointer to the map file
*********************************************************/
void pillsCreate(pillboxes *value);

/*********************************************************
*NAME:          pillsDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Destroys the pills data structure. Also frees memory.
*
*ARGUMENTS:
*  value - Pointer to the pills structure
*********************************************************/
void pillsDestroy(pillboxes *value);

/*********************************************************
*NAME:          pillsSetNumPills
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets the number of pills in the structure 
*
*ARGUMENTS:
*  value    - Pointer to the pillbox structure
*  numPills - The number of pills
*********************************************************/
void pillsSetNumPills(pillboxes *value, BYTE numPills);

/*********************************************************
*NAME:          pillsGetNumPills
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Gets the number of pills in the structure
*
*ARGUMENTS:
*  value    - Pointer to the pillbox structure
*********************************************************/
BYTE pillsGetNumPills(pillboxes *value);

/*********************************************************
*NAME:          pillsSetPill
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets a specific pill with its item data
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  item    - Pointer to a pillbox
*  pillNum - The pillbox number
*********************************************************/
void pillsSetPill(pillboxes *value, pillbox *item, BYTE pillNum);

/*********************************************************
*NAME:          pillsAddItem
*AUTHOR:        John Morrison
*CREATION DATE: 11/9/26
*LAST MODIFIED: 11/9/26
*PURPOSE:
*  Puts a pillbox into the list and returns its number in
*  outPillNum. The lowest removed slot is reused; when
*  every slot in the count is live the list is extended and
*  the count raised. Returns FALSE with outPillNum
*  untouched when all MAX_PILLS pills are live.
*
*ARGUMENTS:
*  value      - Pointer to the pillbox structure
*  item       - The pillbox to store
*  outPillNum - Receives the pillbox number, 1 based
*********************************************************/
bool pillsAddItem(pillboxes *value, const pillbox *item, BYTE *outPillNum);

/*********************************************************
*NAME:          pillsInstallItem
*AUTHOR:        John Morrison
*CREATION DATE: 12/9/26
*LAST MODIFIED: 12/9/26
*PURPOSE:
*  Writes a pillbox at the number it is given and marks that
*  slot live, whatever the slot held before. A number past
*  the count raises the count to cover it and leaves every
*  slot the gap opens up removed: a number arrives from a
*  list that has already filled it, so the gap is the set of
*  pillboxes this list has not been told about. Returns
*  FALSE for number 0 or a number past MAX_PILLS.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  item    - The pillbox to store
*  pillNum - The pillbox number, 1 based
*********************************************************/
bool pillsInstallItem(pillboxes *value, const pillbox *item, BYTE pillNum);

/*********************************************************
*NAME:          pillsRemoveItem
*AUTHOR:        John Morrison
*CREATION DATE: 11/9/26
*LAST MODIFIED: 11/9/26
*PURPOSE:
*  Clears a pillbox's live flag. The slot, the count and
*  every pillbox number above it are left alone, so the
*  numbers the wire and the recordings use keep meaning the
*  same pillbox. Returns FALSE for a number out of range or
*  one already removed.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number, 1 based
*********************************************************/
bool pillsRemoveItem(pillboxes *value, BYTE pillNum);

/*********************************************************
*NAME:          pillsIsActive
*AUTHOR:        John Morrison
*CREATION DATE: 11/9/26
*LAST MODIFIED: 11/9/26
*PURPOSE:
*  Returns whether a pillbox number names a pillbox that is
*  on the map. A removed pillbox keeps its slot and its
*  number, so a number in range is not on its own enough.
*  A number out of range returns FALSE.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number, 1 based
*********************************************************/
bool pillsIsActive(pillboxes *value, BYTE pillNum);

/*********************************************************
*NAME:          pillsSetActive
*AUTHOR:        John Morrison
*CREATION DATE: 12/9/26
*LAST MODIFIED: 12/9/26
*PURPOSE:
*  Puts a pillbox on the map or takes it off it, leaving its
*  record alone either way. The flag is all that moves, so a
*  pillbox put back is the one the slot already held. Returns
*  FALSE for a number out of range.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number, 1 based
*  onMap   - TRUE for on the map, FALSE for off it
*********************************************************/
bool pillsSetActive(pillboxes *value, BYTE pillNum, bool onMap);

/*********************************************************
*NAME:          pillsGetPill
*AUTHOR:        John Morrison
*CREATION DATE: 9/2/99
*LAST MODIFIED: 9/2/99
*PURPOSE:
*  Gets a specific pill.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  item    - Pointer to a pillbox
*  pillNum - The pillbox number
*********************************************************/
void pillsGetPill(pillboxes *value, pillbox *item, BYTE pillNum);

/*********************************************************
*NAME:          pillsExistPos
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Returns whether a pillbox exist at a specific location
*  A pill whose square this client has not been told is
*  current does not count as being there, so movement, turn
*  rate and shell collision never meet a pill that is only
*  remembered. pillsViewExistPos is the variant the view
*  builder draws from.
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
bool pillsExistPos(pillboxes *value, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          pillsGetAllianceNum
*AUTHOR:        John Morrison
*CREATION DATE: 21/12/98
*LAST MODIFIED: 21/12/98
*PURPOSE:
*  Returns the alliance type of a pillbox for drawing
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - Pillbox Number to get 
*********************************************************/
pillAlliance pillsGetAllianceNum(struct GameSim *sim, pillboxes *value, BYTE pillNum);

/*********************************************************
*NAME:          pillsUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 26/12/98
*LAST MODIFIED: 17/1/99
*PURPOSE:
*  Updates each pillboxs reload time and fires if the
*  tank is in range
*
*ARGUMENTS:
*  value     - Pointer to the pillbox structure
*  mp        - Pointer to the map structure
*  bs        - Pointer to the bases structure
*  tanks     - Array of tank pointers (one per player slot)
*  connected - Which player slots are active
*  plyrs     - Players structure (for alliance checks)
*  numTanks  - Number of player slots
*  shs       - Shells Structure
*********************************************************/
void pillsUpdate(struct GameSim *sim, tank tanks[], bool *connected, BYTE numTanks);

/*********************************************************
*NAME:          pillsIsPillHit
*AUTHOR:        John Morrison
*CREATION DATE: 30/10/98
*LAST MODIFIED: 19/3/98
*PURPOSE:
*  Returns whether a pillbox is hit at a specific location
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
bool pillsIsPillHit(pillboxes *value, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          pillsDamagePos
*AUTHOR:        John Morrison
*CREATION DATE: 18/3/98
*LAST MODIFIED: 24/4/01
*PURPOSE:
*  Does damage to a pillbox at xValue, yValue.
*  Returns if the pill is dead
*
*ARGUMENTS:
*  value      - Pointer to the pillbox structure
*  xValue     - X Location
*  yValue     - Y Location
*  wantDamage - TRUE if we just want to do damage to it
*  wantAngry  - TRUE if we just want to make it angry
*********************************************************/
bool pillsDamagePos(struct GameSim *sim, BYTE xValue, BYTE yValue, bool wantDamage, bool wantAngry, BYTE owner);

/*********************************************************
*NAME:          pillsGetScreenHealth
*AUTHOR:        John Morrison
*CREATION DATE: 30/10/98
*LAST MODIFIED: 30/10/98
*PURPOSE:
*  Returns the health of a pillbox as a screen define.
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
BYTE pillsGetScreenHealth(struct GameSim *sim, pillboxes *value, BYTE xValue, BYTE yValue, BYTE viewPlayer);

/*********************************************************
*NAME:          pillsTargetTank
*AUTHOR:        John Morrison
*CREATION DATE:  1/12/98
*LAST MODIFIED:  1/1/99
*PURPOSE:
*  Returns the angle need to fire to hit a tank
*
*ARGUMENTS:
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pills structure
*  bs     - Pointer to the bases structure
*  xValue - X Location of pillbox
*  yValue - Y Location of pillbox
*  tankX  - X Location of tank
*  tankY  - Y Location of tank
*  angle  - Angle of the tank
*  speed  - The speed of the tank
*  onBoat - Is the tank on a boat
*********************************************************/
TURNTYPE pillsTargetTank(struct GameSim *sim, map *mp, pillboxes *pb, bases *bs, WORLD xValue, WORLD yValue, WORLD tankX, WORLD tankY, TURNTYPE angle, BYTE speed, bool onBoat, BYTE boatExitSpeed);

/*********************************************************
*NAME:          pillsTargetTankMove
*AUTHOR:        John Morrison
*CREATION DATE: 31/12/98
*LAST MODIFIED: 17/1/99
*PURPOSE:
*  Returns the angle need to fire to hit a moving tank
*
*ARGUMENTS:
*  mp     - Pointer to the map structyre
*  pb     - Pointer to the pillbox structure
*  xValue - X Location of pillbox
*  yValue - Y Location of pillbox
*  tankX  - X Location of tank
*  tankY  - Y Location of tank
*  angle  - Angle of the tank
*  speed  - The speed of the tank
*  onBoat - Is the tank on a boat
*********************************************************/
TURNTYPE pillsTargetTankMove(struct GameSim *sim, map *mp, pillboxes *pb, bases *bs, WORLD xValue, WORLD yValue, WORLD tankX, WORLD tankY, TURNTYPE angle, BYTE speed, bool onBoat, BYTE boatExitSpeed);

/*********************************************************
*NAME:          pillsDeadPos
*AUTHOR:        John Morrison
*CREATION DATE: 15/1/99
*LAST MODIFIED: 15/1/99
*PURPOSE:
*  Returns whether a pillbox a specific location is dead
*  or not. A pill whose square this client has not been
*  told is current does not count as being there. Every
*  caller is gameplay, so there is no view variant.
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  xValue - X Location of pillbox
*  yValue - Y Location of pillbox
*********************************************************/
bool pillsDeadPos(pillboxes *value, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          pillsGetPillNum
*AUTHOR:        John Morrison
*CREATION DATE: 15/1/99
*LAST MODIFIED: 14/9/00
*PURPOSE:
*  Returns the pill number of a pillbox at that location
*  If not found returns PILL_NOT_FOUND
*  A pill whose square this client has not been told is
*  current is passed over. pillsGetViewPillNum is the
*  variant the camera and the displays use.
*
*ARGUMENTS:
*  value      - Pointer to the pillbox structure
*  xValue     - X Location of pillbox
*  yValue     - Y Location of pillbox
*  careInTank - Whether we are about the in tank state
*  inTank     - The intank state to check if we care
*********************************************************/
BYTE pillsGetPillNum(pillboxes *value, BYTE xValue, BYTE yValue, bool careInTank, bool inTank);

/*********************************************************
*NAME:          pillsGetViewPillNum
*AUTHOR:        John Morrison
*CREATION DATE: 5/9/26
*LAST MODIFIED: 5/9/26
*PURPOSE:
*  pillsGetPillNum without the position-current filter:
*  which pill this client last saw at that square, whether
*  or not the server has confirmed it is still there. The
*  camera and the displays use this so a pill you have lost
*  sight of stays selectable and stays drawn where you last
*  saw it.
*
*ARGUMENTS:
*  value      - Pointer to the pillbox structure
*  xValue     - X Location of pillbox
*  yValue     - Y Location of pillbox
*  careInTank - Whether we are about the in tank state
*  inTank     - The intank state to check if we care
*********************************************************/
BYTE pillsGetViewPillNum(pillboxes *value, BYTE xValue, BYTE yValue, bool careInTank, bool inTank);

/*********************************************************
*NAME:          pillsViewExistPos
*AUTHOR:        John Morrison
*CREATION DATE: 5/9/26
*LAST MODIFIED: 5/9/26
*PURPOSE:
*  pillsExistPos for the view: is there a pill here as far
*  as this client last saw. A pill you have lost sight of
*  stays on screen at the square you last saw it on rather
*  than the ground underneath showing through, but one you
*  watched get carried off — PILL_SQUARE_MOVED — is not
*  drawn anywhere until its real square arrives.
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
bool pillsViewExistPos(pillboxes *value, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          pillsSetPosState
*AUTHOR:        John Morrison
*CREATION DATE: 5/9/26
*LAST MODIFIED: 5/9/26
*PURPOSE:
*  Records what this client knows about a pill's square:
*  one of PILL_SQUARE_CONFIRMED, PILL_SQUARE_REMEMBERED or
*  PILL_SQUARE_MOVED. Only a client ever sets this; every
*  square in the server's own list is confirmed.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - Pillbox index, 0 based
*  state   - One of the PILL_SQUARE_ values
*********************************************************/
void pillsSetPosState(pillboxes *value, BYTE pillNum, BYTE state);

/*********************************************************
*NAME:          pillsGetPosState
*AUTHOR:        John Morrison
*CREATION DATE: 5/9/26
*LAST MODIFIED: 5/9/26
*PURPOSE:
*  Returns what this client knows about a pill's square,
*  as one of the PILL_SQUARE_ values.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - Pillbox index, 0 based
*********************************************************/
BYTE pillsGetPosState(pillboxes *value, BYTE pillNum);

/*********************************************************
*NAME:          pillsUpdatePosState
*AUTHOR:        John Morrison
*CREATION DATE: 5/9/26
*LAST MODIFIED: 5/9/26
*PURPOSE:
*  Folds one arriving pill update into that pill's square
*  state. Call it BEFORE writing the new inTank, so it can
*  see the flag the client held: a pill that was in a tank
*  and is not any more, with no square sent, was put down
*  somewhere this client was never told about.
*
*ARGUMENTS:
*  value      - Pointer to the pillbox structure
*  pillNum    - Pillbox index, 0 based
*  posCurrent - The position-current bit off the wire
*  nowInTank  - The in-tank flag that arrived
*********************************************************/
void pillsUpdatePosState(pillboxes *value, BYTE pillNum, bool posCurrent,
                         bool nowInTank);

/*********************************************************
*NAME:          pillsIsPosStale
*AUTHOR:        John Morrison
*CREATION DATE: 5/9/26
*LAST MODIFIED: 5/9/26
*PURPOSE:
*  Returns whether a pill's square is one this client has
*  not been told is current — remembered or moved, as
*  against confirmed.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - Pillbox index, 0 based
*********************************************************/
bool pillsIsPosStale(pillboxes *value, BYTE pillNum);

/*********************************************************
*NAME:          pillsSetPillInTank
*AUTHOR:        John Morrison
*CREATION DATE: 15/1/99
*LAST MODIFIED: 15/1/99
*PURPOSE:
*  Returns the pill number of a pillbox at that location
*  If not found returns PILL_NOT_FOUND
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number to apply it to
*  inTank  - Is it in tank or not
*********************************************************/
void pillsSetPillInTank(pillboxes *value, BYTE pillNum, bool inTank);

/*********************************************************
*NAME:          pillsGetPillOwner
*AUTHOR:        John Morrison
*CREATION DATE: 15/1/99
*LAST MODIFIED: 15/1/99
*PURPOSE:
*  Returns the owner of the pill.
*  If not found returns PILL_NOT_FOUND
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number to apply it to
*********************************************************/
BYTE pillsGetPillOwner(pillboxes *value, BYTE pillNum);

/*********************************************************
*NAME:          pillsSetPillOwner
*AUTHOR:        John Morrison
*CREATION DATE: 15/1/99
*LAST MODIFIED: 16/2/99
*PURPOSE:
* Sets the pillbox pillNum to owner. Returns the previous
* owner. If migrate is set to TRUE then it has migrated 
* from a alliance when a player left and we shouldn't 
* make a message
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number to apply it to
*  owner   - The new owner
*  migrate - TRUE if it is migrating.
*********************************************************/
BYTE pillsSetPillOwner(struct GameSim *sim, pillboxes *value, BYTE pillNum, BYTE owner, bool migrate);

/*********************************************************
*NAME:          pillsGetDamagePos
*AUTHOR:        John Morrison
*CREATION DATE: 15/1/99
*LAST MODIFIED: 15/1/99
*PURPOSE:
*  Does the amount of damage to the pillbox at the give
*  location. Usually caused by explosions
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  xValue - X Location of pillbox
*  yValue - Y Location of pillbox
*  amount - Amount of damage done to the pillbox
*********************************************************/
void pillsGetDamagePos(struct GameSim *sim, pillboxes *value, BYTE xValue, BYTE yValue, BYTE amount);

/*********************************************************
*NAME:          pillsNumInRect
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 17/1/99
*PURPOSE:
*  Returns the number of hostile pillboxs inside the
*  given rectangle
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  leftPos   - Left of the rectangle
*  rightPos  - Right of the rectangle
*  top    - Top of the rectangle
*  bottom - Bottom of the rectangle
*********************************************************/
BYTE pillsNumInRect(struct GameSim *sim, pillboxes *value, BYTE leftPos, BYTE rightPos, BYTE top, BYTE bottom);

/*********************************************************
*NAME:          pillsRepairPos
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 18/1/99
*PURPOSE:
*  Repair a pillbox at specific location
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  xValue - X Map position
*  yValue - Y Map position
*  treeAmount - Trees the man is carrying
*
*  Returns the trees that weren't needed, for the man to carry home.
*********************************************************/
BYTE pillsRepairPos(struct GameSim *sim, pillboxes *value, BYTE mx, BYTE my, BYTE treeAmount);

/*********************************************************
*NAME:          pillsGetArmourPos
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 18/1/99
*PURPOSE:
*  Returns the amount of armour a pillbox has at a specific
*  location. Returns PILL_NOT_FOUND if no pills exist at
*  that location
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  xValue - X Map position
*  yValue - Y Map position
*********************************************************/
BYTE pillsGetArmourPos(pillboxes *value, BYTE mx, BYTE my);

/*********************************************************
*NAME:          pillsGetNextView
*AUTHOR:        John Morrison
*CREATION DATE: 3/2/99
*LAST MODIFIED: 3/2/99
*PURPOSE:
* Returns the next allied (and alive) pill exists. If so
* then it puts its map co-ordinates into the parameters
* passed. If a previous pill is being used the the 
* parameter 'prev' is true and mx & my are set to the last
* pills location
*
*ARGUMENTS:
*  value    - Pointer to the pillbox structure
*  eligible - Bit per pill index: that pill may be selected
*  mx       - Pointer to hold X Map position (and prev)
*  my       - Pointer to hold Y Map position (and prev)
*  prev     - Whether a previos pill is being passed
*********************************************************/
bool pillsGetNextView(struct GameSim *sim, pillboxes *value, PlayerBitMap eligible, BYTE *mx, BYTE *my, bool prev);

/* Whether viewPlayer may look through pillbox pillIdx: the pill is allied to
 * them (own pills are allied to themselves), it still has armour, and it is
 * not being carried. FALSE for an index past the end of the array. This is
 * the one pill-view predicate — pillsCheckView below answers it for
 * sim->viewPlayer, callers wanting another player ask directly. */
bool pillsCanView(struct GameSim *sim, pillboxes *value, BYTE pillIdx, BYTE viewPlayer);

/*********************************************************
*NAME:          pillsCheckView
*AUTHOR:        John Morrison
*CREATION DATE: 3/2/99
*LAST MODIFIED: 3/2/99
*PURPOSE:
* We are currently in pillview with the pill at position
* mx and my. This function checks that it is still alive
* so we can continue viewing through it.
*
*ARGUMENTS:
*  value - Pointer to the pillbox structure
*  mx    - X Map position
*  my    - Y Map position
*********************************************************/
bool pillsCheckView(struct GameSim *sim, pillboxes *value, BYTE mx, BYTE my);

/*********************************************************
*NAME:          pillsBaseHit
*AUTHOR:        John Morrison
*CREATION DATE: 16/2/99
*LAST MODIFIED: 16/2/99
*PURPOSE:
* A base has been hit. Check to see if any pillboxes are 
* in range and are allied to the base to see if they should
* get angry
*
*ARGUMENTS:
*  value - Pointer to the pillbox structure
*  mx    - X location of the base that was hit
*  my    - Y location of the base that was hit
*  baseOwner - Owner of the base that was hit
*********************************************************/
void pillsBaseHit(struct GameSim *sim, pillboxes *value, BYTE mx, BYTE my, BYTE baseOwner);

/*********************************************************
*NAME:          pillsGetNumNeutral
*AUTHOR:        John Morrison
*CREATION DATE: 21/2/99
*LAST MODIFIED: 21/2/99
*PURPOSE:
* Returns the number of neutral pills
*
*ARGUMENTS:
*  value - Pointer to the pills structure
*********************************************************/
BYTE pillsGetNumNeutral(pillboxes *value);

/*********************************************************
*NAME:          pillsSetPillNetData
*AUTHOR:        John Morrison
*CREATION DATE: 27/02/99
*LAST MODIFIED: 27/07/04
*PURPOSE:
* Sets the pills data to buff.
*
*ARGUMENTS:
*  value   - Pointer to the pills structure
*  buff    - Buffer of data to set pills structure to
*  dataLen - Length of the data
*********************************************************/
void pillsSetPillNetData(pillboxes *value, BYTE *buff, BYTE dataLen);

/*********************************************************
*NAME:          pillsGetPillNetData
*AUTHOR:        John Morrison
*CREATION DATE: 27/02/99
*LAST MODIFIED: 24/07/04
*PURPOSE:
* Gets a copy of the pills data and copies it to buff.
*
*ARGUMENTS:
*  value - Pointer to the pills structure
*  buff  - Buffer to hold copy of pills data
*********************************************************/
BYTE pillsGetPillNetData(pillboxes *value, BYTE *buff);

/*********************************************************
*NAME:          pillsDropSetNeutralOwner
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED: 1/11/99
*PURPOSE:
*  Changes all pills of owner owner back to neutral. Also
*  if they are inside a tank it drops them.
*
*ARGUMENTS:
*  value - Pointer to the pillbox structure
*  owner - Owner to set back to neutral
*********************************************************/
void pillsDropSetNeutralOwner(struct GameSim *sim, BYTE owner);

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
void pillsMigrate(struct GameSim *sim, BYTE oldOwner, BYTE newOwner);

/*********************************************************
*NAME:          pillsMigratePlanted
*AUTHOR:        Minhiriath
*CREATION DATE: 14/03/2009
*LAST MODIFIED: 14/03/2009
*PURPOSE:
*  Causes all pills owned by owner  to migrate to a new 
* owner becuase its owner left alliance
*
*ARGUMENTS:
*  value    - Pointer to the bases structure
*  oldOwner - Old Owner to remove
*  newOwner - Owner to replace with
*********************************************************/
void pillsMigratePlanted(struct GameSim *sim, BYTE oldOwner, BYTE newOwner);

/*********************************************************
*NAME:          pillsExplicitDrop
*AUTHOR:        John Morrison
*CREATION DATE: 10/11/99
*LAST MODIFIED: 10/11/99
*PURPOSE:
*  Drops all pills of owner owner
*
*
*ARGUMENTS:
*  value - Pointer to the pillbox structure
*  owner - Owner to set back to neutral
*********************************************************/
void pillsExplicitDrop(struct GameSim *sim, BYTE owner);

/*********************************************************
*NAME:          pillsGetBrainPillsInRect
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/99
*LAST MODIFIED: 28/11/99
*PURPOSE:
*  Makes the brain pill info for each pill inside the
*  rectangle formed by the function parameters
*
*ARGUMENTS:
*  value  - Pointer to the pills structure
*  leftPos   - Left position of rectangle
*  rightPos  - Right position of rectangle
*  top    - Top position of rectangle
*  bottom - Bottom position of rectangle
*********************************************************/
void pillsGetBrainPillsInRect(struct ClientSim *cs, struct GameSim *sim, pillboxes *value, BYTE leftPos, BYTE rightPos, BYTE top, BYTE bottom);

/*********************************************************
*NAME:          pillsSetBrainView
*AUTHOR:        John Morrison
*CREATION DATE: 13/12/99
*LAST MODIFIED: 13/12/99
*PURPOSE:
*  Returns if we are allowed to view from this pillbox.
*  By allowed the pill must not be in a tank, must not be
*  dead and must be on the same alliance as us.
*
*ARGUMENTS:
*  value     - Pointer to the pills structure
*  pillNum   - The pill number we are requesting the view
*  playerNum - Our player number
*********************************************************/
bool pillsSetView(struct GameSim *sim, pillboxes *value, BYTE pillNum, BYTE playerNum);

/*********************************************************
*NAME:          pillsGetMaxs
*AUTHOR:        John Morrison
*CREATION DATE: 13/6/00
*LAST MODIFIED: 13/6/00
*PURPOSE:
*  Gets the maximum positions values of all the pillboxs.
*  eg left most, right most etc.
*
*ARGUMENTS:
*  value  - Pointer to the pills structure
*  leftPos   - Pointer to hold the left most value
*  rightPos  - Pointer to hold the right most value
*  top    - Pointer to hold the top most value
*  bottom - Pointer to hold the bottom most value
*********************************************************/
void pillsGetMaxs(pillboxes *value, int *leftPos, int *rightPos, int *top, int *bottom);

/*********************************************************
*NAME:          pillsMoveAll
*AUTHOR:        John Morrison
*CREATION DATE: 13/6/00
*LAST MODIFIED: 13/6/00
*PURPOSE:
*  Repositions the pillboxes by moveX, moveY
*
*ARGUMENTS:
*  value  - Pointer to the pills structure
*  moveX  - The X move amount 
*  moveY  - The Y move amount 
*********************************************************/
void pillsMoveAll(pillboxes *value, int moveX, int moveY);

/*********************************************************
*NAME:          pillsGetOwnerBitMask
*AUTHOR:        John Morrison
*CREATION DATE: 22/6/00
*LAST MODIFIED: 22/6/00
*PURPOSE:
* Returns the owner bitmask for all the pills own by a 
* player.
*
*ARGUMENTS:
*  value - Pointer to the pills structure
*  owner - Owner to return pills owned for
*  moveY  - The Y move amount 
*********************************************************/
PlayerBitMap pillsGetOwnerBitMask(pillboxes *value, BYTE owner);

/*********************************************************
*NAME:          pillsGetAttackSpeed
*AUTHOR:        John Morrison
*CREATION DATE: 29/7/00
*LAST MODIFIED: 29/7/00
*PURPOSE:
* Returns the pillboxes aattacking speed. Returns 
* PILLBOX_ATTACK_NORMAL if out of range.
*
*ARGUMENTS:
*  value - Pointer to the pills structure
*  pillNum - Pillbox number to get
*********************************************************/
BYTE pillsGetAttackSpeed(pillboxes *value, BYTE pillNum);

/*********************************************************
*NAME:          pillsIsInView
*AUTHOR:        John Morrison
*CREATION DATE: 29/7/00
*LAST MODIFIED: 29/7/00
*PURPOSE:
* Returns if a location is in view (range +/- 10 squares
* of a pill allied to playerNum
*
*ARGUMENTS:
*  value     - Pointer to the pills structure
*  playerNum - Player number for pill alliance
*  mx        - X Map location
*  my        - Y Map location
*********************************************************/
bool pillsIsInView(struct GameSim *sim, pillboxes *value, BYTE playerNum, BYTE mx, BYTE my);

/*********************************************************
*NAME:          pillsMoveView
*AUTHOR:        John Morrison
*CREATION DATE: 21/1/01
*LAST MODIFIED: 21/1/01
*PURPOSE:
* Allows players to scroll through their pills. Returns
* whether a pill was found in that direction. 
*
*ARGUMENTS:
*  value    - Pointer to the pillbox structure
*  eligible - Bit per pill index: that pill may be selected
*  mx       - Pointer to hold X Map position (and prev)
*  my       - Pointer to hold Y Map position (and prev)
*  xMove    - -1 for moving left, 1 for right, 0 for neither
*  yMove    - -1 for moving up, 1 for down, 0 for neither
*********************************************************/
bool pillsMoveView(struct GameSim *sim, pillboxes *value, PlayerBitMap eligible, BYTE *mx, BYTE *my, int xMove, int yMove);

/*********************************************************
*NAME:          pillsGetNumberOwnedByPlayer
*AUTHOR:        John Morrison
*CREATION DATE: 19/11/03
*LAST MODIFIED: 19/11/03
*PURPOSE:
* Returns the number of pillboxes owned by this player
*
*ARGUMENTS:
*  value     - Pointer to the pills structure
*  playerNum - Player number for pills
*********************************************************/
BYTE pillsGetNumberOwnedByPlayer(pillboxes *value, BYTE playerNum);

void pillsSetPillCompressData(pillboxes *value, BYTE *buff, int dataLen);
/* Clamps every pillbox field a map can supply. See basesValidate. */
void pillsValidate(pillboxes *value);


#endif /* PILLBOX_H */
