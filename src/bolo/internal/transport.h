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
 *Name:          Transport
 *Filename:      transport.h
 *Author:        John Morrison
 *Purpose:
 *  Defines the Transport interface as a struct of function
 *  pointers. Implementations provide local (single-player)
 *  or network (UDP) transport.
 *********************************************************/

#ifndef TRANSPORT_H
#define TRANSPORT_H

#include "global.h"
#include "input_packet.h"

/* Forward decls — both ServerSim and ClientSim are referenced by the
 * transport constructors. Full definitions live in server_sim.h /
 * client_sim.h respectively; this header stays narrow on includes. */
struct ServerSim;
struct ClientSim;

/* Forward declaration — ServerSim is defined in server_sim.h. */
struct ServerSim;

/*********************************************************
 * Transport interface
 *
 * recordInput:   Record an InputPacket for later sending.
 *                Used on keys ticks to buffer input without
 *                sending a UDP packet.  The next sendInput
 *                will include this input via redundancy.
 *                For local transport this is the same as
 *                sendInput (immediate enqueue).
 * sendInput:     Record and send an InputPacket to the server.
 * tick:          Run one server tick (local transport calls
 *                serverSimTick; network transport is a no-op).
 * getSnapshot:   Retrieve the latest snapshot from the server.
 *                Returns TRUE if new state is available.
 *                Fills header, tank/shell/explosion/event arrays.
 *                Precondition: caller holds threadsMutex (e.g. via
 *                clientMutexWaitFor). Implementations may read shared
 *                server state without taking the lock themselves.
 * drainSnapshots: Receive and apply any pending snapshots (and other inbound
 *                packets) without advancing per-tick state — no localTick
 *                advance, resends, acks, ping, or timeouts.  Called from the
 *                per-tick pump and, once per render frame, from the render
 *                seam so a frame composes from the freshest snapshot.  The
 *                local transport has no socket, so its entry is a no-op.
 *                Precondition: same as getSnapshot (caller holds the lock).
 * ctx:           Opaque pointer to implementation data.
 *********************************************************/
typedef struct {
    void (*recordInput)(void *ctx, const InputPacket *input);
    void (*sendInput)(void *ctx, const InputPacket *input);
    bool (*tick)(void *ctx);
    bool (*getSnapshot)(void *ctx, BYTE clientIdx,
                        SnapshotHeader *hdr,
                        TankSnapshot *tanks, int maxTanks,
                        ShellSnapshot *shells, int maxShells,
                        TkExplosionSnapshot *tkExplosions, int maxTkExplosions,
                        BaseSnapshot *bases, int maxBases,
                        PillSnapshot *pills, int maxPills,
                        GameEvent *events, int maxEvents);
    void (*drainSnapshots)(void *ctx);
    void *ctx;
} Transport;

/*********************************************************
 * transport_local — single-player (in-process server)
 *********************************************************/

/* Creates a local transport backed by a ServerSim.
 * The ServerSim must already be initialized.
 * cs is the owning ClientSim — used by the per-tick snapshot apply.
 * playerNum is the local player's slot (or 0 as placeholder; refine
 * later via transportLocalSetPlayerNum once the join assigns a slot). */
Transport transportLocalCreate(struct ServerSim *sim, struct ClientSim *cs, BYTE playerNum);

/* Creates a passive local transport that does NOT call serverSimTick().
 * Used for bot ClientSim instances that share a ServerSim with the
 * human player's transport (which owns the ticking). */
Transport transportLocalCreatePassive(struct ServerSim *sim, struct ClientSim *cs, BYTE playerNum);

/* Destroys local transport resources (does NOT destroy the ServerSim). */
void transportLocalDestroy(Transport *t);

/* Refine the local-transport slot after the join assigns it. The
 * snapshot apply inside localTick uses this as its clientIdx. */
void transportLocalSetPlayerNum(Transport *t, BYTE playerNum);

/* Sets the simulated network latency in milliseconds.
 * delay_ms is converted to ticks internally (1 tick = 20ms).
 * 0 = no latency (default). */
void transportLocalSetDelay(Transport *t, uint16_t delay_ms);

/* Returns the currently configured latency in milliseconds. */
uint16_t transportLocalGetDelay(Transport *t);

/* The frame queue, for a passive transport whose server is ticked on another
 * thread than the one that polls it (desktop single player). A snapshot
 * carries only the events of the frame it was built in, so a poll that comes
 * after two server frames loses the first one's events. With the queue on, the
 * ticking thread calls transportLocalCaptureFrame after every server frame and
 * tick() applies each captured frame in order. Off by default; turning it on
 * or off empties it. Callers of all three hold the threads mutex. */
void transportLocalSetFrameQueue(Transport *t, bool on);
bool transportLocalFrameQueueOn(Transport *t);
void transportLocalCaptureFrame(Transport *t);

/* With the frame queue on, writes every square of the client's map whose
 * terrain differs from the copy the server checksums this slot against, and
 * returns how many it wrote; 0 with the queue off. Mines the client knows of
 * are kept and none are revealed. The single-player answer to a map checksum
 * mismatch, which a UDP client answers with a resync. Caller holds the
 * threads mutex. */
struct GameSim;
int transportLocalRepairMap(Transport *t, struct GameSim *clientGs);

/*********************************************************
 * transport_udp — multiplayer over UDP
 *
 * See transport_udp.h for the full API including
 * server-side functions, join handshake, snapshots,
 * and ping measurement.
 *********************************************************/

#endif /* TRANSPORT_H */
