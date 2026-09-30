/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 *Name:          Client State
 *Filename:      client_state.c
 *Author:        John Morrison
 *Purpose:
 *  Client-side prediction and server reconciliation.
 *  Stores an input history ring buffer so unacknowledged
 *  inputs can be replayed after a server correction.
 *********************************************************/

#include <string.h>
#include <stdlib.h>
#include "global.h"
#include "client_state.h"
#include "client_sim.h"
#include "tank.h"
#include "gametype.h"
#include "input_packet.h"
#include "game_sim.h"

/*********************************************************
 *NAME:          translateButtons
 *PURPOSE:
 *  Converts an InputPacket button bitmask to a tankButton
 *  enum value.
 *********************************************************/
static tankButton translateButtons(uint8_t buttons) {
    bool accel = (buttons & INPUT_BTN_ACCEL) != 0;
    bool decel = (buttons & INPUT_BTN_DECEL) != 0;
    bool left  = (buttons & INPUT_BTN_LEFT)  != 0;
    bool right = (buttons & INPUT_BTN_RIGHT) != 0;

    if (accel && decel) { accel = FALSE; decel = FALSE; }
    if (left && right)  { left = FALSE;  right = FALSE; }

    if (left && accel)  return TLEFTACCEL;
    if (right && accel) return TRIGHTACCEL;
    if (left && decel)  return TLEFTDECEL;
    if (right && decel) return TRIGHTDECEL;
    if (left)           return TLEFT;
    if (right)          return TRIGHT;
    if (accel)          return TACCEL;
    if (decel)          return TDECEL;
    return TNONE;
}

void clientStateCreate(ClientState *cs) {
    memset(cs, 0, sizeof(ClientState));
    cs->initialized = TRUE;
    cs->hasPredictedTank = FALSE;
}

void clientStateDestroy(ClientState *cs) {
    cs->initialized = FALSE;
    cs->hasPredictedTank = FALSE;
}

void clientStateRecordInput(ClientState *cs, const InputPacket *pkt) {
    uint8_t idx = pkt->tick & (CLIENT_INPUT_HISTORY_SIZE - 1);
    cs->history[idx] = *pkt;
    cs->newestInput = pkt->tick;
}

void clientStatePredictTick(ClientSim *csim, ClientState *cs, const InputPacket *pkt,
                            tank *predictedTank, GameSim *sim,
                            bool isKeysTick, bool inBrain) {
    tankButton tb;

    if (*predictedTank == NULL) {
        return;
    }

    /* Don't predict while dead — the server handles death/respawn
     * authoritatively and syncs the result to the client. Running
     * tankUpdate during death would trigger tankDeath on the client
     * side, picking a different start position than the server. */
    if (tankIsDestroyed(predictedTank) || tankGetDeathWait(predictedTank) > 0) {
        return;
    }

    tb = translateButtons(pkt->buttons);

    /* Apply gunsight adjustment to predicted tank for local display */
    {
        uint8_t gsAdj = (pkt->flags & INPUT_FLAG_GUNSIGHT_MASK) >> INPUT_FLAG_GUNSIGHT_SHIFT;
        if (gsAdj == 1) {
            tankGunsightIncrease(csim, sim, predictedTank);
        } else if (gsAdj == 2) {
            tankGunsightDecrease(csim, sim, predictedTank);
        }
    }

    /* Prediction only updates position/angle/speed — all side effects
     * (resources, sounds, map changes, mines, deaths) are server-authoritative. */
    sim->isPredicting = TRUE;
    if (isKeysTick) {
        BYTE bmx = tankGetMX(predictedTank);
        BYTE bmy = tankGetMY(predictedTank);
        tankTurn(sim, predictedTank, bmx, bmy, tb);
    } else {
        /* Pass shoot=FALSE here: shell prediction is handled by
         * clientSimGameTick which checks fire conditions after this
         * call and creates predicted shells in a separate array. */
        tankUpdate(sim, predictedTank, tb, FALSE, inBrain);
    }
    sim->isPredicting = FALSE;
}

bool clientStateReconcile(ClientSim *csim, ClientState *cs, uint32_t lastProcessedInput,
                          tank *predictedTank, tank serverTank,
                          GameSim *sim) {
    WORLD predX, predY, servX, servY;
    TURNTYPE predAngle, servAngle;
    uint32_t tick;
    int32_t dx, dy;

    if (*predictedTank == NULL || serverTank == NULL) {
        return FALSE;
    }

    /* Update oldest unacked based on server acknowledgment */
    cs->oldestUnacked = lastProcessedInput + 1;

    /* Compare predicted position and angle with server state */
    tankGetWorld(predictedTank, &predX, &predY);
    tankGetWorld(&serverTank, &servX, &servY);
    predAngle = tankGetAngle(predictedTank);
    servAngle = tankGetAngle(&serverTank);

    dx = (int32_t)predX - (int32_t)servX;
    dy = (int32_t)predY - (int32_t)servY;

    /* Always sync server-authoritative resource state */
    tankSyncResources(*predictedTank, serverTank);

    if (dx == 0 && dy == 0 && predAngle == servAngle) {
        /* Prediction was correct — no correction needed */
        return FALSE;
    }

    /* Prediction was wrong — snap to server state and replay */
    tankSnapToServer(*predictedTank, serverTank);

    /* Replay all unacknowledged inputs (suppress sounds/side effects) */
    sim->isPredicting = TRUE;
    for (tick = cs->oldestUnacked; tick <= cs->newestInput; tick++) {
        uint8_t idx = tick & (CLIENT_INPUT_HISTORY_SIZE - 1);
        InputPacket *histPkt = &cs->history[idx];
        bool isKeysTick;
        tankButton tb;

        /* Verify this slot holds the expected tick */
        if (histPkt->tick != tick) {
            continue;
        }

        isKeysTick = (tick % 2) == 1;
        tb = translateButtons(histPkt->buttons);

        /* Replay gunsight adjustments */
        {
            uint8_t gsAdj = (histPkt->flags & INPUT_FLAG_GUNSIGHT_MASK) >> INPUT_FLAG_GUNSIGHT_SHIFT;
            if (gsAdj == 1) {
                tankGunsightIncrease(csim, sim, predictedTank);
            } else if (gsAdj == 2) {
                tankGunsightDecrease(csim, sim, predictedTank);
            }
        }

        if (isKeysTick) {
            BYTE bmx = tankGetMX(predictedTank);
            BYTE bmy = tankGetMY(predictedTank);
            tankTurn(sim, predictedTank, bmx, bmy, tb);
        } else {
            /* Replay without shooting to avoid duplicate shells */
            tankUpdate(sim, predictedTank, tb, FALSE, FALSE);
        }
    }
    sim->isPredicting = FALSE;

    return TRUE;
}
