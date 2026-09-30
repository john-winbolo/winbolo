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
*Name:          Shells
*Filename:      shells.h
*Author:        John Morrison
*Creation Date: 25/12/98
*Last Modified:  23/9/00
*Purpose:
*  Responsable for Shells tracking/collision detect etc.
*********************************************************/

#ifndef SHELLS_H
#define SHELLS_H

#include "global.h"
#include "types.h"
#include "util.h"
#include "screenbullet.h"
#include "tankexp.h"

struct GameSim;
struct ClientSim;

/* Why a shell ended, reported by the server to the shell's owner via
 * CTRL_SHELL_DEATH (control_event.h). Terrain/pill/base collisions all
 * collapse to SHELL_OUTCOME_IMPACT because the client treats them
 * identically (cull the predicted ghost, draw the impact). Defined here
 * rather than in control_event.h so shells.c can name these without
 * pulling in the public control-event header (which would drag client_sim
 * et al. into the engine tier). REJECTED is never emitted by shells.c —
 * it is reserved on the wire for a denied fire and handled client-side. */
typedef enum {
  SHELL_OUTCOME_REJECTED = 0, /* fire denied — no shell ever existed */
  SHELL_OUTCOME_EXPIRED,      /* shell reached end of life with no hit */
  SHELL_OUTCOME_IMPACT,       /* hit terrain / pillbox / base */
  SHELL_OUTCOME_TANK_HIT,     /* hit a tank (damaged, not killed) */
  SHELL_OUTCOME_TANK_KILL     /* hit a tank and killed it */
} ShellOutcome;

/* Empty / Non Empty / Head / Tail Macros */
#define IsEmpty(list) ((list) ==NULL)
#define NonEmpty(list) (!IsEmpty(list))
#define ShellsHeadX(list) ((list)->x);
#define ShellsHeadY(list) ((list)->y);
#define ShellsHeadAngle(list) (NonEmpty(list),(list)->angle);
#define ShellsHeadLength(list) ((list)->length);
#define ShellsTail(list) ((list)->next);

/* Shell life is 2 times map length */
#define SHELL_LIFE 8

/* Shells die when their length equals */
#define SHELL_DEATH 0

/* Shell Speed is 32 world units per tick */
#define SHELL_SPEED 32

/* Shells have 4 frames for explosions so add 4 to their life spans */
#define SHELL_START_EXPLODE 8

/* Fudge factor for shootong offset */
#define SHELL_START_ADD 5


/* Brain Stuff */
#define SHELLS_BRAIN_FRIENDLY 0
#define SHELLS_BRAIN_NEUTRAL 2
#define SHELLS_BRAIN_HOSTILE 1
#define SHELLS_BRAIN_OBJECT_TYPE 1


/* Type structure */

typedef struct shellsObj *shells;
struct shellsObj {
  shells next;      /* Pointer to the next shell */
  shells prev;      /* Pointer to previous item */
  WORLD x;          /* Co-ords of the shell */
  WORLD y;
  TURNTYPE angle;   /* The angle the shell is firing */
  BYTE length;      /* Number of map squares for the shell to fire */
  BYTE owner;       /* Who owns the shell */
  BYTE target;      /* The player a pillbox fired this shell at, NEUTRAL for
                       a tank's shell. Server only and never sent: it is
                       what pill_max_shells_at_tank counts. */
  BYTE pill;        /* The pill index of the pillbox that fired this shell,
                       DMG_NO_PILL for a tank's shell. Server only and never
                       sent: the combat questions are handed it, because
                       owner is NEUTRAL for every pillbox. */
  uint16_t passedTanks; /* One bit per tank slot the can_hit question let
                       this shell pass through, and one per pill index in
                       the field below. Server only and never sent. A target
                       is asked once per shell: once let through it is left
                       alone for the rest of the shell's flight, so the
                       answer holds for as long as the two overlap and a
                       shell sitting inside a tank for several ticks does
                       not ask the question on each of them. */
  uint16_t passedPills;
  bool onBoat;      /* Was the shell launched from a boat */
  bool packSent;    /* Has this shell been included in a network packet yet */
  BYTE creator;     /* Creator machines player Number */
  uint32_t fireTick; /* Originating client input tick that fired this shell
                        (0 for pill / gap-fill / network-extracted shells).
                        Echoed in CTRL_SHELL_DEATH so the firing client can
                        match the death to its predicted shell by fireTick.
                        A NUMBER THE CLIENT CHOSE: it starts near zero on a
                        mid-round joiner and a modified client can send any
                        value, so nothing on the server may be measured on
                        it. That is what serverFireTick below is for. */
  uint32_t serverFireTick; /* The SERVER's own tick at the moment this shell
                        was created, handed back by the shellFired callback
                        (0 on the client, which has no server tick and no
                        such callback). Every server-side rule about when a
                        shell left the gun — the three-shot order detector's
                        window and its two quiet seconds — reads this one. */
  bool shellDead;   /* Used to over come the if shell dies straight away and
                       hasn't been sent it never does. So we mark it dead
                       and it doesn't get updated any more but exists till
                       it gets sent (ie packSent == TRUE) */
  uint8_t compensationTicks;  /* rewind ticks for lag compensation (0 = no compensation) */
  /* High-precision fixed-point step: speed * cos/sin * 256, stored once at
   * creation so we don't re-round every tick.  xAcc/yAcc accumulate the
   * fractional world-unit remainder between ticks. */
  int32_t xStep;    /* 24.8 fixed-point X step per tick (shell_speed * cos * 256) */
  int32_t yStep;    /* 24.8 fixed-point Y step per tick (shell_speed * sin * 256) */
  int32_t xAcc;     /* fractional accumulator, range [0, 256) */
  int32_t yAcc;     /* fractional accumulator, range [0, 256) */
};


typedef struct shellsNetHitObj *shellsNetHit;
struct shellsNetHitObj {
  shellsNetHit next; /* Next item */
  TURNTYPE angle;    /* Angle the shell wa traveling on */
  BYTE playerHit;    /* The player hit */
  BYTE hitBy;        /* The player it was hit by */
  BYTE bmx;          /* X and Y positions */
  BYTE bmy;
};

/* Prototypes */

/*********************************************************
*NAME:          shellsCreate
*AUTHOR:        John Morrison
*CREATION DATE: 25/12/98
*LAST MODIFIED: 25/12/98
*PURPOSE:
*  Sets up the shells data structure
*
*ARGUMENTS:
*
*********************************************************/
shells shellsCreate(void);

/*********************************************************
*NAME:          shellsAddItem
*AUTHOR:        John Morrison
*CREATION DATE: 25/12/98
*LAST MODIFIED: 16/2/99
*PURPOSE:
*  Adds an item to the shells data structure. Function
*  also calls the sound playing function
*
*ARGUMENTS:
*  value  - Pointer to the shells data structure
*  x      - X co-ord of the start position
*  y      - Y co-ord of the start position
*  angle  - angle of the shot
*  len    - Length in map units of the item
*  owner  - Who fired the shell
*  target - The player a pillbox is firing at, NEUTRAL
*           for a tank's shell
*  pill   - The pill index of the pillbox firing it,
*           DMG_NO_PILL for a tank's shell
*  onBoat - Was the shell launched from a boat
*********************************************************/
void shellsAddItem(struct GameSim *sim, shells *value, WORLD x, WORLD y, TURNTYPE angle, TURNTYPE len, BYTE owner, BYTE target, BYTE pill, bool onBoat);

/* Pure shell-physics primitives — no game-state mutation. Used by
 * both the live engine (shellsUpdate / shellsAddItem) and the brain's
 * stateless trajectory simulator (brainPathfinderSimulateShot). Must
 * stay bit-identical or the brain's "would my shot hit?" predictions
 * diverge from reality.
 *
 * They take the shell numbers rather than reading them, because the
 * two callers reach them from different places: the engine passes
 * sim->rules, and the brain passes what the bot manager pushed onto
 * the pathfinder. A sim argument would shut the brain out. */

/* Apply the initial offset that shellsAddItem uses before the first
 * tick. Mutates *x, *y in place. xAdd/yAdd are the low-precision
 * integer per-tick step from utilCalcDistance(angle, shell speed);
 * startAdd is shell_start_add. */
void shellApplyStartOffset(WORLD *x, WORLD *y, int xAdd, int yAdd,
                           int startAdd);

/* Advance one tick of high-precision (24.8 fixed-point) shell
 * motion. Adds (xStep, yStep) into the accumulators, extracts the
 * whole-wu portion, and bumps *x, *y by it. xStep/yStep are the
 * shell speed * (sin, -cos) of the angle in 24.8 format from
 * utilCalcDistanceHP — constant for the shell's lifetime. */
void shellAdvance1Tick(WORLD *x, WORLD *y,
                       int32_t *xAcc, int32_t *yAcc,
                       int32_t xStep, int32_t yStep);

/* Compute the shell-life tick budget the same way shellsAddItem
 * stores it on the shell record: 1 + shellLife * len - startAdd,
 * floored at 0. `len` is the value the firing tank passed (sightLen/2
 * for tanks, PILLBOX_FIRE_DISTANCE for pills); shellLife is
 * shell_life and startAdd is shell_start_add. The result goes into a
 * BYTE, which is what the shell_life / gunsight_max pair in
 * simRulesValidate keeps under 256. */
int  shellLifeTicks(float len, int shellLife, int startAdd);

/* Convert a (origin → target) wu vector to the integer bolo bradian
 * angle (0..255) that a shooter would need to fire along that line.
 * Bolo convention: N=0(-y), E=64(+x), S=128(+y), W=192(-x). Returns
 * 0 if the two points coincide. Rounds (not truncates) so we land
 * on the same integer the engine's tank.direction would carry. */
TURNTYPE shellAngleFromTarget(WORLD ox, WORLD oy, WORLD tx, WORLD ty);

/* Compute where a shell physically appears when a tank fires from
 * (tank_x, tank_y) at the given angle. Combines utilCalcDistance
 * (low-precision per-tick step at shellSpeed) with the startAdd
 * initial offset that shellsAddItem applies — same math as the
 * engine, so the brain knows the exact spawn coordinate before the
 * engine sets it. Writes spawn position into *out_x, *out_y. */
void shellSpawnPos(WORLD tank_x, WORLD tank_y, TURNTYPE angle,
                   int shellSpeed, int startAdd,
                   WORLD *out_x, WORLD *out_y);


/*********************************************************
*NAME:          shellsUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 25/12/98
*LAST MODIFIED: 30/10/99
*PURPOSE:
*  Updates each shells position and checks for colisions
*
*ARGUMENTS:
*  value    - Pointer to the shells data structure
*  mp       - Pointer to the map Structure
*  pb       - Pointer to the pillbox Structure
*  tk       - Pointer to an array of tank structures
*  numTanks - Number of tanks in the array
*  isServer - TRUE if we are a server
*********************************************************/
void shellsUpdate(struct GameSim *sim, tank *tk, BYTE numTanks, lgm **lgms, starts *sts);

/*********************************************************
*NAME:          shellsDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 25/12/98
*LAST MODIFIED: 25/12/98
*PURPOSE:
*  Destroys and frees memory for the shells data structure
*
*ARGUMENTS:
*  value - Pointer to the shells data structure
*********************************************************/
void shellsDestroy(shells *value);

/*********************************************************
*NAME:          shellsDeleteItem
*AUTHOR:        John Morrison
*CREATION DATE: 25/12/98
*LAST MODIFIED: 25/12/98
*PURPOSE:
*  Deletes the value from the master list
*
*ARGUMENTS:
*  master  - The master list of all shells
*  value   - Pointer to the shells to delete. Also puts
*            next shell its position
*********************************************************/
void shellsDeleteItem(shells *master, shells *value);

/*********************************************************
*NAME:          shellsCalcScreenBullets
*AUTHOR:        John Morrison
*CREATION DATE: 26/12/98
*LAST MODIFIED: 26/12/98
*PURPOSE:
*  Adds items to the sceenBullets data structure if they
*  are on screen
*
*ARGUMENTS:
*  value    - Pointer to the shells data structure
*  sBullet  - The screenBullets Data structure
*  leftPos  - X Map offset start
*  rightPos - X Map offset end
*  top      - Y Map offset end
*  bottom   - Y Map offset end
*********************************************************/
void shellsCalcScreenBullets(shells *value, screenBullets *sBullets, BYTE leftPos, BYTE rightPos, BYTE top, BYTE bottom);

/*********************************************************
*NAME:          shellsCalcCollision
*AUTHOR:        John Morrison
*CREATION DATE: 29/12/98
*LAST MODIFIED: 30/10/99
*PURPOSE:
*  Returns whether the collision has occured.
*  
*ARGUMENTS:
*  map      - Pointer to the Map Structure
*  pb       - Pointer to the pillboxes structure
*  tk       - Pointer to an array of tank structures
*  bs       - Pointer to the bases structure
*  xValue   - X position
*  yValue   - Y position
*  shell    - The shell, for its angle, owner, boat flag,
*             lag compensation, firing pill and the
*             targets can_hit has already let it pass
*  numTanks - Number of tanks in the array
*  outOutcome - NULL-tolerant out-param; on a collision, set to the
*               SHELL_OUTCOME_* describing what was hit (TANK_HIT /
*               TANK_KILL / IMPACT). Untouched when no collision occurs.
*********************************************************/
bool shellsCalcCollision(struct GameSim *sim, tank *tk, WORLD *xValue, WORLD *yValue, shells shell, BYTE numTanks, uint8_t *outOutcome);

/*********************************************************
*NAME:          shellsCheckRoad
*AUTHOR:        John Morrison
*CREATION DATE: 6/1/99
*LAST MODIFIED: 6/1/99
*PURPOSE:
*  A shell has been fired from a boat and has hit a road.
*  This function returns whether the road should be kept
*  as road or destroyed and replaced as river
*  
*ARGUMENTS:
*  mp    - Pointer to the Map Data structure
*  pb    - Pointer to the pillbox structure
*  bs    - Pointer to the bases structure
*  mapX  - X position of the hit
*  mapY  - Y position of the hit
*  angle - The angle the shell is travelling
*********************************************************/
BYTE shellsCheckRoad(struct GameSim *sim, BYTE mapX, BYTE mapY, TURNTYPE dir);

/*********************************************************
*NAME:          shellsNetMake
*AUTHOR:        John Morrison
*CREATION DATE: 6/3/99
*LAST MODIFIED: 8/9/00
*PURPOSE:
*  When we have the token we inform all the players of
*  shells we have fired since last time we had the token.
*  Returns the length of the data created
*  
*ARGUMENTS:
*  value       - Pointer to shells structure
*  buff        - Pointer to a buffer to hold the shells 
*                net data
*  noPlayerNum - If the shells ->creator equals this
*                do not send it
*  sentState   - What to set the send state to
*********************************************************/
BYTE shellsNetMake(shells *value, BYTE *buff, BYTE noPlayerNum, bool sentState);

/*********************************************************
*NAME:          shellsGetBrainShellsInRect
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/99
*LAST MODIFIED: 28/11/99
*PURPOSE:
*  Makes the brain shell info for each shell inside the
*  rectangle formed by the function parameters.
*
*ARGUMENTS:
*  value     - Pointer to the shells structure
*  leftPos   - Left position of rectangle
*  rightPos  - Right position of rectangle
*  topPos    - Top position of rectangle
*  bottomPos - Bottom position of rectangle
*********************************************************/
void shellsGetBrainShellsInRect(struct ClientSim *cs, struct GameSim *sim, shells *value, BYTE leftPos, BYTE rightPos, BYTE topPos, BYTE bottomPos);

/* --- Debug: shell-hit ring buffer (populated by shellsUpdate on collision). --- */
void shellsDebugHitLogClear(void);
int  shellsDebugHitLogCount(void);
int  shellsDebugHitLogGet(int i, int *wx, int *wy, uint32_t *tick, uint8_t *owner);

#endif /* SHELLS_H */
