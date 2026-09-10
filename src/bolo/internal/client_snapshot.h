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
 *Name:          Client Snapshot
 *Filename:      client_snapshot.h
 *Purpose:
 *  Public API for client_snapshot.c — applies server
 *  snapshots to the ClientSim, packages local input into
 *  InputPackets, and finalizes tank setup after the server
 *  has placed the player.
 *********************************************************/

#ifndef CLIENT_SNAPSHOT_H
#define CLIENT_SNAPSHOT_H

#include "global.h"
#include "input_packet.h"
#include "tank.h"

struct ClientSim;

void clientApplySnapshot(struct ClientSim *cs,
                         const SnapshotHeader *hdr,
                         const TankSnapshot *tanks, int tankCount,
                         const ShellSnapshot *shellSnaps, int shellCount,
                         const TkExplosionSnapshot *tkExplSnaps, int tkExplosionCount,
                         const BaseSnapshot *baseSnaps, int baseCount,
                         const PillSnapshot *pillSnaps, int pillCount,
                         const GameEvent *events, int eventCount,
                         BYTE playerNum);

/* Settles base-death prediction for base `idx` against authoritative
 * armour dated by `serverInputTick` (the carrying snapshot's
 * lastProcessedInput). Keeps a stamp the server has not yet reached while
 * one more hit still kills, drops one the server has disproved, and arms a
 * landing that was waiting on an earlier hit's armour. Both the full-sync
 * loop and the EVENT_BASE_STOCK path go through here. */
void clientBaseArmourArrived(struct ClientSim *cs, BYTE idx, BYTE armour,
                             uint32_t serverInputTick);

/* Per-render-frame display update for other players' tanks: interpolates
 * each remote tank against a render clock and writes the result to the
 * players struct.  Moved out of clientApplySnapshot's per-arrival path. */
void clientSnapshotRenderInterp(struct ClientSim *cs, uint32_t nowMs,
                                float extraDelayMs, bool discrete);

#endif /* CLIENT_SNAPSHOT_H */
