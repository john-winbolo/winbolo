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
#include "position_history.h"
#include "../gui/lang.h"

typedef struct GameSimCallbacks {
    void (*messageAdd)(void *ctx, messageType msgType,
                       langid topId, langid bodyId,
                       const MessageArgs *args);
    void (*soundDist)(void *ctx, sndEffects value, BYTE mx, BYTE my);
    void (*soundDistShoot)(void *ctx, BYTE mx, BYTE my, BYTE owner);
    void (*soundDistTankHit)(void *ctx, BYTE mx, BYTE my, BYTE hitPlayer);
    void (*tankKill)(void *ctx, BYTE killer, BYTE killed, BYTE deathCause, BYTE carriedPills);
    void (*centerTank)(void *ctx);
    void (*consoleMessage)(void *ctx, char *msg);
    void (*mineVisible)(void *ctx, BYTE mx, BYTE my, BYTE sourcePlayer);
    void (*explosion)(void *ctx, BYTE mx, BYTE my, BYTE px, BYTE py);
    void (*tkExplosion)(void *ctx, WORLD x, WORLD y, TURNTYPE angle,
                        BYTE length, BYTE explodeType, BYTE creator);
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
    BYTE        viewPlayer; /* which player's perspective we render from */
    bool        isServer;
    bool        isLocalTransport; /* true for local/single-player, false for UDP */
    bool        inStartFind; /* Whether tank is searching for start position */
    bool        isPredicting; /* true during client-side prediction — suppresses all side effects */

    /* Callbacks for behavior that differs between client and server */
    GameSimCallbacks callbacks;

    /* Re-entrancy guard used during tankDestroy() */
    bool        tankShuttingDown;

    /* Whether the UI is in a menu (engine checks this during tick) */
    bool        isInMenu;

    /* True when this sim is driving the built-in tutorial. The server
     * uses it to halt the player's tank at each tutorial trigger row;
     * the client uses it to gate the frontEndTutorial dialog sequence.
     * Set by the frontend after sim creation; zero-initialised by
     * memset in clientSimCreate / serverSimCreate. */
    bool        isTutorial;

    /* Pointer to the owning ClientSim's brainMap (NULL on server).
     * Used by shared code (bolo_map.c) to update the fog-of-war brain map. */
    BYTE (*brainMap)[MAP_ARRAY_SIZE];

    /* Tree growth (was treegrow.c globals) */
    BYTE        treeGrowX;
    BYTE        treeGrowY;
    int         treeGrowTime;
    int         treeGrowScore;
    WORD        treeGrowSeed;

    /* Base refuel timers (was bases.c global) */
    int         baseTimer[MAX_TANKS];

    /* Tank explosion update throttle (per-sim so server/client don't share) */
    BYTE        tkExpUpdateTime;

    /* Lag compensation (server-only, zeroed on client) */
    uint8_t lagCompTicks;                    /* Set before each player's tankUpdate */
    uint8_t perPlayerCompTicks[MAX_TANKS];   /* Per-player comp ticks for pill shells */
    PosHistory *posHistoryPtr;               /* NULL on client, points to ServerSim.posHistory on server */
    PosHistory *lgmPosHistoryPtr;            /* NULL on client, points to ServerSim.lgmPosHistory on server */

    /* Pre-computed start indices used during the lobby->game batch tank
     * creation. Each slot is a start index in [0, MAX_STARTS); MAX_STARTS
     * itself is the "unassigned" sentinel. startsGetStart consumes and
     * clears the slot, falling through to the per-player algorithm when
     * unassigned. Scatter and direction conversion happen lazily on
     * consumption so siblings already created in the batch loop are
     * visible during the per-square nudge. */
    BYTE        pendingStartIdx[MAX_TANKS];
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
