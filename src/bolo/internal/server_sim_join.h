/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef BOLO_INTERNAL_SERVER_SIM_JOIN_H
#define BOLO_INTERNAL_SERVER_SIM_JOIN_H

#include "server_sim.h"

typedef enum {
    LOCAL_JOIN_OK,
    LOCAL_JOIN_INVALID_NAME,
    LOCAL_JOIN_SLOT_FULL,
    LOCAL_JOIN_GAME_LOCKED,
    LOCAL_JOIN_INVALID_INPUT,
} LocalJoinResult;

/* Lowest free slot in [0, sim->maxPlayers); -1 if full. Skips bot slots. */
int  serverSimFindFreeSlot(const ServerSim *sim);

/* Four step-internals. None of these publish CTRL_PLAYER_JOIN by themselves —
 * a single publish lives in fillAndPublishPlayerJoin once all four fields
 * (name, country, clientType, clientFlags) are populated. */
void addPlayerInternal(ServerSim *sim, BYTE slot, const char *name, bool wantRejoin);
void setPlayerCountryInternal(ServerSim *sim, BYTE slot, const char *country);
void setClientTypeFlagsInternal(ServerSim *sim, BYTE slot,
                                uint8_t clientType, uint8_t clientFlags);
void fillAndPublishPlayerJoin(ServerSim *sim, BYTE slot);

/* Synchronous local-join entry point. Picks a slot via serverSimFindFreeSlot,
 * resolves the country directly from fallbackCountry (no peer addr), runs
 * the four-step internal sequence under the country-before-PLAYER_JOIN
 * invariant, then fires the WBN tracker event gated on
 * (winbolonetIsRunning() && server is WBN-registered). On non-OK return the
 * sim state is unchanged. The assigned slot is written into *outSlot when
 * the result is LOCAL_JOIN_OK; left untouched otherwise. */
LocalJoinResult serverSimLocalJoin(ServerSim *sim,
                                   const char *playerName,
                                   const char *fallbackCountry,
                                   uint8_t clientType,
                                   uint8_t clientFlags,
                                   BYTE *outSlot);

#endif
