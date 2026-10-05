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
#include "control_event.h"
#include "game_sim.h"    /* GameSim — the client map transportLocalRepairMap writes */
#include "bolo_map.h"    /* mapSetPos */
#include "../common/wb_log.h"
/* The passive variant is driven from a different thread than the one that
 * ticks ServerSim, so it self-serialises on the server's threadsMutex. */
#include "../server/threads.h"

/* Size of the delayed input queue — must be a power of 2 */
#define INPUT_QUEUE_SIZE 256

/* Frames the frame queue holds before it starts dropping the oldest: 64 server
 * frames is 1.28 s of a main thread that has stopped polling (a window being
 * dragged, a modal dialog). A frame dropped here loses its one-shot events the
 * way an unqueued poll does; the full sync puts pills and bases back, and
 * transportLocalRepairMap puts the terrain back. */
#define LOCAL_FRAME_QUEUE_SIZE 64

/* Frames at the new end of a drain that keep their sound events. A poll that
 * is a frame late applies two frames and should sound like both. */
#define LOCAL_FRAME_SOUND_KEEP 2

/* One server frame's snapshot for this transport's slot, built at the end of
 * the frame by transportLocalCaptureFrame. */
typedef struct {
    SnapshotHeader      hdr;
    TankSnapshot        tanks[MAX_TANKS];
    ShellSnapshot       shells[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot tkExplosions[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot        bases[MAX_SNAPSHOT_BASES];
    PillSnapshot        pills[MAX_SNAPSHOT_PILLS];
    GameEvent           events[MAX_SNAPSHOT_EVENTS];
} LocalFrame;

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
    /* The frame queue, or NULL when it is off. A snapshot carries only the
     * events of the frame it was built in, so a client that polls after the
     * server has run two frames never sees the first frame's events — map
     * changes included. Desktop single player ticks its server on a timer
     * thread and polls from the main thread, which can fall a frame behind,
     * so it turns this on and its timer captures every frame here; localTick
     * then applies each captured frame in order instead of building one. */
    LocalFrame *frames;
    uint32_t    frameHead;      /* Index of the oldest captured frame */
    uint32_t    frameCount;     /* Frames captured and not yet applied */
    uint32_t    framesDropped;  /* Frames lost to a full queue, for the log */
} TransportLocalCtx;

/* Control events apply synchronously, ahead of any captured snapshots the
 * main thread has not polled yet. A world boundary must discard those old
 * snapshots before they can undo the reset or write into the replacement map.
 * The subscriber invokes this observer under the same threads mutex as capture
 * and polling. Only a transport with its frame queue on installs it. */
static void localFrameQueueControlObserver(void *ctx, const ControlEvent *evt) {
    TransportLocalCtx *lctx = (TransportLocalCtx *)ctx;
    switch (evt->type) {
    case CTRL_LOBBY_MAP_CHANGE:
    case CTRL_GAME_PHASE_LOBBY:
    case CTRL_GAME_PHASE_RUNNING:
        lctx->frameHead = 0;
        lctx->frameCount = 0;
        /* A new world can reuse the previous world's tick number. Its
         * events must not be suppressed by the old same-tick dedup. */
        lctx->hasLastDelivered = false;
        /* A discarded frame may have consumed this slot's full sync. */
        if (lctx->sim != NULL && lctx->playerNum < MAX_TANKS) {
            lctx->sim->lastFullSyncTick[lctx->playerNum] = 0;
        }
        break;
    default:
        break;
    }
}

/* Takes the sound events out of a captured frame, keeping the order of the
 * rest. */
static void localFrameDropSounds(LocalFrame *f) {
    int kept = 0;
    int i;
    for (i = 0; i < (int)f->hdr.reliableEventCount; i++) {
        if (soundEventIsSound(f->events[i].type)) continue;
        f->events[kept++] = f->events[i];
    }
    f->hdr.reliableEventCount = (uint8_t)kept;
}

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

    /* With the frame queue on, apply every frame captured since the last
     * poll, oldest first, and build nothing here: the capture already built
     * each one, and building again would spend the full sync and the arrival
     * stock push a second time. No captured frame means the server has not
     * run since the last poll, and there is nothing new to apply. */
    if (lctx->cs != NULL && lctx->frames != NULL) {
        while (lctx->frameCount > 0) {
            LocalFrame *f = &lctx->frames[lctx->frameHead];
            /* A main thread that stalled drains a run of frames at once, and
             * their sounds would all play on top of each other. Only the
             * newest frames keep theirs; everything else in a frame still
             * applies. */
            if (lctx->frameCount > LOCAL_FRAME_SOUND_KEEP) {
                localFrameDropSounds(f);
            }
            clientSimSyncFromSnapshot(lctx->cs, &f->hdr,
                                      f->tanks, f->hdr.tankCount,
                                      f->shells, f->hdr.shellCount,
                                      f->tkExplosions, f->hdr.tkExplosionCount,
                                      f->bases, f->hdr.baseCount,
                                      f->pills, f->hdr.pillCount,
                                      f->events, f->hdr.reliableEventCount,
                                      lctx->playerNum);
            lctx->frameHead = (lctx->frameHead + 1) % LOCAL_FRAME_QUEUE_SIZE;
            lctx->frameCount--;
        }
    } else if (lctx->cs != NULL) {
        /* Pull and apply a snapshot every tick — the local transport now
         * owns the client-side snapshot apply that frontends used to drive
         * via per-frame clientSimNetSyncSnapshot calls.  Routed through
         * localGetSnapshot so the per-tick event dedup applies here too. */
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

void transportLocalSetFrameQueue(Transport *t, bool on) {
    TransportLocalCtx *lctx;
    if (t == NULL || t->ctx == NULL) return;
    lctx = (TransportLocalCtx *)t->ctx;
    /* Only a transport with a ClientSim to apply into polls the frames. */
    if (on && lctx->frames == NULL && lctx->cs != NULL) {
        lctx->frames = (LocalFrame *)calloc(LOCAL_FRAME_QUEUE_SIZE,
                                            sizeof(LocalFrame));
        if (lctx->frames != NULL) {
            clientSimSetTransportControlObserver(lctx->cs,
                                                  localFrameQueueControlObserver,
                                                  lctx);
        }
    } else if (!on && lctx->frames != NULL) {
        clientSimSetTransportControlObserver(lctx->cs, NULL, NULL);
        free(lctx->frames);
        lctx->frames = NULL;
    }
    lctx->frameHead = 0;
    lctx->frameCount = 0;
}

bool transportLocalFrameQueueOn(Transport *t) {
    return t != NULL && t->ctx != NULL &&
           ((TransportLocalCtx *)t->ctx)->frames != NULL;
}

void transportLocalCaptureFrame(Transport *t) {
    TransportLocalCtx *lctx;
    LocalFrame *f;
    if (t == NULL || t->ctx == NULL) return;
    lctx = (TransportLocalCtx *)t->ctx;
    if (lctx->frames == NULL) return;

    if (lctx->frameCount == LOCAL_FRAME_QUEUE_SIZE) {
        lctx->frameHead = (lctx->frameHead + 1) % LOCAL_FRAME_QUEUE_SIZE;
        lctx->frameCount--;
        lctx->framesDropped++;
        if (lctx->framesDropped == 1 || (lctx->framesDropped % 500) == 0) {
            WB_LOG_WARN(WB_LOG_CAT_CLIENT,
                        "local frame queue full: %u frame(s) dropped so far",
                        (unsigned)lctx->framesDropped);
        }
    }
    f = &lctx->frames[(lctx->frameHead + lctx->frameCount) % LOCAL_FRAME_QUEUE_SIZE];
    /* Through localGetSnapshot, so a capture carries the arrival stock push
     * and the same-tick dedup the pull has always had: a lobby frame that
     * did not move the tick delivers its events once. */
    localGetSnapshot(lctx, lctx->playerNum, &f->hdr,
                     f->tanks, MAX_TANKS,
                     f->shells, MAX_SNAPSHOT_SHELLS,
                     f->tkExplosions, MAX_SNAPSHOT_TK_EXPLOSIONS,
                     f->bases, MAX_SNAPSHOT_BASES,
                     f->pills, MAX_SNAPSHOT_PILLS,
                     f->events, MAX_SNAPSHOT_EVENTS);
    lctx->frameCount++;
}

/* A terrain byte with any mine taken off it — the form mapCalcChecksum
 * compares in. */
static BYTE localStripMine(BYTE t) {
    return (t >= MINE_START && t <= MINE_END) ? (BYTE)(t - MINE_SUBTRACT) : t;
}

int transportLocalRepairMap(Transport *t, GameSim *clientGs) {
    TransportLocalCtx *lctx;
    map *known;
    int repaired = 0;
    int x, y;

    if (t == NULL || t->ctx == NULL || clientGs == NULL ||
        clientGs->mp == NULL) {
        return 0;
    }
    lctx = (TransportLocalCtx *)t->ctx;
    /* Only behind the frame queue, which is to say desktop single player.
     * Bots and the transports that tick the server themselves read every
     * frame as it ends and have nothing to lose. */
    if (lctx->frames == NULL || lctx->sim == NULL ||
        lctx->playerNum >= MAX_TANKS) {
        return 0;
    }
    /* The copy the snapshot checksum is taken over, so a repaired map is one
     * the next full sync agrees with. */
    known = (lctx->sim->clientKnownMap[lctx->playerNum] != NULL)
                ? &lctx->sim->clientKnownMap[lctx->playerNum]
                : &lctx->sim->sim.mp;

    for (x = 0; x < MAP_ARRAY_SIZE; x++) {
        for (y = 0; y < MAP_ARRAY_SIZE; y++) {
            BYTE held = (*clientGs->mp).mapItem[x][y];
            BYTE raw = (*known)->mapItem[x][y];
            BYTE truth = localStripMine(raw);
            BYTE terrain = truth;
            if (localStripMine(held) == truth) continue;
            /* Mines are the client's own business: it keeps a mine it knows
             * of while the server still has one on the square, and is never
             * shown a mine it does not know of. A mine the server no longer
             * has went with the change being repaired — it blew up and left
             * the crater — so it is not carried onto the new ground. */
            if (held >= MINE_START && held <= MINE_END &&
                raw >= MINE_START && raw <= MINE_END) {
                terrain = raw;
            }
            /* The path an EVENT_MAP_CHANGE takes on the client. */
            mapSetPos(clientGs, &clientGs->mp, (BYTE)x, (BYTE)y, terrain,
                      FALSE, TRUE);
            repaired++;
        }
    }
    if (repaired > 0) {
        WB_LOG_WARN(WB_LOG_CAT_CLIENT,
                    "local map checksum mismatch: repaired %d square(s) from the server",
                    repaired);
    }
    return repaired;
}

void transportLocalDestroy(Transport *t) {
    if (t->ctx != NULL) {
        TransportLocalCtx *lctx = (TransportLocalCtx *)t->ctx;
        if (lctx->frames != NULL) {
            clientSimSetTransportControlObserver(lctx->cs, NULL, NULL);
        }
        free(lctx->frames);
        free(t->ctx);
        t->ctx = NULL;
    }
}
