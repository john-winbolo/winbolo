/*
 * Quiet-lobby reliable control delivery under loss, with no input flowing.
 *
 * Control events ride reliable channel 2 (CHANNEL_CONTROL).  During a quiet
 * lobby the client sends no input, so its control acks can only travel on the
 * per-tick standalone PACKET_CHANNEL trailer it emits every connected tick —
 * the retired PACKET_CONTROL_ACK path is gone.  This test proves that path
 * carries acks: a client joins a lobby under loss=5 / burst=2 and is never fed
 * input; the server then floods it with more than CHANNEL_CONTROL_WINDOW (64)
 * control events, paced so the window drains between batches.
 *
 * If the standalone-trailer ack path were broken, the server's control channel
 * window would fill at 64 unacked and the next channelSend would defer a
 * disconnect — so the client would drop.  We assert the opposite: the client
 * stays connected and in the lobby, and the server's CHANNEL_CONTROL ackedSeq
 * advances by at least the flooded count (well past a single window), proving
 * every event was delivered and acked back with no input in play.  Convergence
 * is proven behaviourally, with no exact-trace dependence.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "channel_mux.h"           /* CHANNEL_CONTROL */
#include "transport_udp.h"         /* transportUdpServerChannelTestStats */
#include "server_sim.h"            /* serverSimPublishControl */
#include "control_event.h"         /* ControlEvent, CTRL_SERVER_TEXT */
#include "threads.h"               /* threadsWaitForMutex / threadsReleaseMutex */
#include "test_harness.h"
#include "loopback_harness.h"

#define LOBBY_CONNECT_MAX 2000   /* join + map download under loss */
#define SETTLE_PUMPS       300   /* let the join control-replay drain + ack */
#define FLOOD_BATCHES       12   /* 12 * 8 = 96 events, > CHANNEL_CONTROL_WINDOW (64) */
#define FLOOD_BATCH_SIZE     8   /* small enough that a batch can't fill the window */
#define BATCH_PUMPS         40   /* drain each batch (acks ride the standalone trailer) */
#define STABILITY_PUMPS    700   /* well past the deferred-disconnect window */

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* Publish one CTRL_SERVER_TEXT at every connected client, holding the tick
 * mutex as serverInstanceTick would when it publishes. */
static void publish_one(LoopbackHarness *h) {
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SERVER_TEXT;
    SDL_strlcpy(evt.u.serverText.text, "quiet-lobby probe",
                sizeof(evt.u.serverText.text));
    threadsWaitForMutex();
    serverSimPublishControl(h->sim, &evt);
    threadsReleaseMutex();
}

int run_loopback_quiet_lobby_control_loss(void) {
    LoopbackHarness h;
    int slot;
    int b, i;
    uint32_t baseSrvAck = 0, srvAck = 0;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "QuietJoiner", /*lobbyMode*/ true,
                                       /*impairSpec*/ "loss=5,burst=2",
                                       /*seed*/ 0xC0FFEEu),
                  "harness start failed");

    int connectedAt = loopbackHarnessPumpUntil(&h, LOBBY_CONNECT_MAX,
                                               pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps",
                LOBBY_CONNECT_MAX);
    }
    UT_ASSERT_MSG(clientSimIsInLobby(h.cs),
                  "client connected but not in lobby phase");

    slot = (int)clientSimGetMyPlayerNum(h.cs);

    /* Let the join control-replay finish delivering + acking, then baseline the
     * server's control ackedSeq.  Pump WITHOUT feeding input — the client's
     * standalone PACKET_CHANNEL trailer is the only ack carrier here. */
    for (i = 0; i < SETTLE_PUMPS; i++) {
        loopbackHarnessPump(&h);
    }
    transportUdpServerChannelTestStats(slot, CHANNEL_CONTROL, NULL, &baseSrvAck, NULL);

    /* Flood control events in paced batches, never feeding input.  Each batch
     * is far smaller than the window; the pumps between let the window drain as
     * acks ride back on the standalone trailer. */
    for (b = 0; b < FLOOD_BATCHES; b++) {
        int k;
        for (k = 0; k < FLOOD_BATCH_SIZE; k++) {
            publish_one(&h);
        }
        for (i = 0; i < BATCH_PUMPS; i++) {
            loopbackHarnessPump(&h);
        }
    }

    /* Stability window: keep pumping (still no input) past the deferred-
     * disconnect horizon.  A broken ack path would have filled the window and
     * dropped the client by now. */
    for (i = 0; i < STABILITY_PUMPS; i++) {
        loopbackHarnessPump(&h);
    }

    transportUdpServerChannelTestStats(slot, CHANNEL_CONTROL, NULL, &srvAck, NULL);

    {
        ClientConnectState st = clientSimGetConnectState(h.cs);
        uint32_t flooded = (uint32_t)(FLOOD_BATCHES * FLOOD_BATCH_SIZE);
        fprintf(stderr, "  quiet-lobby control (loss): inLobby=%d state=%d "
                        "ackedSeq=%u (base %u, flooded %u)\n",
                (int)clientSimIsInLobby(h.cs), (int)st,
                (unsigned)srvAck, (unsigned)baseSrvAck, (unsigned)flooded);

        if (st != CLIENT_CONNECT_CONNECTED) {
            loopbackHarnessStop(&h);
            UT_FAIL("client dropped during quiet-lobby flood (state=%d) — "
                    "standalone-trailer control ack path stalled", (int)st);
        }
        if (!clientSimIsInLobby(h.cs)) {
            loopbackHarnessStop(&h);
            UT_FAIL("client left the lobby unexpectedly");
        }
        /* Every flooded event must have been acked back with no input — the
         * advance past a full window can only happen via the standalone
         * trailer ack path. */
        if (srvAck < baseSrvAck + flooded) {
            loopbackHarnessStop(&h);
            UT_FAIL("control acks did not converge with no input: "
                    "ackedSeq=%u < base %u + flooded %u",
                    (unsigned)srvAck, (unsigned)baseSrvAck, (unsigned)flooded);
        }
    }

    loopbackHarnessStop(&h);
    return 0;
}
