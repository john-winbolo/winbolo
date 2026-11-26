/*
 * Exact lag-compensation viewTick (test_viewtick_rewind.c).
 *
 * Two surfaces:
 *
 *   1. serverSimComputeLagCompTicks — the pure rewind math. When the client
 *      stamps a known viewTick, the server rewinds the real view age
 *      (simTick - viewTick) server ticks / 2 = posHistory entries; viewTick==0
 *      or a viewTick ahead of the server falls back to the old ping estimate;
 *      and the result clamps to LAG_COMP_MAX_TICKS.
 *
 *   2. clientSimGetViewTick — the client stamps the *displayed* frame, which is
 *      the second-newest applied snapshot's serverTick, and reports 0 until two
 *      snapshots have been applied.
 *
 * Always built — the math is sim-independent and the client case drives a bare
 * ClientSim with empty snapshots (no transport).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"     /* serverSimComputeLagCompTicks, LAG_COMP_MAX_TICKS */
#include "interpolation.h"           /* INTERP_BUFFER_MS */
#include "client_sim.h"
#include "client_net.h"              /* clientSimGetViewTick */
#include "input_packet.h"
#include "transport_udp_server_internal.h" /* udpServer, BotHostSeat, BotHostConn */
#include "test_harness.h"

/* The pre-viewTick ping-based estimate, recomputed here so the fallback
 * assertions pin the helper to the exact formula it replaced. */
static uint8_t ping_fallback(uint16_t pingMs) {
    uint32_t delayMs = ((uint32_t)pingMs / 2) + INTERP_BUFFER_MS;
    return (uint8_t)(delayMs / 20);
}

int run_viewtick_rewind(void) {
    /* viewTick a known age behind simTick → (simTick - viewTick) / 2 history
     * entries, independent of ping. 6 ticks behind → 3. */
    UT_ASSERT(serverSimComputeLagCompTicks(100, 94, 0) == 3);
    UT_ASSERT(serverSimComputeLagCompTicks(100, 94, 9999) == 3);

    /* viewTick == 0 → ping fallback equals the old formula. */
    UT_ASSERT(serverSimComputeLagCompTicks(100, 0, 100) == ping_fallback(100));
    UT_ASSERT(serverSimComputeLagCompTicks(100, 0, 200) == ping_fallback(200));
    UT_ASSERT(ping_fallback(100) == 3);   /* (50+20)/20 */
    UT_ASSERT(ping_fallback(200) == 6);   /* (100+20)/20 */

    /* viewTick ahead of the server (stale/garbage) → ping fallback, never a
     * huge or negative rewind. */
    UT_ASSERT(serverSimComputeLagCompTicks(50, 100, 100) == ping_fallback(100));

    /* A large real view age clamps to the cap. */
    UT_ASSERT(serverSimComputeLagCompTicks(1000, 2, 0) == LAG_COMP_MAX_TICKS);
    UT_ASSERT(LAG_COMP_MAX_TICKS == 14);

    return 0;
}

/* Apply an otherwise-empty snapshot carrying just a header serverTick. */
static void apply_empty_snapshot(ClientSim *cs, uint32_t serverTick) {
    SnapshotHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.serverTick = serverTick;
    clientSimSyncFromSnapshot(cs, &hdr,
                              NULL, 0,   /* tanks */
                              NULL, 0,   /* shells */
                              NULL, 0,   /* tk explosions */
                              NULL, 0,   /* bases */
                              NULL, 0,   /* pills */
                              NULL, 0,   /* events */
                              0);        /* playerNum */
}

int run_viewtick_displayed_tick(void) {
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT_MSG(cs != NULL, "clientSimAlloc returned NULL");
    clientSimCreate(cs);

    /* Before any snapshot: unknown. */
    UT_ASSERT_MSG(clientSimGetViewTick(cs) == 0,
                  "viewTick should be 0 before any snapshot, got %u",
                  (unsigned)clientSimGetViewTick(cs));

    /* One applied snapshot: still 0 (need two to know the displayed frame). */
    apply_empty_snapshot(cs, 1000);
    UT_ASSERT_MSG(clientSimGetViewTick(cs) == 0,
                  "viewTick should be 0 after one snapshot, got %u",
                  (unsigned)clientSimGetViewTick(cs));

    /* Second applied snapshot: the displayed frame is the second-newest =
     * the first snapshot's serverTick. */
    apply_empty_snapshot(cs, 1002);
    UT_ASSERT_MSG(clientSimGetViewTick(cs) == 1000,
                  "viewTick should be the second-newest applied tick (1000), got %u",
                  (unsigned)clientSimGetViewTick(cs));

    /* A third keeps tracking the second-newest. */
    apply_empty_snapshot(cs, 1004);
    UT_ASSERT_MSG(clientSimGetViewTick(cs) == 1002,
                  "viewTick should advance to 1002, got %u",
                  (unsigned)clientSimGetViewTick(cs));

    clientSimDestroy(cs);
    return 0;
}

/* ── Bot Host bots get the same rewind as a human ──────────────────────────
 *
 * Until a client has applied two snapshots (at join and at each round
 * start) it sends view tick 0, and the server rewinds by its ping estimate
 * instead. A bot whose brain runs on a remote Bot Host also sends view tick 0
 * then, but a bot connection shows ping 0, so it got (0 + 20) / 20 = 1 entry
 * however far away its host was. The rewind must use the host's measured
 * lag, as a human's uses his ping. The shown ping stays 0.
 *
 * Drives the real path: serverSimApplyInput reads the transport, and
 * serverSimTick computes the rewind on each applied input
 * (statLastRewindTicks). The transport state of slot 0 is set by hand and
 * put back at the end. */

#define VB_SLOT 0

static uint32_t vb_next;

/* Feeds two inputs (view tick 0) and runs one server tick, three times, so
 * the last applied input's rewind is in statLastRewindTicks. */
static uint8_t vb_rewind(ServerSim *sim) {
    int frame;
    for (frame = 0; frame < 3; frame++) {
        int k;
        for (k = 0; k < 2; k++) {
            InputPacket pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.tick = vb_next++;
            pkt.playerNum = VB_SLOT;
            pkt.viewTick = 0;
            serverSimApplyInput(sim, &pkt);
        }
        serverSimTick(sim);
    }
    return sim->statLastRewindTicks[VB_SLOT];
}

int run_viewtick_rewind_bot_host(void) {
    ServerSim *sim = ut_make_running_sim("BotHostRewind");
    UdpServerClient savedClient;
    BotHostSeat savedSeat;
    BotHostConn savedHost;
    UdpServerClient *c = &udpServer.clients[VB_SLOT];
    BotHostSeat *seat = &udpServer.botSeats[VB_SLOT];
    BotHostConn *hc = &udpServer.botHosts[0];
    uint8_t r;
    int fail = 0;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    savedClient = *c;
    savedSeat = *seat;
    savedHost = *hc;
    vb_next = 1;

    /* An in-process bot: no bot connection, seat LOCAL. One entry. */
    c->connected = false;
    c->botSlot = false;
    seat->state = BOT_SEAT_LOCAL;
    seat->host = -1;
    r = vb_rewind(sim);
    if (r != 1) {
        fprintf(stderr, "  local bot rewind %u, expected 1\n", (unsigned)r);
        fail = 1;
    }

    /* A human with ping 100: the ping estimate, as before. */
    c->connected = true;
    c->botSlot = false;
    c->pingMs = 100;
    r = vb_rewind(sim);
    if (r != ping_fallback(100)) {
        fprintf(stderr, "  human (ping 100) rewind %u, expected %u\n",
                (unsigned)r, (unsigned)ping_fallback(100));
        fail = 1;
    }

    /* A bot thinking on Bot Host 0, whose smoothed lag is 60 ms: the same
     * estimate a human with 60 ms ping gets, (30 + 20) / 20 = 2. */
    c->connected = true;
    c->botSlot = true;
    c->pingMs = 0;
    seat->state = BOT_SEAT_REMOTE;
    seat->host = 0;
    hc->connected = true;
    hc->lagKnown = true;
    hc->lagMs = 60.0;
    r = vb_rewind(sim);
    if (r != ping_fallback(60) || r <= 1) {
        fprintf(stderr, "  Bot Host bot (host lag 60) rewind %u, expected %u\n",
                (unsigned)r, (unsigned)ping_fallback(60));
        fail = 1;
    }

    /* A farther host, 200 ms: 6, the same as a human with 200 ms ping. */
    hc->lagMs = 200.0;
    r = vb_rewind(sim);
    if (r != ping_fallback(200)) {
        fprintf(stderr, "  Bot Host bot (host lag 200) rewind %u, expected %u\n",
                (unsigned)r, (unsigned)ping_fallback(200));
        fail = 1;
    }

    /* The shown ping of the Bot Host bot stays 0. */
    if (sim->playerPing[VB_SLOT] != 0) {
        fprintf(stderr, "  Bot Host bot shown ping %u, expected 0\n",
                (unsigned)sim->playerPing[VB_SLOT]);
        fail = 1;
    }

    /* No lag measured yet for the host: today's one entry. */
    hc->lagKnown = false;
    r = vb_rewind(sim);
    if (r != 1) {
        fprintf(stderr, "  Bot Host bot (lag not known) rewind %u, expected 1\n",
                (unsigned)r);
        fail = 1;
    }

    /* Bound but not yet thinking on the host: the brain still runs in the
     * server, so one entry. */
    hc->lagKnown = true;
    hc->lagMs = 200.0;
    seat->state = BOT_SEAT_BOUND;
    r = vb_rewind(sim);
    if (r != 1) {
        fprintf(stderr, "  bound bot (brain in server) rewind %u, expected 1\n",
                (unsigned)r);
        fail = 1;
    }

    *c = savedClient;
    *seat = savedSeat;
    *hc = savedHost;
    serverSimDestroy(sim);
    UT_ASSERT_MSG(fail == 0, "Bot Host bot rewind does not match the human rule");
    return 0;
}
