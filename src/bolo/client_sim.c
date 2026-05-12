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
 *Name:          Client Simulation
 *Filename:      client_sim.c
 *Author:        John Morrison
 *Purpose:
 *  ClientSim lifecycle API and tick functions.
 *  Moved from screen.c as part of the client/server
 *  simulation separation.
 *********************************************************/

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "client_sim.h"
#include "client_sim_internal.h"
#include "client_state.h"
#include "screen.h"
#include "global.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "tank.h"
#include "shells.h"
#include "rubble.h"
#include "explosions.h"
#include "messages.h"
#include "grass.h"
#include "swamp.h"
#include "lgm.h"
#include "floodfill.h"
#include "minesexp.h"
#include "treegrow.h"
#include "mines.h"
#include "labels.h"
#include "players.h"
#include "tankexp.h"
#include "log.h"
#include "screenbrainmap.h"
#include "util.h"
#include "netpacks.h"
#include "../gui/lang.h"
#include "../gui/dnsLookups.h"
#include "../gui/clientmutex.h"
#include "../gui/dialogAlliance.h"
#include "../winbolonet/winbolonet.h"
#include "frontend.h"

/* Must match the value used inside shellsAddItem (shells.c redefines
 * SHELL_START_ADD from 6 to 5 locally). */
#undef SHELL_START_ADD
#define SHELL_START_ADD 5

/* Forward declarations */
static void clientSimAddPredictedShellAt(ClientSim *cs, WORLD wx, WORLD wy, TURNTYPE angle, tank *tk, uint32_t fireTick);

/* GameSim callback wrappers for the client side */
static void csCallbackMessageAdd(void *ctx, messageType msgType,
                                 langid topId, langid bodyId,
                                 const MessageArgs *args) {
  ClientSim *cs = (ClientSim *)ctx;
  /* Render at receive time using the client's currently-loaded language.
   * langGetTextFmt returns a pointer into a small thread-local ring of
   * buffers, so copy the result before any further langGet* call could
   * recycle the slot. The message log copies the strings character by
   * character in messageAddItem, so once clientMessageAdd returns the
   * stack buffers are safe to drop. Switching language mid-game does
   * not retranslate prior log entries — that's by design. */
  char topBuf[FILENAME_MAX];
  char bodyBuf[FILENAME_MAX];
  const char *topRendered = langGetText(topId);
  const char *bodyRendered = langGetTextFmt(bodyId, args);
  topBuf[0] = '\0';
  bodyBuf[0] = '\0';
  if (topRendered) {
    strncpy(topBuf, topRendered, sizeof(topBuf) - 1);
    topBuf[sizeof(topBuf) - 1] = '\0';
  }
  if (bodyRendered) {
    strncpy(bodyBuf, bodyRendered, sizeof(bodyBuf) - 1);
    bodyBuf[sizeof(bodyBuf) - 1] = '\0';
  }
  clientMessageAdd(&cs->messages, msgType, topBuf, bodyBuf);
}

static void csCallbackSoundDist(void *ctx, sndEffects value, BYTE mx, BYTE my) {
  ClientSim *cs = (ClientSim *)ctx;
  if (cs->sim.isPredicting) return;
  clientSoundDist(&cs->sim, value, mx, my);
}

static void csCallbackCenterTank(void *ctx) {
  ClientSim *cs = (ClientSim *)ctx;
  screenTankViewCS(cs);
}

static void csCallbackSoundDistShoot(void *ctx, BYTE mx, BYTE my, BYTE owner) {
  /* no-op — client plays shootSelf via prediction; other players' shoots arrive via EVENT_SOUND_SHOOT */
}

static void csCallbackSoundDistTankHit(void *ctx, BYTE mx, BYTE my, BYTE hitPlayer) {
  /* no-op — client receives tank hit sounds via EVENT_SOUND_TANK_HIT from server */
}

static void csCallbackTankKill(void *ctx, BYTE killer, BYTE killed, BYTE deathCause, BYTE carriedPills) {
  (void)deathCause; (void)carriedPills;
  /* no-op — client receives kills via EVENT_TANK_KILLED from server */
}

static void csCallbackConsoleMessage(void *ctx, char *msg) {
  /* no-op on the client */
}

static void csCallbackMineVisible(void *ctx, BYTE mx, BYTE my, BYTE sourcePlayer) {
  /* no-op — client receives mine visibility via EVENT_MINE_VISIBLE from server */
}

/* Brain state now lives inside the ClientSim struct (see client_sim.h).
 * The static globals were removed in the Phase 0 refactor. */

/*********************************************************
 *NAME:          clientSimAlloc
 *PURPOSE:
 *  Heap-allocates a zero-initialised ClientSim. Pairs with
 *  clientSimDestroy, which frees the returned pointer.
 *********************************************************/
ClientSim *clientSimAlloc(void) {
  return (ClientSim *)calloc(1, sizeof(ClientSim));
}

/*********************************************************
 *NAME:          clientSimCreate
 *PURPOSE:
 *  Initializes a ClientSim struct with all simulation state.
 *  This is the simulation-only initialization; rendering
 *  setup is handled separately by viewportInit().
 *
 *ARGUMENTS:
 *  cs         - Pointer to the ClientSim to initialize
 *  game       - The game type-Open/tournament/strict tournament
 *  hiddenMines - Are hidden mines allowed
 *  srtDelay   - Game start delay (50th second increments)
 *  gmeLen     - Length of the game (in 50ths) (-1 =unlimited)
 *********************************************************/
bool clientSimCreate(ClientSim *cs, gameType game, bool hiddenMines, int srtDelay, int32_t gmeLen) {
  (void)srtDelay;  /* Used by screen.c for display */
  (void)gmeLen;    /* Used by screen.c for display */
  
  memset(cs, 0, sizeof(*cs));
  cs->myPlayerNum = 0;
  cs->sim.viewPlayer = 0;

  /* Sentinel value for "no batch start assigned" — clients never run the
   * batch placement, but startsGetStart still checks the slot when bots /
   * single-player tankCreate runs, so leave them all unset. */
  for (int i = 0; i < MAX_TANKS; i++) {
    cs->sim.pendingStartIdx[i] = MAX_STARTS;
  }

  /* Initialize GameSim identity and callbacks */
  cs->sim.isServer = false;
  cs->sim.isLocalTransport = true;
  cs->sim.hiddenMines = hiddenMines;
  cs->sim.callbacks.messageAdd = csCallbackMessageAdd;
  cs->sim.callbacks.soundDist = csCallbackSoundDist;
  cs->sim.callbacks.soundDistShoot = csCallbackSoundDistShoot;
  cs->sim.callbacks.soundDistTankHit = csCallbackSoundDistTankHit;
  cs->sim.callbacks.tankKill = csCallbackTankKill;
  cs->sim.callbacks.centerTank = csCallbackCenterTank;
  cs->sim.callbacks.consoleMessage = csCallbackConsoleMessage;
  cs->sim.callbacks.mineVisible = csCallbackMineVisible;
  cs->sim.callbacks.explosion = NULL;
  cs->sim.callbacks.tkExplosion = NULL;
  cs->sim.callbacks.ctx = cs;

  cs->currentBuildSelect = BsTrees;
  cs->labelOwnTank = TRUE;
  cs->labelMessage = lblShort;
  cs->labelTankLabel = lblShort;

  minesCreate(&cs->sim.mns, hiddenMines);
  gameTypeSet(&cs->sim.game, game);
  mapCreate(&cs->sim.mp);
  startsCreate(&cs->sim.ss);
  basesCreate(&cs->sim.bs);
  playersCreate(&cs->sim.plyrs, FALSE);
  cs->sim.shs = shellsCreate();
  cs->serverShellCount = 0;
  explosionsCreate(&cs->sim.expl);
  rubbleCreate(&cs->sim.rbl);
  buildingCreate(&cs->sim.blds);
  messageCreate(&cs->messages);
  scrollCreate(&cs->scroll);
  grassCreate(&cs->sim.grs);
  swampCreate(&cs->sim.swp);
  MY_LGM(cs) = lgmCreate(cs->myPlayerNum);
  floodCreate(&cs->sim.ff);
  tkExplosionCreate(&cs->sim.tankExplosions);
  minesExpCreate(&cs->sim.minesExplosions);
  treeGrowCreate(&cs->sim);
  /* Init base timers (was in basesCreate, now lives in GameSim) */
  {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
      cs->sim.baseTimer[i] = 30000;
    }
    cs->sim.baseTimer[cs->myPlayerNum] = BASE_TICKS_BETWEEN_REFUEL;
  }
  pillsCreate(&cs->sim.pb);
  screenBrainMapCreate(cs);
  
  /* Initialize brain state (now per-instance in the struct).
   * memset above already zeroed the scalar fields. */
  cs->brainBuildInfo = (BuildInfo*) malloc(sizeof(BuildInfo));
  if (cs->brainBuildInfo != NULL) {
    cs->brainBuildInfo->action = 0;
  }

  clientStateCreate(&cs->clientState);
  interpCreate(&cs->interpCtx, 0);

  cs->sim.inStartFind = TRUE;
  cs->running = TRUE;

  /* Network state defaults (memset already zeroed pointers) */
  cs->networkGameType = netNone;
  cs->netStat = netRunning;

  /* Lobby state defaults (memset already zeroed, but be explicit) */
  memset(cs->lobbySlots, 0, sizeof(cs->lobbySlots));
  cs->countdownSeconds = 0;
  cs->mapDownloadComplete = false;
  cs->inLobby = false;

  return true;
}

void clientSimSetPlayerNum(ClientSim *cs, BYTE playerNum) {
    cs->myPlayerNum = playerNum;
    cs->sim.viewPlayer = playerNum;
    if (playerNum != 0) {
        cs->sim.tanks[playerNum] = cs->sim.tanks[0];
        cs->sim.tanks[0] = NULL;
        cs->sim.lgmen[playerNum] = cs->sim.lgmen[0];
        cs->sim.lgmen[0] = NULL;
        lgmSetPlayerNum(&cs->sim.lgmen[playerNum], playerNum);
        cs->sim.baseTimer[playerNum] = BASE_TICKS_BETWEEN_REFUEL;
    }
}

void clientSimSetupSelf(ClientSim *cs, BYTE playerNum,
                        const char *playerName,
                        uint8_t clientType, uint8_t clientFlags) {
    if (MY_TANK(cs) != NULL) {
        tankDestroy(&cs->sim, &MY_TANK(cs));
        MY_TANK(cs) = NULL;
    }
    tankCreate(&cs->sim, &MY_TANK(cs));
    playersSetSelf(cs, &cs->sim, &cs->sim.plyrs,
                   (playerNumbers)playerNum,
                   (char *)playerName, TRUE);
    playersSetClientType (&cs->sim.plyrs, playerNum, clientType);
    playersSetClientFlags(&cs->sim.plyrs, playerNum, clientFlags);
}

/*********************************************************
 *NAME:          clientSimDestroy
 *PURPOSE:
 *  Cleans up all simulation state in a ClientSim struct and
 *  frees the cs pointer (pairs with clientSimAlloc).
 *  Rendering cleanup is handled separately by viewportDestroy().
 *  Accepts NULL as a no-op.
 *
 *ARGUMENTS:
 *  cs - Pointer to the ClientSim to destroy and free
 *********************************************************/
void clientSimDestroy(ClientSim *cs) {
  if (cs == NULL) return;
  cs->running = FALSE;
  clientStateDestroy(&cs->clientState);
  tankDestroy(&cs->sim, &MY_TANK(cs));
  MY_TANK(cs) = NULL;
  mapDestroy(&cs->sim.mp);
  startsDestroy(&cs->sim.ss);
  basesDestroy(&cs->sim.bs);
  shellsDestroy(&cs->sim.shs);
  cs->serverShellCount = 0;
  explosionsDestroy(&cs->sim.expl);
  rubbleDestroy(&cs->sim.rbl);
  buildingDestroy(&cs->sim.blds);
  messageDestroy(&cs->messages);
  grassDestroy(&cs->sim.grs);
  floodDestroy(&cs->sim.ff);
  lgmDestroy(&MY_LGM(cs));
  swampDestroy(&cs->sim.swp);
  cs->sim.swp = NULL;
  screenBrainMapDestroy(cs);
  tkExplosionDestroy(&cs->sim.tankExplosions);
  minesExpDestroy(&cs->sim.minesExplosions);
  treeGrowDestroy(&cs->sim);
  pillsDestroy(&cs->sim.pb);
  playersDestroy(&cs->sim.plyrs);
  
  if (cs->brainBuildInfo != NULL) {
    free(cs->brainBuildInfo);
    cs->brainBuildInfo = NULL;
  }
  
  cs->sim.mp = NULL;
  cs->sim.bs = NULL;
  cs->sim.pb = NULL;
  cs->sim.ss = NULL;
  MY_TANK(cs) = NULL;
  cs->sim.shs = NULL;
  MY_LGM(cs) = NULL;
  cs->sim.plyrs = NULL;

  /* Clear lobby chat buffer */
  cs->lobbyChatHistory[0] = '\0';

  /* Clear network callbacks */
  cs->chatSendFunc = NULL;
  cs->nameChangeSendFunc = NULL;
  cs->allianceRequestFunc = NULL;
  cs->allianceAcceptFunc = NULL;
  cs->allianceLeaveFunc = NULL;
  cs->lockToggleSendFunc = NULL;

  free(cs);
}

/*********************************************************
 *NAME:          clientSimKeysTick
 *PURPOSE:
 *  Records the input and applies prediction for a keys
 *  tick (tankTurn only). Called BEFORE the server tick.
 *
 *ARGUMENTS:
 *  cs  - Pointer to the ClientSim
 *  pkt - The InputPacket for this tick
 *********************************************************/
void clientSimKeysTick(ClientSim *cs, const InputPacket *pkt) {
  if (!cs->clientState.initialized || !cs->clientState.hasPredictedTank) {
    return;
  }
  clientStateRecordInput(&cs->clientState, pkt);
  clientStatePredictTick(cs, &cs->clientState, pkt, &MY_TANK(cs), &cs->sim, TRUE, FALSE);
}

/*********************************************************
 *NAME:          clientSimGameTick
 *PURPOSE:
 *  Records the input and applies prediction for a full
 *  game tick (tankUpdate). Called BEFORE the server tick.
 *
 *ARGUMENTS:
 *  cs      - Pointer to the ClientSim
 *  pkt     - The InputPacket for this tick
 *  isBrain - TRUE if a brain is running
 *********************************************************/
void clientSimGameTick(ClientSim *cs, const InputPacket *pkt, bool isBrain) {
  WORLD preX = 0, preY = 0;
  TURNTYPE preAngle = 0;
  bool canFire = false;

  if (!cs->clientState.initialized || !cs->clientState.hasPredictedTank) {
    return;
  }

  /* Capture pre-movement state for shell prediction. The server fires
   * the shell BEFORE moving the tank inside tankUpdate, so we need the
   * tank's position before clientStatePredictTick moves it.
   * Check reload <= 1 because tankUpdate decrements reload before the
   * fire check — a reload of 1 becomes 0 and allows firing. */
  if ((pkt->actions & INPUT_ACTION_FIRE) && MY_TANK(cs) != NULL) {
    tank *tk = &MY_TANK(cs);
    if (tankGetReloadTime(tk) <= 1 &&
        tankGetShells(tk) > 0 &&
        tankGetArmour(tk) <= TANK_FULL_ARMOUR) {
      tankGetWorld(tk, &preX, &preY);
      preAngle = tankGetAngle(tk);
      canFire = true;
    }
  }

  clientStateRecordInput(&cs->clientState, pkt);
  clientStatePredictTick(cs, &cs->clientState, pkt, &MY_TANK(cs), &cs->sim, FALSE, isBrain);

  /* Create predicted shell using pre-movement position to match server */
  if (canFire) {
    if (!isBrain) {
      frontEndPlaySound(cs, shootSelf);
    }
    clientSimAddPredictedShellAt(cs, preX, preY, preAngle, &MY_TANK(cs), pkt->tick);
    /* Update predicted tank state to match what the server will do */
    tankSetReload(&MY_TANK(cs), TANK_RELOAD_TIME);
    tankSetShells(&MY_TANK(cs), tankGetShells(&MY_TANK(cs)) - 1);
  }

  /* Advance existing predicted shells */
  clientSimAdvancePredictedShells(cs);
}

void clientSimSyncFromSnapshot(ClientSim *cs, const SnapshotHeader *hdr,
                               const TankSnapshot *tanks, int tankCount,
                               const ShellSnapshot *shellSnaps, int shellCount,
                               const TkExplosionSnapshot *tkExplSnaps, int tkExplosionCount,
                               const BaseSnapshot *baseSnaps, int baseCount,
                               const PillSnapshot *pillSnaps, int pillCount,
                               const GameEvent *events, int eventCount,
                               BYTE playerNum) {
  screenSyncFromSnapshotCS(cs, hdr, tanks, tankCount, shellSnaps, shellCount,
                           tkExplSnaps, tkExplosionCount,
                           baseSnaps, baseCount,
                           pillSnaps, pillCount, events, eventCount, playerNum);
}

void clientSimDisplayTick(ClientSim *cs, bool isBrain) {
  screenSimDisplayTickCS(cs, isBrain);
  basesTickMessageQueue(&cs->sim, cs);
}

/*********************************************************
 *NAME:          clientSimAddPredictedShell
 *PURPOSE:
 *  Creates a predicted shell matching what the server will
 *  create from the same fire input. Uses the same parameters
 *  as shellsAddItem.
 *********************************************************/
static void clientSimAddPredictedShellAt(ClientSim *cs, WORLD wx, WORLD wy, TURNTYPE angle, tank *tk, uint32_t fireTick) {
  PredictedShell *ps;
  int xAdd, yAdd;
  TURNTYPE sightLen;

  if (cs->predictedShellCount >= MAX_PREDICTED_SHELLS) {
    return;
  }

  sightLen = tankGetGunsightLength(tk);

  /* Replicate shellsAddItem's offset calculation */
  utilCalcDistance(&xAdd, &yAdd, angle, SHELL_SPEED);
  wx = (WORLD)(wx + SHELL_START_ADD * xAdd);
  wy = (WORLD)(wy + SHELL_START_ADD * yAdd);

  ps = &cs->predictedShells[cs->predictedShellCount];
  ps->x = wx;
  ps->y = wy;
  ps->fx = (float)(int)wx;
  ps->fy = (float)(int)wy;
  {
    int32_t xStepHP, yStepHP;
    utilCalcDistanceHP(&xStepHP, &yStepHP, angle, SHELL_SPEED);
    ps->vx = (float)xStepHP / 256.0f;
    ps->vy = (float)yStepHP / 256.0f;
  }
  ps->angle = angle;
  ps->length = (uint8_t)(1 + (SHELL_LIFE * (sightLen / 2)) - SHELL_START_ADD);
  ps->owner = gameSimGetTankPlayer(&cs->sim, tk);
  ps->onBoat = tankIsOnBoat(tk);
  ps->fireTick = fireTick;
  ps->active = true;
  cs->predictedShellCount++;
}

/*********************************************************
 *NAME:          clientSimAdvancePredictedShells
 *PURPOSE:
 *  Moves each predicted shell forward by one tick and
 *  removes any that have expired.
 *********************************************************/
void clientSimAdvancePredictedShells(ClientSim *cs) {
  int i;
  for (i = 0; i < cs->predictedShellCount; ) {
    PredictedShell *ps = &cs->predictedShells[i];
    WORLD newX, newY;
    BYTE mapX, mapY;
    if (ps->length <= SHELL_DEATH) {
      /* Shell expired — remove by swapping with last */
      cs->predictedShells[i] = cs->predictedShells[cs->predictedShellCount - 1];
      cs->predictedShellCount--;
      continue;
    }
    /* Move shell forward using float accumulation — matches server HP precision */
    ps->fx += ps->vx;
    ps->fy += ps->vy;
    newX = (WORLD)(int)ps->fx;
    newY = (WORLD)(int)ps->fy;

    /* Client-side collision check: remove predicted shell if it hits
     * a pillbox, impassable terrain, or a hostile base. This is purely
     * visual — the server does the authoritative collision. */
    mapX = (BYTE)(newX >> TANK_SHIFT_MAPSIZE);
    mapY = (BYTE)(newY >> TANK_SHIFT_MAPSIZE);
    if (pillsIsPillHit(&cs->sim.pb, mapX, mapY) ||
        (!mapIsPassable(&cs->sim.mp, mapX, mapY, ps->onBoat) &&
         !basesExistPos(&cs->sim.bs, mapX, mapY)) ||
        (basesExistPos(&cs->sim.bs, mapX, mapY) &&
         (ps->onBoat || basesCanHit(&cs->sim, mapX, mapY, ps->owner)))) {
      cs->predictedShells[i] = cs->predictedShells[cs->predictedShellCount - 1];
      cs->predictedShellCount--;
      continue;
    }

    /* Check collision with other players' tanks (interpolated positions) */
    {
      BYTE p;
      bool tankHit = false;
      for (p = 0; p < MAX_TANKS && !tankHit; p++) {
        WORLD tkX, tkY;
        TURNTYPE tkAngle;
        bool tkOnBoat;
        if (p == cs->interpCtx.localPlayer) continue;
        if (!interpIsAlive(&cs->interpCtx, p)) continue;
        if (interpGetPosition(&cs->interpCtx, p, 1.0f, &tkX, &tkY, &tkAngle, &tkOnBoat)) {
          if (abs((int)newX - (int)tkX) < 128 && abs((int)newY - (int)tkY) < 128) {
            tankHit = true;
          }
        }
      }
      if (tankHit) {
        cs->predictedShells[i] = cs->predictedShells[cs->predictedShellCount - 1];
        cs->predictedShellCount--;
        continue;
      }
    }

    ps->x = newX;
    ps->y = newY;
    ps->length--;
    i++;
  }
}

/*********************************************************
 *NAME:          clientSimReconcilePredictedShells
 *PURPOSE:
 *  No-op. Predicted shells are no longer reconciled against
 *  server state — they expire naturally via their length
 *  counter in clientSimAdvancePredictedShells. Server
 *  shells owned by the local player are filtered from
 *  rendering so there is no duplication.
 *********************************************************/
void clientSimReconcilePredictedShells(ClientSim *cs, uint32_t lastProcessedInput) {
  (void)cs;
  (void)lastProcessedInput;
}

/*********************************************************
 *NAME:          clientSimGetBrainState
 *PURPOSE:
 *  Returns pointers to brain state variables within the
 *  given ClientSim instance.
 *********************************************************/
uint32_t *clientSimGetBrainHoldKeys(ClientSim *cs) { return &cs->brainHoldKeys; }
uint32_t *clientSimGetBrainTapKeys(ClientSim *cs) { return &cs->brainTapKeys; }
BuildInfo **clientSimGetBrainBuildInfo(ClientSim *cs) { return &cs->brainBuildInfo; }
PlayerBitMap *clientSimGetBrainsWantAllies(ClientSim *cs) { return &cs->brainsWantAllies; }
PlayerBitMap *clientSimGetBrainsMessageDest(ClientSim *cs) { return &cs->brainsMessageDest; }
char *clientSimGetBrainsMessage(ClientSim *cs) { return cs->brainsMessage; }
unsigned short *clientSimGetBrainsNumObjects(ClientSim *cs) { return &cs->brainsNumObjects; }
ObjectInfo *clientSimGetBrainObjects(ClientSim *cs) { return cs->brainObjects; }
aiType *clientSimGetAllowComputerTanks(ClientSim *cs) { return &cs->allowComputerTanks; }

/* -------------------------------------------------------
 * Network state accessors (per-instance)
 * ------------------------------------------------------- */
netType clientSimGetNetType(ClientSim *cs) { return cs->networkGameType; }
netStatus clientSimGetNetStatus(ClientSim *cs) { return cs->netStat; }
void clientSimSetNetType(ClientSim *cs, netType value) { cs->networkGameType = value; }
void clientSimSetNetStatus(ClientSim *cs, netStatus value) { cs->netStat = value; }

void clientSimSetChatSendFunc(ClientSim *cs, NetChatSendFunc func) { cs->chatSendFunc = func; }
void clientSimSetNameChangeSendFunc(ClientSim *cs, NetNameChangeSendFunc func) { cs->nameChangeSendFunc = func; }
void clientSimSetAllianceRequestFunc(ClientSim *cs, NetAllianceRequestFunc func) { cs->allianceRequestFunc = func; }
void clientSimSetAllianceAcceptFunc(ClientSim *cs, NetAllianceAcceptFunc func) { cs->allianceAcceptFunc = func; }
void clientSimSetAllianceLeaveFunc(ClientSim *cs, NetAllianceLeaveFunc func) { cs->allianceLeaveFunc = func; }
void clientSimSetLockToggleSendFunc(ClientSim *cs, NetLockToggleSendFunc func) { cs->lockToggleSendFunc = func; }

void clientSimAppendLobbyChat(ClientSim *cs, const char *name, const char *message) {
  size_t histLen = strlen(cs->lobbyChatHistory);
  size_t needed = strlen(name) + 2 + strlen(message) + 2; /* "name: message\n" */
  if (histLen + needed < sizeof(cs->lobbyChatHistory)) {
    if (histLen > 0) {
      strcat(cs->lobbyChatHistory, "\n");
    }
    strcat(cs->lobbyChatHistory, name);
    strcat(cs->lobbyChatHistory, ": ");
    strcat(cs->lobbyChatHistory, message);
  }
}

void clientSimMessageSendAllPlayers(ClientSim *cs, BYTE playerNum, char *message) {
  if (cs->chatSendFunc != NULL) {
    cs->chatSendFunc(0xFF, message);
  }
}

void clientSimMessageSendPlayer(ClientSim *cs, BYTE playerNum, BYTE destPlayer, char *message) {
  if (cs->chatSendFunc != NULL) {
    cs->chatSendFunc(destPlayer, message);
  }
}

void clientSimSendChangePlayerName(ClientSim *cs, BYTE playerNum, char *newName) {
  if (cs->nameChangeSendFunc != NULL) {
    cs->nameChangeSendFunc(newName);
  }
}

void clientSimRequestAlliance(ClientSim *cs, BYTE playerNum, BYTE requestTo) {
  if (cs->allianceRequestFunc != NULL) {
    cs->allianceRequestFunc(requestTo);
  }
}

void clientSimAllianceAccept(ClientSim *cs, BYTE playerNum) {
  if (cs->allianceAcceptFunc != NULL) {
    if (playersIsInUse(&cs->sim.plyrs, playerNum) == TRUE) {
      cs->allianceAcceptFunc(playerNum);
    }
  }
}

void clientSimLeaveAlliance(ClientSim *cs, BYTE playerNum) {
  if (cs->allianceLeaveFunc != NULL) {
    cs->allianceLeaveFunc();
  }
}

void clientSimSetAllowNewPlayers(ClientSim *cs, bool allow) {
  if (cs->lockToggleSendFunc != NULL) {
    cs->lockToggleSendFunc(allow);
  }
}


void netGetStats(ClientSim *cs, char *status, int *ping, int *ppsec, int *retrans) {
  if (cs->netStat == netFailed) {
    strcpy(status, langGetText(NET_STATUS_FAILED));
  } else {
    strcpy(status, langGetText(NET_STATUS_OK));
  }
  *ping = 0;
  *ppsec = 0;
  *retrans = 0;
}

void netGetServerAddressStr(ClientSim *cs, char *dest) {
  if (cs->networkGameType == netUdp) {
    strcpy(dest, "Network Game");
  } else {
    strcpy(dest, NET_SINGLE_PLAYER_GAME);
  }
}

void netGetOurAddressStr(ClientSim *cs, char *dest) {
  if (cs->networkGameType == netUdp) {
    dest[0] = '\0';
  } else {
    strcpy(dest, NET_SINGLE_PLAYER_GAME);
  }
}

BYTE netGetDownloadPos(void) {
  return 255; /* complete */
}

void netSecond(void) {
  /* no-op — old ring protocol timing removed */
}

int netGetNetTime(void) {
  return 0;
}

void netSendTrackerUpdate(void) {
  /* no-op — tracker update removed */
}

/* Alliance dialog window — created/destroyed with network lifecycle */
static void *dlgAllianceWnd = NULL;

bool netSetup(ClientSim *cs, netType value, unsigned short myPort, char *targetIp, unsigned short targetPort, char *password, bool usCreate, char *trackerAddr, unsigned short trackerPort, bool useTracker, bool wantRejoin, bool useWinboloNet, char *wbnToken) {
  cs->networkGameType = value;
  cs->netStat = netRunning;
  dlgAllianceWnd = dialogAllianceCreate();

  if (value == netUdp) {
    dnsLookupsCreate(cs);
  }
  return TRUE;
}

void netDestroy(ClientSim *cs) {
  if (dlgAllianceWnd != NULL) {
    dialogAllianceDestroy(dlgAllianceWnd);
    dlgAllianceWnd = NULL;
  }
  if (cs->networkGameType == netUdp) {
    dnsLookupsDestroy();
  }
  if (winbolonetIsRunning()) {
    winbolonetDestroy(FALSE);
  }
  cs->networkGameType = netNone;
  cs->netStat = netRunning;
  cs->chatSendFunc = NULL;
  cs->nameChangeSendFunc = NULL;
  cs->allianceRequestFunc = NULL;
  cs->allianceAcceptFunc = NULL;
  cs->allianceLeaveFunc = NULL;
  cs->lockToggleSendFunc = NULL;
}

/* DNS lookup completion callback — called from dns_lookups.c thread */
void netProcessedDnsLookup(ClientSim *cs, char *ip, char *host) {
  clientMutexWaitFor();
  playerSetLocation(&cs->sim.plyrs, ip, host);
  clientMutexRelease();
}

/* ================================================================
 * Read accessors — see header for the contract.
 * ================================================================ */

bool clientSimIsRunning(const ClientSim *cs)              { return cs->running; }
bool clientSimIsBot(const ClientSim *cs)                  { return cs->isBot; }
bool clientSimIsInPillView(const ClientSim *cs)           { return cs->viewport.inPillView; }
bool clientSimIsNeedScreenReCalc(const ClientSim *cs)     { return cs->viewport.needRecalc; }
bool clientSimIsInLobby(const ClientSim *cs)              { return cs->inLobby; }
bool clientSimIsMapDownloadComplete(const ClientSim *cs)  { return cs->mapDownloadComplete; }
bool clientSimIsMapSkipAvailable(const ClientSim *cs)     { return cs->mapSkipAvailable; }
bool clientSimIsMapSkipMyVote(const ClientSim *cs)        { return cs->mapSkipMyVote; }
bool clientSimIsLobbyHiddenMines(const ClientSim *cs)     { return cs->lobbyHiddenMines; }
bool clientSimIsBalanceProposalActive(const ClientSim *cs){ return cs->balanceProposalActive; }
bool clientSimIsLabelOwnTank(const ClientSim *cs)         { return cs->labelOwnTank; }

buildSelect clientSimGetCurrentBuildSelect(const ClientSim *cs) { return cs->currentBuildSelect; }
gameType    clientSimGetLobbyGameType(const ClientSim *cs)      { return cs->lobbyGameType; }
labelLen    clientSimGetLabelMessage(const ClientSim *cs)       { return cs->labelMessage; }
labelLen    clientSimGetLabelTankLabel(const ClientSim *cs)     { return cs->labelTankLabel; }

BYTE     clientSimGetMyPlayerNum(const ClientSim *cs)       { return cs->myPlayerNum; }
BYTE     clientSimGetXOffset(const ClientSim *cs)           { return cs->viewport.xOffset; }
BYTE     clientSimGetYOffset(const ClientSim *cs)           { return cs->viewport.yOffset; }
BYTE     clientSimGetPillViewX(const ClientSim *cs)         { return cs->viewport.pillViewX; }
BYTE     clientSimGetPillViewY(const ClientSim *cs)         { return cs->viewport.pillViewY; }
BYTE     clientSimGetPendingBuildAction(const ClientSim *cs){ return cs->pendingBuildAction; }
BYTE     clientSimGetPendingBuildX(const ClientSim *cs)     { return cs->pendingBuildX; }
BYTE     clientSimGetPendingBuildY(const ClientSim *cs)     { return cs->pendingBuildY; }
int      clientSimGetCursorPosX(const ClientSim *cs)        { return cs->viewport.cursorPosX; }
int      clientSimGetCursorPosY(const ClientSim *cs)        { return cs->viewport.cursorPosY; }
int      clientSimGetGmeStartDelay(const ClientSim *cs)     { return cs->gmeStartDelay; }
int      clientSimGetCountdownSeconds(const ClientSim *cs)  { return cs->countdownSeconds; }
int      clientSimGetServerShellCount(const ClientSim *cs)  { return cs->serverShellCount; }
int      clientSimGetPredictedShellCount(const ClientSim *cs){ return cs->predictedShellCount; }
int      clientSimGetBrainEventCount(const ClientSim *cs)   { return cs->brainEventCount; }
int32_t  clientSimGetGmeLength(const ClientSim *cs)         { return cs->gmeLength; }
int32_t  clientSimGetLobbyTimeLimit(const ClientSim *cs)    { return cs->lobbyTimeLimit; }
uint8_t  clientSimGetLobbyAiType(const ClientSim *cs)       { return cs->lobbyAiType; }
uint8_t  clientSimGetLobbyPillCount(const ClientSim *cs)    { return cs->lobbyPillCount; }
uint8_t  clientSimGetLobbyBaseCount(const ClientSim *cs)    { return cs->lobbyBaseCount; }
uint8_t  clientSimGetLobbyStartCount(const ClientSim *cs)   { return cs->lobbyStartCount; }
uint8_t  clientSimGetBrainLastAssistMsg(const ClientSim *cs){ return cs->brainLastAssistMsg; }
uint32_t clientSimGetLastServerTick(const ClientSim *cs)    { return cs->lastServerTick; }
unsigned short clientSimGetServerPort(const ClientSim *cs)  { return cs->serverPort; }
time_t   clientSimGetTimeStart(const ClientSim *cs)         { return cs->timeStart; }

const char *clientSimGetMapName(const ClientSim *cs)          { return cs->mapName; }
const char *clientSimGetLobbyChatHistory(const ClientSim *cs) { return cs->lobbyChatHistory; }
const char *clientSimGetMyLastPlayerName(const ClientSim *cs) { return cs->myLastPlayerName; }

const ClientLobbySlot *clientSimGetLobbySlot(const ClientSim *cs, BYTE n) {
  if (n >= 16) return NULL;
  return &cs->lobbySlots[n];
}

bool clientSimIsMapSkipVote(const ClientSim *cs, BYTE n) {
  if (n >= 16) return false;
  return cs->mapSkipVotes[n];
}

uint8_t clientSimGetBalanceProposal(const ClientSim *cs, BYTE n) {
  if (n >= 16) return 0;
  return cs->balanceProposal[n];
}

BYTE *clientSimGetBrainMap(ClientSim *cs) {
  return &cs->brainMap[0][0];
}

const ShellSnapshot *clientSimGetServerShellSnaps(const ClientSim *cs) {
  return cs->serverShellSnaps;
}

const PredictedShell *clientSimGetPredictedShells(const ClientSim *cs) {
  return cs->predictedShells;
}

const GameEvent *clientSimGetBrainEvents(const ClientSim *cs) {
  return cs->brainEvents;
}

struct in_addr clientSimGetServerAddress(const ClientSim *cs) {
  return cs->serverAddress;
}

GameSim       *clientSimGetGameSim(ClientSim *cs)    { return &cs->sim; }
MessageState  *clientSimGetMessages(ClientSim *cs)   { return &cs->messages; }
ScrollState   *clientSimGetScroll(ClientSim *cs)     { return &cs->scroll; }
InterpContext *clientSimGetInterpCtx(ClientSim *cs)  { return &cs->interpCtx; }
screen        *clientSimGetView(ClientSim *cs)       { return &cs->viewport.view; }
screenMines   *clientSimGetMineView(ClientSim *cs)   { return &cs->viewport.mineView; }

const struct ViewPort *clientSimViewport(const ClientSim *cs)  { return &cs->viewport; }
struct ViewPort       *clientSimViewportMut(ClientSim *cs)     { return &cs->viewport; }

BYTE *clientSimGetXOffsetPtr(ClientSim *cs)          { return &cs->viewport.xOffset; }
BYTE *clientSimGetYOffsetPtr(ClientSim *cs)          { return &cs->viewport.yOffset; }
BYTE *clientSimGetPillViewXPtr(ClientSim *cs)        { return &cs->viewport.pillViewX; }
BYTE *clientSimGetPillViewYPtr(ClientSim *cs)        { return &cs->viewport.pillViewY; }

char *clientSimGetMapNameMutable(ClientSim *cs)      { return cs->mapName; }

/* ================================================================
 * Mutators — see header for the contract.
 * ================================================================ */

void clientSimSetBalanceProposalActive(ClientSim *cs, bool active) {
  cs->balanceProposalActive = active;
}

void clientSimClearBalanceProposal(ClientSim *cs) {
  memset(cs->balanceProposal, 0, sizeof(cs->balanceProposal));
}

void clientSimSetMapSkipMyVote(ClientSim *cs, bool vote) {
  cs->mapSkipMyVote = vote;
}

void clientSimSetXOffset(ClientSim *cs, BYTE v)            { cs->viewport.xOffset = v; }
void clientSimSetYOffset(ClientSim *cs, BYTE v)            { cs->viewport.yOffset = v; }
void clientSimSetCursorPosX(ClientSim *cs, int v)          { cs->viewport.cursorPosX = v; }
void clientSimSetCursorPosY(ClientSim *cs, int v)          { cs->viewport.cursorPosY = v; }
void clientSimSetNeedScreenReCalc(ClientSim *cs, bool v)   { cs->viewport.needRecalc = v; }
void clientSimSetInPillView(ClientSim *cs, bool v)         { cs->viewport.inPillView = v; }
void clientSimSetPillViewX(ClientSim *cs, BYTE v)          { cs->viewport.pillViewX = v; }
void clientSimSetPillViewY(ClientSim *cs, BYTE v)          { cs->viewport.pillViewY = v; }
void clientSimSetView(ClientSim *cs, screen v)             { cs->viewport.view = v; }
void clientSimSetMineView(ClientSim *cs, screenMines v)    { cs->viewport.mineView = v; }

void clientSimSetGmeStartDelay(ClientSim *cs, int v)       { cs->gmeStartDelay = v; }
void clientSimSetGmeLength(ClientSim *cs, int32_t v)       { cs->gmeLength = v; }
void clientSimSetTimeStart(ClientSim *cs, time_t v)        { cs->timeStart = v; }
void clientSimSetRunning(ClientSim *cs, bool v)            { cs->running = v; }
void clientSimSetCurrentBuildSelect(ClientSim *cs, buildSelect v) { cs->currentBuildSelect = v; }

void clientSimSetPendingBuild(ClientSim *cs, BYTE action, BYTE x, BYTE y) {
  cs->pendingBuildAction = action;
  cs->pendingBuildX = x;
  cs->pendingBuildY = y;
}

void clientSimSetBrainLastAssistMsg(ClientSim *cs, uint8_t v) { cs->brainLastAssistMsg = v; }
void clientSimSetBrainEventCount(ClientSim *cs, int v)        { cs->brainEventCount = v; }
void clientSimSetLabelMessage(ClientSim *cs, labelLen v)      { cs->labelMessage = v; }
void clientSimSetLabelTankLabel(ClientSim *cs, labelLen v)    { cs->labelTankLabel = v; }
void clientSimSetLabelOwnTank(ClientSim *cs, bool v)          { cs->labelOwnTank = v; }
void clientSimSetMyLastPlayerName(ClientSim *cs, const char *name) {
  strcpy(cs->myLastPlayerName, name);
}

void clientSimSetInLobby(ClientSim *cs, bool v)             { cs->inLobby = v; }
void clientSimSetMapDownloadComplete(ClientSim *cs, bool v) { cs->mapDownloadComplete = v; }

void clientSimSetServerAddress(ClientSim *cs, struct in_addr v) { cs->serverAddress = v; }
void clientSimSetServerPort(ClientSim *cs, unsigned short v)    { cs->serverPort = v; }
void clientSimSetIsBot(ClientSim *cs, bool v)                   { cs->isBot = v; }
