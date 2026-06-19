/*
 * Edge-send predicate: udpInputEdgeChanged decides when a recorded input
 * is promoted to an immediate send.
 *
 * The UDP client samples input on keys ticks (clientSimNetRecordInput) and
 * normally lets the next 50/s game-tick send carry it. When the sampled
 * controls change — a button press or release — the transport sends one
 * packet immediately instead of waiting up to a tick, shaving that latency
 * off every input edge. The predicate is the gate: it compares the buttons
 * and actions of two InputPackets and ignores the per-send stamps (tick,
 * the reliable ACKs, ping) so a re-stamped but otherwise identical input
 * never looks like an edge.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "input_packet.h"
#include "transport_udp_internal.h"  /* udpInputEdgeChanged */
#include "test_harness.h"

/* A press or release on either bitmask is an edge; a re-stamp is not. */
static int check_edge_predicate(void) {
    InputPacket a, b;

    memset(&a, 0, sizeof(a));
    a.tick            = 100;
    a.playerNum       = 3;
    a.buttons         = INPUT_BTN_ACCEL | INPUT_BTN_LEFT;
    a.actions         = INPUT_ACTION_FIRE;
    a.buildAction     = 0;
    a.flags           = INPUT_FLAG_AUTOSLOW;

    /* Differing buttons → edge. */
    b = a;
    b.buttons = a.buttons | INPUT_BTN_RIGHT;
    UT_ASSERT(udpInputEdgeChanged(&a, &b));

    /* A release (button cleared) is just as much an edge as a press. */
    b = a;
    b.buttons = a.buttons & ~INPUT_BTN_ACCEL;
    UT_ASSERT(udpInputEdgeChanged(&a, &b));

    /* Differing actions → edge. */
    b = a;
    b.actions = a.actions | INPUT_ACTION_LAY_MINE;
    UT_ASSERT(udpInputEdgeChanged(&a, &b));

    /* Identical controls but different per-send stamps → NOT an edge.
     * tick, the reliable map ACK and ping are re-stamped on every
     * record; they must never read as a control change. */
    b = a;
    b.tick            = a.tick + 1;
    b.mapEventAck     = 0x12345678u;
    b.pingMs          = 250;
    UT_ASSERT(!udpInputEdgeChanged(&a, &b));

    /* Identical everything → NOT an edge (held input, cadence carries it). */
    b = a;
    UT_ASSERT(!udpInputEdgeChanged(&a, &b));

    /* buildAction/buildX/buildY/flags are out of the predicate by design —
     * a build issued without a buttons/actions change is not an edge. */
    b = a;
    b.buildAction = 1;
    b.buildX      = 40;
    b.buildY      = 60;
    b.flags       = 0;
    UT_ASSERT(!udpInputEdgeChanged(&a, &b));

    return 0;
}

int run_edge_send_predicate(void) {
    int rc;
    if ((rc = check_edge_predicate()) != 0) return rc;
    return 0;
}
