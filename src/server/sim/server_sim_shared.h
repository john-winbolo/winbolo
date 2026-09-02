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
 * server_sim.c both feed it. */
void serverSimTrackAppend(ServerSim *sim, const void *rec, size_t n);

#endif
