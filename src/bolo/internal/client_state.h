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
 *Name:          Client State
 *Filename:      client_state.h
 *Author:        John Morrison
 *Purpose:
 *  Client-side prediction and server reconciliation.
 *  Stores an input history ring buffer so unacknowledged
 *  inputs can be replayed after a server correction.
 *********************************************************/

#ifndef CLIENT_STATE_H
#define CLIENT_STATE_H

#include "global.h"
#include "input_packet.h"
#include "tank.h"
#include "bolo_map.h"
#include "bases.h"
#include "pillbox.h"
#include "shells.h"
#include "starts.h"

struct GameSim;

/* Ring buffer size — must be a power of 2 */
#define CLIENT_INPUT_HISTORY_SIZE 256

typedef struct {
    InputPacket history[CLIENT_INPUT_HISTORY_SIZE];
    uint32_t oldestUnacked;   /* Oldest tick not yet confirmed by server */
    uint32_t newestInput;     /* Most recent tick recorded */
    uint32_t serverLastProcessedInput; /* hdr->lastProcessedInput from the most
                                        * recent snapshot apply. The producer
                                        * renumbers past this when its counter
                                        * falls behind (clientBuildInputPacket),
                                        * because the server consumes tick
                                        * numbers when it substitutes for a
                                        * starved stream. Zeroed with the rest
                                        * of the connection state by
                                        * clientStateCreate; refreshed on every
                                        * snapshot. */
    uint32_t lastAppliedServerTick; /* hdr->serverTick of the most recent applied
                                     * snapshot. */
    uint32_t prevAppliedServerTick; /* hdr->serverTick of the second-newest applied
                                     * snapshot — the frame interp actually displays
                                     * (curr is one behind the newest). Stays 0 until
                                     * two snapshots have been applied. Stamped onto
                                     * outgoing inputs as viewTick for exact lag
                                     * compensation. Zeroed with the rest of the
                                     * connection state by clientStateCreate. */
    bool initialized;
    bool hasPredictedTank;    /* TRUE once we own a separate predicted tank */
} ClientState;

/*********************************************************
 *NAME:          clientStateCreate
 *PURPOSE:
 *  Initializes the client state for prediction.
 *
 *ARGUMENTS:
 *  cs - Pointer to ClientState to initialize
 *********************************************************/
void clientStateCreate(ClientState *cs);

/*********************************************************
 *NAME:          clientStateDestroy
 *PURPOSE:
 *  Cleans up client state.
 *
 *ARGUMENTS:
 *  cs - Pointer to ClientState to destroy
 *********************************************************/
void clientStateDestroy(ClientState *cs);

/*********************************************************
 *NAME:          clientStateRecordInput
 *PURPOSE:
 *  Records an InputPacket in the history ring buffer.
 *
 *ARGUMENTS:
 *  cs  - Pointer to ClientState
 *  pkt - The InputPacket to record
 *********************************************************/
void clientStateRecordInput(ClientState *cs, const InputPacket *pkt);

/*********************************************************
 *NAME:          clientStatePredictTick
 *PURPOSE:
 *  Applies the current input to the predicted tank using
 *  the same physics as the server (tankTurn/tankUpdate).
 *
 *ARGUMENTS:
 *  cs            - Pointer to ClientState
 *  pkt           - The InputPacket being applied
 *  predictedTank - Pointer to the predicted tank
 *  sim           - Pointer to GameSim
 *  isKeysTick    - TRUE for keys-only tick (tankTurn),
 *                  FALSE for full game tick (tankUpdate)
 *  inBrain       - TRUE if a brain is running
 *********************************************************/
void clientStatePredictTick(struct ClientSim *csim, ClientState *cs, const InputPacket *pkt,
                            tank *predictedTank, struct GameSim *sim,
                            bool isKeysTick, bool inBrain);

/*********************************************************
 *NAME:          clientStateReconcile
 *PURPOSE:
 *  Compares the predicted tank with the server's
 *  authoritative tank. If they differ, snaps the
 *  predicted tank to the server's state and replays
 *  all unacknowledged inputs.
 *
 *  Returns TRUE if a correction was applied.
 *
 *ARGUMENTS:
 *  cs                 - Pointer to ClientState
 *  lastProcessedInput - Tick of last input server processed
 *  predictedTank      - Pointer to the client's predicted tank
 *  serverTank         - The server's authoritative tank
 *  sim                - Pointer to GameSim
 *********************************************************/
bool clientStateReconcile(struct ClientSim *csim, ClientState *cs, uint32_t lastProcessedInput,
                          tank *predictedTank, tank serverTank,
                          struct GameSim *sim);

#endif /* CLIENT_STATE_H */
