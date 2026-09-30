/*
 * A slot locked out by a server hitch recovers, over the real transport.
 *
 * A server that stops ticking and then catches up in one burst consumes a
 * run of tick numbers in no wall-clock time at all. Once its slot has been
 * dry for more than STALL_ADVANCE_DRY_TICKS half-steps it substitutes the
 * held buttons every half-step and advances lastProcessedInput past tick
 * numbers the client has not produced yet; only a fresh apply resets the dry
 * counter. Under a one-way delay the client can never catch that run on its
 * own — it produces two tick numbers per frame and the substitution consumes
 * two per frame, so a packet built now still lands a whole round trip behind
 * — and every input it sends is dropped as stale. The player cannot move.
 *
 * Two changes break that, one on each end, and this case is where they are
 * driven together over an actual socket rather than against a modelled peer:
 * intake admits the newest tick a slot has ever sent although it is stale and
 * the tick boundary rebases onto it (test_stall_lockout_rebase covers that
 * against a ServerSim on its own), and clientBuildInputPacket jumps the
 * packet's tick past lastProcessedInput and keeps the jump as an offset
 * (test_input_tick_offset covers that against one ClientSim).
 *
 * What this case pins:
 *   - The client reaches the hitch with a MEASURED round trip, and one that
 *     matches the path it is on. clientBuildInputPacket has two branches and
 *     only the projectionPingMs > 0 one belongs to a networked client; a run
 *     that reached the hitch with the ping still at 0 would exercise the
 *     local-producer renumber and prove nothing here. The measurement is also
 *     checked against the delay= spec: the PING stamp, the PONG that
 *     subtracts it and the impairment layer's delivery times all read
 *     udpClientVirtualNow, so a round trip nowhere near the simulated one
 *     means they have come off the same clock and the forward jump is being
 *     sized off a path the client is not on.
 *   - After the burst, a button value the client has never sent reaches the
 *     server's lastInputButtons. A substitute is synthesised from
 *     lastInputButtons (the stall branch in simRunHalfStep), so it can only
 *     ever repeat a value that is already there — a new one can only have
 *     come from a real input that was applied.
 *   - lastProcessedInput at that moment sits on a tick the client actually
 *     emitted, recorded in fedTick as the packets were built.
 *   - Real applies keep happening. One apply is not a repair, so the button
 *     is changed again several times and each new value has to reach the
 *     server in turn.
 *
 * What it deliberately does not pin: anything about
 * statDroppedStaleInputs. Every input packet carries the newest
 * INPUT_REDUNDANCY_COUNT entries, intake admits an entry on
 * tick > lastProcessedInput, and the jitter buffer holds entries queued but
 * unapplied — so a redundant copy of a queued tick is admitted a second time
 * and dropped as stale at the dequeue. That happens on a perfectly healthy
 * slot (the dequeue loop says so, and test_input_redundancy asserts it), so
 * the counter neither reaches zero nor stops climbing after recovery. It is
 * reported in the failure messages for diagnosis and never tested.
 *
 * Every input is built by clientBuildInputPacket and then handed to
 * clientSimNetSendInput, the way the headless producer does it: that function
 * is where inputTickOffset lives, so the hand-built InputPacket the other
 * loopback cases pass straight to clientSimNetSendInput would exercise the
 * server end of this and silently skip the client end. The counter runs
 * through the hitch without pausing — a producer that stops counting is a
 * different scenario.
 *
 * The client runs on delay=80 with the harness virtual clock, so the
 * impairment layer is driven by a counter the pump advances 20ms per tick,
 * one frame: the delay is exactly 4 pumps each way and costs no real time,
 * and the ping measured across it reports that same simulated round trip.
 * Server state is read off the ServerSim struct (the unittests profile
 * permits internal access).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* lastProcessedInput, lastInputButtons */
#include "server_lifecycle.h"      /* serverInstanceTick — the catch-up burst */
#include "client_sim.h"
#include "client_sim_internal.h"   /* MY_TANK, projectionPingMs */
#include "client_net.h"            /* clientBuildInputPacket */
#include "client_connect_state.h"
#include "client_enums.h"          /* tankButton */
#include "input_packet.h"
#include "test_harness.h"
#include "loopback_harness.h"

/* One-way delay. Past the 60ms the issue's own table says a slot never
 * recovers from, and an exact 4 pumps under the virtual clock. */
#define HITCH_SPEC "delay=80"
#define HITCH_SEED 0x817C4u

/* What that spec makes the client measure. Both legs are held 80ms — 4 pumps
 * each — so the round trip is 160ms, and the client's own turnaround (it
 * reads the PONG out of the socket on the pump after the one the layer
 * released it on) adds a pump, so a settled sample is about 180ms.
 *
 * The bounds are wide because this pins a clock, not a number. One pump under
 * the 160ms floor, so nothing about exactly when the pump advances the counter
 * can fail it, and still several times the few tens of milliseconds a
 * wall-clock measurement of the same pumps would report, which is the drift it
 * exists to catch. Three pumps of slack above, and pingMinWindowPush returns
 * the smallest sample in its window, so one late delivery never sets the
 * value. */
#define HITCH_PING_MIN_MS 140u
#define HITCH_PING_MAX_MS 240u

/* Join handshake plus the map download, each leg 4 pumps. The clean-path
 * cases budget 2000 pumps with no delay at all and the lossy ones 8000; the
 * map is ~20 CHANNEL_BULK segments, so this is one window and a handful of
 * round trips, with room for the recv thread to be late. */
#define HITCH_CONNECT_MAX 4000

/* Reach a tank, a measured round trip and an established input stream. The
 * ping is the long pole: the client sends one every PING_INTERVAL_TICKS (20)
 * client ticks and the reply is 9 pumps behind it, so a first sample is
 * ~30 pumps away. An order of magnitude over that. */
#define HITCH_ESTABLISH_MAX 600

/* Steady state before the baseline is taken, so the jitter buffer has settled
 * at whatever depth this path drives it to rather than still climbing. Four
 * ping intervals and several round trips. */
#define HITCH_SETTLE 80

/* Client-only pumps, and the number of server ticks burst afterwards. The
 * slot's queue is drained in the burst's first tick (intake admits at most
 * INPUT_REDUNDANCY_COUNT inputs per tick, so 8 of the 40 pumps' worth get in)
 * and the recv queue is then empty for the remaining 39 ticks: that is 78
 * consecutive dry half-steps, against a STALL_ADVANCE_DRY_TICKS of 4. Well
 * past it with no reliance on exactly how many inputs the first tick took.
 * Larger would only lengthen the same run. */
#define HITCH_PUMPS 40

/* Pumps allowed for the first real apply after the burst. The client's own
 * packets need 4 pumps to reach the server; the rebase is gated on the dry
 * run being past the threshold, so a packet arriving just after an apply
 * waits a few half-steps for the next one. That is tens of pumps, not
 * hundreds — this is a convergence bound, not an estimate. */
#define HITCH_RECOVER_MAX 400

/* Further button changes, and the bound on each. Each one costs the same
 * 4-pump flight plus the dry-run wait as the first, so the same order. */
#define HITCH_CYCLES 4
#define HITCH_CHANGE_MAX 200

/* Buttons held through connect and the hitch, and the two the client
 * alternates afterwards. INPUT_BTN_ACCEL alone is what the server has in
 * lastInputButtons when the burst ends, so every value below carries a bit
 * that has never been in it. */
#define HITCH_TB_HOLD  TACCEL       /* INPUT_BTN_ACCEL                  */
#define HITCH_TB_ALT_A TLEFTACCEL   /* INPUT_BTN_ACCEL | INPUT_BTN_LEFT */
#define HITCH_TB_ALT_B TRIGHTACCEL  /* INPUT_BTN_ACCEL | INPUT_BTN_RIGHT */

/* Ticks the client emits are recorded here so an apply can be checked against
 * what was actually sent. Sized well past anything this case reaches: the
 * counter advances two per pump over at most ~2000 pumps of input, and the
 * forward jump adds at most CLIENT_INPUT_JUMP_MAX_RTT_HALFSTEPS plus the
 * margin on top of a lastProcessedInput bounded by the half-steps run. */
#define HITCH_TICK_MAX (1u << 16)
static uint8_t hitchFedTick[HITCH_TICK_MAX];

/* The client's own input-tick counter, the parity source for every packet.
 * Starts at 1 so the first packet is a keys tick on an odd number, matching
 * the parity clientBuildInputPacket enforces (game even, keys odd). */
static uint32_t hitchClientTick;

static bool hitchConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool hitchHaveTank(LoopbackHarness *h, void *user) {
    BYTE mx = 0, my = 0;
    (void)user;
    return clientSimGetMyTankMapPos(h->cs, &mx, &my) == TRUE;
}

/* Build one input the way a frontend does — through clientBuildInputPacket,
 * so the packet's tick goes through the offset logic — run the local
 * prediction step for its half, and send it. Records the tick that actually
 * went on the wire, which is the jumped one, not the counter. */
static void hitchFeedOne(LoopbackHarness *h, tankButton tb, bool isGameTick) {
    InputPacket pkt;
    BYTE slot = clientSimGetMyPlayerNum(h->cs);

    clientBuildInputPacket(h->cs, &pkt, tb, /*isShoot*/ false, /*isMine*/ false,
                           /*isBrain*/ false, isGameTick, slot,
                           hitchClientTick);
    if (isGameTick) {
        clientSimGameTick(h->cs, &pkt, /*isBrain*/ false);
    } else {
        clientSimKeysTick(h->cs, &pkt);
    }
    clientSimNetSendInput(h->cs, &pkt);
    if (pkt.tick < HITCH_TICK_MAX) {
        hitchFedTick[pkt.tick] = 1;
    }
    hitchClientTick++;
}

/* One frame's worth: the keys half then the game half, the counter advancing
 * by one after each. Two tick numbers per call, matching the two half-steps
 * a server tick runs. */
static void hitchFeedFrame(LoopbackHarness *h, tankButton tb) {
    hitchFeedOne(h, tb, /*isGameTick*/ false);
    hitchFeedOne(h, tb, /*isGameTick*/ true);
}

/* The button bitmask a tankButton becomes on the wire, so the test names the
 * value it is waiting for without re-deriving clientBuildInputPacket's
 * mapping in more than one place. */
static uint8_t hitchButtonsFor(tankButton tb) {
    switch (tb) {
    case TLEFTACCEL:  return (uint8_t)(INPUT_BTN_LEFT | INPUT_BTN_ACCEL);
    case TRIGHTACCEL: return (uint8_t)(INPUT_BTN_RIGHT | INPUT_BTN_ACCEL);
    case TACCEL:      return (uint8_t)INPUT_BTN_ACCEL;
    default:          return 0;
    }
}

int run_loopback_hitch_recovers(void) {
    LoopbackHarness h;
    BYTE slot;
    int  at, i, cycle;
    uint32_t lpiBefore;
    uint16_t pingBefore;
    uint32_t lpiAtApply = 0;
    int  appliedAt = -1;

    memset(hitchFedTick, 0, sizeof(hitchFedTick));
    hitchClientTick = 1;

    if (!loopbackHarnessStart(&h, "Hitch", /*lobbyMode*/ false,
                              HITCH_SPEC, HITCH_SEED)) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start (hitch) failed");
    }
    /* Before the first pump: the impairment queues are empty at this point,
     * so nothing is stranded by the clock jumping back to zero. */
    loopbackHarnessUseVirtualClock(&h, true);

    at = loopbackHarnessPumpUntil(&h, HITCH_CONNECT_MAX, hitchConnected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never reached CONNECTED within %d pumps on %s",
                HITCH_CONNECT_MAX, HITCH_SPEC);
    }
    fprintf(stderr, "  loopback hitch: connected after %d pump(s)\n", at);

    at = loopbackHarnessPumpUntil(&h, HITCH_ESTABLISH_MAX, hitchHaveTank, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never got a tank within %d pumps",
                HITCH_ESTABLISH_MAX);
    }
    slot = clientSimGetMyPlayerNum(h.cs);
    if (slot >= MAX_TANKS) {
        loopbackHarnessStop(&h);
        UT_FAIL("no slot after the tank arrived");
    }

    /* Establish the stream and measure a round trip. lastProcessedInput > 0
     * is the server's own "this stream filled at least once" signal and is
     * what gates the stall-advance being tested below; without it the hitch
     * would take the idle path instead. */
    at = -1;
    for (i = 1; i <= HITCH_ESTABLISH_MAX; i++) {
        hitchFeedFrame(&h, HITCH_TB_HOLD);
        loopbackHarnessPump(&h);
        if (h.cs->projectionPingMs != 0 &&
            h.sim->lastProcessedInput[slot] > 0) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("no measured round trip (ping=%u) or no established stream "
                "(lastProcessedInput=%u) within %d pumps",
                (unsigned)h.cs->projectionPingMs,
                (unsigned)h.sim->lastProcessedInput[slot],
                HITCH_ESTABLISH_MAX);
    }
    for (i = 0; i < HITCH_SETTLE; i++) {
        hitchFeedFrame(&h, HITCH_TB_HOLD);
        loopbackHarnessPump(&h);
    }

    /* Without a tank clientBuildInputPacket returns before it reaches the
     * tick logic and every packet above was empty. */
    if (MY_TANK(h.cs) == NULL) {
        loopbackHarnessStop(&h);
        UT_FAIL("own tank missing after the stream established");
    }
    pingBefore = h.cs->projectionPingMs;
    if (pingBefore == 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("projectionPingMs fell back to 0 before the hitch — the "
                "local-producer branch would be under test, not the lockout");
    }
    if (pingBefore < HITCH_PING_MIN_MS || pingBefore > HITCH_PING_MAX_MS) {
        loopbackHarnessStop(&h);
        UT_FAIL("measured round trip %ums is outside %u-%ums for a %s path "
                "(160ms both legs): the ping measurement and the impairment "
                "layer are no longer reading the same clock, so the forward "
                "jump is sized off a path this client is not on",
                (unsigned)pingBefore, HITCH_PING_MIN_MS, HITCH_PING_MAX_MS,
                HITCH_SPEC);
    }
    lpiBefore = h.sim->lastProcessedInput[slot];
    fprintf(stderr, "  loopback hitch: established after %d pump(s), "
                    "ping=%ums lastProcessedInput=%u stale=%u\n",
            at, (unsigned)pingBefore, (unsigned)lpiBefore,
            (unsigned)h.sim->statDroppedStaleInputs[slot]);

    /* The hitch. The client keeps producing at its own rate; the server does
     * not tick, so nothing drains its recv queue. One pump at a time because
     * the producer has to run between them — a counter that pauses here is a
     * different scenario. */
    for (i = 0; i < HITCH_PUMPS; i++) {
        hitchFeedFrame(&h, HITCH_TB_HOLD);
        loopbackHarnessPumpClientOnly(&h, 1);
    }

    /* The catch-up: serverGameTimer runs serverInstanceTick in a loop with no
     * cap until the wall clock is caught up, so the debt is paid in one
     * burst with no client tick between. */
    for (i = 0; i < HITCH_PUMPS; i++) {
        serverInstanceTick(h.sim);
    }
    fprintf(stderr, "  loopback hitch: after the burst lastProcessedInput=%u "
                    "(was %u) dry=%u stall=%u\n",
            (unsigned)h.sim->lastProcessedInput[slot], (unsigned)lpiBefore,
            (unsigned)h.sim->inputDryTicks[slot],
            (unsigned)h.sim->statStallTicks[slot]);

    /* Resume, on a button value the server has never held. A substitute
     * copies lastInputButtons, so it can only repeat INPUT_BTN_ACCEL; the
     * turn bit can only arrive on a real input that was applied. */
    {
        uint8_t want = hitchButtonsFor(HITCH_TB_ALT_A);
        for (i = 1; i <= HITCH_RECOVER_MAX; i++) {
            hitchFeedFrame(&h, HITCH_TB_ALT_A);
            loopbackHarnessPump(&h);
            if (h.sim->lastInputButtons[slot] == want) {
                appliedAt  = i;
                lpiAtApply = h.sim->lastProcessedInput[slot];
                break;
            }
        }
        if (appliedAt < 0) {
            loopbackHarnessStop(&h);
            UT_FAIL("no real input was applied in %d pumps after the burst — "
                    "lastInputButtons is still 0x%02x, expected 0x%02x "
                    "(lastProcessedInput=%u, client tick=%u, stale=%u)",
                    HITCH_RECOVER_MAX, (unsigned)h.sim->lastInputButtons[slot],
                    (unsigned)want, (unsigned)h.sim->lastProcessedInput[slot],
                    (unsigned)hitchClientTick,
                    (unsigned)h.sim->statDroppedStaleInputs[slot]);
        }
        if (lpiAtApply >= HITCH_TICK_MAX || !hitchFedTick[lpiAtApply]) {
            loopbackHarnessStop(&h);
            UT_FAIL("the slot recovered at lastProcessedInput=%u, which is not "
                    "a tick the client sent — a substitute, not an apply",
                    (unsigned)lpiAtApply);
        }
        fprintf(stderr, "  loopback hitch: real input applied %d pump(s) after "
                        "the burst, at tick %u\n", appliedAt,
                (unsigned)lpiAtApply);
    }

    /* One apply is not a repair. Change the button again and again; each new
     * value has to reach the server in turn, and each arrival is another real
     * input that was applied rather than substituted. */
    for (cycle = 0; cycle < HITCH_CYCLES; cycle++) {
        tankButton tb = ((cycle % 2) == 0) ? HITCH_TB_ALT_B : HITCH_TB_ALT_A;
        uint8_t    want = hitchButtonsFor(tb);
        int        got = -1;

        for (i = 1; i <= HITCH_CHANGE_MAX; i++) {
            hitchFeedFrame(&h, tb);
            loopbackHarnessPump(&h);
            if (h.sim->lastInputButtons[slot] == want) {
                got = i;
                lpiAtApply = h.sim->lastProcessedInput[slot];
                break;
            }
        }
        if (got < 0) {
            loopbackHarnessStop(&h);
            UT_FAIL("change %d of %d never reached the server in %d pumps — "
                    "lastInputButtons 0x%02x, expected 0x%02x: the slot "
                    "applied one input and stopped (lastProcessedInput=%u, "
                    "stale=%u)",
                    cycle + 1, HITCH_CYCLES, HITCH_CHANGE_MAX,
                    (unsigned)h.sim->lastInputButtons[slot], (unsigned)want,
                    (unsigned)h.sim->lastProcessedInput[slot],
                    (unsigned)h.sim->statDroppedStaleInputs[slot]);
        }
        if (lpiAtApply >= HITCH_TICK_MAX || !hitchFedTick[lpiAtApply]) {
            loopbackHarnessStop(&h);
            UT_FAIL("change %d applied at lastProcessedInput=%u, which is not "
                    "a tick the client sent", cycle + 1, (unsigned)lpiAtApply);
        }
        fprintf(stderr, "  loopback hitch: change %d applied after %d pump(s) "
                        "at tick %u\n", cycle + 1, got, (unsigned)lpiAtApply);
    }

    if (hitchClientTick >= HITCH_TICK_MAX) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client counter reached %u — past the fedTick table, so "
                "the membership checks above were not all real",
                (unsigned)hitchClientTick);
    }

    loopbackHarnessStop(&h);
    return 0;
}
