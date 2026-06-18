/*
 * Input-redundancy invariants.
 *
 * Each client input rides in INPUT_REDUNDANCY_COUNT consecutive packets
 * so a short loss burst no longer starves the server's per-player input
 * queue. The client-side packing loop (buildInputPacket) is static
 * inside transport_udp_client.c, so this exercises the reachable
 * invariants instead:
 *
 *   1. the constants that size the redundancy window are consistent
 *      (count, power-of-two queue, ring big enough to hold a window),
 *   2. a fully-populated InputPacket survives the wire roundtrip and a
 *      full redundancy window fits inside one UDP datagram,
 *   3. the sim discards redundant duplicate ticks as stale rather than
 *      re-applying them, while still advancing to the newest tick.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"     /* SERVER_INPUT_QUEUE_SIZE, internal field access */
#include "input_packet.h"
#include "netpacks.h"                /* INPUT_REDUNDANCY_COUNT, PACKET_HEADER_SIZE */
#include "transport_udp_internal.h"  /* CLIENT_INPUT_RING_SIZE, UDP_MAX_PAYLOAD, pack/unpack */
#include "test_harness.h"

/* The redundancy window must fit the queue/ring it flows through. */
static int check_constants(void) {
    UT_ASSERT(INPUT_REDUNDANCY_COUNT == 8);
    UT_ASSERT(SERVER_INPUT_QUEUE_SIZE == 16);
    /* Power of two — the enqueue/consume paths mask with & (SIZE - 1). */
    UT_ASSERT((SERVER_INPUT_QUEUE_SIZE & (SERVER_INPUT_QUEUE_SIZE - 1)) == 0);
    /* The client ring must hold a full redundancy window. */
    UT_ASSERT(CLIENT_INPUT_RING_SIZE >= INPUT_REDUNDANCY_COUNT);
    return 0;
}

/* A fully-populated InputPacket survives pack/unpack, and a full window
 * of INPUT_REDUNDANCY_COUNT packets fits in one UDP datagram. */
static int check_wire_roundtrip(void) {
    InputPacket in, out;
    uint8_t buf[INPUT_PACKET_WIRE_SIZE];
    int packed;

    memset(&in, 0, sizeof(in));
    in.tick            = 0x01020304u;
    in.playerNum       = 0x11;
    in.buttons         = 0x22;
    in.actions         = 0x33;
    in.buildAction     = 0x44;
    in.buildX          = 0x55;
    in.buildY          = 0x66;
    in.flags           = 0x77;
    in.eventAck        = 0x0A0B0C0Du;
    in.mapEventAck     = 0x1A1B1C1Du;
    in.pingMs          = 0xBEEF;
    in.viewTick        = 0x12345678u;

    packed = packInputPacket(buf, &in);
    UT_ASSERT(packed == INPUT_PACKET_WIRE_SIZE);

    memset(&out, 0, sizeof(out));
    unpackInputPacket(buf, &out);

    UT_ASSERT(out.tick == in.tick);
    UT_ASSERT(out.playerNum == in.playerNum);
    UT_ASSERT(out.buttons == in.buttons);
    UT_ASSERT(out.actions == in.actions);
    UT_ASSERT(out.buildAction == in.buildAction);
    UT_ASSERT(out.buildX == in.buildX);
    UT_ASSERT(out.buildY == in.buildY);
    UT_ASSERT(out.flags == in.flags);
    UT_ASSERT(out.eventAck == in.eventAck);
    UT_ASSERT(out.mapEventAck == in.mapEventAck);
    UT_ASSERT(out.pingMs == in.pingMs);
    UT_ASSERT(out.viewTick == in.viewTick);

    /* Header + 1 count byte + a full redundancy window of packed inputs. */
    UT_ASSERT(PACKET_HEADER_SIZE + 1 + INPUT_REDUNDANCY_COUNT * packed
              <= UDP_MAX_PAYLOAD);
    return 0;
}

/* Enqueue one input tick for slot 0 the way an arriving packet would. */
static void apply_tick(ServerSim *sim, uint32_t tick) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick = tick;
    pkt.playerNum = 0;
    serverSimApplyInput(sim, &pkt);
}

/* Feed overlapping redundancy windows: each frame carries two fresh
 * ticks plus a duplicate of an already-applied tick (the overlap a
 * redundant retransmit produces). serverSimTick consumes two inputs per
 * call, so the 16-deep queue never overflows. The duplicate must be
 * discarded as stale; the newest fresh tick must still be reached. */
static int check_sim_dedup(void) {
    ServerSim *sim = ut_make_running_sim("Redundant");
    uint32_t newestTick = 0;
    int iter;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    /* 24 frames stays well under the per-second (100 sub-tick) [netstat]
     * reset that would zero statDroppedStaleInputs mid-test. */
    for (iter = 0; iter < 24; iter++) {
        uint32_t base = (uint32_t)iter * 2;  /* last frame's newest fresh tick */
        apply_tick(sim, base);               /* stale duplicate (already applied) */
        apply_tick(sim, base + 1);           /* fresh */
        apply_tick(sim, base + 2);           /* fresh */
        newestTick = base + 2;
        serverSimTick(sim);                  /* two half-steps consume two inputs */
    }

    /* Sample now, with no draining: the two-fresh-per-frame cadence keeps
     * the stream in lockstep, so the last frame leaves lastProcessedInput
     * exactly at the newest fresh tick. Empty drain frames would not help —
     * under stall-advance an idle frame substitutes the held buttons and
     * advances lastProcessedInput past newestTick (correct, but no longer
     * equal to it). */
    UT_ASSERT_MSG(sim->lastProcessedInput[0] == newestTick,
                  "lastProcessedInput=%u expected newest=%u",
                  (unsigned)sim->lastProcessedInput[0], (unsigned)newestTick);
    UT_ASSERT_MSG(sim->statDroppedStaleInputs[0] > 0,
                  "expected redundant duplicates to be dropped as stale, got %u",
                  (unsigned)sim->statDroppedStaleInputs[0]);

    serverSimDestroy(sim);
    return 0;
}

int run_input_redundancy(void) {
    int rc;
    if ((rc = check_constants()) != 0) return rc;
    if ((rc = check_wire_roundtrip()) != 0) return rc;
    if ((rc = check_sim_dedup()) != 0) return rc;
    return 0;
}
