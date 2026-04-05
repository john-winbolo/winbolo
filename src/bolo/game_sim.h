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
 *Name:          Game Simulation
 *Filename:      game_sim.h
 *Author:        John Morrison
 *Purpose:
 *  Shared game simulation struct embedded by both
 *  ServerSim and ClientSim. Contains all state common
 *  to client and server, plus callbacks for behavior
 *  that differs between them.
 *********************************************************/

#ifndef GAME_SIM_H
#define GAME_SIM_H

#include "global.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "tank.h"
#include "shells.h"
#include "lgm.h"
#include "explosions.h"
#include "mines.h"
#include "minesexp.h"
#include "floodfill.h"
#include "building.h"
#include "rubble.h"
#include "swamp.h"
#include "grass.h"
#include "tankexp.h"
#include "gametype.h"
#include "players.h"
#include "messages.h"
#include "sounddist.h"
#include "util.h"

typedef struct GameSimCallbacks {
    void (*messageAdd)(void *ctx, messageType msgType, char *top, char *bottom);
    void (*soundDist)(void *ctx, sndEffects value, BYTE mx, BYTE my);
    void (*soundDistShoot)(void *ctx, BYTE mx, BYTE my, BYTE owner);
    void (*soundDistTankHit)(void *ctx, BYTE mx, BYTE my, BYTE hitPlayer);
    void (*tankKill)(void *ctx, BYTE killer, BYTE killed, BYTE deathCause, BYTE carriedPills);
    void (*centerTank)(void *ctx);
    void (*consoleMessage)(void *ctx, char *msg);
    void *ctx;  /* opaque pointer: ClientSim* or ServerSim* */
} GameSimCallbacks;

typedef struct GameSim {
    /* Core game objects — identical types in client and server */
    map         mp;
    bases       bs;
    pillboxes   pb;
    starts      ss;
    players     plyrs;

    /* Per-player objects */
    tank        tanks[MAX_TANKS];
    lgm         lgmen[MAX_TANKS];

    /* Shared subsystems */
    shells      shs;
    explosions  expl;
    mines       mns;
    floodFill   ff;
    building    blds;
    tkExplosion tankExplosions;
    minesExp    minesExplosions;
    rubble      rbl;
    swamp       swp;
    grass       grs;

    /* Game rules */
    gameType    game;
    bool        hiddenMines;

    /* Identity — lets shared code know if it's running as server */
    bool        isServer;
    bool        isLocalTransport; /* true for local/single-player, false for UDP */
    bool        inStartFind; /* Whether tank is searching for start position */

    /* Callbacks for behavior that differs between client and server */
    GameSimCallbacks callbacks;

    /* Pointer to the owning ClientSim's brainMap (NULL on server).
     * Used by shared code (bolo_map.c) to update the fog-of-war brain map. */
    BYTE (*brainMap)[MAP_ARRAY_SIZE];
} GameSim;

/*********************************************************
 * Utility functions for shared code that needs to look up
 * player numbers or check tank ranges via the sim.
 *********************************************************/

static inline BYTE gameSimGetTankPlayer(GameSim *sim, tank *value) {
  BYTE count = 0;
  while (count < MAX_TANKS) {
    if (*value == sim->tanks[count]) {
      return count;
    }
    count++;
  }
  return NEUTRAL;
}

static inline bool gameSimCheckTankRange(GameSim *sim, BYTE x, BYTE y, BYTE playerNum, double distance) {
  WORLD ourWX, ourWY, testWX, testWY;
  double dummy;
  BYTE count;

  ourWX = (x << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE;
  ourWY = (y << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE;

  for (count = 0; count < MAX_TANKS; count++) {
    if (playerNum != count && sim->tanks[count] != NULL && count != playerNum) {
      tankGetWorld(&sim->tanks[count], &testWX, &testWY);
      if (utilIsItemInRange(ourWX, ourWY, testWX, testWY, (WORLD) distance, &dummy)) {
        return FALSE;
      }
    }
  }
  return TRUE;
}

#endif /* GAME_SIM_H */
