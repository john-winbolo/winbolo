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

/* Lowest free slot in [0, sim->maxPlayers); -1 if full. Skips bot slots.
 * forBot says who the seat is for, because the two answers differ: a
 * scenario's player cap refuses a person past it and lets a bot sit above
 * it, and this is the only place the seat's kind is known. */
int  serverSimFindFreeSlot(ServerSim *sim, bool forBot);

/* Four step-internals. None of these publish CTRL_PLAYER_JOIN by themselves —
 * a single publish lives in fillAndPublishPlayerJoin once all four fields
 * (name, country, clientType, clientFlags) are populated. */
/* country is applied to the slot before the log_PlayerJoined event is written,
 * so the recorded event carries it. NULL (bots, and callers with nothing to
 * supply) or a malformed code leaves the slot on the "XX" unknown placeholder. */
void addPlayerInternal(ServerSim *sim, BYTE slot, const char *name,
                       const char *country, bool wantRejoin);
void setPlayerCountryInternal(ServerSim *sim, BYTE slot, const char *country);
void setClientTypeFlagsInternal(ServerSim *sim, BYTE slot,
                                uint8_t clientType, uint8_t clientFlags);
void fillAndPublishPlayerJoin(ServerSim *sim, BYTE slot);

/* Reserves a free lobby start for one slot, clustered near its teammates
 * and kept to the slot's team side. Called from the join path, the bot-add
 * path, the team change, the map-change reconcile and the claim command,
 * which do not share a translation unit. Defined in server_sim_players.c. */
void serverSimAssignLobbyStartOnJoin(ServerSim *sim, BYTE slot);

/* START_SIDE_BIT_* mask of a 1-based lobby start under the current map's
 * start bounding box; 0 for a centre start or an index off the start list.
 * The one lookup the claim command and the release below share. */
BYTE serverSimLobbyStartSideMask(ServerSim *sim, BYTE idx1);

/* Union of the START_SIDE_BIT_* every team other than the slot's own has
 * chosen, counting only teams with at least one connected member — a team
 * with a side and no players closes nothing, the same "teams present" rule
 * startsAssignBatch applies. With the side mask above and the slot's team
 * side, startSideEligible answers whether the slot may hold a start; the
 * lobby pick, the release below and the claim command all ask it. */
BYTE serverSimLobbyClosedMaskFor(const ServerSim *sim, BYTE slot);

/* Drops the slot's reservation (to 0xFF) when the start it holds is not
 * one its team side allows under the sides the other teams present chose,
 * or when the index is off the current start list. Returns whether the
 * reservation changed. Does not publish. */
bool serverSimReleaseIneligibleStart(ServerSim *sim, BYTE slot);

/* Re-picks every connected slot with no reservation, humans first and
 * then bots, in slot order within each group, and publishes the slots
 * whose reservation changed. Called after a departure and a map change. */
void serverSimBackfillLobbyStarts(ServerSim *sim);

/* Clears every connected slot's reservation and re-picks them all the way
 * serverSimBackfillLobbyStarts does, publishing the slots whose reservation
 * ended up different. Called when a team's start side changes. */
void serverSimRepickAllLobbyStarts(ServerSim *sim);

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
