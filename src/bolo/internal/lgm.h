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
*Name:          lgm
*Filename:      lgm.h 
*Author:        John Morrison
*Creation Date: 17/01/99
*Last Modified: 02/02/04
*Purpose:
*  Operations on tanks LGM are handled by this module
*********************************************************/

#ifndef LGM_H
#define LGM_H


/* Includes */
#include "global.h"
#include "types.h"

struct GameSim;

#define LGM_TREE_REQUEST 0
#define LGM_ROAD_REQUEST 1
#define LGM_BUILDING_REQUEST 2
#define LGM_PILL_REQUEST 3
#define LGM_MINE_REQUEST 4
#define LGM_BOAT_REQUEST 5
/* Man is doing nothing */
#define LGM_IDLE 6

#define LGM_STATE_IDLE 0
#define LGM_STATE_GOING 1
#define LGM_STATE_RETURN 2

/* Why a build request would be turned down, as lgmRequestRefusal reports it.
   Only two answers because only two things are ever wrong: the square will
   not take the job, or the tank cannot pay for it. */
#define LGM_REFUSE_NONE 0
#define LGM_REFUSE_SQUARE 1
#define LGM_REFUSE_STOCK 2

#define LGM_COST_ROAD 2
#define LGM_COST_BUILDING 2
#define LGM_COST_REPAIRBUILDING 1
#define LGM_COST_BOAT 20
#define LGM_COST_PILLNEW 4
#define LGM_COST_PILLREPAIR 1
#define LGM_COST_MINE 1

/* Trees the man carries out to repair a pill. Each one is worth
   pill_repair_amount armour, and simRulesValidate holds the pair so this
   many always covers a pill on zero armour. Anything left over comes back
   in the tank. */
#define LGM_LOAD_PILLREPAIR 4

#define LGM_NO_PILL 37

#define LGM_GATHER_TREE 4

#define LGM_SIZE_X 3
#define LGM_SIZE_Y 4

/* There are 3 frames in animation 0-3 */
#define LGM_MAX_FRAMES 2

/* Blessed area surrounding the tank */
#define LGM_TANKBOAT_LEAVE 144
#define LGM_TANKBOAT_RETURN 160

/* Speed the helicpter flys at */
#define LGM_HELICOPTER_SPEED 3
/* The frame number for the helicopter */
#define LGM_HELICOPTER_FRAME 3

/* 20 ticks to builds something */
#define LGM_BUILD_TIME 20

/* Min and Max distance away from thing to achieve goal */
#define LGM_MIN_GOAL -16
#define LGM_MAX_GOAL 16

/* Min and Max distance away from return to the tank goal */
/* currently these are set to 16 * 5 which is about 80 world coordinates.
   however, the lgm should be able to return to the tank, from the tanks perimeter, so, we're changing this to 8
*/
#define LGM_RETURN_MIN_GOAL (-16 * 8)
#define LGM_RETURN_MAX_GOAL (16 * 8)

/* The LGM's Brain State */
#define LGM_BRAIN_INTANK 0
#define LGM_BRAIN_DEAD 1
#define LGM_BRAIN_MOVING 2 /* Otherwise */

/* LGM Obstructed state */
#define LGM_BRAIN_FREE 0
#define LGM_BRAIN_PARTIAL 1
#define LGM_BRAIN_TOTAL 2

typedef struct lgmObj *lgm;

struct lgmObj {
  WORLD x;         /* X and Y positions */
  WORLD y;     
  WORLD destX;      /* Where the man is heading */
  WORLD destY;      
  BYTE state;      /* Present state 0 = Idle, 1 = Going to do work, 2 = returning to tank */
  BYTE waitTime;   /* Is it counting down waiting for something? */
  bool inTank;     /* Is the builder in the tank */
  bool isDead;     /* Is the builder dead ? */
  BYTE numTrees;   /* Number of trees carrying */
  BYTE numMines;   /* Number of mines carrying */
  BYTE numPills;   /* Number of pills being carried */
  BYTE action;     /* What its current task is */
  BYTE nextX;      /* Map Co-ordinates of next action */
  BYTE nextY; 
  BYTE nextAction; /* Next action to do */
  BYTE frame;      /* Animation frame */
  BYTE blessX;     /* The lgm blessed X map square (can travel over it) */
  BYTE blessY;     /* The lgm blessed Y map square (can travel over it) */
  bool onTop;      /* did the lgm land ontop of a building/base/orpillbox? from a parachute?*/
  BYTE obstructed; /* For brain - 0 = free, 1 = touching wall, 2 = completely stuck */
  BYTE playerNum;  /* Our player Number */
};

/* Prototypes */

/*********************************************************
*NAME:          lgmCreate 
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 3/12/00
*PURPOSE:
*  Sets up the LGM structure
*
*ARGUMENTS:
*
*********************************************************/
lgm lgmCreate(BYTE playerNum);

/*********************************************************
*NAME:          lgmDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
* Destroys the LGM structure
*
*ARGUMENTS:
*  value - Pointer to the lgm sturcture
*********************************************************/
void lgmDestroy(lgm *value);

/*********************************************************
*NAME:          lgmUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Game tick has passed. Update the lgm position
*
*ARGUMENTS:
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillbox structure
*  bs     - Pointer to the base structure
*  tnk    - Pointer to the tank structure
*********************************************************/
void lgmUpdate(struct GameSim *sim, lgm *lgman, tank *tnk);

/*********************************************************
*NAME:          lgmAddRequest
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Adds a new request to the lgm structure
*
*ARGUMENTS:
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillbox structure
*  tnk    - Pointer to the tank structure
*  bs     - Pointer to the base structure
*  mapX   - X Co-ordinate of the new action
*  mapY   - Y Co-ordinate of the new action
*  action - What the new action is
*********************************************************/
void lgmAddRequest(struct GameSim *sim, lgm *lgman, tank *tnk, BYTE mapX, BYTE mapY, BYTE action);


/*********************************************************
*NAME:          lgmTankDied
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  The tank has died. Cancel all pending orders if any
*
*ARGUMENTS:
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillbox structure
*  bs     - Pointer to the base structure
*  tnk    - Pointer to the tank structure
*********************************************************/
void lgmTankDied(lgm *lgman);

/*********************************************************
*NAME:          lgmNewPrimaryRequest
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  A new primary request is wanted. Check if it is
*  possible. If so then make it so.
*
*ARGUMENTS:
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillbox structure
*  tnk    - Pointer to the tank structure
*  bs     - Pointer to the base structure
*  mapX   - X Co-ordinate of the new action
*  mapY   - Y Co-ordinate of the new action
*  action - What the new action is
*********************************************************/
void lgmNewPrimaryRequest(struct GameSim *sim, lgm *lgman, tank *tnk, BYTE mapX, BYTE mapY, BYTE action);

/*********************************************************
*NAME:          lgmRequestIsValid
*PURPOSE:
*  Returns whether a build request would be accepted right
*  now, without acting on it and without sending the player
*  an assistant message. For re-testing an order that was
*  commanded earlier against the current map.
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm structure
*  tnk    - Pointer to the tank structure
*  mapX   - X Co-ordinate of the request
*  mapY   - Y Co-ordinate of the request
*  action - What the request is (LGM_*_REQUEST)
*********************************************************/
bool lgmRequestIsValid(struct GameSim *sim, lgm *lgman, tank *tnk, BYTE mapX, BYTE mapY, BYTE action);

/*********************************************************
*NAME:          lgmRequestRefusal
*PURPOSE:
*  Why a build request would be turned down right now, or
*  LGM_REFUSE_NONE when it would be accepted. The same dry
*  run lgmRequestIsValid makes — that call is this one with
*  the reason thrown away — for a caller that has to tell a
*  square that will not take the job from a tank that cannot
*  pay for it.
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm structure
*  tnk    - Pointer to the tank structure
*  mapX   - X Co-ordinate of the request
*  mapY   - Y Co-ordinate of the request
*  action - What the request is (LGM_*_REQUEST)
*********************************************************/
BYTE lgmRequestRefusal(struct GameSim *sim, lgm *lgman, tank *tnk, BYTE mapX, BYTE mapY, BYTE action);

/*********************************************************
*NAME:          lgmRecall
*PURPOSE:
*  Turns the man round wherever he is and drops whatever
*  order was waiting behind the one in hand, so lgmReturn
*  walks him back to the tank from the next tick. The same
*  turn lgmMoveAway makes when it finds the way blocked.
*  The caller decides whether the man is out to be recalled.
*
*ARGUMENTS:
*  sim    - The game the man belongs to
*  lgman  - Pointer to the lgm structure
*********************************************************/
void lgmRecall(struct GameSim *sim, lgm *lgman);

/*********************************************************
*NAME:          lgmKill
*PURPOSE:
*  Kills the man where he stands: plays the dying sound,
*  drops the pillbox he was carrying on the nearest square
*  that will hold one, marks him dead in the helicopter
*  frame, points him at the tank and starts him flying in
*  from a random start. Records the loss, reports it to
*  WinBolo.net, credits owner with the kill when owner is a
*  player, and publishes the newswire event. The caller
*  decides whether the man dies; this is what dying does.
*
*ARGUMENTS:
*  sim    - The game the man belongs to
*  lgman  - Pointer to the lgm structure
*  tnk    - Pointer to the man's tank, or NULL when he has
*           none left to fly back to
*  owner  - Slot credited with the kill, NEUTRAL for a death
*           nobody caused
*********************************************************/
void lgmKill(struct GameSim *sim, lgm *lgman, tank *tnk, BYTE owner);

/*********************************************************
*NAME:          lgmSetCarried
*PURPOSE:
*  Writes what the man is carrying out to his job, capped at
*  the amounts a tank can hold, since everything he carries
*  came out of one and unloads back into one.
*
*ARGUMENTS:
*  sim    - The game whose rules the caps come from
*  lgman  - Pointer to the lgm structure
*  trees  - Trees he is to carry
*  mines  - Mines he is to carry
*********************************************************/
void lgmSetCarried(struct GameSim *sim, lgm *lgman, BYTE trees, BYTE mines);

/*********************************************************
*NAME:          lgmMoveAway
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Man is moving towrads his destination.
*
*ARGUMENTS:
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillbox structure
*  bs     - Pointer to the base structure
*  tnk    - Pointer to the tank structure
*********************************************************/
void lgmMoveAway(struct GameSim *sim, lgm *lgman, tank *tnk);

/*********************************************************
*NAME:          lgmReturn
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Man is moving towards the tank
*
*ARGUMENTS:
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillbox structure
*  bs     - Pointer to the base structure
*  tnk    - Pointer to the tank structure
*********************************************************/
void lgmReturn(struct GameSim *sim, lgm *lgman, tank *tnk);

/*********************************************************
*NAME:          lgmDoWork
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Man has arrived at objective. Do whatever it is he
*  came to do.
*
*ARGUMENTS:
*  mp - Pointer to the map structure
*  pb - Pointer to the pillbox structure 
*  bs - Pointer to the bases structure 
*********************************************************/
void lgmDoWork(struct GameSim *sim, lgm *lgman, tank *tnk);

/*********************************************************
*NAME:          lgmBackInTank
*AUTHOR:        John Morrison
*CREATION DATE: 18/01/99
*LAST MODIFIED: 01/02/03
*PURPOSE:
*  Man has arrived back in tank. Dump stuff off
*
*ARGUMENTS:
*  mp        - Pointer to the map structure
*  pb        - Pointer to the pillbox structure
*  bs        - Pointer to the base structure
*  tnk       - Pointer to the tank structure
*  sendItems - if TRUE, send the carriend items back 
*              to the client
*********************************************************/
void lgmBackInTank(struct GameSim *sim, lgm *lgman, tank *tnk, bool sendItems);

/*********************************************************
*NAME:          lgmOnScreen
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns wehther the man is on screen
*
*ARGUMENTS:
*  leftPos   - Left bounds of screen
*  rightPos  - Right bounds of screen
*  top    - Top bounds of screen
*  bottom - Bottom bounds of screen
*********************************************************/
bool lgmOnScreen(lgm *lgman, BYTE leftPos, BYTE rightPos, BYTE top, BYTE bottom);

/*********************************************************
*NAME:          lgmGetScreenCoords
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns wehther the man is on screen
*
*ARGUMENTS:
*  leftPos - Left bounds of screen
*  topPos  - Top bounds of screen
*  mx      - X Map co ord
*  my      - Y Map co ord
*  px      - X pixel co ord
*  py      - Y pixel co ord
*********************************************************/
void lgmGetScreenCoords(lgm *lgman, BYTE leftPos, BYTE topPos, BYTE *mx, BYTE *my, BYTE *px, BYTE *py, BYTE *frame);

/*********************************************************
*NAME:          lgmDeathCheck
*AUTHOR:        John Morrison
*CREATION DATE: 18/01/99
*LAST MODIFIED: 02/02/04
*PURPOSE:
*  Called when an item explodes to check to see if the
*  lgm should be killed because he is on the screen.
*
*ARGUMENTS:
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillboxes structure
*  bs     - Pointer to the bases structure
*  wx     - X World co ord
*  wy     - Y World co ord
*  owner  - Who owned the firing shell (NEUTRAL for mines)
*********************************************************/
void lgmDeathCheck(struct GameSim *sim, lgm *lgman, WORLD wx, WORLD wy, BYTE owner, tank *tnk);

/*********************************************************
*NAME:          lgmDeathCheckAtPosition
*AUTHOR:        John Morrison
*CREATION DATE: 17/04/26
*LAST MODIFIED: 17/04/26
*PURPOSE:
*  Like lgmDeathCheck but tests the explosion against a
*  supplied LGM world position (for lag compensation).
*  Death effects (pill drop, parachute) use the LGM's
*  real current position.
*
*ARGUMENTS:
*  sim    - Pointer to the game sim structure
*  lgman  - Pointer to the lgm pointer
*  lgmWorldX - LGM X world position to test against
*  lgmWorldY - LGM Y world position to test against
*  wx     - X World co ord of explosion
*  wy     - Y World co ord of explosion
*  owner  - Who owned the firing shell (NEUTRAL for mines)
*  tnk    - Pointer to the tank
*********************************************************/
void lgmDeathCheckAtPosition(struct GameSim *sim, lgm *lgman, WORLD lgmWorldX, WORLD lgmWorldY, WORLD wx, WORLD wy, BYTE owner, tank *tnk);

/*********************************************************
*NAME:          lgmParchutingIn
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Man is parachuting back in.
*
*ARGUMENTS:
*
*********************************************************/
void lgmParchutingIn(struct GameSim *sim, lgm *lgman);

/*********************************************************
*NAME:          lgmCheckRemove
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  An building operation has happened. Check to see if 
*  it should remove from grass/building data structures.
*
*ARGUMENTS:
*  terrain - Terrain type of the sqaure
*  mx      - Map X position 
*  my      - Map Y position
*********************************************************/
void lgmCheckRemove(struct GameSim *sim, BYTE terrain, BYTE mx, BYTE my);

/*********************************************************
*NAME:          lgmCheckTankBoat
*AUTHOR:        John Morrison
*CREATION DATE: 19/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  This is to check the builder is close to the tank. If 
*  he is then it is considered "blessed" and can freely
*  travel to it.
*
*ARGUMENTS:
*  tnk  - Pointer to the tank structure
*  dist - Distance to check
*********************************************************/
bool lgmCheckTankBoat(lgm *lgman, tank *tnk, WORLD dist);

/*********************************************************
*NAME:          lgmGetMX
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns the lgm map X position
*
*ARGUMENTS:
*
*********************************************************/
BYTE lgmGetMX(lgm *lgman);

/*********************************************************
*NAME:          lgmGetMY
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns the lgm map Y position
*
*ARGUMENTS:
*
*********************************************************/
BYTE lgmGetMY(lgm *lgman);

/*********************************************************
*NAME:          lgmGetPX
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns the lgm pixel X position
*
*ARGUMENTS:
*
*********************************************************/
BYTE lgmGetPX(lgm *lgman);

/*********************************************************
*NAME:          lgmGetPY
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns the lgm pixel Y position
*
*ARGUMENTS:
*
*********************************************************/
BYTE lgmGetPY(lgm *lgman);

/*********************************************************
*NAME:          lgmGetFrame
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns the lgm animation frame
*
*ARGUMENTS:
*
*********************************************************/
BYTE lgmGetFrame(lgm *lgman);

bool lgmIsOut(lgm *lgman);

/*********************************************************
*NAME:          lgmIsIdle
*PURPOSE:
*  Returns whether the man has no order in hand. This is
*  the test lgmAddRequest makes: an idle man acts on a new
*  order at once, a busy one has it queued as his next order
*  and checked when he gets back in the tank.
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm structure
*********************************************************/
bool lgmIsIdle(lgm *lgman);

/*********************************************************
*NAME:          lgmGetStatus
*AUTHOR:        John Morrison
*CREATION DATE: 14/11/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Gets the man status for drawing on the status bars
*
*ARGUMENTS:
*  tnk    - Pointer to this LGM's tank.
*  isOut  - TRUE if man is out of tank
*  isDead - TRUE if man is dead
*  angle  - Angle man is travelling on
*********************************************************/
void lgmGetStatus(lgm *lgman, tank *tnk, bool *isOut, bool *isDead, TURNTYPE *angle);

/*********************************************************
*NAME:          lgmGetWX
*AUTHOR:        John Morrison
*CREATION DATE: 25/11/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns lgm World X co-ord
*
*ARGUMENTS:
*
*********************************************************/
WORLD lgmGetWX(lgm *lgman);

/*********************************************************
*NAME:          lgmGetWY
*AUTHOR:        John Morrison
*CREATION DATE: 25/11/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns lgm World Y co-ord
*
*ARGUMENTS:
*
*********************************************************/
WORLD lgmGetWY(lgm *lgman);

/*********************************************************
*NAME:          lgmGetDir
*AUTHOR:        John Morrison
*CREATION DATE: 25/11/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns lgm direction
*
*ARGUMENTS:
*  tnk - Pointer to the tank object
*********************************************************/
BYTE lgmGetDir(lgm *lgman, tank *tnk);

/*********************************************************
*NAME:          lgmGetBrainState
*AUTHOR:        John Morrison
*CREATION DATE: 25/11/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns lgm's state required for brain.
*
*ARGUMENTS:
*
*********************************************************/
BYTE lgmGetBrainState(lgm *lgman);

/*********************************************************
*NAME:          lgmGetBrainObstructed
*AUTHOR:        John Morrison
*CREATION DATE: 25/11/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns lgm's obstructed state required for brain
*
*ARGUMENTS:
*
*********************************************************/
BYTE lgmGetBrainObstructed(lgm *lgman);

/*********************************************************
*NAME:          lgmSetBrainObstructed
*AUTHOR:        John Morrison
*CREATION DATE: 16/4/01
*LAST MODIFIED: 16/4/01
*PURPOSE:
*  Stores lgm's obstructed state required for brain
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  obstructed - The obstructed state to set
*********************************************************/
void lgmSetBrainObstructed(lgm *lgman, BYTE obstructed);

/*********************************************************
*NAME:          lgmPutWorld
*AUTHOR:        John Morrison
*CREATION DATE: 23/9/00
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns the lgm map X position
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  x      - World X Co-ordinate
*  y      - World Y Co-ordinate
*********************************************************/
void lgmPutWorld(lgm *lgmman, WORLD x, WORLD y, BYTE frame);

/*********************************************************
*NAME:          lgmNetBackInTank
*AUTHOR:        John Morrison
*CREATION DATE: 2/12/00
*LAST MODIFIED: 2/12/00
*PURPOSE:
*  Network message. LGM Back in tank
*
*ARGUMENTS:
*  lgman    - Pointer to the lgm sturcture
*  mp       - Pointer to the map structure
*  pb       - Pointer to the pillbox structure
*  bs       - Pointer to the base structure
*  tnk      - Pointer to the tank structure
*  numTrees - Amount of trees lgm is carrying
*  numMines - Amount of mines the lgm is carrying
*  pillNum  - Pillbox being carries
*********************************************************/
void lgmNetBackInTank(struct GameSim *sim, lgm *lgman, tank *tnk, BYTE numTrees, BYTE numMines, BYTE pillNum);

/*********************************************************
*NAME:          lgmNetManWorking
*AUTHOR:        John Morrison
*CREATION DATE: 01/02/03
*LAST MODIFIED: 01/02/03
*PURPOSE:
*  Network message. LGM out doing work 
*
*ARGUMENTS:
*  lgman    - Pointer to the lgm sturcture
*  tnk      - Pointer to the tank structure
*  mapX     - Bless X map position 
*  mapY     - Bless Y map position 
*  numTrees - Amount of trees lgm is carrying
*  numMines - Amount of mines the lgm is carrying
*  pillNum  - Pillbox being carries
*********************************************************/
void lgmNetManWorking(struct GameSim *sim, lgm *lgman, tank *tnk, BYTE mapX, BYTE mapY, BYTE numTrees, BYTE numMines, BYTE pillNum);

/*********************************************************
*NAME:          lgmSetPlayerNum
*AUTHOR:        John Morrison
*CREATION DATE: 3/12/00
*LAST MODIFIED: 3/12/00
*PURPOSE:
*  Sets the player number
*
*ARGUMENTS:
*  lgman     - Pointer to the lgm sturcture
*  playerNum - Player number to set to
*********************************************************/
void lgmSetPlayerNum(lgm *lgman, BYTE playerNum);

/*********************************************************
*NAME:          lgmSetIsDead
*AUTHOR:        John Morrison
*CREATION DATE: 28/12/00
*LAST MODIFIED: 28/12/00
*PURPOSE:
*  Sets if the lgm is dead of not
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  isDead - Flag whether the LGM is dead or not
*********************************************************/
void lgmSetIsDead(struct GameSim *sim, lgm *lgman, bool isDead);

/*********************************************************
*NAME:          lgmConnectionLost
*AUTHOR:        John Morrison
*CREATION DATE: 24/02/03
*LAST MODIFIED: 24/02/03
*PURPOSE:
* Called if our connection is lost from the server 
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  tnk    - Pointer to our tank structure
*********************************************************/
struct GameSim;
void lgmConnectionLost(struct GameSim *sim, lgm *lgman, tank *tnk, starts *sts);

#endif /* LGM_H */
