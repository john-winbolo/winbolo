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
 *Name:          Server Simulation Shared Internals
 *Filename:      server_sim_shared.h
 *Author:        John Morrison
 *Purpose:
 *  The handful of functions that were file-local to
 *  server_sim.c and now cross a translation-unit boundary
 *  between the sources in this directory and their parent.
 *  This list is meant to stay short: every entry is a
 *  function that failed to stay inside one translation
 *  unit.
 *********************************************************/

#ifndef SERVER_SIM_SHARED_H
#define SERVER_SIM_SHARED_H

#include "server_sim_internal.h"   /* ServerSim, plus the sim types the signatures below name */

/* Defined in server_sim_callbacks.c — the callbacks GameSim invokes on the
 * server. serverSimInit takes the address of each one to fill
 * sim->sim.callbacks; nothing else calls them. */
void serverSimCbMessageAdd(void *ctx, messageType msgType,
                           langid topId, langid bodyId,
                           const MessageArgs *args);
void serverSimCbSoundDist(void *ctx, sndEffects value, BYTE mx, BYTE my);
void serverSimCbSoundDistShoot(void *ctx, BYTE mx, BYTE my, BYTE owner);
void serverSimCbSoundDistTankHit(void *ctx, BYTE mx, BYTE my, BYTE hitPlayer);
void serverSimCbMineVisible(void *ctx, BYTE mx, BYTE my, BYTE sourcePlayer);
void serverSimCbExplosion(void *ctx, BYTE mx, BYTE my, BYTE px, BYTE py);
void serverSimCbShellDeath(void *ctx, uint32_t fireTick, BYTE owner,
                           WORLD impactWX, WORLD impactWY,
                           uint8_t outcome);
void serverSimCbTkExplosion(void *ctx, WORLD x, WORLD y,
                            TURNTYPE angle, BYTE length,
                            BYTE explodeType, BYTE creator);
void serverSimCbTankKill(void *ctx, BYTE killer, BYTE killed, BYTE deathCause,
                         BYTE carriedPills);
void serverSimCbRecordDamage(void *ctx, BYTE attacker, BYTE targetKind,
                             BYTE targetIndex, BYTE source,
                             uint16_t dealt, bool destroyed,
                             BYTE mapX, BYTE mapY);
void serverSimCbRecordPlayerAction(void *ctx, BYTE player, BYTE actionKind,
                                   BYTE mapX, BYTE mapY);
void serverSimCbRecordPillPickup(void *ctx, BYTE picker, BYTE pillIndex,
                                 BYTE mapX, BYTE mapY);
void serverSimCbCenterTank(void *ctx);
void serverSimCbConsoleMessage(void *ctx, char *msg);

/* Defined in server_sim_callbacks.c. Appends a packed attribution record to
 * the per-round buffer; the record callbacks above and serverSimAddEvent in
 * server_sim_control.c both feed it. */
void serverSimTrackAppend(ServerSim *sim, const void *rec, size_t n);

/* Defined in server_sim_snapshot.c — the base and pill collectors. They fill
 * the snapshot's periodic full sync, and the tick core in server_sim_tick.c reads
 * them each half-step to diff this frame's bases and pills against the last. */
int serverSimGetBases(ServerSim *sim, BaseSnapshot *out, int maxOut);
int serverSimGetPills(ServerSim *sim, PillSnapshot *out, int maxOut);

/* Defined in server_sim_round.c — round-lifecycle functions that were file-local
 * to server_sim.c until their definitions moved out. Each still has a caller
 * left behind there. */

/* Full lobby reset when the last human leaves — removes bots, restores the
 * startup settings snapshot, and unlocks the lobby. serverSimRemovePlayer in
 * server_sim_players.c calls this when the departing player was the last
 * human. */
void serverSimResetLobbyToDefaults(ServerSim *sim);

/* The player slot every base owner is allied to when one side has swept
 * the map, or NEUTRAL when no side has. Same predicate as
 * serverSimCheckGameWin: a base at or below MIN_ARMOUR_CAPTURE is dead
 * and recapturable, so it does not count toward a sweep. The tick core in
 * server_sim_tick.c reads it each half-step to spot a win. */
BYTE serverSimWinningOwner(ServerSim *sim);

/* Caches the active map's BMAPBOLO MD5 so WinBolo.net can match the map against
 * its library. Called by serverSimCreate in server_sim.c and the map-reload
 * path in server_sim_maps.c, and by serverSimChangeMap alongside it. */
void serverSimCacheMapMd5FromFile(ServerSim *sim, const char *path);

/* Defined in server_sim_vote.c — the two publish helpers that were file-local
 * to server_sim.c until their definitions moved out. Each still has a caller
 * left behind there. */

/* Broadcasts the current per-slot map-skip votes as a CTRL_MAP_SKIP_STATE
 * event. serverSimRemovePlayer in server_sim_players.c calls it so a departing
 * player's vote drops off every client's tally. */
void publishMapSkipState(ServerSim *sim);

/* Server-originated English broadcast, published as CTRL_SERVER_TEXT to every
 * subscriber. The base-sweep branch of the tick core in server_sim_tick.c calls
 * it. */
void publishServerMessage(ServerSim *sim, const char *message);

/* The same line held to one team (1..MAX_TANKS-1), through destTeam on the
 * event. The surrender vote's private notices use it, and so does the scenario
 * funnel's team-text arm in server_sim_scenario.c. */
void publishServerMessageToTeam(ServerSim *sim, const char *message,
                                BYTE teamId);

/* Defined in server_sim_control.c — the one control-event filler with a caller
 * outside that translation unit. */

/* Fills a CTRL_MAP_SKIP_STATE event with the current per-slot skip votes.
 * publishMapSkipState in server_sim_vote.c calls it; the encoding itself sits
 * with the other control-event fillers in server_sim_control.c. */
void serverSimFillMapSkipStateEvent(const ServerSim *sim, ControlEvent *evt);

/* Defined in server_sim.c — the entries owned by the parent rather than by a
 * source in this directory. */

/* Arms the per-thread active-sim slot that serverSimGetActive reads back; the
 * activeSim pointer itself stays file-local to server_sim.c, so a sim/ source
 * that needs to arm it goes through this. serverSimStartGame in
 * server_sim_round.c and serverSimPublishControl in server_sim_control.c are
 * the callers. */
void serverSimSetActive(ServerSim *sim);

/* Records a terrain change into the active sim's dedicated map-event buffer,
 * where map events never compete with sound and game events for slots.
 * simRunHalfStep in server_sim_tick.c installs it through mapSetChangeCallback
 * for the duration of a running half-step. It reads the activeSim slot
 * directly, which is why it stays beside that slot in server_sim.c rather than
 * travelling with its caller. */
void simMapChangeCallback(BYTE x, BYTE y, BYTE terrain);

#endif
