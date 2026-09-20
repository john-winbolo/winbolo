/*
 * One client quits; the other keeps playing.
 *
 * A departure runs a good deal of work on the server between receiving the
 * quitting client's packet and returning to the tick: the slot is
 * disconnected, its control subscriber is unregistered, its pillboxes and
 * bases are migrated, and CTRL_PLAYER_LEAVE is published to everyone still
 * connected. All of it happens on the thread that also drains the remaining
 * clients' input and sends their snapshots, so a change to any of it can cost
 * the peers their session without touching anything that names them.
 *
 * This case is a guard on that path, not a test written against a defect. It
 * passes as the code stands; its value is that it fails if a later change to
 * the leave path starts dropping the peer, stalling its input, or leaving the
 * departed player in its roster.
 *
 * What it pins, all from client 1's own state:
 *   - Client 1 observes the departure. Slot 2 is in its roster while both are
 *     playing and out of it afterwards. The removal rides CTRL_PLAYER_LEAVE
 *     on the reliable control queue (client_sim_control.c), but the assertion
 *     is on the roster the client ends up with rather than on the event, so
 *     it holds whichever of the two removal paths arrives first.
 *   - Client 1 stays connected for 600 pumps after the departure: its connect
 *     state stays CONNECTED and its net status stays netRunning at every one
 *     of them, not merely at the end.
 *   - Client 1's inputs keep being applied across that window. Its slot's
 *     lastProcessedInput advances, and the values it advances onto are tick
 *     numbers client 1 actually put on the wire, recorded in quitFedTick as
 *     the packets were built. A bare "the number went up" would be satisfied
 *     by stall-advance, which synthesises inputs from the last held buttons
 *     and moves lastProcessedInput onto tick numbers no client ever sent.
 *
 * Client 2 leaves through clientSimDisconnect, which is the client-initiated
 * quit: transportUdpClientDestroy sends PACKET_QUIT to the server while the
 * join state is still CONNECTED, and the server's handler runs
 * serverDisconnectClient plus serverSimRemovePlayer — the same departure a
 * player closing the game produces. Nothing here calls into the server to
 * remove the slot, so what is under test is the path a real quit takes.
 *
 * Client 1's inputs are built by clientBuildInputPacket and then handed to
 * clientSimNetSendInput, the way the frontend does it, so the tick recorded
 * in quitFedTick is the one that went on the wire rather than the producer's
 * own counter.
 *
 * What it deliberately does not pin: that no tick is ever applied off
 * quitFedTick. A dry run longer than STALL_ADVANCE_DRY_TICKS stall-advances
 * the slot, and on a loaded machine the server's recv thread can lag enough
 * pumps to open one on a clean path. Such an advance cannot manufacture the
 * real applies this case counts, so the count is the assertion and any stray
 * is reported for diagnosis.
 *
 * Clean path, no impairment and no virtual clock: nothing here is measuring a
 * round trip. Server state is read off the ServerSim struct (the unittests
 * profile permits internal access).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* lastProcessedInput, inputDryTicks */
#include "client_sim.h"
#include "client_net.h"            /* clientBuildInputPacket, connect state */
#include "client_connect_state.h"
#include "client_enums.h"          /* tankButton, netStatus */
#include "game_sim.h"
#include "players.h"               /* playersIsInUse */
#include "input_packet.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define QUIT_SEED 0x5E41Du

/* Join plus the map download on a clean path, for each client in turn. The
 * other clean-path loopback cases budget 2000 pumps for the same work. */
#define QUIT_CONNECT_MAX 2000

/* Reaching a tank after the join lands, and establishing the input stream
 * (lastProcessedInput > 0 is the server's own "this stream filled at least
 * once" signal). Both are a handful of round trips on a clean path; this is
 * two orders of magnitude over that. */
#define QUIT_ESTABLISH_MAX 600

/* Steady frames before the quit, so the jitter buffer has settled at whatever
 * depth this path drives it to rather than still filling. */
#define QUIT_SETTLE 200

/* Pumps allowed for the leave to reach client 1. CTRL_PLAYER_LEAVE is on the
 * reliable control queue and the path is clean, so this is a few pumps in
 * practice; the budget is a convergence bound, not an estimate. */
#define QUIT_LEAVE_MAX 600

/* The window client 1 has to survive after the departure, checked at every
 * pump rather than only at the end. */
#define QUIT_WATCH 600

/* Real applies required across that window. Client 1 feeds two tick numbers
 * per pump and the server consumes two per tick, so lastProcessedInput is
 * expected to move at nearly every one of the 600 samples. A third of that is
 * far above anything a stalled stream would reach and far below what a
 * healthy one produces. */
#define QUIT_MIN_APPLIES 200

/* And the least it must advance by over the window, for the same reason. */
#define QUIT_MIN_ADVANCE 200

/* Buttons client 1 holds throughout. Nothing here reads the tank's position,
 * so one value for the whole run is enough. */
#define QUIT_TB_HOLD TACCEL

/* Ticks client 1 emits are recorded here so an apply can be checked against
 * what was actually sent. Sized well past anything this case reaches: the
 * counter advances two per pump over at most a few thousand pumps, and the
 * forward jump in clientBuildInputPacket adds a bounded margin on top of a
 * lastProcessedInput bounded by the half-steps run. */
#define QUIT_TICK_MAX (1u << 16)
static uint8_t quitFedTick[QUIT_TICK_MAX];

/* Client 1's own input-tick counter, the parity source for every packet.
 * Starts at 1 so the first packet is a keys tick on an odd number, matching
 * the parity clientBuildInputPacket enforces (game even, keys odd). */
static uint32_t quitClientTick;

static bool quitBothConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs)  == CLIENT_CONNECT_CONNECTED &&
           clientSimGetConnectState(h->cs2) == CLIENT_CONNECT_CONNECTED;
}

static bool quitBothHaveTanks(LoopbackHarness *h, void *user) {
    BYTE mx = 0, my = 0;
    (void)user;
    return clientSimGetMyTankMapPos(h->cs,  &mx, &my) == TRUE &&
           clientSimGetMyTankMapPos(h->cs2, &mx, &my) == TRUE;
}

/* Is `slot` present in the roster this client keeps? */
static bool quitRosterHas(ClientSim *cs, BYTE slot) {
    return playersIsInUse(&clientSimGetGameSim(cs)->plyrs, slot) == TRUE;
}

/* Build one input for client 1 the way a frontend does — through
 * clientBuildInputPacket, so the packet's tick goes through the offset logic
 * — run the local prediction step for its half, and send it. Records the tick
 * that actually went on the wire, which is the jumped one, not the counter. */
static void quitFeedOne(LoopbackHarness *h, tankButton tb, bool isGameTick) {
    InputPacket pkt;
    BYTE slot = clientSimGetMyPlayerNum(h->cs);

    clientBuildInputPacket(h->cs, &pkt, tb, /*isShoot*/ false, /*isMine*/ false,
                           /*isBrain*/ false, isGameTick, slot,
                           quitClientTick);
    if (isGameTick) {
        clientSimGameTick(h->cs, &pkt, /*isBrain*/ false);
    } else {
        clientSimKeysTick(h->cs, &pkt);
    }
    clientSimNetSendInput(h->cs, &pkt);
    if (pkt.tick < QUIT_TICK_MAX) {
        quitFedTick[pkt.tick] = 1;
    }
    quitClientTick++;
}

/* One frame's worth: the keys half then the game half. Two tick numbers per
 * call, matching the two half-steps a server tick runs. */
static void quitFeedFrame(LoopbackHarness *h, tankButton tb) {
    quitFeedOne(h, tb, /*isGameTick*/ false);
    quitFeedOne(h, tb, /*isGameTick*/ true);
}

int run_loopback_quit_keeps_peer(void) {
    LoopbackHarness h;
    BYTE slot1, slot2;
    int  at, i;
    int  leftAt = -1;
    uint32_t lpiBefore, lpiAfter, lpiSeen;
    int  realApplies = 0;
    int  strayApplies = 0;

    memset(quitFedTick, 0, sizeof(quitFedTick));
    quitClientTick = 1;

    if (!loopbackHarnessStart(&h, "QuitStays", /*lobbyMode*/ false,
                              /*impairSpec*/ NULL, QUIT_SEED)) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start (quit keeps peer) failed");
    }

    if (!loopbackHarnessAddClient(&h, "QuitLeaves")) {
        loopbackHarnessStop(&h);
        UT_FAIL("the second client failed to connect");
    }

    at = loopbackHarnessPumpUntil(&h, QUIT_CONNECT_MAX, quitBothConnected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("both clients never reached CONNECTED within %d pumps "
                "(client 1 state=%d, client 2 state=%d)",
                QUIT_CONNECT_MAX,
                (int)clientSimGetConnectState(h.cs),
                (int)clientSimGetConnectState(h.cs2));
    }
    fprintf(stderr, "  loopback quit: both connected after %d pump(s)\n", at);

    at = loopbackHarnessPumpUntil(&h, QUIT_ESTABLISH_MAX, quitBothHaveTanks,
                                  NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("both clients never got a tank within %d pumps",
                QUIT_ESTABLISH_MAX);
    }

    slot1 = clientSimGetMyPlayerNum(h.cs);
    slot2 = clientSimGetMyPlayerNum(h.cs2);
    if (slot1 >= MAX_TANKS || slot2 >= MAX_TANKS || slot1 == slot2) {
        loopbackHarnessStop(&h);
        UT_FAIL("the two clients hold slots %u and %u — quitting one of them "
                "would prove nothing", (unsigned)slot1, (unsigned)slot2);
    }

    /* Client 1 has to be holding the peer before the peer can be seen to go.
     * CTRL_PLAYER_JOIN is on the same reliable queue the leave rides. */
    at = -1;
    for (i = 1; i <= QUIT_LEAVE_MAX; i++) {
        loopbackHarnessPump(&h);
        if (quitRosterHas(h.cs, slot2)) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client 1 never had slot %u in its roster within %d pumps — "
                "there is no departure to observe", (unsigned)slot2,
                QUIT_LEAVE_MAX);
    }

    if (clientSimGetNetStatus(h.cs) != netRunning) {
        loopbackHarnessStop(&h);
        UT_FAIL("client 1's net status is %d, not netRunning, before the quit",
                (int)clientSimGetNetStatus(h.cs));
    }

    /* Establish client 1's input stream. lastProcessedInput > 0 is the
     * server's own signal that the stream filled at least once, and it is what
     * the applies counted below are measured against. */
    at = -1;
    for (i = 1; i <= QUIT_ESTABLISH_MAX; i++) {
        quitFeedFrame(&h, QUIT_TB_HOLD);
        loopbackHarnessPump(&h);
        if (h.sim->lastProcessedInput[slot1] > 0) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client 1's input stream never established within %d pumps "
                "(lastProcessedInput=%u)", QUIT_ESTABLISH_MAX,
                (unsigned)h.sim->lastProcessedInput[slot1]);
    }
    for (i = 0; i < QUIT_SETTLE; i++) {
        quitFeedFrame(&h, QUIT_TB_HOLD);
        loopbackHarnessPump(&h);
    }
    fprintf(stderr, "  loopback quit: client 1 slot=%u client 2 slot=%u "
                    "lastProcessedInput=%u\n",
            (unsigned)slot1, (unsigned)slot2,
            (unsigned)h.sim->lastProcessedInput[slot1]);

    /* The quit. clientSimDisconnect tears the transport down, and
     * transportUdpClientDestroy sends PACKET_QUIT on the way out because the
     * join state is still CONNECTED. Nothing is said to the server here. */
    clientSimDisconnect(h.cs2);

    /* Client 1 sees the departure. */
    for (i = 1; i <= QUIT_LEAVE_MAX; i++) {
        quitFeedFrame(&h, QUIT_TB_HOLD);
        loopbackHarnessPump(&h);
        if (!quitRosterHas(h.cs, slot2)) {
            leftAt = i;
            break;
        }
    }
    if (leftAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client 1 still has slot %u in its roster %d pumps after the "
                "quit — the departure never reached it", (unsigned)slot2,
                QUIT_LEAVE_MAX);
    }
    fprintf(stderr, "  loopback quit: client 1 saw the leave after %d pump(s)\n",
            leftAt);

    /* The window. Client 1 keeps producing; nothing about it may change. */
    lpiBefore = h.sim->lastProcessedInput[slot1];
    lpiSeen   = lpiBefore;
    for (i = 1; i <= QUIT_WATCH; i++) {
        uint32_t lpi;

        quitFeedFrame(&h, QUIT_TB_HOLD);
        loopbackHarnessPump(&h);

        if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
            int st = (int)clientSimGetConnectState(h.cs);
            loopbackHarnessStop(&h);
            UT_FAIL("client 1 left CONNECTED (state=%d) at pump %d of %d "
                    "after the peer's quit", st, i, QUIT_WATCH);
        }
        if (clientSimGetNetStatus(h.cs) != netRunning) {
            int ns = (int)clientSimGetNetStatus(h.cs);
            loopbackHarnessStop(&h);
            UT_FAIL("client 1's net status became %d at pump %d of %d after "
                    "the peer's quit", ns, i, QUIT_WATCH);
        }
        if (quitRosterHas(h.cs, slot2)) {
            loopbackHarnessStop(&h);
            UT_FAIL("slot %u came back into client 1's roster at pump %d of "
                    "%d after the quit", (unsigned)slot2, i, QUIT_WATCH);
        }

        lpi = h.sim->lastProcessedInput[slot1];
        if (lpi != lpiSeen) {
            if (lpi < QUIT_TICK_MAX && quitFedTick[lpi]) {
                realApplies++;
            } else {
                strayApplies++;
            }
            lpiSeen = lpi;
        }
    }

    lpiAfter = h.sim->lastProcessedInput[slot1];
    fprintf(stderr, "  loopback quit: over %d pumps lastProcessedInput %u -> "
                    "%u, %d applied on a sent tick, %d off one, dry=%u "
                    "stall=%u\n",
            QUIT_WATCH, (unsigned)lpiBefore, (unsigned)lpiAfter,
            realApplies, strayApplies,
            (unsigned)h.sim->inputDryTicks[slot1],
            (unsigned)h.sim->statStallTicks[slot1]);

    if (realApplies < QUIT_MIN_APPLIES) {
        loopbackHarnessStop(&h);
        UT_FAIL("client 1's inputs stopped applying after the peer quit: only "
                "%d of the %d pumps landed lastProcessedInput on a tick "
                "client 1 sent (%d landed off one), needed %d",
                realApplies, QUIT_WATCH, strayApplies, QUIT_MIN_APPLIES);
    }
    if (lpiAfter <= lpiBefore || lpiAfter - lpiBefore < QUIT_MIN_ADVANCE) {
        loopbackHarnessStop(&h);
        UT_FAIL("client 1's lastProcessedInput moved %u -> %u over %d pumps, "
                "less than the %d expected of a stream that is still being "
                "applied", (unsigned)lpiBefore, (unsigned)lpiAfter,
                QUIT_WATCH, QUIT_MIN_ADVANCE);
    }
    if (lpiAfter >= QUIT_TICK_MAX || !quitFedTick[lpiAfter]) {
        loopbackHarnessStop(&h);
        UT_FAIL("client 1's slot ended on tick %u, which client 1 never sent "
                "— the last thing applied to it was not its own input",
                (unsigned)lpiAfter);
    }

    loopbackHarnessStop(&h);
    return 0;
}
