/*
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
*Name:          Interpolation
*Filename:      interpolation.h
*Purpose:
*  Per-player interpolation of other tanks' positions
*  between server snapshots for smooth rendering.
*********************************************************/

#ifndef INTERPOLATION_H
#define INTERPOLATION_H

#include "global.h"

/* A snapshot of one player's tank state at a point in time.
 * Named InterpSnapshot to distinguish from the wire-format
 * TankSnapshot in input_packet.h. */
typedef struct {
  WORLD worldX;
  WORLD worldY;
  TURNTYPE angle;
  SPEEDTYPE speed;
  bool onBoat;
  bool alive;       /* TRUE if armour <= TANK_FULL_ARMOUR */
  BYTE lgmMX;
  BYTE lgmMY;
  BYTE lgmPX;
  BYTE lgmPY;
  BYTE lgmFrame;
} InterpSnapshot;

/* Per-player interpolation state */
typedef struct {
  InterpSnapshot prev;
  InterpSnapshot curr;
  uint32_t snapshotTick;   /* Tick when curr was received */
  bool hasData;            /* At least one snapshot received */
  bool hasPrev;            /* Two snapshots received (can interpolate) */
  uint32_t missedTicks;    /* Consecutive ticks with no update */
} InterpPlayer;

/* The full interpolation context for all other players */
typedef struct {
  InterpPlayer players[MAX_TANKS];
  BYTE localPlayer;        /* Our own player number (skip interpolation) */
} InterpContext;

/*********************************************************
*NAME:          interpCreate
*PURPOSE:
*  Initializes the interpolation context.
*
*ARGUMENTS:
*  ctx         - Pointer to the InterpContext
*  localPlayer - Our own player number
*********************************************************/
void interpCreate(InterpContext *ctx, BYTE localPlayer);

/*********************************************************
*NAME:          interpUpdate
*PURPOSE:
*  Called when a new server snapshot arrives for a player.
*  Shifts current→previous, stores new snapshot as current.
*
*ARGUMENTS:
*  ctx       - Pointer to the InterpContext
*  playerNum - Which player this snapshot is for
*  snap      - The new snapshot data
*  tick      - Server tick number
*********************************************************/
void interpUpdate(InterpContext *ctx, BYTE playerNum,
                  const InterpSnapshot *snap, uint32_t tick);

/*********************************************************
*NAME:          interpMarkMissing
*PURPOSE:
*  Called each tick for players that did NOT receive a
*  snapshot update (e.g., out of view or disconnected).
*
*ARGUMENTS:
*  ctx       - Pointer to the InterpContext
*  playerNum - Which player had no update
*********************************************************/
void interpMarkMissing(InterpContext *ctx, BYTE playerNum);

/*********************************************************
*NAME:          interpGetPosition
*PURPOSE:
*  Returns interpolated world position and angle for a
*  player at the given fractional tick offset.
*
*ARGUMENTS:
*  ctx       - Pointer to the InterpContext
*  playerNum - Which player to interpolate
*  t         - Interpolation factor [0.0, 1.0] where
*              0 = previous snapshot, 1 = current snapshot
*  outX      - Output: interpolated world X
*  outY      - Output: interpolated world Y
*  outAngle  - Output: interpolated angle
*  outOnBoat - Output: on boat status
*
*RETURNS:
*  TRUE if valid interpolated data is available,
*  FALSE if player has no data or is stale
*********************************************************/
bool interpGetPosition(const InterpContext *ctx, BYTE playerNum,
                       float t,
                       WORLD *outX, WORLD *outY,
                       TURNTYPE *outAngle, bool *outOnBoat);

/*********************************************************
*NAME:          interpGetLgm
*PURPOSE:
*  Returns the most recent LGM data for a player.
*
*ARGUMENTS:
*  ctx       - Pointer to the InterpContext
*  playerNum - Which player
*  outMX     - Output: LGM map X
*  outMY     - Output: LGM map Y
*  outPX     - Output: LGM pixel X
*  outPY     - Output: LGM pixel Y
*  outFrame  - Output: LGM frame
*
*RETURNS:
*  TRUE if valid LGM data is available
*********************************************************/
bool interpGetLgm(const InterpContext *ctx, BYTE playerNum,
                  BYTE *outMX, BYTE *outMY,
                  BYTE *outPX, BYTE *outPY, BYTE *outFrame);

/*********************************************************
*NAME:          interpHasData
*PURPOSE:
*  Returns whether we have any snapshot data for a player.
*
*ARGUMENTS:
*  ctx       - Pointer to the InterpContext
*  playerNum - Which player
*********************************************************/
bool interpHasData(const InterpContext *ctx, BYTE playerNum);

/*********************************************************
*NAME:          interpIsAlive
*PURPOSE:
*  Returns whether the player's most recent snapshot shows
*  them as alive.
*
*ARGUMENTS:
*  ctx       - Pointer to the InterpContext
*  playerNum - Which player
*********************************************************/
bool interpIsAlive(const InterpContext *ctx, BYTE playerNum);

#endif /* INTERPOLATION_H */
