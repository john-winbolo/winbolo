/*
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

#ifndef GAMESIM_TYPEDEF
#define GAMESIM_TYPEDEF
typedef struct GameSim GameSim;
#endif

/* The game timer is 20 milliseconds between events the game_tick_length is half this */
#define GAME_TICK_LENGTH 10
#define GAME_NUMTOTALTICKS_SEC (1000 / GAME_TICK_LENGTH)
#define GAME_NUMGAMETICKS_SEC (1000 / 20)

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
#include "../../gui/lang.h"

/* recordDamage targetKind */
#define DMG_TARGET_TANK 0
#define DMG_TARGET_PILL 1
#define DMG_TARGET_BASE 2
/* recordDamage source — what inflicted the hit. Mirrors ATTR_SRC_* on-disk. */
#define DMG_SRC_UNKNOWN 0
#define DMG_SRC_SHELL   1
#define DMG_SRC_MINE    2
/* recordPlayerAction actionKind */
#define PLAYER_ACTION_FARM  0
#define PLAYER_ACTION_BUILD 1
#define PLAYER_ACTION_MINE  2
#define PLAYER_ACTION_SHELL 3
/* canDie kind — what the blow would destroy. index is the tank slot for a
 * tank and for the builder riding in it, and the pill index for a pill. */
#define DIE_KIND_TANK    0
#define DIE_KIND_BUILDER 1
#define DIE_KIND_PILL    2
/* canCapture kind — what is being taken, with index the pill or base. */
#define CAPTURE_KIND_PILL 0
#define CAPTURE_KIND_BASE 1

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
    /* Server-only: a shell owned by `owner` ended. Wired to publish a
     * unicast CTRL_SHELL_DEATH so the firing client can match fireTick to
     * its predicted shell, cull the ghost, and draw the impact at
     * (impactWX, impactWY). NULL on the client (not authoritative over
     * shell death). outcome is a SHELL_OUTCOME_* (shells.h). */
    void (*shellDeath)(void *ctx, uint32_t fireTick, BYTE owner,
                       WORLD impactWX, WORLD impactWY, uint8_t outcome);
    /* Server-only stats attribution; NULL on the client (call sites null-check).
     * recordDamage: `attacker` dealt `dealt` effective armour damage to a target
     * of `targetKind` identified by `targetIndex` (the tank slot, or pill/base
     * index that was hit); `source` is a DMG_SRC_* value naming what inflicted
     * the hit; `destroyed` is true only when this blow dropped a pillbox to 0
     * armour. recordPlayerAction: `player` performed a farm/build/mine/shell.
     * recordPillPickup: `picker` scooped dead pillbox `pillIndex` into its tank.
     * mapX/mapY are the event's map cell (0 if unknown), for offline highlights. */
    void (*recordDamage)(void *ctx, BYTE attacker, BYTE targetKind,
                         BYTE targetIndex, BYTE source,
                         uint16_t dealt, bool destroyed, BYTE mapX, BYTE mapY);
    void (*recordPlayerAction)(void *ctx, BYTE player, BYTE actionKind,
                               BYTE mapX, BYTE mapY);
    void (*recordPillPickup)(void *ctx, BYTE picker, BYTE pillIndex,
                             BYTE mapX, BYTE mapY);
    /* Server-only announcements of a fact the sim core has just settled.
     * Each is fired where the core used to build the GameEvent itself: the
     * event, and the queue it goes on, are the server's. NULL on the client,
     * which never reaches any of them — every call sits inside the
     * sim->isServer test that was already around the emit.
     *
     * baseOwnerChanged / pillOwnerChanged: the objective at `index` — the
     * 0-based item[] slot, the base the rest of these files call the pill or
     * base number less one — passed from `oldOwner` to `newOwner`.
     * captureClass is a CAPTURE_CLASS_* separating a take from neutral, a
     * steal from an enemy and a hand-over between allies; it is worked out at
     * the call site because that is where the alliance is known. mapX/mapY
     * are the objective's square.
     * lgmDied: `victim`'s builder was lost and `killer` is credited, NEUTRAL
     * for a death nobody caused. mapX/mapY are the man's square as it stands
     * when the call is made. */
    void (*baseOwnerChanged)(void *ctx, BYTE index, BYTE oldOwner,
                             BYTE newOwner, BYTE captureClass,
                             BYTE mapX, BYTE mapY);
    void (*pillOwnerChanged)(void *ctx, BYTE index, BYTE oldOwner,
                             BYTE newOwner, BYTE captureClass,
                             BYTE mapX, BYTE mapY);
    void (*lgmDied)(void *ctx, BYTE victim, BYTE killer, BYTE mapX, BYTE mapY);
    /* The rest of the facts the sim core settles and the server publishes.
     * Same shape and same rule as the three above: NULL on the client, fired
     * inside the isServer test at each site, and every index is the 0-based
     * item[] slot.
     *
     * tankSpawned: `player`'s tank is on the map at mapX/mapY. respawn is
     * false for the tank a round or a join creates and true for one coming
     * back from a death.
     * lgmLanded: `player`'s builder finished the flight back and is standing
     * at mapX/mapY — the square he actually reached, not the one he left.
     * pillPlaced: `player`'s builder put a carried pill down as pill `index`.
     * pillKilled: pill `index` lost its last armour. attacker is the slot
     * credited, or NEUTRAL where the blow names nobody.
     * built: `player`'s builder finished a job. action is the builder's own
     * request code, the number BuilderJob uses.
     * mineLaid: `player` put a mine on mapX/mapY. The event this becomes
     * never reaches a client — it would give away hidden mines.
     * mineExploded: the mine on mapX/mapY went up. layer is the slot that
     * laid it, or NEUTRAL, read before the mine is taken off the field. */
    void (*tankSpawned)(void *ctx, BYTE player, BYTE mapX, BYTE mapY,
                        bool respawn);
    void (*lgmLanded)(void *ctx, BYTE player, BYTE mapX, BYTE mapY);
    void (*pillPlaced)(void *ctx, BYTE player, BYTE index, BYTE mapX,
                       BYTE mapY);
    void (*pillKilled)(void *ctx, BYTE index, BYTE attacker);
    void (*built)(void *ctx, BYTE player, BYTE action, BYTE mapX, BYTE mapY);
    void (*mineLaid)(void *ctx, BYTE player, BYTE mapX, BYTE mapY);
    void (*mineExploded)(void *ctx, BYTE mapX, BYTE mapY, BYTE layer);
    /* Policy queries — the members that return an answer rather than
     * announcing something. Each asks the host a decision the sim would
     * otherwise make alone; the server's implementation is the only place
     * that knows what is deciding, so shared code asks the question and
     * never learns who answered it. NULL is the classic rule and every call
     * site treats it as such, which is how the client — which registers none
     * of these — keeps the classic answer.
     *
     * chooseStart: where `player` starts. True with a start index in
     * *startIdx; the caller range-checks it and falls back to its own pick.
     * spawnLoadout: what a spawning tank is handed. True with the four
     * amounts filled; false leaves the sim's game type to decide.
     * canRespawn: whether a dead tank may come back. False holds the death
     * wait where it is and the question is asked again next tick.
     * damageScale: the percent one blow actually does, 100 being the classic
     * amount and 0 a hit that costs no armour at all. cause is a
     * LAST_DEATH_BY_* value.
     * canBuild: whether a build order may go ahead. False turns it down the
     * way an order on an impossible square is turned down. pillIdx is the
     * pillbox a place-pill order would put down, LGM_NO_PILL otherwise.
     * canCapture: whether a pill or base may change hands. False leaves the
     * objective where it is; it does not block the tank.
     * canDie: whether this blow may destroy what it landed on. False leaves a
     * tank at zero armour and alive, a builder untouched and a pill at one.
     *
     * Ask these through the gameSim* helpers below rather than through the
     * member, so the NULL answer is written once. */
    bool (*chooseStart)(void *ctx, BYTE player, BYTE *startIdx);
    bool (*spawnLoadout)(void *ctx, BYTE player, BYTE *shells, BYTE *mines,
                         BYTE *armour, BYTE *trees);
    bool (*canRespawn)(void *ctx, BYTE player);
    int  (*damageScale)(void *ctx, BYTE attacker, BYTE victim, BYTE cause);
    bool (*canBuild)(void *ctx, BYTE player, BYTE action, BYTE mapX,
                     BYTE mapY, BYTE pillIdx);
    bool (*canCapture)(void *ctx, BYTE kind, BYTE index, BYTE player);
    bool (*canDie)(void *ctx, BYTE kind, BYTE index, BYTE killer, BYTE cause);
    void *ctx;  /* opaque pointer: ClientSim* or ServerSim* */
} GameSimCallbacks;

struct GameSim {
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

    /* Originating client input tick of the fire currently being applied,
     * set by the server right before the firing tankUpdate and read by
     * shellsAddItem to stamp the shell's fireTick. 0 outside a player fire
     * (pill shells, gap-fill, substitutes, client). */
    uint32_t    fireInputTick;

    /* Client-side base-death prediction (client only; both 0 on the server,
     * which holds the real armour and needs no prediction).
     *
     * basePredictedDeadTick[b] is the input tick at which this client's own
     * predicted shell is expected to drop base b to MIN_ARMOUR_CAPTURE, or 0
     * for none. tankBuildingCollision treats the square as drivable from that
     * tick on, so a tank driving into a base it is killing does not spend a
     * round trip fighting its own prediction. Authoritative armour for the
     * base (a full sync or EVENT_BASE_STOCK) settles it through
     * clientBaseArmourArrived: the stamp survives only while the server has
     * not yet processed that tick and one more hit would still cross the
     * threshold, so a stale update from before the hit leaves it alone and a
     * prediction the server has disproved is taken back.
     *
     * basePredictedHitTick[b] is the input tick at which this client's latest
     * predicted shell landed on base b, whatever the armour read at the time,
     * or 0 for none. A landing that could not arm the stamp — the mirror
     * armour still had an earlier hit outstanding — is armed retroactively
     * when that earlier hit's armour lands and says one more would kill,
     * provided the server had not yet processed the landing. Cleared once
     * the server has.
     *
     * replayTick is the input tick currently being simulated — set on the
     * forward predicted tick and again for each tick of a reconciliation
     * replay, so a replay spanning the death resolves every replayed tick
     * against the state that actually applied at it rather than against the
     * one current value. 0 outside prediction. */
    uint32_t    replayTick;
    uint32_t    basePredictedDeadTick[MAX_BASES];
    uint32_t    basePredictedHitTick[MAX_BASES];

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

    /* When TRUE, tankUpdate returns early so shell, mine and timer
     * progression freezes. The frontend toggles this around modal
     * tutorial dialogs so the world doesn't drift while the player
     * reads a message. */
    bool        paused;

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
    /* A start a scenario op named for a slot, MAX_STARTS for none. Honoured
       by startsGetStart ahead of the placement policy and consumed there; the
       batch slot above is the engine choosing and stays below the policy. */
    BYTE        scenarioStartIdx[MAX_TANKS];
    /* Tutorial respawn start index. While sim->isTutorial, startsGetStart
       returns this fixed start (not the open-game algorithm). The GUI raises
       it from 0 (sea) to 1 (far bank) once the player passes the boat step.
       Zero-initialised by memset at sim creation. */
    BYTE        tutorialStartIdx;
    /* Deepest (lowest) map row the tank has reached this tutorial run. The
       server only halts the tank at a stop row the player hasn't passed yet
       (newbmy < tutorialMinRow), so a respawn driving back through already-seen
       rows isn't stopped again. Initialised to 0xFF (no progress) in
       serverSimInit — memset alone would leave it 0 and disable every stop. */
    BYTE        tutorialMinRow;
    /* Set by tankDeath's server respawn branch when a tutorial respawn lands at
       start 1 (the far bank). The GUI takes-and-clears it to show the one-shot
       respawn message. Zero-initialised by memset at sim creation. */
    bool        tutorialRespawn1Pending;
};

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

/*********************************************************
 * The combat policy questions. Each is the one place the
 * classic answer is written down, so a site asks without
 * testing the member and a sim with nothing registered —
 * every ClientSim — takes the classic branch by
 * construction.
 *********************************************************/

/* The percent one blow does. Negative is read as nothing at all; the caller
   caps the top end where it multiplies. */
static inline int gameSimDamageScale(GameSim *sim, BYTE attacker, BYTE victim,
                                     BYTE cause) {
  int pct;

  if (sim == NULL || sim->callbacks.damageScale == NULL) {
    return 100;
  }
  pct = sim->callbacks.damageScale(sim->callbacks.ctx, attacker, victim, cause);
  return (pct < 0) ? 0 : pct;
}

/* The start the host names for a player, or FALSE for the engine's pick. */
static inline bool gameSimChooseStart(GameSim *sim, BYTE player, BYTE *startIdx) {
  if (sim->callbacks.chooseStart == NULL) {
    return FALSE;
  }
  return sim->callbacks.chooseStart(sim->callbacks.ctx, player, startIdx);
}

/* What a spawning tank is handed, or FALSE for the game type's loadout. */
static inline bool gameSimSpawnLoadout(GameSim *sim, BYTE player, BYTE *shells,
                                       BYTE *mines, BYTE *armour, BYTE *trees) {
  if (sim->callbacks.spawnLoadout == NULL) {
    return FALSE;
  }
  return sim->callbacks.spawnLoadout(sim->callbacks.ctx, player, shells, mines,
                                     armour, trees);
}

/* Whether a dead tank may come back this tick. */
static inline bool gameSimCanRespawn(GameSim *sim, BYTE player) {
  if (sim->callbacks.canRespawn == NULL) {
    return TRUE;
  }
  return sim->callbacks.canRespawn(sim->callbacks.ctx, player);
}

static inline bool gameSimCanBuild(GameSim *sim, BYTE player, BYTE action,
                                   BYTE mapX, BYTE mapY, BYTE pillIdx) {
  if (sim->callbacks.canBuild == NULL) {
    return TRUE;
  }
  return sim->callbacks.canBuild(sim->callbacks.ctx, player, action, mapX,
                                 mapY, pillIdx);
}

static inline bool gameSimCanCapture(GameSim *sim, BYTE kind, BYTE index,
                                     BYTE player) {
  if (sim->callbacks.canCapture == NULL) {
    return TRUE;
  }
  return sim->callbacks.canCapture(sim->callbacks.ctx, kind, index, player);
}

static inline bool gameSimCanDie(GameSim *sim, BYTE kind, BYTE index,
                                 BYTE killer, BYTE cause) {
  if (sim->callbacks.canDie == NULL) {
    return TRUE;
  }
  return sim->callbacks.canDie(sim->callbacks.ctx, kind, index, killer, cause);
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
