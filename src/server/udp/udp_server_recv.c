/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          UDP Server Recv
 *Filename:      udp_server_recv.c
 *Author:        John Morrison
 *Purpose:
 *  The server transport's inbound datagram path, split
 *  out of transport_udp_server.c.
 *    - The dedicated recv thread and the single-producer
 *      / single-consumer ring it fills.
 *    - The two per-tick drain entry points that empty it,
 *      polled and threaded.
 *    - The network impairment rig those two feed, on the
 *      inbound and the outbound paths.
 *********************************************************/

#include <stdio.h>                         /* fprintf, stderr — queue-full warning */

#include "transport_udp_internal.h"        /* UDP_MAX_PAYLOAD, udpRecvFrom, udpSendTo, SDL3 */
#include "transport_udp_server_internal.h" /* udpServer, RecvQueueEntry, RECV_QUEUE_SIZE,
                                            * serverProcessPacket, LOBBY_REQ_COOLDOWN_TICKS */
#include "server_sim.h"                    /* ServerSim */
#include "net_impair.h"                    /* NetImpair, netImpairPop, NET_IMPAIR_MAX_PACKET,
                                            * WB_ENABLE_NETIMPAIR */
#include "../../common/wb_log.h"           /* WB_LOG_INFO, WB_LOG_WARN, WB_LOG_CAT_NET */
#include "../../common/mp_diag_log.h"      /* mpDiagLog */

/* ---- Server dedicated recv thread ----
 * A background thread continuously polls the server socket and queues
 * packets into an SPSC ring buffer.  The timer callback drains the
 * queue each tick, keeping packet processing on the main thread.
 *
 * The ring is drained once per 20ms server tick, so it must hold a full
 * tick's worth of inbound bursts (many clients plus join/info-request and
 * resync traffic) or packets are dropped. Each entry is ~1.4 KB, so 1024
 * slots cost ~1.4 MB — cheap insurance against burst-driven drops. */

static RecvQueueEntry recvQueue[RECV_QUEUE_SIZE];
static SDL_AtomicInt  recvQueueHead;  /* written by recv thread */
static SDL_AtomicInt  recvQueueTail;  /* written by timer thread */
static SDL_AtomicInt  recvThreadRunning;
static SDL_Thread    *recvThread = NULL;
static SOCKET         recvThreadSock = INVALID_SOCKET; /* copy for thread */
static uint32_t       recvDropCount = 0; /* packets dropped due to full queue */

static int SDLCALL serverRecvThreadFunc(void *userdata) {
    (void)userdata;
    SOCKET sock = recvThreadSock;

    while (SDL_GetAtomicInt(&recvThreadRunning)) {
        fd_set readfds;
        struct timeval tv;
        int selRet;

        if (sock == INVALID_SOCKET) break;

        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 1000; /* 1ms timeout */

        selRet = select((int)(sock + 1), &readfds, NULL, NULL, &tv);
        if (selRet <= 0) continue;

        /* Drain all available packets from the socket */
        while (SDL_GetAtomicInt(&recvThreadRunning)) {
            int head = SDL_GetAtomicInt(&recvQueueHead);
            int tail = SDL_GetAtomicInt(&recvQueueTail);
            int next = (head + 1) % RECV_QUEUE_SIZE;

            if (next == tail) {
                /* Queue full — drop packet by reading and discarding */
                uint8_t discard[UDP_MAX_PAYLOAD];
                struct sockaddr_in discardAddr;
                socklen_t addrLen = sizeof(discardAddr);
                int ret = recvfrom(sock, (char *)discard, sizeof(discard), 0,
                                   (struct sockaddr *)&discardAddr, &addrLen);
                if (ret <= 0) break;
                recvDropCount++;
                if ((recvDropCount & 255) == 1) {
                    fprintf(stderr, "[UDP SERVER] Recv queue full, dropped %u packets\n",
                            recvDropCount);
                }
                continue;
            }

            {
                RecvQueueEntry *entry = &recvQueue[head];
                socklen_t addrLen = sizeof(entry->fromAddr);
                int ret = recvfrom(sock, (char *)entry->data, UDP_MAX_PAYLOAD, 0,
                                   (struct sockaddr *)&entry->fromAddr, &addrLen);
                if (ret <= 0) break; /* No more data or error */
                entry->len = ret;
                SDL_SetAtomicInt(&recvQueueHead, next);
            }
        }
    }
    return 0;
}

/* Empty the ring and start the recv thread on a copy of the server socket.
 * The copy is what lets transportUdpServerDestroy close the socket without
 * racing the thread. A thread that fails to start leaves recvThreadRunning
 * clear, and the caller falls back to the polled receive path. */
void udpServerRecvThreadStart(SOCKET sock) {
    SDL_SetAtomicInt(&recvQueueHead, 0);
    SDL_SetAtomicInt(&recvQueueTail, 0);
    recvDropCount = 0;
    recvThreadSock = sock;
    SDL_SetAtomicInt(&recvThreadRunning, 1);
    recvThread = SDL_CreateThread(serverRecvThreadFunc, "SrvRecv", NULL);
    if (recvThread) {
        WB_LOG_INFO(WB_LOG_CAT_NET, "recv thread started");
    } else {
        WB_LOG_WARN(WB_LOG_CAT_NET,
            "failed to create recv thread, using polled fallback: %s",
            SDL_GetError());
        SDL_SetAtomicInt(&recvThreadRunning, 0);
    }
}

/* Ask the recv thread to stop and wait for it. A no-op when no thread is
 * running, so the caller can run it unconditionally. */
void udpServerRecvThreadStop(void) {
    if (recvThread) {
        SDL_SetAtomicInt(&recvThreadRunning, 0);
        SDL_WaitThread(recvThread, NULL);
        recvThread = NULL;
        recvThreadSock = INVALID_SOCKET;
    }
}

/* Packets the recv thread discarded because the ring was full, since the
 * thread last started. */
uint32_t udpServerRecvDropCount(void) {
    return recvDropCount;
}

/* Runtime network impairment (delay/jitter/loss/burst) on the server's
 * inbound (client->server) and outbound (server->client) datagram paths.
 * Disabled unless transportUdpServerSetNetImpair() enables them.  Driven
 * only from the per-tick recv/drain entry points — never from the recv
 * thread, since bolo_rand is not thread-safe. */
NetImpair srvImpairIn;
NetImpair srvImpairOut;

void transportUdpServerSetNetImpair(const char *spec) {
#if WB_ENABLE_NETIMPAIR
    NetImpairConfig cfg;
    if (spec == NULL || !netImpairParseConfig(spec, &cfg)) {
        WB_LOG_WARN(WB_LOG_CAT_NET,
            "netimpair: bad spec '%s' — impairment left off",
            spec ? spec : "(null)");
        return;
    }
    netImpairEnable(&srvImpairIn, &cfg);
    netImpairEnable(&srvImpairOut, &cfg);
    WB_LOG_INFO(WB_LOG_CAT_NET,
        "netimpair enabled: delay=%ums jitter=%ums loss=%u%% burst=%u",
        (unsigned)cfg.baseDelayMs, (unsigned)cfg.jitterMs,
        (unsigned)cfg.lossPercent, (unsigned)cfg.burstLossLen);
#else
    /* Impairment tooling compiled out (WB_ENABLE_NETIMPAIR == 0). */
    (void)spec;
#endif
}

/* Decrement each connected client's per-tick rate-limit window for
 * lobby setter packets (LOBBY_SET_SETTING, LOBBY_PREVIEW_RANDOM, etc.).
 * Called once per server tick from whichever receive path is active —
 * without this, a client's cooldown would stick at LOBBY_REQ_COOLDOWN_TICKS
 * after its first rate-limited request and every subsequent one would be
 * silently dropped. */
static void udpServerTickPerClientCooldowns(void) {
    for (int i = 0; i < MAX_TANKS; i++) {
        SDL_assert(udpServer.clientReqCooldownTicks[i] <= LOBBY_REQ_COOLDOWN_TICKS);
        if (udpServer.clientReqCooldownTicks[i] > 0) {
            udpServer.clientReqCooldownTicks[i]--;
        }
    }
}

/* Release any datagrams now due from the impairment queues: inbound
 * packets back into serverProcessPacket, outbound packets onto the wire.
 * Both pops are no-ops while their layer is disabled (nothing queued), so
 * the disabled path is byte-for-byte the direct path.  Called from the
 * per-tick recv/drain entry points only — never the recv thread. */
static void srvDrainImpair(ServerSim *sim) {
    uint8_t pbuf[NET_IMPAIR_MAX_PACKET];
    struct sockaddr_in paddr;
    uint64_t now = (uint64_t)SDL_GetTicks();
    int plen;

    while ((plen = netImpairPop(&srvImpairIn, pbuf, sizeof(pbuf),
                                &paddr, now)) > 0) {
        serverProcessPacket(sim, pbuf, plen, &paddr);
    }
    while ((plen = netImpairPop(&srvImpairOut, pbuf, sizeof(pbuf),
                                &paddr, now)) > 0) {
        udpSendTo(udpServer.sock, pbuf, plen, &paddr);
    }

#if WB_ENABLE_NETIMPAIR
    /* Once-per-second impairment-queue summary so genuine injected loss
     * (overflow = the 512-slot queue filled, the only drop path when loss=0)
     * can be told apart from jitter-induced reordering — which is not loss at
     * all but shows up on the per-player [netstat] line as stale= when an
     * overtaken packet arrives after a newer one and is discarded. If overflow
     * holds at 0 while stale climbs, the "loss" is reordering, not drops. */
    if (netImpairEnabled(&srvImpairIn) || netImpairEnabled(&srvImpairOut)) {
        static uint64_t lastImpairLogMs = 0;
        if (now - lastImpairLogMs >= 1000) {
            lastImpairLogMs = now;
            mpDiagLog("[netimpair] in: q=%d overflow=%u  out: q=%d overflow=%u",
                      srvImpairIn.count, (unsigned)srvImpairIn.overflowDrops,
                      srvImpairOut.count, (unsigned)srvImpairOut.overflowDrops);
        }
    }
#endif
}

/* Receive all pending packets from clients (polled fallback) */
void transportUdpServerRecv(ServerSim *sim) {
    uint8_t buf[UDP_MAX_PAYLOAD];
    struct sockaddr_in fromAddr;
    int len;
    int c;

    if (!udpServer.running) return;

    udpServerTickPerClientCooldowns();

    for (c = 0; c < MAX_TANKS; c++) {
        udpServer.clients[c].inputsThisTick = 0;
    }

    udpServer.tickCount++;

    while ((len = udpRecvFrom(udpServer.sock, buf, sizeof(buf), &fromAddr)) > 0) {
        if (netImpairEnabled(&srvImpairIn)) {
            netImpairOffer(&srvImpairIn, buf, len, &fromAddr,
                           (uint64_t)SDL_GetTicks());
        } else {
            serverProcessPacket(sim, buf, len, &fromAddr);
        }
    }
    srvDrainImpair(sim);
}

/* Drain the recv thread's packet queue (called from timer callback) */
void transportUdpServerDrainRecvQueue(ServerSim *sim) {
    int c;
    int head, tail;

    if (!udpServer.running) return;

    udpServerTickPerClientCooldowns();

    for (c = 0; c < MAX_TANKS; c++) {
        udpServer.clients[c].inputsThisTick = 0;
    }

    udpServer.tickCount++;

    tail = SDL_GetAtomicInt(&recvQueueTail);
    head = SDL_GetAtomicInt(&recvQueueHead);

    while (tail != head) {
        RecvQueueEntry *entry = &recvQueue[tail];
        if (netImpairEnabled(&srvImpairIn)) {
            netImpairOffer(&srvImpairIn, entry->data, entry->len,
                           &entry->fromAddr, (uint64_t)SDL_GetTicks());
        } else {
            serverProcessPacket(sim, entry->data, entry->len, &entry->fromAddr);
        }
        tail = (tail + 1) % RECV_QUEUE_SIZE;
        SDL_SetAtomicInt(&recvQueueTail, tail);
        /* Re-read head in case more packets arrived during processing */
        head = SDL_GetAtomicInt(&recvQueueHead);
    }
    srvDrainImpair(sim);
}

/* Returns true if a dedicated recv thread is running */
bool transportUdpServerHasRecvThread(void) {
    return recvThread != NULL;
}
