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
#include "server_sim_internal.h" /* serverSimGameVoteToggle — T2 (sim co-owner) */
#include "client_sim.h"  /* clientSimSyncFromSnapshot — per-tick snapshot apply */
#include "control_event.h" /* ControlEvent + CTRL_CHAT — local sendBytes publishes directly */
#include "netpacks.h"    /* PACKET_HEADER_SIZE, PACKET_CHAT_MESSAGE, etc. */
#include "wire_limits.h" /* PACKET_MAX_CHAT_MESSAGE */
#include "log.h"         /* logAddEvent — replay-log parity with UDP server handlers */
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
     * via per-frame clientSimNetSyncSnapshot calls. */
    if (lctx->cs != NULL) {
        SnapshotHeader snapHdr;
        TankSnapshot snapTanks[MAX_TANKS];
        ShellSnapshot snapShells[MAX_SNAPSHOT_SHELLS];
        TkExplosionSnapshot snapTkExplosions[MAX_SNAPSHOT_TK_EXPLOSIONS];
        BaseSnapshot snapBases[MAX_SNAPSHOT_BASES];
        PillSnapshot snapPills[MAX_SNAPSHOT_PILLS];
        GameEvent snapEvents[MAX_SNAPSHOT_EVENTS];
        serverSimBuildSnapshot(lctx->sim, lctx->playerNum, &snapHdr,
                               snapTanks, MAX_TANKS,
                               snapShells, MAX_SNAPSHOT_SHELLS,
                               snapTkExplosions, MAX_SNAPSHOT_TK_EXPLOSIONS,
                               snapBases, MAX_SNAPSHOT_BASES,
                               snapPills, MAX_SNAPSHOT_PILLS,
                               snapEvents, MAX_SNAPSHOT_EVENTS,
                               false);
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

/* Local transport's sendBytes: decode the packet type out of the
 * standard 8-byte header and dispatch the same way the UDP server's
 * serverProcessPacket would. This is what makes a local-transport
 * client (SP host human, in-process bot) reach the server through
 * the SAME client_net.h send wrappers a UDP-connected client uses —
 * without this hook, the isUdpTransport gate in client_net.c would
 * silently drop every non-input send for any client on a local
 * transport.
 *
 * Sender attribution: the wire packets don't carry fromPlayer (the
 * UDP server resolves it via serverFindClient(fromAddr) from the
 * source IP). For local transport we use lctx->playerNum, which is
 * the slot this transport was set up for (and refined by
 * transportLocalSetPlayerNum after the join).
 *
 * Each handler block here is the same shape as the matching case in
 * transport_udp_server.c's serverProcessPacket — keep them in sync. */
static void localSendBytes(void *ctx, const uint8_t *buf, size_t len) {
    TransportLocalCtx *lctx = (TransportLocalCtx *)ctx;
    uint8_t pktType;

    if (lctx == NULL || lctx->sim == NULL || buf == NULL) return;
    if (len < PACKET_HEADER_SIZE) return;
    /* Same magic-check shape as getPacketType() in transport_udp_common.c —
     * inlined here to avoid pulling in transport_udp_internal.h (which
     * drags SDL + platform_net) from the local transport TU. */
    if (buf[0] != BOLO_NEW_MAGIC_0 || buf[1] != BOLO_NEW_MAGIC_1) return;
    pktType = buf[2];

    switch (pktType) {
        case PACKET_CHAT_MESSAGE: {
            /* [header 8][destPlayer 1][message...] — same layout the
             * UDP server expects. Build a CTRL_CHAT and publish; this
             * mirrors the PACKET_CHAT_MESSAGE case in
             * transport_udp_server.c's serverProcessPacket. The
             * subscriber fanout (in-process MessageState delivery
             * via client_sim_control.c's CTRL_CHAT handler + the
             * per-client UDP codec encoder) is shared, so a single
             * publish reaches every audience just like the UDP path. */
            if (len > PACKET_HEADER_SIZE + 1) {
                ControlEvent evt;
                BYTE destPlayer = buf[PACKET_HEADER_SIZE];
                size_t msgLen   = len - PACKET_HEADER_SIZE - 1;
                if (msgLen > PACKET_MAX_CHAT_MESSAGE) {
                    msgLen = PACKET_MAX_CHAT_MESSAGE;
                }
                memset(&evt, 0, sizeof(evt));
                evt.type = CTRL_CHAT;
                evt.u.chat.fromPlayer = lctx->playerNum;
                evt.u.chat.destPlayer = destPlayer;
                evt.u.chat.bodyLen    = (uint16_t)msgLen;
                if (msgLen > 0) {
                    memcpy(evt.u.chat.body,
                           buf + PACKET_HEADER_SIZE + 1, msgLen);
                }
                serverSimPublishControl(lctx->sim, &evt);
            }
            break;
        }
        case PACKET_ALLIANCE_REQUEST: {
            /* [header 8][fromPlayer 1][toPlayer 1] — fromPlayer byte
             * on the wire is informational; we use lctx->playerNum as
             * the authoritative sender just like the UDP server uses
             * serverFindClient(fromAddr). Mirrors the request case in
             * transport_udp_server.c (ranked-mode block, publish
             * CTRL_ALLIANCE_REQUEST, log_AllyRequest). The toPlayer-
             * connected check the UDP path does is UDP-table state
             * (udpServer.clients[].connected); for local we just
             * range-check the slot. */
            if (serverSimGetRanked(lctx->sim)) break;
            if (len >= PACKET_HEADER_SIZE + 2) {
                BYTE toPlayer = buf[PACKET_HEADER_SIZE + 1];
                if (toPlayer < MAX_TANKS) {
                    ControlEvent evt;
                    memset(&evt, 0, sizeof(evt));
                    evt.type = CTRL_ALLIANCE_REQUEST;
                    evt.u.allianceRequest.fromPlayer = lctx->playerNum;
                    evt.u.allianceRequest.toPlayer   = toPlayer;
                    serverSimPublishControl(lctx->sim, &evt);
                    logAddEvent(log_AllyRequest, lctx->playerNum, toPlayer,
                                0, 0, 0, NULL);
                }
            }
            break;
        }
        case PACKET_ALLIANCE_ACCEPT: {
            /* [header 8][fromPlayer 1][toPlayer 1] — accepter is us
             * (lctx->playerNum); newMember = buf[+1]. T1 entry handles
             * both the alliance mutation and the WBN + log side
             * effects, so this case stays a one-liner. */
            if (len >= PACKET_HEADER_SIZE + 2) {
                BYTE newMember = buf[PACKET_HEADER_SIZE + 1];
                serverSimAcceptAlliance(lctx->sim, lctx->playerNum, newMember);
            }
            break;
        }
        case PACKET_ALLIANCE_LEAVE: {
            /* [header 8][playerNum 1] — leaver is always us. T1 entry
             * handles mutation + WBN + log. */
            if (len >= PACKET_HEADER_SIZE + 1) {
                serverSimLeaveAlliance(lctx->sim, lctx->playerNum);
            }
            break;
        }
        case PACKET_GAME_VOTE_TOGGLE: {
            /* [header 8][kind 1][toggleMode 1] — mirrors the UDP-side
             * case in transport_udp_server.c. The UDP path runs a
             * per-client cooldown (clientReqCooldownTicks); local SP
             * host is rate-limited by the user's own clicks so the
             * cooldown is unnecessary here. */
            if (len >= PACKET_HEADER_SIZE + 2) {
                uint8_t kind   = buf[PACKET_HEADER_SIZE + 0];
                uint8_t toggle = buf[PACKET_HEADER_SIZE + 1];
                serverSimGameVoteToggle(lctx->sim, lctx->playerNum,
                                        kind, toggle);
            }
            break;
        }
        default:
            /* Other client→server packet types (NAME_CHANGE, TEAM_SET,
             * READY, LOCK_TOGGLE, SURRENDER_VOTE, etc.) are not yet
             * wired through the local-transport dispatch. Their
             * client_net.h wrappers still UDP-gate, so the bot-pool
             * and SP-host paths don't exercise them. As features that
             * need bot participation come online, mirror the matching
             * serverProcessPacket case here. */
            break;
    }
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
    t.sendBytes = localSendBytes;
    t.tick = localTick;
    t.getSnapshot = localGetSnapshot;
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
