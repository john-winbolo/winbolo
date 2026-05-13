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
 * ctx:           Opaque pointer to implementation data.
 *********************************************************/
/* Identifies a Transport's underlying implementation. The lobby /
 * gamefront layer uses this to skip UDP-only command sends
 * (transportUdpClient* family) when running in single-player against
 * the local transport — those send fns reinterpret ctx as a
 * TransportUdpClientCtx and would corrupt memory if called against a
 * TransportLocalCtx. Set by each implementation in its create fn. */
typedef enum {
    TRANSPORT_KIND_UNKNOWN = 0,
    TRANSPORT_KIND_LOCAL,
    TRANSPORT_KIND_UDP_CLIENT
} TransportKind;

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
    void *ctx;
    TransportKind kind;
} Transport;

/*********************************************************
 * transport_local — single-player (in-process server)
 *********************************************************/

/* Creates a local transport backed by a ServerSim.
 * The ServerSim must already be initialized.
 * playerNum is the local player's slot. */
Transport transportLocalCreate(struct ServerSim *sim, BYTE playerNum);

/* Creates a passive local transport that does NOT call serverSimTick().
 * Used for bot ClientSim instances that share a ServerSim with the
 * human player's transport (which owns the ticking). */
Transport transportLocalCreatePassive(struct ServerSim *sim, BYTE playerNum);

/* Destroys local transport resources (does NOT destroy the ServerSim). */
void transportLocalDestroy(Transport *t);

/* Sets the simulated network latency in milliseconds.
 * delay_ms is converted to ticks internally (1 tick = 20ms).
 * 0 = no latency (default). */
void transportLocalSetDelay(Transport *t, uint16_t delay_ms);

/* Returns the currently configured latency in milliseconds. */
uint16_t transportLocalGetDelay(Transport *t);

/*********************************************************
 * transport_udp — multiplayer over UDP
 *
 * See transport_udp.h for the full API including
 * server-side functions, join handshake, snapshots,
 * and ping measurement.
 *********************************************************/

#endif /* TRANSPORT_H */
