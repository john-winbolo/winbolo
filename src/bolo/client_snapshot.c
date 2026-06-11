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
 *Name:          Client Snapshot
 *Filename:      client_snapshot.c
 *Purpose:
 *  Client-network glue: applies a server snapshot to the
 *  ClientSim each tick (state diff + reliable event switch)
 *  and packages local input into the InputPacket sent back
 *  upstream. Runs for both human and bot clients; the event
 *  switch fires Steam stats, achievements, and newswire
 *  messages (newswire path debounced via
 *  basesEnqueueCaptureMessage).
 *
 *  Contents:
 *    clientBuildInputPacket  - pack keys/build into an InputPacket
 *                              (public — declared in client_net.h)
 *    clientApplySnapshot     - apply a server snapshot to the ClientSim
 *                              (internal — declared in client_snapshot.h);
 *                              the local-tank first-snapshot branch is
 *                              where the viewport finalisation (centre +
 *                              mine-view clear + recalc) fires, once per
 *                              game life, so frontends don't orchestrate it.
 *
 *  Companion file: brain_data.c (Lua-brain data shaping).
 *********************************************************/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <SDL3/SDL.h>

#include "../common/wb_log.h"
#include "global.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "tank.h"
#include "shells.h"
#include "rubble.h"
#include "explosions.h"
#include "screenbullet.h"
#include "frontend.h"
#include "../gui/lang.h"
#include "sounddist.h"
#include "messages.h"
#include "grass.h"
#include "swamp.h"
#include "building.h"
#include "tilenum.h"
#include "tankexp.h"
#include "scroll.h"
#include "lgm.h"
#include "log.h"
#include "floodfill.h"
#include "minesexp.h"
#include "treegrow.h"
#include "mines.h"
#include "labels.h"
#include "players.h"
#include "screenbrainmap.h"
#include "client_snapshot.h"
#include "client_state.h"
#include "interpolation.h"
#include "util.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "server_sim.h"
#include "../steam/steam_wrapper.h"

/*********************************************************
*NAME:          clientBuildInputPacket
*PURPOSE:
*  Builds an InputPacket from the current input state.
*  Handles both keyboard input and brain input.
*********************************************************/
void clientBuildInputPacket(ClientSim *csPtr, InputPacket *pkt, tankButton tb, bool isShoot, bool isMine, bool isBrain, bool isGameTick, BYTE playerNum, uint32_t tick) {
  memset(pkt, 0, sizeof(InputPacket));
  pkt->tick = tick;
  pkt->playerNum = playerNum;

  /* If our local tank doesn't exist yet, leave the packet otherwise empty.
   * This is the brief window between CTRL_GAME_PHASE_RUNNING flipping
   * inLobby=false on the client and the first tank-bearing snapshot
   * arriving — over UDP loopback that's a multi-tick gap, and the frontend
   * may have already entered windowRunGameTick before MY_TANK is populated.
   * Sending an empty input keeps the server's pingMs / ack piggyback
   * channel alive without dereferencing a NULL tank in any of the
   * autoslowdown / brain-keys / build-action paths below. */
  if (MY_TANK(csPtr) == NULL) {
    return;
  }

  /* Pack autoslowdown state into flags (sent every packet so server stays in sync) */
  if (tankGetAutoSlowdown(&MY_TANK(csPtr))) {
    pkt->flags |= INPUT_FLAG_AUTOSLOW;
  }

  if (isBrain) {
    /* Translate brain keys to tankButton + shoot, same as screenTranslateBrainButtons
     * but without the side effects (gunsight, mine laying, etc.) that the brain
     * handles through the InputPacket instead */
    uint32_t *holdKeys = clientSimGetBrainHoldKeys(csPtr);
    uint32_t *tapKeys = clientSimGetBrainTapKeys(csPtr);
    unsigned long temp = *holdKeys;

    /* Handle tap keys for movement */
    if (testkey(*tapKeys, KEY_faster))    { setkey(temp, KEY_faster); }
    if (testkey(*tapKeys, KEY_slower))    { setkey(temp, KEY_slower); }
    if (testkey(*tapKeys, KEY_turnleft))  { setkey(temp, KEY_turnleft); }
    if (testkey(*tapKeys, KEY_turnright)) { setkey(temp, KEY_turnright); }

    /* Build button bitmask from brain keys */
    if (testkey(temp, KEY_faster))    { pkt->buttons |= INPUT_BTN_ACCEL; }
    if (testkey(temp, KEY_slower))    { pkt->buttons |= INPUT_BTN_DECEL; }
    if (testkey(temp, KEY_turnleft))  { pkt->buttons |= INPUT_BTN_LEFT; }
    if (testkey(temp, KEY_turnright)) { pkt->buttons |= INPUT_BTN_RIGHT; }

    /* Fire action — only on game ticks */
    if (isGameTick) {
      if (testkey(*tapKeys, KEY_shoot) || testkey(*holdKeys, KEY_shoot)) {
        pkt->actions |= INPUT_ACTION_FIRE;
      }
    }

    /* Mine laying */
    if (testkey(*tapKeys, KEY_dropmine) || testkey(*holdKeys, KEY_dropmine)) {
      pkt->actions |= INPUT_ACTION_LAY_MINE;
    }

    /* Gunsight adjustment (bits 2-3 of flags) */
    if (testkey(*tapKeys, KEY_morerange) || testkey(*holdKeys, KEY_morerange)) {
      pkt->flags |= (1 << INPUT_FLAG_GUNSIGHT_SHIFT);  /* increase */
    } else if (testkey(*tapKeys, KEY_lessrange) || testkey(*holdKeys, KEY_lessrange)) {
      pkt->flags |= (2 << INPUT_FLAG_GUNSIGHT_SHIFT);  /* decrease */
    }

    /* Build action from brain (1-based: 0=none, 1=BsTrees, ...).
     * Only consume on game ticks — the server ignores buildAction on keys ticks. */
    BuildInfo *buildInfo = *clientSimGetBrainBuildInfo(csPtr);
    if (isGameTick && buildInfo != NULL && buildInfo->action != 0) {
      pkt->buildAction = buildInfo->action;
      pkt->buildX = buildInfo->x;
      pkt->buildY = buildInfo->y;
      buildInfo->action = 0;  /* consume the request */
    }

    /* Client-side display actions (pill/tank view toggle) */
    if (testkey(*tapKeys, KEY_TankView) || testkey(*holdKeys, KEY_TankView)) {
      csPtr->viewport.inPillView = FALSE;
      clientSimCenterTank(csPtr);
    }
    if (testkey(*tapKeys, KEY_PillView) || testkey(*holdKeys, KEY_PillView)) {
      clientSimPillView(csPtr, 0, 0);
    }

    /* Clear tap keys after reading — preserve shoot tap on non-game ticks
     * so it fires on the next game tick (matches screenTranslateBrainButtons) */
    {
      uint32_t preserved = 0;
      if (!isGameTick && testkey(*tapKeys, KEY_shoot)) {
        setkey(preserved, KEY_shoot);
      }
      *tapKeys = preserved;
    }
  } else {
    /* Keyboard input: convert tankButton enum to bitmask */
    switch (tb) {
      case TLEFTACCEL:  pkt->buttons = INPUT_BTN_LEFT | INPUT_BTN_ACCEL; break;
      case TRIGHTACCEL: pkt->buttons = INPUT_BTN_RIGHT | INPUT_BTN_ACCEL; break;
      case TLEFTDECEL:  pkt->buttons = INPUT_BTN_LEFT | INPUT_BTN_DECEL; break;
      case TRIGHTDECEL: pkt->buttons = INPUT_BTN_RIGHT | INPUT_BTN_DECEL; break;
      case TLEFT:       pkt->buttons = INPUT_BTN_LEFT; break;
      case TRIGHT:      pkt->buttons = INPUT_BTN_RIGHT; break;
      case TACCEL:      pkt->buttons = INPUT_BTN_ACCEL; break;
      case TDECEL:      pkt->buttons = INPUT_BTN_DECEL; break;
      default:          pkt->buttons = 0; break;
    }

    /* Fire action — only on game ticks */
    if (isGameTick && isShoot) {
      pkt->actions |= INPUT_ACTION_FIRE;
    }

    /* Mine laying */
    if (isMine) {
      pkt->actions |= INPUT_ACTION_LAY_MINE;
    }
  }

  /* Pending human-player build request (from screenManMove).
   * Only consume on game ticks — the server ignores buildAction on keys ticks. */
  if (!isBrain && isGameTick && csPtr->pendingBuildAction != 0) {
    pkt->buildAction = csPtr->pendingBuildAction;
    pkt->buildX = csPtr->pendingBuildX;
    pkt->buildY = csPtr->pendingBuildY;
    csPtr->pendingBuildAction = 0;
    csPtr->pendingBuildX = 0;
    csPtr->pendingBuildY = 0;
  }
}
/*********************************************************
*NAME:          clientApplySnapshot
*PURPOSE:
*  Applies a server snapshot received over UDP transport
*  to the ClientSim. Updates tank positions (own tank via
*  reconciliation, others via interpolation), rebuilds
*  shells and explosions from snapshot data, and processes
*  game events (map changes, etc.).
*********************************************************/
void clientApplySnapshot(ClientSim *csPtr,
                              const SnapshotHeader *hdr,
                              const TankSnapshot *tanks, int tankCount,
                              const ShellSnapshot *shellSnaps, int shellCount,
                              const TkExplosionSnapshot *tkExplSnaps, int tkExplosionCount,
                              const BaseSnapshot *baseSnaps, int baseCount,
                              const PillSnapshot *pillSnaps, int pillCount,
                              const GameEvent *events, int eventCount,
                              BYTE playerNum) {
  int i;
  bool isHuman = !csPtr->isBot;

  /* Update other players via interpolation */
  csPtr->interpCtx.localPlayer = playerNum;
  for (i = 0; i < tankCount; i++) {
    BYTE pn = (BYTE)(tanks[i].playerNum & TANK_SNAPSHOT_PLAYER_MASK);

    /* Hidden stub: this slot is connected but its tank/LGM are outside our
     * viewport.  Mark the slot missing for interp and clear the players
     * struct entry so the renderer doesn't keep drawing the last in-view
     * position as a ghost. */
    if (tanks[i].playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) {
      if (pn != playerNum && pn < MAX_TANKS) {
        interpMarkMissing(&csPtr->interpCtx, pn);
        if (csPtr->sim.plyrs != NULL && playersIsInUse(&csPtr->sim.plyrs, pn)) {
          playersUpdate(&csPtr->sim.plyrs, pn, 0, 0, 0, 0, 0, FALSE,
                        0, 0, 0, 0, 0);
        }
      }
      continue;
    }

    /* Update ping and client flags for all players from snapshot */
    playersSetPing(&csPtr->sim.plyrs, pn, tanks[i].pingMs);
    /* Push the fresh ping into the frontend's per-slot cache too —
     * the HUD player rows read from that cache, not from the players
     * struct, and it would otherwise stay frozen at the value set by
     * the join-time frontEndSetPlayer call. */
    frontEndUpdatePlayerPing(csPtr, (playerNumbers)pn, tanks[i].pingMs);
    {
      /* Snapshot is authoritative only for these bits — preserve any others
       * (e.g. STEAM_BUILD set once from JOIN_REQUEST, PLAYER_FLAG_BOT set at
       * bot creation) across snapshot ticks. Out-of-view (stub) tanks send
       * clientFlags=0, so bits not in this mask must be kept from the
       * existing value rather than clobbered from the wire. PLAYER_FLAG_BOT
       * must NOT go in this mask for that reason. */
      const uint8_t snapshotMask = PLAYER_FLAG_WBN_VERIFIED
                                 | PLAYER_FLAG_WBN_STEAM_LINKED
                                 | PLAYER_FLAG_SUPPORTER;
      uint8_t cur = playersGetClientFlags(&csPtr->sim.plyrs, pn);
      uint8_t next = (uint8_t)((cur & ~snapshotMask) | (tanks[i].clientFlags & snapshotMask));
      playersSetClientFlags(&csPtr->sim.plyrs, pn, next);
      frontEndUpdatePlayerFlags(csPtr, (playerNumbers)pn,
                                playersGetClientType(&csPtr->sim.plyrs, pn), next);
    }
    if (tanks[i].clientFlags != 0) {
      WB_LOG_TRACE(WB_LOG_CAT_CLIENT, "[WBN] player %d clientFlags=0x%02x", pn, tanks[i].clientFlags);
    }

    if (pn == playerNum) {
      /* Own tank: first sync or reconcile */
      if (csPtr->clientState.initialized && !csPtr->clientState.hasPredictedTank && MY_TANK(csPtr) != NULL) {
        /* First snapshot — initialize predicted tank from server state */
        TURNTYPE decodedAngle = (TURNTYPE)tanks[i].angle / 256.0f;
        SPEEDTYPE decodedSpeed = (SPEEDTYPE)tanks[i].speed / 256.0f;
        tankSetWorld(&csPtr->sim, &MY_TANK(csPtr), tanks[i].worldX, tanks[i].worldY,
                     decodedAngle, FALSE);
        {
          BYTE isDead, onBoat;
          utilGetNibbles(tanks[i].tankStatus, &isDead, &onBoat);
          tankSetOnBoat(&MY_TANK(csPtr), onBoat);
        }
        tankSetSpeed(&MY_TANK(csPtr), decodedSpeed);
        tankSetFirstLeft(&MY_TANK(csPtr), tanks[i].firstLeft);
        tankSetFirstRight(&MY_TANK(csPtr), tanks[i].firstRight);
        tankSetArmour(&MY_TANK(csPtr), tanks[i].armour);
        csPtr->lastServerArmour = tanks[i].armour;
        tankSetShells(&MY_TANK(csPtr), tanks[i].shells);
        tankSetMines(&MY_TANK(csPtr), tanks[i].mines);
        tankSetTrees(&MY_TANK(csPtr), tanks[i].trees);
        tankSetGunsightLength(&MY_TANK(csPtr), tanks[i].gunsightLen);
        tankSetReload(&MY_TANK(csPtr), tanks[i].reload);
        csPtr->clientState.hasPredictedTank = TRUE;
        if (isHuman) {
          /* The local tank just became live on the map: the server's chosen
           * start has been copied into MY_TANK above, and the transport has
           * already driven map install (inline on MAP_DOWNLOAD for nolobby,
           * via the CTRL_GAME_PHASE LOBBY→RUNNING watcher for lobby) — so
           * viewport->mineView is allocated. Centre on the tank, clear the
           * per-player seen-mines tracking, and recalc the viewport. */
          BYTE count, count2;
          clientSimCenterTank(csPtr);
          for (count = 0; count < MAIN_BACK_BUFFER_SIZE_X; count++) {
            for (count2 = 0; count2 < MAIN_BACK_BUFFER_SIZE_Y; count2++) {
              (*clientSimGetMineView(csPtr))->mineItem[count][count2] = FALSE;
            }
          }
          clientSimRecalc(csPtr);
        }
      } else if (csPtr->clientState.initialized && csPtr->clientState.hasPredictedTank && MY_TANK(csPtr) != NULL) {
        /* Build a temporary tank-like state for reconciliation.
         * We use the wire snapshot data to check/correct our prediction. */
        WORLD predX, predY, servX, servY;
        TURNTYPE predAngle, servAngle;
        SPEEDTYPE decodedSpeed;
        int32_t dx, dy;

        tankGetWorld(&MY_TANK(csPtr), &predX, &predY);
        predAngle = tankGetAngle(&MY_TANK(csPtr));
        servX = tanks[i].worldX;
        servY = tanks[i].worldY;
        servAngle = (TURNTYPE)tanks[i].angle / 256.0f;
        decodedSpeed = (SPEEDTYPE)tanks[i].speed / 256.0f;

        /* Update oldest unacked based on server acknowledgment */
        csPtr->clientState.oldestUnacked = hdr->lastProcessedInput + 1;

        dx = (int32_t)predX - (int32_t)servX;
        dy = (int32_t)predY - (int32_t)servY;

        /* Compare angles in the quantized uint16 domain the wire uses
         * (angle × 256) to avoid false mismatches from float round-trip. */
        {
          bool angleMismatch = ((uint16_t)(predAngle * 256.0f) != tanks[i].angle);

          if (dx != 0 || dy != 0 || angleMismatch) {
            /* Record reconcile stats for this window. Position error in
             * pixels from the world-unit deltas (16 world units per
             * rendered pixel at base zoom); an angle-only mismatch is 0. */
            float errPx = sqrtf((float)dx * dx + (float)dy * dy) / 16.0f;
            csPtr->reconCountThisWindow++;
            csPtr->reconErrSumPx += errPx;
            if (errPx > csPtr->reconErrMaxPx) {
              csPtr->reconErrMaxPx = errPx;
            }

            /* Prediction diverged — snap to server state and replay.
             * Save the predicted angle: if the angle was actually correct
             * (quantized values match) we restore it after replay to avoid
             * gunsight jitter caused by speed-quantization-induced position
             * drift triggering unnecessary angle changes during replay. */
            TURNTYPE savedAngle = predAngle;
            BYTE savedFirstLeft = tankGetFirstLeft(&MY_TANK(csPtr));
            BYTE savedFirstRight = tankGetFirstRight(&MY_TANK(csPtr));

            tankSetWorld(&csPtr->sim, &MY_TANK(csPtr), servX, servY, servAngle, FALSE);
            {
              BYTE isDead, onBoat;
              utilGetNibbles(tanks[i].tankStatus, &isDead, &onBoat);
              tankSetOnBoat(&MY_TANK(csPtr), onBoat);
            }
            tankSetSpeed(&MY_TANK(csPtr), decodedSpeed);
            tankSetFirstLeft(&MY_TANK(csPtr), tanks[i].firstLeft);
            tankSetFirstRight(&MY_TANK(csPtr), tanks[i].firstRight);
            tankSetReload(&MY_TANK(csPtr), tanks[i].reload);

            /* Replay unacknowledged inputs (suppress sounds/side effects) */
            csPtr->sim.isPredicting = TRUE;
            {
              uint32_t tick;
              for (tick = csPtr->clientState.oldestUnacked; tick <= csPtr->clientState.newestInput; tick++) {
                uint8_t idx = tick & (CLIENT_INPUT_HISTORY_SIZE - 1);
                InputPacket *histPkt = &csPtr->clientState.history[idx];
                bool isKeysTick;
                tankButton tb = TNONE;
                bool accel, decel, left, right;

                if (histPkt->tick != tick) continue;

                isKeysTick = (tick % 2) == 1;
                accel = (histPkt->buttons & INPUT_BTN_ACCEL) != 0;
                decel = (histPkt->buttons & INPUT_BTN_DECEL) != 0;
                left  = (histPkt->buttons & INPUT_BTN_LEFT)  != 0;
                right = (histPkt->buttons & INPUT_BTN_RIGHT) != 0;
                if (accel && decel) { accel = FALSE; decel = FALSE; }
                if (left && right)  { left = FALSE;  right = FALSE; }
                if (left && accel)  tb = TLEFTACCEL;
                else if (right && accel) tb = TRIGHTACCEL;
                else if (left && decel)  tb = TLEFTDECEL;
                else if (right && decel) tb = TRIGHTDECEL;
                else if (left)  tb = TLEFT;
                else if (right) tb = TRIGHT;
                else if (accel) tb = TACCEL;
                else if (decel) tb = TDECEL;

                if (isKeysTick) {
                  BYTE bmx = tankGetMX(&MY_TANK(csPtr));
                  BYTE bmy = tankGetMY(&MY_TANK(csPtr));
                  tankTurn(&csPtr->sim, &MY_TANK(csPtr), bmx, bmy, tb);
                } else {
                  tankUpdate(&csPtr->sim, &MY_TANK(csPtr), tb, FALSE, FALSE);
                  /* Simulate fire's effect on reload during replay.
                   * tankUpdate was called with shoot=FALSE to avoid creating
                   * shells, but we must still apply the reload reset so the
                   * client doesn't think it can fire again. */
                  if ((histPkt->actions & INPUT_ACTION_FIRE) &&
                      tankGetReloadTime(&MY_TANK(csPtr)) == 0 &&
                      tankGetShells(&MY_TANK(csPtr)) > 0 &&
                      tankGetArmour(&MY_TANK(csPtr)) <= TANK_FULL_ARMOUR) {
                    tankSetReload(&MY_TANK(csPtr), TANK_RELOAD_TIME);
                    tankSetShells(&MY_TANK(csPtr), tankGetShells(&MY_TANK(csPtr)) - 1);
                  }
                }
              }
            }
            csPtr->sim.isPredicting = FALSE;

            /* If the server's angle matched our prediction (quantized), the
             * reconciliation was triggered only by position drift (e.g. from
             * speed quantization).  Restore the
             * predicted angle + turn ramp-up so the gunsight doesn't flicker
             * from tiny position-induced turn-rate differences during replay. */
            if (!angleMismatch) {
              WORLD finalX, finalY;
              tankGetWorld(&MY_TANK(csPtr), &finalX, &finalY);
              tankSetWorld(&csPtr->sim, &MY_TANK(csPtr), finalX, finalY, savedAngle, FALSE);
              tankSetFirstLeft(&MY_TANK(csPtr), savedFirstLeft);
              tankSetFirstRight(&MY_TANK(csPtr), savedFirstRight);
            }
          }
        }

        /* Detect death/respawn transitions using server armour values
         * (not predicted state, which may already reflect the death) */
        {
          tankSetArmour(&MY_TANK(csPtr), tanks[i].armour);
          if (csPtr->lastServerArmour <= TANK_FULL_ARMOUR && tanks[i].armour > TANK_FULL_ARMOUR) {
            /* alive→dead: set death type for static screen rendering */
            tankSetLastTankDeath(&MY_TANK(csPtr), LAST_DEATH_BY_SHELL);
            tankAddDeath(&csPtr->sim, &MY_TANK(csPtr));
          }
          if (csPtr->lastServerArmour > TANK_FULL_ARMOUR && tanks[i].armour <= TANK_FULL_ARMOUR) {
            /* dead→alive: recenter view on respawn */
            csPtr->sim.inStartFind = FALSE;
            if (isHuman) {
              csPtr->viewport.inPillView = FALSE;
              clientSimCenterTank(csPtr);
            }
          }
          csPtr->lastServerArmour = tanks[i].armour;
        }

        /* Sync resources from server — but not reload/shells, which are
         * already set correctly by the reconciliation replay (it accounts
         * for unprocessed fire inputs that the server hasn't seen yet). */
        tankSetShells(&MY_TANK(csPtr), tanks[i].shells);
        tankSetMines(&MY_TANK(csPtr), tanks[i].mines);
        tankSetTrees(&MY_TANK(csPtr), tanks[i].trees);
        tankSetGunsightLength(&MY_TANK(csPtr), tanks[i].gunsightLen);
        tankSetDeathWait(&MY_TANK(csPtr), tanks[i].deathWait);
        tankSetReload(&MY_TANK(csPtr), tanks[i].reload);
        /* Sync boat state from server — prediction skips the boat state
         * machine (isPredicting guard in tankUpdate), so the client's
         * onBoat flag can go stale if no position mismatch triggers
         * reconciliation. Always apply the server's value. */
        {
          BYTE isDead, onBoat;
          utilGetNibbles(tanks[i].tankStatus, &isDead, &onBoat);
          tankSetOnBoat(&MY_TANK(csPtr), onBoat);
        }

        /* Correct shells/reload for any unprocessed fire inputs.
         * The server snapshot reflects state before our fire was processed,
         * so we must re-apply the fire effect to prevent double-firing. */
        {
          uint32_t tick;
          for (tick = csPtr->clientState.oldestUnacked; tick <= csPtr->clientState.newestInput; tick++) {
            uint8_t idx = tick & (CLIENT_INPUT_HISTORY_SIZE - 1);
            InputPacket *histPkt = &csPtr->clientState.history[idx];
            if (histPkt->tick != tick) continue;
            if ((tick % 2) == 1) continue; /* keys tick — no fire */
            /* Decrement reload like tankUpdate would */
            if (tankGetReloadTime(&MY_TANK(csPtr)) > 0) {
              tankSetReload(&MY_TANK(csPtr), tankGetReloadTime(&MY_TANK(csPtr)) - 1);
            }
            if ((histPkt->actions & INPUT_ACTION_FIRE) &&
                tankGetReloadTime(&MY_TANK(csPtr)) == 0 &&
                tankGetShells(&MY_TANK(csPtr)) > 0 &&
                tankGetArmour(&MY_TANK(csPtr)) <= TANK_FULL_ARMOUR) {
              tankSetReload(&MY_TANK(csPtr), TANK_RELOAD_TIME);
              tankSetShells(&MY_TANK(csPtr), tankGetShells(&MY_TANK(csPtr)) - 1);
            }
          }
        }
      }
      continue;
    }

    /* Other player: feed into interpolation system */
    {
      InterpSnapshot snap;
      snap.worldX = tanks[i].worldX;
      snap.worldY = tanks[i].worldY;
      snap.angle = (TURNTYPE)tanks[i].angle / 256.0f;
      snap.speed = (SPEEDTYPE)tanks[i].speed / 256.0f;
      {
        BYTE isDead, onBoat;
        utilGetNibbles(tanks[i].tankStatus, &isDead, &onBoat);
        snap.onBoat = onBoat;
        snap.alive = (isDead == 0);
      }
      snap.lgmMX = tanks[i].lgmMX;
      snap.lgmMY = tanks[i].lgmMY;
      snap.lgmPX = tanks[i].lgmPX;
      snap.lgmPY = tanks[i].lgmPY;
      snap.lgmFrame = tanks[i].lgmFrame > 0 ? tanks[i].lgmFrame - 1 : 0;
      interpUpdate(&csPtr->interpCtx, pn, &snap, hdr->serverTick);

      /* Auto-register player if not yet known */
      if (csPtr->sim.plyrs != NULL && playersIsInUse(&csPtr->sim.plyrs, pn) == FALSE) {
        char name[FILENAME_MAX];
        sprintf(name, "Player %d", pn);
        WB_LOG_INFO(WB_LOG_CAT_CLIENT, "Auto-registering player %d from snapshot (pos=%u,%u)",
                pn, tanks[i].worldX, tanks[i].worldY);
        playersSetPlayer(csPtr, &csPtr->sim.plyrs, csPtr->myPlayerNum, pn, name, "XX",
                         0, 0, 0, 0, 0, FALSE, 0, NULL, FALSE);
      }

      /* Store speed on player struct for brain access (brain API uses * 4 scale) */
      if (csPtr->sim.plyrs != NULL) {
        (*csPtr->sim.plyrs).item[pn].speed = (uint8_t)(tanks[i].speed >> 6);
      }

      /* Update players struct for rendering */
      if (csPtr->sim.plyrs != NULL) {
        WORLD interpX, interpY;
        TURNTYPE interpAngle;
        bool interpOnBoat;
        if (interpGetPosition(&csPtr->interpCtx, pn, 1.0f,
                              &interpX, &interpY,
                              &interpAngle, &interpOnBoat)) {
          BYTE mx = (BYTE)(interpX >> TANK_SHIFT_MAPSIZE);
          BYTE px = (BYTE)((interpX & 0xFF) >> TANK_SHIFT_RIGHT2);
          BYTE my = (BYTE)(interpY >> TANK_SHIFT_MAPSIZE);
          BYTE py = (BYTE)((interpY & 0xFF) >> TANK_SHIFT_RIGHT2);
          BYTE frame = utilGetDir(interpAngle);
          playersUpdate(&csPtr->sim.plyrs, pn, mx, my, px, py, frame,
                        interpOnBoat,
                        snap.lgmMX, snap.lgmMY, snap.lgmPX, snap.lgmPY,
                        snap.lgmFrame);
        } else if (!snap.alive) {
          /* Dead player: move tank off-screen but keep LGM visible —
           * the LGM outlives its owner tank and the server still sends
           * its position in every snapshot. */
          playersUpdate(&csPtr->sim.plyrs, pn, 0, 0, 0, 0, 0, FALSE,
                        snap.lgmMX, snap.lgmMY, snap.lgmPX, snap.lgmPY,
                        snap.lgmFrame);
        }
      }
    }
  }

  /* Mark players NOT in the snapshot as missing.  With per-tick stubs the
   * server emits one entry per connected slot, so reaching here means the
   * slot is truly absent (disconnect, never-yet-seen, packet drop on first
   * snapshot).  Mask the stub flag when comparing slot indices. */
  {
    BYTE p;
    for (p = 0; p < MAX_TANKS; p++) {
      bool found = FALSE;
      if (p == playerNum) continue;
      for (i = 0; i < tankCount; i++) {
        BYTE entryPn = (BYTE)(tanks[i].playerNum & TANK_SNAPSHOT_PLAYER_MASK);
        if (entryPn == p) { found = TRUE; break; }
      }
      if (!found) {
        interpMarkMissing(&csPtr->interpCtx, p);
      }
    }
  }

  /* Store server shell snapshot data for rendering.
   * These are rendered directly to screen bullets in screenUpdate(),
   * bypassing the shells linked list (which would apply unwanted offsets).
   *
   * Humans filter out their own shells here because they're rendered via
   * the local prediction layer instead. Bots have no prediction layer
   * and rely on this snapshot for everything they shoot, so KEEP their
   * own shells — without this the brain (info.objects, BrainTest
   * shell-hitbox overlay) never sees shells fired by the bot itself. */
  if (shellSnaps != NULL) {
    int si, di = 0;
    for (si = 0; si < shellCount && di < MAX_SNAPSHOT_SHELLS; si++) {
      if (isHuman && shellSnaps[si].owner == playerNum) continue;
      csPtr->serverShellSnaps[di++] = shellSnaps[si];
    }
    csPtr->serverShellCount = di;
  }

  /* Tank fireballs are spawned via EVENT_TK_EXPLOSION (handled below) and
   * simulated locally by tkExplosionUpdate — no per-tick replication. */
  (void)tkExplSnaps; (void)tkExplosionCount;

  /* Update own LGM from snapshot data */
  for (i = 0; i < tankCount; i++) {
    if (tanks[i].playerNum == playerNum && MY_LGM(csPtr) != NULL) {
      if (tanks[i].lgmFrame > 0) {
        /* LGM is out on server — lgmFrame is encoded as frame+1 (1-3),
         * so 0 means idle and >0 reliably means out */
        BYTE actualFrame = tanks[i].lgmFrame - 1;
        WORLD lgmWX = (WORLD)((tanks[i].lgmMX << 8) + (tanks[i].lgmPX << 4));
        WORLD lgmWY = (WORLD)((tanks[i].lgmMY << 8) + (tanks[i].lgmPY << 4));
        MY_LGM(csPtr)->inTank = FALSE;
        MY_LGM(csPtr)->isDead = (actualFrame == LGM_HELICOPTER_FRAME) ? TRUE : FALSE;
        MY_LGM(csPtr)->frame = actualFrame;
        lgmPutWorld(&MY_LGM(csPtr), lgmWX, lgmWY, actualFrame);
      } else {
        /* LGM is idle/in tank on server — sync client state */
        MY_LGM(csPtr)->inTank = TRUE;
        MY_LGM(csPtr)->state = LGM_STATE_IDLE;
      }
      break;
    }
  }

  /* Apply base snapshots */
  if (baseSnaps != NULL && csPtr->sim.bs != NULL) {
    for (i = 0; i < baseCount && i < MAX_BASES; i++) {
      (*csPtr->sim.bs).item[i].owner = baseSnaps[i].owner;
      (*csPtr->sim.bs).item[i].armour = baseSnaps[i].armour;
      (*csPtr->sim.bs).item[i].shells = baseSnaps[i].shells;
      (*csPtr->sim.bs).item[i].mines = baseSnaps[i].mines;
    }
  }

  /* Apply pill snapshots */
  if (pillSnaps != NULL && csPtr->sim.pb != NULL) {
    for (i = 0; i < pillCount && i < MAX_PILLS; i++) {
      (*csPtr->sim.pb).item[i].x = pillSnaps[i].x;
      (*csPtr->sim.pb).item[i].y = pillSnaps[i].y;
      (*csPtr->sim.pb).item[i].owner = pillSnaps[i].owner;
      (*csPtr->sim.pb).item[i].armour = pillSnaps[i].armour;
      (*csPtr->sim.pb).item[i].speed = pillSnaps[i].speed;
      (*csPtr->sim.pb).item[i].inTank = pillSnaps[i].inTank ? TRUE : FALSE;
    }
  }

  /* Store server tick for brain access */
  csPtr->lastServerTick = hdr->serverTick;

  /* Buffer events for brain consumption (accumulate across syncs;
   * reset happens when the brain consumes them in brainDataMakeInfo) */
  if (events != NULL) {
    for (i = 0; i < eventCount; i++) {
      switch (events[i].type) {
      case EVENT_SOUND:
      case EVENT_SOUND_SHOOT:
      case EVENT_SOUND_TANK_HIT:
      case EVENT_PILL_CAPTURED:
      case EVENT_BASE_CAPTURED:
      case EVENT_TANK_KILLED:
      case EVENT_LGM_LOST:
      case EVENT_PLAYER_LEAVE:
      case EVENT_PILL_UPDATE:
      case EVENT_BASE_UPDATE:
      case EVENT_EXPLOSION:
        if (csPtr->brainEventCount < MAX_BRAIN_EVENTS) {
          csPtr->brainEvents[csPtr->brainEventCount++] = events[i];
        }
        break;
      case EVENT_ASSISTANT_MSG:
        if (events[i].data[0] == playerNum) {
          csPtr->brainLastAssistMsg = events[i].data[1];
        }
        if (csPtr->brainEventCount < MAX_BRAIN_EVENTS) {
          csPtr->brainEvents[csPtr->brainEventCount++] = events[i];
        }
        break;
      default:
        break;
      }
    }
  }

  /* Process game events (MAP_CHANGE, SOUND — all reliable) */
  bool steamStatsUpdated = false;
  if (events != NULL) {
    for (i = 0; i < eventCount; i++) {
      switch (events[i].type) {
      case EVENT_MAP_CHANGE:
        /* data: [mx, my, newTerrain] */
        if (csPtr->sim.mp != NULL) {
          mapSetPos(&csPtr->sim, &csPtr->sim.mp, events[i].data[0], events[i].data[1],
                    events[i].data[2], FALSE, TRUE);
          clientSimRecalc(csPtr);
        }
        break;
      case EVENT_SOUND:
        /* data: [soundId, mx, my, sourcePlayer] — play with distance attenuation.
         * Sounds are server-authoritative (isPredicting suppresses prediction-side
         * sounds). Bubbles and tank-sink are gated to the local player only:
         * they're tied to the player's own boat/drown event and would otherwise
         * play whenever any remote tank within distance went into water. */
        if (isHuman) {
          sndEffects sid = (sndEffects)events[i].data[0];
          bool selfOnly = (sid == bubbles || sid == tankSinkNear || sid == tankSinkFar);
          if (!selfOnly || events[i].data[3] == csPtr->myPlayerNum) {
            clientSoundDist(&csPtr->sim, sid, events[i].data[1], events[i].data[2]);
          }
        }
        break;
      case EVENT_SOUND_SHOOT:
        /* data: [soundId, mx, my, firingPlayer] — skip own shots (client plays shootSelf via prediction) */
        if (isHuman && events[i].data[3] != csPtr->myPlayerNum) {
          clientSoundDist(&csPtr->sim, shootNear, events[i].data[1], events[i].data[2]);
        }
        break;
      case EVENT_SOUND_TANK_HIT:
        /* data: [soundId, mx, my, hitPlayer] */
        if (isHuman) {
          if (events[i].data[3] == csPtr->myPlayerNum) {
            frontEndPlaySound(csPtr, hitTankSelf);
          } else {
            clientSoundDist(&csPtr->sim, hitTankNear, events[i].data[1], events[i].data[2]);
          }
        }
        break;
      case EVENT_EXPLOSION:
        /* data: [mx, my, px, py] — create explosion locally */
        explosionsAddItem(&csPtr->sim.expl,
                           events[i].data[0], events[i].data[1],
                           events[i].data[2], events[i].data[3],
                           EXPLOSION_START);
        break;
      case EVENT_TK_EXPLOSION: {
        /* data: [xHi, xLo, yHi, yLo, angle, length, explodeType, creator]
         * Spawn the fireball locally; tkExplosionUpdate animates it. */
        WORLD tkX = (WORLD)(((uint16_t)events[i].data[0] << 8) | events[i].data[1]);
        WORLD tkY = (WORLD)(((uint16_t)events[i].data[2] << 8) | events[i].data[3]);
        TURNTYPE tkAngle = (TURNTYPE)events[i].data[4];
        BYTE tkLength = events[i].data[5];
        BYTE tkType = events[i].data[6];
        BYTE tkCreator = events[i].data[7];
        tkExplosionAddItemFromSnapshot(&csPtr->sim, tkX, tkY, tkAngle,
                                       tkLength, tkType, tkCreator);
        break;
      }
      case EVENT_BASE_CAPTURED:
        /* data: [newOwner, previousOwner] */
        if (isHuman) {
          basesEnqueueCaptureMessage(&csPtr->sim, csPtr,
                                     events[i].data[0], events[i].data[1]);
        }
        /* Steam stat: base captures */
        if (events[i].data[0] == playerNum) {
          if (events[i].data[1] == NEUTRAL) {
            steam_increment_stat("STAT_BASES_CAPTURED_NEUTRAL", 1);
            steamStatsUpdated = true;
          } else if (playersIsAllie(&csPtr->sim.plyrs, playerNum, events[i].data[1]) == FALSE) {
            steam_increment_stat("STAT_BASES_CAPTURED_ENEMY", 1);
            steamStatsUpdated = true;
          }
        }
        /* Steam achievement: first base capture (networked, non-allied) */
        if (csPtr->networkGameType != netSingle &&
            csPtr->hasAnyBaseCaptured == false &&
            events[i].data[0] == playerNum &&
            events[i].data[1] != NEUTRAL &&
            playersIsAllie(&csPtr->sim.plyrs, playerNum, events[i].data[1]) == FALSE) {
          steam_set_achievement("ACH_FIRST_BASE");
          steamStatsUpdated = true;
        }
        csPtr->hasAnyBaseCaptured = true;
        break;
      case EVENT_PILL_CAPTURED:
        /* data: [newOwner, previousOwner] */
        if (isHuman) {
          BYTE newOwner = events[i].data[0];
          BYTE prevOwner = events[i].data[1];
          bool suppressAllied = (prevOwner != NEUTRAL &&
              playersIsAllie(&csPtr->sim.plyrs, newOwner, prevOwner) == TRUE);
          if (!suppressAllied) {
            MessageArgs args;
            memset(&args, 0, sizeof(args));
            playersMakeMessageName(csPtr, &csPtr->sim.plyrs, csPtr->myPlayerNum, newOwner, args.playerName);
            args.playerFlags = playersGetAccountFlags(&csPtr->sim.plyrs, newOwner);
            playersGetCountryCode(&csPtr->sim.plyrs, newOwner, args.playerCountry);
            if (prevOwner != NEUTRAL) {
              playersGetPlayerName(&csPtr->sim.plyrs, prevOwner, args.otherName, FALSE);
              args.otherFlags = playersGetAccountFlags(&csPtr->sim.plyrs, prevOwner);
              playersGetCountryCode(&csPtr->sim.plyrs, prevOwner, args.otherCountry);
              csPtr->sim.callbacks.messageAdd(csPtr->sim.callbacks.ctx, newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_STOLE_PILL, &args);
            } else {
              csPtr->sim.callbacks.messageAdd(csPtr->sim.callbacks.ctx, newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_CAPTURE_PILL, &args);
            }
          }
        }
        /* Steam stat: pill captures */
        if (events[i].data[0] == playerNum) {
          if (events[i].data[1] == NEUTRAL) {
            steam_increment_stat("STAT_PILLS_CAPTURED_NEUTRAL", 1);
            steamStatsUpdated = true;
          } else if (playersIsAllie(&csPtr->sim.plyrs, playerNum, events[i].data[1]) == FALSE) {
            steam_increment_stat("STAT_PILLS_CAPTURED_ENEMY", 1);
            steamStatsUpdated = true;
          }
        }
        /* Steam achievement: first pill capture (networked, non-allied) */
        if (csPtr->networkGameType != netSingle &&
            csPtr->hasAnyPillCaptured == false &&
            events[i].data[0] == playerNum &&
            events[i].data[1] != NEUTRAL &&
            playersIsAllie(&csPtr->sim.plyrs, playerNum, events[i].data[1]) == FALSE) {
          steam_set_achievement("ACH_FIRST_PILL");
          steamStatsUpdated = true;
        }
        csPtr->hasAnyPillCaptured = true;
        break;
      case EVENT_PILL_UPDATE:
        /* data: [pillIndex, x, y, owner, armour, speed, inTank] */
        {
          BYTE idx = events[i].data[0];
          if (idx < MAX_PILLS && csPtr->sim.pb != NULL) {
            (*csPtr->sim.pb).item[idx].x      = events[i].data[1];
            (*csPtr->sim.pb).item[idx].y      = events[i].data[2];
            (*csPtr->sim.pb).item[idx].owner  = events[i].data[3];
            (*csPtr->sim.pb).item[idx].armour = events[i].data[4];
            (*csPtr->sim.pb).item[idx].speed  = events[i].data[5];
            (*csPtr->sim.pb).item[idx].inTank = events[i].data[6] ? TRUE : FALSE;
          }
        }
        break;
      case EVENT_BASE_UPDATE:
        /* data: [baseIndex, owner, armour, shells, mines] */
        {
          BYTE idx = events[i].data[0];
          if (idx < MAX_BASES && csPtr->sim.bs != NULL) {
            (*csPtr->sim.bs).item[idx].owner  = events[i].data[1];
            (*csPtr->sim.bs).item[idx].armour = events[i].data[2];
            (*csPtr->sim.bs).item[idx].shells = events[i].data[3];
            (*csPtr->sim.bs).item[idx].mines  = events[i].data[4];
          }
        }
        break;
      case EVENT_PLAYER_LEAVE:
        /* Intentionally no player removal here. Removal rides the reliable
         * CTRL_PLAYER_LEAVE control event (client_sim_control.c), which is
         * delivered immediately and carries identity. This snapshot
         * game-event is slot-only and accumulates in sim->events while no
         * snapshots flow (e.g. the whole lobby); by the time it is flushed
         * at game start the slot may have been reused by a new player, so
         * acting on it would wrongly remove the new occupant (rendering it
         * as "???") and emit a bogus "<name> has left". The event is still
         * consumed for bot brain input (the other switch above). */
        break;
      case EVENT_SERVER_MSG:
        /* data: [msgId] — server status message (human only) */
        if (isHuman) {
          switch (events[i].data[0]) {
          case SERVER_MSG_GAME_LOCKED:
            clientSimNetStatusMessage(csPtr, "This game is now locked to new players (server lock)");
            break;
          case SERVER_MSG_GAME_UNLOCKED:
            clientSimNetStatusMessage(csPtr, "This game is now unlocked to new players (server unlock)");
            break;
          }
        }
        break;
      case EVENT_LGM_LOST:
        /* data: [victim, killer] — builder killed, broadcast newswire */
        if (isHuman) {
          MessageArgs args;
          memset(&args, 0, sizeof(args));
          playersGetPlayerName(&csPtr->sim.plyrs, events[i].data[0], args.playerName, FALSE);
          args.playerFlags = playersGetAccountFlags(&csPtr->sim.plyrs, events[i].data[0]);
          playersGetCountryCode(&csPtr->sim.plyrs, events[i].data[0], args.playerCountry);
          csPtr->sim.callbacks.messageAdd(csPtr->sim.callbacks.ctx, newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_LGM_DEAD, &args);
        }
        /* Steam stats: LGM losses and kills */
        if (events[i].data[0] == playerNum) {
          steam_increment_stat("STAT_LGM_LOSSES", 1);
          steamStatsUpdated = true;
          csPtr->myLgmLossesThisGame++;
        }
        if (events[i].data[1] == playerNum && events[i].data[0] != playerNum) {
          /* No stats for killing your own LGM */
          steam_increment_stat("STAT_LGM_KILLS", 1);
          steamStatsUpdated = true;
        }
        break;
      case EVENT_ASSISTANT_MSG:
        /* data: [targetPlayer, msgId] — player-specific assistant message */
        if (isHuman && events[i].data[0] == playerNum) {
          langid assistLangId = 0;
          switch (events[i].data[1]) {
          case ASSIST_MSG_MAN_DEAD:          assistLangId = LGM_MAN_DEAD; break;
          case ASSIST_MSG_NO_TREE:           assistLangId = LGM_NO_TREE; break;
          case ASSIST_MSG_NO_BUILD:          assistLangId = LGM_NO_BUILD; break;
          case ASSIST_MSG_NO_BUILD_BOAT:     assistLangId = LGM_NO_BUILD_UNDER_BOAT; break;
          case ASSIST_MSG_INSUFFICIENT_TREES: assistLangId = LGM_INSUFFICIENT_TREES; break;
          case ASSIST_MSG_BUILDTANK:         assistLangId = LGM_BUILDTANK; break;
          case ASSIST_MSG_PILL_NO_REPAIR:    assistLangId = LGM_PILL_NO_NEED_REPAIR; break;
          case ASSIST_MSG_NO_PILLS:          assistLangId = LGM_NO_PILLS; break;
          case ASSIST_MSG_INSUFFICIENT_MINES: assistLangId = LGM_INSUFFICIENT_MINES; break;
          case ASSIST_MSG_PILL_ON_MINE:      assistLangId = LGM_PILL_NO_BUILD_ON_MINE; break;
          case ASSIST_MSG_TANK_SUNK:         assistLangId = MESSAGE_TANKSUNK; break;
          }
          if (assistLangId != 0) {
            csPtr->sim.callbacks.messageAdd(csPtr->sim.callbacks.ctx, assistantMessage, MESSAGE_ASSISTANT, assistLangId, NULL);
          }
        }
        break;
      case EVENT_TANK_KILLED:
        /* data: [killer, killed, deathCause, carriedPills] */
        if (events[i].data[0] == playerNum && events[i].data[0] != events[i].data[1]) {
          tankAddKill(&csPtr->sim, &MY_TANK(csPtr));
          steam_increment_stat("STAT_TANK_KILLS", 1);
          steamStatsUpdated = true;
        }
        if (events[i].data[1] == playerNum) {
          csPtr->myDeathsThisGame++;
          /* Steam achievements: drown */
          if (events[i].data[2] == LAST_DEATH_BY_DEEPSEA) {
            steam_set_achievement("ACH_DROWN");
            if (events[i].data[3] > 0) {
              steam_set_achievement("ACH_DROWN_WITH_PILLS");
            }
            steamStatsUpdated = true;
          }
          /* Rapid death tracking (ACH_RAPID_DEATH) */
          {
            uint32_t now = (uint32_t)SDL_GetTicks();
            csPtr->deathTimestamps[csPtr->deathTimestampIdx] = now;
            csPtr->deathTimestampIdx = (csPtr->deathTimestampIdx + 1) % 10;
            /* Check if 10 deaths within 45 seconds */
            uint32_t oldest = csPtr->deathTimestamps[csPtr->deathTimestampIdx];
            if (oldest != 0) {
              uint32_t newest = csPtr->deathTimestamps[(csPtr->deathTimestampIdx + 9) % 10];
              if (newest - oldest < 45000) {
                steam_set_achievement("ACH_RAPID_DEATH");
                steamStatsUpdated = true;
              }
            }
          }
        }
        break;
      case EVENT_MINE_VISIBLE:
        /* data: [mx, my, sourcePlayer] — reveal mine at position */
        minesAddItem(&csPtr->sim.mns, events[i].data[0], events[i].data[1]);
        clientSimRecalc(csPtr);
        break;
      default:
        break;
      }
    }
  }

  /* Player count tracking (ACH_PLAYERS_6/8/16) */
  {
    BYTE numPlayers = playersGetNumPlayers(&csPtr->sim.plyrs);
    if (numPlayers > csPtr->maxPlayersSeenThisGame) {
      csPtr->maxPlayersSeenThisGame = numPlayers;
      if (numPlayers >= 16) {
        steam_set_achievement("ACH_PLAYERS_16");
        steam_set_achievement("ACH_PLAYERS_8");
        steam_set_achievement("ACH_PLAYERS_6");
        steamStatsUpdated = true;
      } else if (numPlayers >= 8) {
        steam_set_achievement("ACH_PLAYERS_8");
        steam_set_achievement("ACH_PLAYERS_6");
        steamStatsUpdated = true;
      } else if (numPlayers >= 6) {
        steam_set_achievement("ACH_PLAYERS_6");
        steamStatsUpdated = true;
      }
    }
  }

  if (steamStatsUpdated) {
    steam_store_stats();
  }

  /* Check map checksum on full sync ticks — must be after EVENT_MAP_CHANGE
   * processing so the client map includes changes from this snapshot */
  if (hdr->mapChecksum != 0) {
    uint16_t clientChecksum = mapCalcChecksum(&csPtr->sim.mp);
    if (clientChecksum != hdr->mapChecksum) {
      WB_LOG_WARN(WB_LOG_CAT_CLIENT, "Map checksum mismatch: server=%04x client=%04x",
              hdr->mapChecksum, clientChecksum);
    }
  }

  /* Invalidate tile cache after applying snapshot state (human only) */
  if (isHuman) {
    csPtr->viewport.needRecalc = TRUE;
  }
}
