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
 *Name:          Transport Local
 *Filename:      transport_local.c
 *Author:        John Morrison
 *Purpose:
 *  Local (in-process) transport for single-player.
 *  sendInput() enqueues into ServerSim input queue.
 *  tick() calls serverSimTick() directly.
 *
 *  When delay_ms > 0, inputs are queued and delivered to
 *  the server with a delay, simulating network latency.
 *  This allows testing client-side prediction without a
 *  real network.
 *********************************************************/

#include <stdlib.h>
#include <string.h>
#include "global.h"
#include "transport.h"
#include "server_sim.h"
#include "server_sim_internal.h"  /* serverSimTakeArrivalBaseStock — local arrival push */
#include "client_sim.h"  /* clientSimSyncFromSnapshot — per-tick snapshot apply */
/* The passive variant is driven from a different thread than the one that
 * ticks ServerSim, so it self-serialises on the server's threadsMutex. */
#include "../server/threads.h"

/* Size of the delayed input queue — must be a power of 2 */
#define INPUT_QUEUE_SIZE 256

typedef struct {
    ServerSim *sim;
    BYTE playerNum;
    uint16_t delay_ticks;       /* Simulated one-way latency in ticks (0 = none) */
    InputPacket inputQueue[INPUT_QUEUE_SIZE];
    uint32_t queueCount;        /* Number of queued inputs */
    uint32_t queueReadIdx;
    uint32_t queueWriteIdx;
    bool ticksServer;           /* If false, tick() skips serverSimTick() */
    ClientSim *cs;              /* Owning ClientSim — snapshot dest at tick end */
    /* Per-tick event dedup. Client polls every ~10ms but the server
     * advances every ~20ms, so the same EVENT_* lands in two snapshots
     * back-to-back unless we gate.  Without this, harvest/explosion
     * sounds echo, newswire lines duplicate, and Steam stats double-
     * count.  Matches the per-event dedup the UDP transport gets
     * from CHANNEL_GAME's in-order, exactly-once delivery. */
    uint32_t lastDeliveredTick;
    bool     hasLastDelivered;
} TransportLocalCtx;

static void localSendInput(void *ctx, const InputPacket *input) {
    TransportLocalCtx *lctx = (TransportLocalCtx *)ctx;

    if (!lctx->ticksServer) {
        threadsWaitForMutex();
    }

    if (lctx->delay_ticks == 0) {
        /* Zero latency: deliver immediately */
        serverSimApplyInput(lctx->sim, input);
    } else {
        /* Queue the input for delayed delivery */
        uint8_t idx = lctx->queueWriteIdx & (INPUT_QUEUE_SIZE - 1);
        lctx->inputQueue[idx] = *input;
        lctx->queueWriteIdx++;
        lctx->queueCount++;
    }

    if (!lctx->ticksServer) {
        threadsReleaseMutex();
    }
}

static bool localGetSnapshot(void *ctx, BYTE clientIdx,
                             SnapshotHeader *hdr,
                             TankSnapshot *tanks, int maxTanks,
                             ShellSnapshot *shells, int maxShells,
                             TkExplosionSnapshot *tkExplosions, int maxTkExplosions,
                             BaseSnapshot *bases, int maxBases,
                             PillSnapshot *pills, int maxPills,
                             GameEvent *events, int maxEvents) {
    TransportLocalCtx *lctx = (TransportLocalCtx *)ctx;
    serverSimBuildSnapshot(lctx->sim, clientIdx, hdr,
                           tanks, maxTanks,
                           shells, maxShells,
                           tkExplosions, maxTkExplosions,
                           bases, maxBases,
                           pills, maxPills,
                           events, maxEvents,
                           false);
    /* The snapshot build no longer emits the arrival base-stock push (it
     * raced across clients through shared sim state). The local transport has
     * no event channel to drain, so take the push here and append it to the
     * events this snapshot delivers. The per-tick dedup below zeroes it on a
     * repeat call just like the build's events, and serverSimTakeArrivalBaseStock
     * returns false once the closest-base token is consumed, so a second call
     * the same tick adds nothing. */
    if ((int)hdr->reliableEventCount < maxEvents) {
        GameEvent arrivalEv;
        if (serverSimTakeArrivalBaseStock(lctx->sim, clientIdx, &arrivalEv)) {
            events[hdr->reliableEventCount++] = arrivalEv;
        }
    }
    if (lctx->hasLastDelivered && hdr->serverTick == lctx->lastDeliveredTick) {
        hdr->reliableEventCount = 0;
    }
    lctx->lastDeliveredTick = hdr->serverTick;
    lctx->hasLastDelivered = true;
    return TRUE;
}

static bool localTick(void *ctx) {
    TransportLocalCtx *lctx = (TransportLocalCtx *)ctx;
    bool selfLocked = !lctx->ticksServer;

    if (selfLocked) {
        threadsWaitForMutex();
    }

    /* Deliver delayed inputs when they've aged enough */
    if (lctx->delay_ticks > 0 && lctx->queueCount > lctx->delay_ticks) {
        uint8_t idx = lctx->queueReadIdx & (INPUT_QUEUE_SIZE - 1);
        serverSimApplyInput(lctx->sim, &lctx->inputQueue[idx]);
        lctx->queueReadIdx++;
        lctx->queueCount--;
    }

    if (lctx->ticksServer) {
        serverSimTick(lctx->sim);
    }

    /* Pull and apply a snapshot every tick — the local transport now
     * owns the client-side snapshot apply that frontends used to drive
     * via per-frame clientSimNetSyncSnapshot calls.  Routed through
     * localGetSnapshot so the per-tick event dedup applies here too. */
    if (lctx->cs != NULL) {
        SnapshotHeader snapHdr;
        TankSnapshot snapTanks[MAX_TANKS];
        ShellSnapshot snapShells[MAX_SNAPSHOT_SHELLS];
        TkExplosionSnapshot snapTkExplosions[MAX_SNAPSHOT_TK_EXPLOSIONS];
        BaseSnapshot snapBases[MAX_SNAPSHOT_BASES];
        PillSnapshot snapPills[MAX_SNAPSHOT_PILLS];
        GameEvent snapEvents[MAX_SNAPSHOT_EVENTS];
        localGetSnapshot(ctx, lctx->playerNum, &snapHdr,
                         snapTanks, MAX_TANKS,
                         snapShells, MAX_SNAPSHOT_SHELLS,
                         snapTkExplosions, MAX_SNAPSHOT_TK_EXPLOSIONS,
                         snapBases, MAX_SNAPSHOT_BASES,
                         snapPills, MAX_SNAPSHOT_PILLS,
                         snapEvents, MAX_SNAPSHOT_EVENTS);
        clientSimSyncFromSnapshot(lctx->cs, &snapHdr,
                                  snapTanks, snapHdr.tankCount,
                                  snapShells, snapHdr.shellCount,
                                  snapTkExplosions, snapHdr.tkExplosionCount,
                                  snapBases, snapHdr.baseCount,
                                  snapPills, snapHdr.pillCount,
                                  snapEvents, snapHdr.reliableEventCount,
                                  lctx->playerNum);
    }

    if (selfLocked) {
        threadsReleaseMutex();
    }
    return TRUE;
}

/* The local (in-process) transport has no socket to drain — its snapshots
 * are pulled and applied inline in localTick — so the render-path drain is a
 * no-op for it.  Remote tanks on a listen-server host still interpolate via
 * the render seam; only the socket receive has nothing to do here. */
static void localDrainSnapshots(void *ctx) {
    (void)ctx;
}

Transport transportLocalCreate(ServerSim *sim, ClientSim *cs, BYTE playerNum) {
    Transport t;
    TransportLocalCtx *lctx = (TransportLocalCtx *)malloc(sizeof(TransportLocalCtx));
    memset(lctx, 0, sizeof(TransportLocalCtx));
    lctx->sim = sim;
    lctx->cs = cs;
    lctx->playerNum = playerNum;
    lctx->delay_ticks = 0;
    lctx->ticksServer = true;
    t.recordInput = localSendInput;
    t.sendInput = localSendInput;
    t.tick = localTick;
    t.getSnapshot = localGetSnapshot;
    t.drainSnapshots = localDrainSnapshots;
    t.ctx = lctx;
    return t;
}

Transport transportLocalCreatePassive(ServerSim *sim, ClientSim *cs, BYTE playerNum) {
    Transport t = transportLocalCreate(sim, cs, playerNum);
    TransportLocalCtx *lctx = (TransportLocalCtx *)t.ctx;
    lctx->ticksServer = false;
    return t;
}

void transportLocalSetPlayerNum(Transport *t, BYTE playerNum) {
    TransportLocalCtx *lctx;
    if (t == NULL || t->ctx == NULL) return;
    lctx = (TransportLocalCtx *)t->ctx;
    lctx->playerNum = playerNum;
}

void transportLocalSetDelay(Transport *t, uint16_t delay_ms) {
    TransportLocalCtx *lctx;
    if (t == NULL || t->ctx == NULL) {
        return;
    }
    lctx = (TransportLocalCtx *)t->ctx;
    /* Each tick is ~20ms, so delay_ticks = delay_ms / 20 */
    lctx->delay_ticks = delay_ms / 20;
}

uint16_t transportLocalGetDelay(Transport *t) {
    TransportLocalCtx *lctx;
    if (t == NULL || t->ctx == NULL) {
        return 0;
    }
    lctx = (TransportLocalCtx *)t->ctx;
    return lctx->delay_ticks * 20;
}

void transportLocalDestroy(Transport *t) {
    if (t->ctx != NULL) {
        free(t->ctx);
        t->ctx = NULL;
    }
}
