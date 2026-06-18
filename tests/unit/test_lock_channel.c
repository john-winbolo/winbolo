/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * test_lock_channel.c — the server "locked to new players" notice now travels
 * on CHANNEL_GAME, the same reliable path the per-tick game-event producer
 * uses, rather than the per-client snapshot reliable tail.
 *
 * The check: bring up an idle running game (no shooting, so an idle tank
 * produces no channel-0 traffic — see run_empty_flow in test_loopback_channel),
 * force a known-unlocked baseline, then call transportUdpServerSetLock(true)
 * under the server mutex (the way the servermain console and the lobby
 * game-start do).  The client's CHANNEL_GAME expectedSeq must advance — the
 * notice was delivered over the channel drain, not the snapshot tail.
 *
 * Observability is the existing transportUdpClientChannelTestStats hook; no
 * production observability is added.  It asserts delivery over channel 0 (the
 * point of the migration), not the EVENT_SERVER_MSG payload byte.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"            /* clientSimNetSendInput */
#include "client_connect_state.h"  /* CLIENT_CONNECT_CONNECTED */
#include "client_sim_internal.h"   /* ClientSim::transport (Transport handle) */
#include "input_packet.h"
#include "channel_mux.h"           /* CHANNEL_GAME */
#include "transport_udp.h"         /* SetLock / GetLock + test channel hooks */
#include "threads.h"               /* threadsWaitForMutex / threadsReleaseMutex */
#include "loopback_harness.h"
#include "test_harness.h"

#define CONNECT_MAX   2000   /* connect + map-download round trips */
#define SETTLE_PUMPS   300   /* drain the join control replay before baselining */
#define DELIVER_MAX   2000   /* notice delivery over channel 0 */

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* The producer skips a slot until the server flips downloadComplete (a
 * CHANNEL_BULK ack round-trip after the client reports CONNECTED), so wait on
 * it before driving the lock path. */
static bool pred_download_complete(LoopbackHarness *h, void *user) {
    (void)user;
    return transportUdpServerTestDownloadComplete(
        (int)clientSimGetMyPlayerNum(h->cs));
}

/* One zeroed input so the client emits a datagram carrying its channel trailer
 * and the return snapshot carries the server's. Keeps the tank idle, so no
 * channel-0 game events are produced. */
static uint32_t feed_idle(LoopbackHarness *h, uint32_t tick) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick      = tick;
    pkt.playerNum = clientSimGetMyPlayerNum(h->cs);
    clientSimNetSendInput(h->cs, &pkt);
    return tick + 1;
}

/* Drive the lock path the way real callers do: under the server mutex. */
static void set_lock(LoopbackHarness *h, bool locked) {
    threadsWaitForMutex();
    transportUdpServerSetLock(h->sim, locked);
    threadsReleaseMutex();
}

int run_lock_channel(void) {
    LoopbackHarness h;
    uint32_t inputTick = 1;
    Transport *ct;
    uint32_t baseExp = 0, cliExp = 0;
    bool delivered = false;
    int i;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "LockChan", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 0x10CCu),
                  "harness start (lock channel) failed");

    if (loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }
    if (loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_download_complete, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("server slot never download-complete within %d pumps", CONNECT_MAX);
    }

    ct = &h.cs->transport;

    /* Settle the join control replay so channel 0 is quiescent, then force a
     * known-unlocked baseline (a running game may have auto-locked on start)
     * and settle again so any unlock delivery is folded into the baseline. */
    for (i = 0; i < SETTLE_PUMPS; i++) { inputTick = feed_idle(&h, inputTick); loopbackHarnessPump(&h); }
    set_lock(&h, false);
    for (i = 0; i < SETTLE_PUMPS; i++) { inputTick = feed_idle(&h, inputTick); loopbackHarnessPump(&h); }
    transportUdpClientChannelTestStats(ct, CHANNEL_GAME, &baseExp, NULL, NULL);

    /* Lock: the notice must travel over CHANNEL_GAME. With the tank idle, any
     * channel-0 advance past the baseline is the lock notice. */
    set_lock(&h, true);
    for (i = 1; i <= DELIVER_MAX; i++) {
        inputTick = feed_idle(&h, inputTick);
        loopbackHarnessPump(&h);
        transportUdpClientChannelTestStats(ct, CHANNEL_GAME, &cliExp, NULL, NULL);
        if (cliExp > baseExp) { delivered = true; break; }
    }

    fprintf(stderr, "  lock notice over channel 0: baseExp=%u cliExp=%u after %d pump(s)\n",
            (unsigned)baseExp, (unsigned)cliExp, i);

    if (!delivered) {
        uint32_t srvAck = 0;
        transportUdpServerChannelTestStats((int)clientSimGetMyPlayerNum(h.cs),
                                           CHANNEL_GAME, NULL, &srvAck, NULL);
        loopbackHarnessStop(&h);
        UT_FAIL("lock notice never delivered over channel 0 within %d pumps "
                "(client expectedSeq=%u baseline=%u server ackedSeq=%u)",
                DELIVER_MAX, (unsigned)cliExp, (unsigned)baseExp, (unsigned)srvAck);
    }
    if (!transportUdpServerGetLock()) {
        loopbackHarnessStop(&h);
        UT_FAIL("server lock state not set after lock");
    }
    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("client dropped during lock exchange");
    }

    loopbackHarnessStop(&h);
    return 0;
}
