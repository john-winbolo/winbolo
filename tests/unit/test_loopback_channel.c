/*
 * Loopback integration test for the parallel reliable-ordered channel layer
 * (channel_mux.c) once it is wired into the live UDP transport.
 *
 * The channel runs empty and alongside the existing reliable-event queues: a
 * channel frame rides every snapshot (server→client) and every input
 * (client→server) as a trailer, and a standalone PACKET_CHANNEL carries it
 * when no such datagram flows.  These two cases drive the wiring end-to-end
 * over the real loopback transport.
 *
 *   1. Empty flow — a running session with no channel traffic.  Frames are
 *      still exchanged (the 2-byte empty frame rides every trailer), but no
 *      message is ever queued, so every channel's expectedSeq / ackedSeq must
 *      stay at 0 and neither side disconnects: the empty channel is inert.
 *
 *   2. Synthetic round-trip under loss + jitter + dup.  A message is injected
 *      on a server channel via the test-only hook; under impairment it must
 *      still arrive intact on the client (drained via channelReceive) and the
 *      client's ack must make it back so the server's ackedSeq advances —
 *      proving retransmit / cumulative-ack survive a hostile path.  It runs on
 *      CHANNEL_CONTROL: the client transport now consumes CHANNEL_GAME itself
 *      (case 3), so a raw byte payload injected there would be drained out from
 *      under the receive peek and mis-decoded as a game event.
 *
 *   3. Real game events ride channel 0.  A running session fires under loss;
 *      each shot expires into an EVENT_EXPLOSION the server routes onto
 *      CHANNEL_GAME.  The client must deliver it over the channel (expectedSeq
 *      advances) and apply it through the shared game-event path (brain event
 *      buffer grows) — the migration-parity check that the channel carries the
 *      same observable game events the per-client event queue used to.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "client_sim_internal.h"   /* ClientSim::transport (Transport handle) */
#include "input_packet.h"
#include "channel_mux.h"           /* CHANNEL_GAME / CHANNEL_COUNT / CHANNEL_MAX_SEG */
#include "transport_udp.h"         /* test-only channel hooks, spliceGameEventsBeforeTail */
#include "server_sim.h"            /* serverSimAddEvent (overflow case) */
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX     2000   /* join + map download */
#define EMPTY_PUMPS      300   /* steady state long enough for trailers to flow */
#define ROUNDTRIP_MAX   3000   /* delivery + ack convergence under impairment */

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* Send one (zeroed) input so the client transmits a datagram carrying its
 * channel trailer. Returns the next input tick. */
static uint32_t feed_input(LoopbackHarness *h, uint32_t tick) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick      = tick;
    pkt.playerNum = clientSimGetMyPlayerNum(h->cs);
    clientSimNetSendInput(h->cs, &pkt);
    return tick + 1;
}

/* Send one fire input. The server creates a shell; when it expires it spawns
 * an EVENT_EXPLOSION near the tank, which the server routes onto CHANNEL_GAME.
 * Returns the next input tick. */
static uint32_t feed_fire(LoopbackHarness *h, uint32_t tick) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick      = tick;
    pkt.playerNum = clientSimGetMyPlayerNum(h->cs);
    pkt.actions   = INPUT_ACTION_FIRE;
    clientSimNetSendInput(h->cs, &pkt);
    return tick + 1;
}

/* Case 1: the empty channel is inert — frames flow but no sequence moves. */
static int run_empty_flow(void) {
    LoopbackHarness h;
    int connectedAt;
    uint32_t inputTick = 1;
    int i;
    Transport *ct;
    int slot;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "ChanEmpty", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 0x5EEDu),
                  "harness start (empty flow) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    ct   = &h.cs->transport;
    slot = (int)clientSimGetMyPlayerNum(h.cs);

    /* Steady state: drive inputs so both directions carry channel trailers. */
    for (i = 0; i < EMPTY_PUMPS; i++) {
        inputTick = feed_input(&h, inputTick);
        loopbackHarnessPump(&h);
    }

    /* Frames must have been exchanged on both endpoints. */
    {
        uint32_t cliFrames = 0, srvFrames = 0;
        transportUdpClientChannelTestStats(ct, CHANNEL_GAME, NULL, NULL, &cliFrames);
        transportUdpServerChannelTestStats(slot, CHANNEL_GAME, NULL, NULL, &srvFrames);
        if (cliFrames == 0 || srvFrames == 0) {
            loopbackHarnessStop(&h);
            UT_FAIL("no channel frames exchanged (client=%u server=%u)",
                    (unsigned)cliFrames, (unsigned)srvFrames);
        }
    }

    /* Every channel's sequence state must be untouched on both sides. */
    {
        uint8_t ch;
        for (ch = 0; ch < CHANNEL_COUNT; ch++) {
            uint32_t cExp = 0, cAck = 0, sExp = 0, sAck = 0;
            transportUdpClientChannelTestStats(ct, ch, &cExp, &cAck, NULL);
            transportUdpServerChannelTestStats(slot, ch, &sExp, &sAck, NULL);
            if (cExp != 0 || cAck != 0 || sExp != 0 || sAck != 0) {
                loopbackHarnessStop(&h);
                UT_FAIL("ch %u moved on empty flow "
                        "(client exp=%u ack=%u, server exp=%u ack=%u)",
                        (unsigned)ch, (unsigned)cExp, (unsigned)cAck,
                        (unsigned)sExp, (unsigned)sAck);
            }
        }
    }

    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("client dropped during empty-flow steady state");
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 2: a message survives loss + jitter + dup and is acked back. */
static int run_roundtrip_impaired(void) {
    LoopbackHarness h;
    int connectedAt;
    uint32_t inputTick = 1;
    int slot;
    Transport *ct;
    static const uint8_t kMsg[] = "channel-roundtrip!";
    const uint16_t kLen = (uint16_t)sizeof(kMsg);  /* includes NUL */
    bool gotMsg = false;
    bool msgOk  = false;
    int i;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "ChanRT", /*lobbyMode*/ false,
                                       /*impairSpec*/ "loss=10,jitter=5,dup=20",
                                       /*seed*/ 0xC0FFEEu),
                  "harness start (round-trip) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    slot = (int)clientSimGetMyPlayerNum(h.cs);
    ct   = &h.cs->transport;

    /* Inject a single message on the server's control channel for this slot.
     * CHANNEL_CONTROL is not consumed by the client transport, so it stays in
     * the receive ring for the peek below. */
    UT_ASSERT_MSG(transportUdpServerChannelTestSend(slot, CHANNEL_CONTROL,
                                                    kMsg, kLen),
                  "test channel send rejected");

    /* Pump (feeding input so the client's ack rides back) until the message
     * arrives intact AND the server sees the ack advance its ackedSeq. */
    for (i = 1; i <= ROUNDTRIP_MAX; i++) {
        uint32_t srvAck = 0;

        inputTick = feed_input(&h, inputTick);
        loopbackHarnessPump(&h);

        if (!gotMsg) {
            uint8_t out[CHANNEL_MAX_SEG];
            uint16_t outLen = 0;
            if (transportUdpClientChannelTestReceive(ct, CHANNEL_CONTROL,
                                                     out, &outLen)) {
                gotMsg = true;
                msgOk  = (outLen == kLen && memcmp(out, kMsg, kLen) == 0);
            }
        }

        transportUdpServerChannelTestStats(slot, CHANNEL_CONTROL, NULL, &srvAck, NULL);
        if (gotMsg && srvAck >= 1) {
            break;
        }
    }

    fprintf(stderr, "  channel round-trip (impaired): converged after %d pump(s) "
                    "(cap %d) gotMsg=%d msgOk=%d\n", i, ROUNDTRIP_MAX,
            (int)gotMsg, (int)msgOk);

    if (!gotMsg) {
        loopbackHarnessStop(&h);
        UT_FAIL("message never delivered to client within %d pumps", ROUNDTRIP_MAX);
    }
    if (!msgOk) {
        loopbackHarnessStop(&h);
        UT_FAIL("delivered message did not match the injected bytes");
    }
    {
        uint32_t srvAck = 0, cliExp = 0;
        transportUdpServerChannelTestStats(slot, CHANNEL_CONTROL, NULL, &srvAck, NULL);
        transportUdpClientChannelTestStats(ct, CHANNEL_CONTROL, &cliExp, NULL, NULL);
        if (srvAck < 1) {
            loopbackHarnessStop(&h);
            UT_FAIL("server ackedSeq never advanced (=%u)", (unsigned)srvAck);
        }
        if (cliExp < 1) {
            loopbackHarnessStop(&h);
            UT_FAIL("client expectedSeq never advanced (=%u)", (unsigned)cliExp);
        }
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 3: real game events produced by the running sim ride channel 0 and are
 * applied on the client, under loss. */
static int run_game_event_channel(void) {
    LoopbackHarness h;
    int connectedAt;
    uint32_t inputTick = 1;
    Transport *ct;
    int slot;
    bool gotEvent = false;
    int i;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "ChanGE", /*lobbyMode*/ false,
                                       /*impairSpec*/ "loss=10",
                                       /*seed*/ 0x9A11Eu),
                  "harness start (game event) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    ct   = &h.cs->transport;
    slot = (int)clientSimGetMyPlayerNum(h.cs);

    /* Fire repeatedly. Each shot's shell expires into an EVENT_EXPLOSION the
     * server distributes onto CHANNEL_GAME. The client must deliver it over the
     * channel (expectedSeq advances) AND apply it through the shared game-event
     * path (brainEventCount grows — the snapshot game tail is empty now, so the
     * only path that buffers a brain event is the channel drain). */
    for (i = 1; i <= ROUNDTRIP_MAX; i++) {
        uint32_t cliExp = 0;

        inputTick = feed_fire(&h, inputTick);
        loopbackHarnessPump(&h);

        transportUdpClientChannelTestStats(ct, CHANNEL_GAME, &cliExp, NULL, NULL);
        if (cliExp >= 1 && clientSimGetBrainEventCount(h.cs) > 0) {
            gotEvent = true;
            break;
        }
    }

    fprintf(stderr, "  channel game-event (impaired): converged after %d pump(s) "
                    "(cap %d) gotEvent=%d brainEvents=%d\n", i, ROUNDTRIP_MAX,
            (int)gotEvent, clientSimGetBrainEventCount(h.cs));

    if (!gotEvent) {
        uint32_t cliExp = 0, srvAck = 0;
        transportUdpClientChannelTestStats(ct, CHANNEL_GAME, &cliExp, NULL, NULL);
        transportUdpServerChannelTestStats(slot, CHANNEL_GAME, NULL, &srvAck, NULL);
        loopbackHarnessStop(&h);
        UT_FAIL("no game event delivered+applied via channel 0 within %d pumps "
                "(client expectedSeq=%u server ackedSeq=%u brainEvents=%d)",
                ROUNDTRIP_MAX, (unsigned)cliExp, (unsigned)srvAck,
                clientSimGetBrainEventCount(h.cs));
    }

    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("client dropped while exchanging game events");
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 4: the splice that the snapshot-trailer drain uses to merge
 * channel-delivered game events ahead of the staged map tail. Pure index math,
 * tested directly: order preserved, nothing lost or duplicated, clamps to cap. */
static int run_game_event_splice(void) {
    GameEvent ev[8];
    GameEvent chan[3];
    int n, i;

    /* Two map-tail events staged at [0,2); splice three channel game events in
     * front. Tag each by data[0] so position is verifiable. */
    memset(ev, 0, sizeof(ev));
    memset(chan, 0, sizeof(chan));
    ev[0].type = EVENT_MAP_CHANGE;    ev[0].data[0] = 0xA0;
    ev[1].type = EVENT_MAP_CHANGE;    ev[1].data[0] = 0xA1;
    chan[0].type = EVENT_EXPLOSION;   chan[0].data[0] = 0x10;
    chan[1].type = EVENT_EXPLOSION;   chan[1].data[0] = 0x11;
    chan[2].type = EVENT_TANK_KILLED; chan[2].data[0] = 0x12;

    n = spliceGameEventsBeforeTail(ev, /*tailStart*/ 0, /*tailCount*/ 2,
                                   chan, /*chanCount*/ 3, /*cap*/ 8);
    if (n != 5) UT_FAIL("splice count = %d, want 5", n);
    if (ev[0].data[0] != 0x10 || ev[1].data[0] != 0x11 || ev[2].data[0] != 0x12)
        UT_FAIL("channel events not spliced in order at the front");
    if (ev[3].data[0] != 0xA0 || ev[4].data[0] != 0xA1)
        UT_FAIL("map tail not preserved behind the channel events");

    /* chanCount == 0 is a no-op that returns the existing count. */
    memset(ev, 0, sizeof(ev));
    ev[0].data[0] = 0xA0;
    ev[1].data[0] = 0xA1;
    n = spliceGameEventsBeforeTail(ev, 0, 2, chan, 0, 8);
    if (n != 2 || ev[0].data[0] != 0xA0 || ev[1].data[0] != 0xA1)
        UT_FAIL("zero-channel splice mutated the tail (n=%d)", n);

    /* Overflow: more channel events than cap. They clamp to cap, the tail is
     * dropped for lack of room, and nothing is written past ev[cap]. */
    {
        GameEvent big[6];
        GameEvent many[10];
        memset(big, 0, sizeof(big));
        memset(many, 0, sizeof(many));
        for (i = 0; i < 10; i++) many[i].data[0] = (uint8_t)(0x20 + i);
        big[4].type = 0x7F;  /* sentinels just past cap=4 */
        big[5].type = 0x7F;
        n = spliceGameEventsBeforeTail(big, 0, 2, many, 10, 4);
        if (n != 4) UT_FAIL("overflow splice count = %d, want 4", n);
        if (big[4].type != 0x7F || big[5].type != 0x7F)
            UT_FAIL("splice wrote past cap");
        if (big[0].data[0] != 0x20 || big[3].data[0] != 0x23)
            UT_FAIL("overflow splice did not place clamped channel events");
    }

    return 0;
}

/* Case 5: a game-event channel send that overflows the window mid-tick defers
 * the disconnect (sets the pending-removal flag) instead of acting inline, and
 * the removal drain then clears it. Drives the real producer, not a stub. */
static int run_game_channel_overflow(void) {
    LoopbackHarness h;
    int connectedAt;
    int slot;
    int i;
    bool windowFull = false;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "ChanOvf", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 0x0FFEEu),
                  "harness start (overflow) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    slot = (int)clientSimGetMyPlayerNum(h.cs);

    if (transportUdpServerTestPendingRemove(slot)) {
        loopbackHarnessStop(&h);
        UT_FAIL("slot %d already flagged for removal before overflow", slot);
    }

    /* Fill the slot's game-channel send window without pumping, so no client
     * ack drains it, until channelSend rejects the next message. */
    {
        uint8_t msg[GAME_EVENT_MAX_WIRE_SIZE];
        memset(msg, 0, sizeof(msg));
        for (i = 0; i < CHANNEL_GAME_WINDOW + 8; i++) {
            if (!transportUdpServerChannelTestSend(slot, CHANNEL_GAME, msg,
                                                   (uint16_t)sizeof(msg))) {
                windowFull = true;
                break;
            }
        }
    }
    if (!windowFull) {
        loopbackHarnessStop(&h);
        UT_FAIL("could not fill the game channel window for slot %d", slot);
    }

    /* Inject one real game event and run the real producer. Its channelSend
     * must hit the full window and defer — not perform — the disconnect.
     * EVENT_TANK_KILLED is non-sound and not distance-culled, so it reaches
     * the channelSend unconditionally. */
    {
        GameEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = EVENT_TANK_KILLED;
        serverSimAddEvent(h.sim, &ev);
        transportUdpServerDrainEvents(h.sim);
    }

    if (!transportUdpServerTestPendingRemove(slot)) {
        loopbackHarnessStop(&h);
        UT_FAIL("channel overflow did not defer a disconnect for slot %d", slot);
    }

    /* The deferred removal is resolved at the safe point and the flag clears. */
    transportUdpServerDrainPendingRemovals(h.sim);
    if (transportUdpServerTestPendingRemove(slot)) {
        loopbackHarnessStop(&h);
        UT_FAIL("pending removal not cleared by the drain for slot %d", slot);
    }

    loopbackHarnessStop(&h);
    return 0;
}

int run_loopback_channel(void) {
    int rc = run_empty_flow();
    if (rc != 0) return rc;
    rc = run_roundtrip_impaired();
    if (rc != 0) return rc;
    rc = run_game_event_channel();
    if (rc != 0) return rc;
    rc = run_game_event_splice();
    if (rc != 0) return rc;
    return run_game_channel_overflow();
}
