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
 *      proving retransmit / cumulative-ack survive a hostile path.
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
#include "transport_udp.h"         /* test-only channel hooks */
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

    /* Inject a single message on the server's game channel for this slot. */
    UT_ASSERT_MSG(transportUdpServerChannelTestSend(slot, CHANNEL_GAME,
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
            if (transportUdpClientChannelTestReceive(ct, CHANNEL_GAME,
                                                     out, &outLen)) {
                gotMsg = true;
                msgOk  = (outLen == kLen && memcmp(out, kMsg, kLen) == 0);
            }
        }

        transportUdpServerChannelTestStats(slot, CHANNEL_GAME, NULL, &srvAck, NULL);
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
        transportUdpServerChannelTestStats(slot, CHANNEL_GAME, NULL, &srvAck, NULL);
        transportUdpClientChannelTestStats(ct, CHANNEL_GAME, &cliExp, NULL, NULL);
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

int run_loopback_channel(void) {
    int rc = run_empty_flow();
    if (rc != 0) return rc;
    return run_roundtrip_impaired();
}
