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

#endif /* CLIENT_SNAPSHOT_H */
