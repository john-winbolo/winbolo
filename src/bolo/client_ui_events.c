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
*Name:          ClientUiEvents
*Filename:      client_ui_events.c
*Author:        John Morrison
*Purpose:
*  Post-tick UI fan-out for the client. Pushes synced
*  ClientSim state to the front end each tick: scrolling,
*  status bars, kill/death counters, LGM indicator,
*  pillbox view, messages, and game-over. Extracted from
*  screen.c.
*********************************************************/

/* Includes */
#include "global.h"
#include "client_ui_events.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "tank.h"
#include "bases.h"
#include "messages.h"
#include "scroll.h"
#include "frontend.h"

/*********************************************************
*NAME:          clientUiOnTick
*PURPOSE:
*  Performs display-only updates after a ServerSim tick.
*  Handles scrolling, messages, status bars, pillbox view.
*********************************************************/
void clientUiOnTick(ClientSim *csPtr, bool isBrain) {
  WORLD tankX, tankY;
  BYTE armour, shellsAmount, minesAmount;
  BYTE tmx, tmy, pmx, pmy;

  if (clientSimIsRunning(csPtr) == FALSE || clientSimGetNetStatus(csPtr) == netFailed) {
    return;
  }

  if (clientSimGetGmeStartDelay(csPtr) > 0) {
    clientSimGetMessages(csPtr)->messageTime++;
    if (clientSimGetMessages(csPtr)->messageTime == MESSAGE_SCROLL_TIME) {
      messageUpdate(csPtr, clientSimGetMessages(csPtr));
      clientSimGetMessages(csPtr)->messageTime = 0;
    }
    return;
  }

  if (clientSimGetGmeLength(csPtr) == 0) {
    clientSimSetRunning(csPtr, FALSE);
    frontEndGameOver(csPtr);
    return;
  }

  if (clientSimGetNetStatus(csPtr) != netRunning) {
    clientSimGetMessages(csPtr)->messageTime++;
    if (clientSimGetMessages(csPtr)->messageTime == MESSAGE_SCROLL_TIME) {
      messageUpdate(csPtr, clientSimGetMessages(csPtr));
      clientSimGetMessages(csPtr)->messageTime = 0;
    }
    return;
  }

  /* Update scrolling based on tank position. Autoscroll needs to tick
   * every frame even when parked so its event detection (parked-rear
   * threat) and sub-tile slide keep progressing. Other scroll modes
   * (manual, no-auto) only need ticks when the tank is actually moving. */
  if (tankGetSpeed(&MY_TANK(csPtr)) > 0 || clientSimGetScroll(csPtr)->autoScroll) {
    /* Follow the rendered (smoothed) pose, not the raw sim pose, so the
     * camera tracks the hull the player sees — otherwise the hull would
     * shear against a camera centred on the un-smoothed position. With
     * err == 0 the rendered pose equals the sim pose. */
    WORLD rwx, rwy;
    float rang;
    BYTE rmx, rmy;
    clientSimGetRenderedTankPos(csPtr, &rwx, &rwy, &rang);
    tankGetGunsightAt(&MY_TANK(csPtr), rwx, rwy, rang, &tmx, &tmy, &pmx, &pmy);
    /* Same shift math as tankGetScreenMX/MY. */
    rmx = (BYTE)(((WORLD)(rwx - TANK_SUBTRACT)) >> TANK_SHIFT_MAPSIZE);
    rmy = (BYTE)(((WORLD)(rwy - TANK_SUBTRACT)) >> TANK_SHIFT_MAPSIZE);
    if (clientSimIsInItemView(csPtr) == FALSE) {
      int oldXOffset = clientSimGetXOffset(csPtr);
      int oldYOffset = clientSimGetYOffset(csPtr);
      if (scrollUpdate(clientSimGetScroll(csPtr), clientSimGetGameSim(csPtr), clientSimGetXOffsetPtr(csPtr), clientSimGetYOffsetPtr(csPtr), rmx, rmy, TRUE, tmx, tmy, tankGetSpeed(&MY_TANK(csPtr)), tankGetArmour(&MY_TANK(csPtr)), (TURNTYPE)(tankGetTravelAngel(&MY_TANK(csPtr))), FALSE, clientSimTankIsDead(csPtr), rwx, rwy) == TRUE) {
        /* Keep the pointer cell on the world tile it was over as the view
         * scrolls out from under it, so a frontend that only refreshes the
         * cell on a real pointer event still reports the same square between
         * events. The OS pointer itself is not moved — the build selection is
         * the build cursor's latched absolute tile, not this cell. */
        if (oldXOffset < clientSimGetXOffset(csPtr)) {
          clientSimSetCursorPosX(csPtr, clientSimGetCursorPosX(csPtr) - 1);
        } else if (oldXOffset > clientSimGetXOffset(csPtr)) {
          clientSimSetCursorPosX(csPtr, clientSimGetCursorPosX(csPtr) + 1);
        }
        if (oldYOffset < clientSimGetYOffset(csPtr)) {
          clientSimSetCursorPosY(csPtr, clientSimGetCursorPosY(csPtr) - 1);
        } else if (oldYOffset > clientSimGetYOffset(csPtr)) {
          clientSimSetCursorPosY(csPtr, clientSimGetCursorPosY(csPtr) + 1);
        }
        clientSimRecalc(csPtr);
      }
    }
  }

  /* Follow the death fireball when the tank is dead */
  if (clientSimIsInItemView(csPtr) == FALSE && clientSimTankIsDead(csPtr)) {
    BYTE expMX, expMY;
    if (tkExplosionGetOwnPosition(&clientSimGetGameSim(csPtr)->tankExplosions, clientSimGetMyPlayerNum(csPtr), &expMX, &expMY)) {
      scrollCenterObject(clientSimGetScroll(csPtr), clientSimGetXOffsetPtr(csPtr), clientSimGetYOffsetPtr(csPtr), expMX, expMY);
      clientSimRecalc(csPtr);
    }
  }

  /* Check we are still allowed to be watching what we are watching — the pill
   * is alive and ours, the base has not been captured or gone neutral, the
   * ally is alive, still allied and still here. An ally view also re-centres
   * on its target as it drives.
   *
   * Under a decay policy there is a second way to lose the view: the clock on
   * the item runs out. The server stops sending its squares at the same
   * moment, so staying parked there would only show ground going stale. The
   * upkeep call comes first and always runs — it is what follows a moving
   * ally, not just a test. */
  if (clientSimIsInItemView(csPtr) == TRUE) {
    ViewCycleInputs cycleIn;
    bool stillWatchable;

    clientSimFillViewCycleInputs(csPtr, &cycleIn);
    stillWatchable = viewportUpdateItemView(clientSimViewportMut(csPtr),
                                            clientSimGetGameSim(csPtr),
                                            clientSimGetScroll(csPtr),
                                            &cycleIn);
    if (stillWatchable == FALSE || clientSimViewDecayExpired(csPtr) == TRUE) {
      clientSimTankView(csPtr);
    }
  }

  /* An ally view has a third way to end: the tank has arrived as a hidden
   * stub for longer than the grace, which means the server has stopped
   * granting its rect and there is nothing left to follow. The timer runs
   * whatever the camera is doing and clears itself outside an ally view, so
   * a later entry starts a fresh grace. */
  if (clientSimAllyViewStubExpired(csPtr) == TRUE) {
    clientSimTankView(csPtr);
  }

  /* Tell the server which view we are in, if it has changed. */
  clientSimSyncViewState(csPtr);

  /* Update tank status bars — the server runs tankDeath/tankUpdate with
   * isServer=TRUE so frontEndUpdateTankStatusBars is not called from game
   * logic.  The client must push synced state to the display each tick. */
  frontEndUpdateTankStatusBars(csPtr, tankGetShells(&MY_TANK(csPtr)), tankGetMines(&MY_TANK(csPtr)),
                               tankGetArmour(&MY_TANK(csPtr)), tankGetTrees(&MY_TANK(csPtr)));

  /* Update kills/deaths display — same reason as above */
  {
    int kills, deaths;
    tankGetKillsDeaths(&MY_TANK(csPtr), &kills, &deaths);
    frontEndKillsDeaths(csPtr, kills, deaths);
  }

  /* Update LGM status indicator — the server runs lgmUpdate with the
   * server context so frontEndManStatus calls inside lgm.c may not fire
   * on the client.  Query the synced LGM state and update the display. */
  {
    bool lgmIsOut, lgmIsDead;
    TURNTYPE lgmAngle = 0;
    lgmGetStatus(&MY_LGM(csPtr), &MY_TANK(csPtr), &lgmIsOut, &lgmIsDead, &lgmAngle);
    if (!lgmIsOut) {
      frontEndManClear(csPtr);
    } else {
      frontEndManStatus(csPtr, lgmIsDead, lgmAngle);
    }
  }

  /* Update base status bars */
  tankGetWorld(&MY_TANK(csPtr), &tankX, &tankY);
  basesGetStats(&clientSimGetGameSim(csPtr)->bs, basesGetClosest(clientSimGetGameSim(csPtr), tankX, tankY), &shellsAmount, &minesAmount, &armour);
  frontEndUpdateBaseStatusBars(csPtr, shellsAmount, minesAmount, armour);

  /* Advance tank explosions locally for smooth animation/sounds between snapshots.
   * Destructive operations (pill damage, lgm death) are gated behind isServer
   * inside tkExplosionUpdate, so passing NULL/0 for lgm args is safe. */
  tkExplosionUpdate(clientSimGetGameSim(csPtr), NULL, 0, NULL, &clientSimGetGameSim(csPtr)->ss);
  explosionsUpdate(&clientSimGetGameSim(csPtr)->expl);

  /* Update network LGM frames */
  playersGameTickUpdate(&clientSimGetGameSim(csPtr)->plyrs);

  /* Messaging */
  clientSimGetMessages(csPtr)->messageTime++;
  if (clientSimGetMessages(csPtr)->messageTime == MESSAGE_SCROLL_TIME) {
    messageUpdate(csPtr, clientSimGetMessages(csPtr));
    clientSimGetMessages(csPtr)->messageTime = 0;
  }
}
