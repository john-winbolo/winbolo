/*
 * Nothing either end of a game puts on the wire is larger than the send cap.
 *
 * UDP_MAX_SEND (1232) is the most a sender may put in one datagram: the
 * smallest MTU every IPv6 path has to carry, 1280, less the IPv6 and UDP
 * headers. IPv6 routers do not fragment, so a datagram over it can be lost
 * on such a path every time it is sent. Each sender holds its own budget to
 * the cap; this case watches the socket instead, so a sender that builds
 * against the size of its buffer, or one added later that forgets the cap,
 * shows up here.
 *
 * The test build of transport_udp_common.c records the largest length any
 * caller hands udpSendTo. The client and the server transports in this
 * process both send through it, so one counter covers both directions. The
 * case resets it before the harness starts, then runs a whole game on a
 * clean path:
 *
 *   - the join handshake and the map download into a running server, which
 *     is where the largest datagrams are (standalone CHANNEL_BULK frames,
 *     packed to the budget);
 *   - a tank, and one minute of game time of play: 3000 pumps, each one
 *     20 ms server frame, with the tank driving, turning and firing so the
 *     snapshots carry shells and the input packets carry a full redundancy
 *     window and a channel-frame trailer.
 *
 * It then asserts the largest datagram recorded is more than zero (the
 * counter saw the traffic at all) and at most UDP_MAX_SEND.
 *
 * The harness runs on its virtual clock, so the pump is in lockstep and
 * sleeps nowhere: a minute of game time costs a second or two of wall
 * clock rather than a minute.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"                 /* clientSimNetSendInput */
#include "client_connect_state.h"
#include "input_packet.h"               /* InputPacket, INPUT_BTN_*, INPUT_ACTION_FIRE */
#include "transport_udp_internal.h"     /* UDP_MAX_SEND, udpTestLargestSend,
                                         * udpTestResetLargestSend */
#include "test_harness.h"
#include "loopback_harness.h"

/* Join, cookie handshake and the map download on a clean path. The other
 * clean-path loopback cases budget the same. */
#define SC_CONNECT_MAX 4000

/* A tank to drive. */
#define SC_TANK_MAX    600

/* One minute of game time: one pump is one 20 ms server frame. */
#define SC_PLAY_PUMPS  3000

/* How often the tank turns and fires, in pumps. Prime to each other so the
 * turns and the shots fall on different frames. */
#define SC_TURN_EVERY  37
#define SC_FIRE_EVERY  5

#define SC_SEED 0x5E2Du

static bool scConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool scHaveTank(LoopbackHarness *h, void *user) {
    BYTE mx = 0, my = 0;
    (void)user;
    return clientSimGetMyTankMapPos(h->cs, &mx, &my);
}

/* One frame of input: the keys half on an odd tick and the game half on the
 * even one after it, the split the server keys on. A shot rides the game
 * half, which is the only one the server fires from. */
static void scSendFrame(LoopbackHarness *h, BYTE slot, uint32_t *tick,
                        uint8_t buttons, bool fire) {
    int n;
    for (n = 0; n < 2; n++) {
        InputPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        *tick += 1;
        pkt.tick      = *tick;
        pkt.playerNum = slot;
        pkt.buttons   = buttons;
        if (fire && (*tick % 2u) == 0u) {
            pkt.actions = INPUT_ACTION_FIRE;
        }
        clientSimNetSendInput(h->cs, &pkt);
    }
}

int run_send_cap_join_download_play(void) {
    LoopbackHarness h;
    BYTE     slot;
    uint32_t tick = 0;
    uint64_t startMs;
    int      at, i, largest;
    uint8_t  turn = 0;

    /* Before the harness starts, so the server's startup and the first JOIN
     * are counted along with everything after them. */
    udpTestResetLargestSend();
    startMs = SDL_GetTicks();

    if (!loopbackHarnessStart(&h, "SendCap", /*lobbyMode*/ false,
                              /*impairSpec*/ NULL, SC_SEED)) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start (send cap) failed");
    }
    loopbackHarnessUseVirtualClock(&h, true);

    at = loopbackHarnessPumpUntil(&h, SC_CONNECT_MAX, scConnected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never reached CONNECTED (join and map download) "
                "within %d pumps", SC_CONNECT_MAX);
    }
    fprintf(stderr, "  send cap: connected after %d pump(s), largest "
                    "datagram so far %d\n", at, udpTestLargestSend());

    at = loopbackHarnessPumpUntil(&h, SC_TANK_MAX, scHaveTank, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never got a tank within %d pumps", SC_TANK_MAX);
    }
    slot = clientSimGetMyPlayerNum(h.cs);
    if (slot >= MAX_TANKS) {
        loopbackHarnessStop(&h);
        UT_FAIL("no slot after the tank arrived");
    }

    for (i = 0; i < SC_PLAY_PUMPS; i++) {
        uint8_t buttons = INPUT_BTN_ACCEL;
        if ((i % SC_TURN_EVERY) == 0) {
            turn = (uint8_t)((turn + 1u) % 3u);
        }
        if (turn == 1u) buttons |= INPUT_BTN_LEFT;
        if (turn == 2u) buttons |= INPUT_BTN_RIGHT;
        scSendFrame(&h, slot, &tick, buttons, (i % SC_FIRE_EVERY) == 0);
        loopbackHarnessPump(&h);
    }

    largest = udpTestLargestSend();
    fprintf(stderr, "  send cap: %d pump(s) of play, largest datagram %d of "
                    "%d, %llu ms of wall clock\n", SC_PLAY_PUMPS, largest,
            UDP_MAX_SEND, (unsigned long long)(SDL_GetTicks() - startMs));

    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client dropped during play, so the minute of play did "
                "not run");
    }
    loopbackHarnessStop(&h);

    UT_ASSERT_MSG(largest > 0,
                  "no datagram was recorded at all, so the counter in "
                  "udpSendTo did not see the traffic");
    UT_ASSERT_MSG(largest <= UDP_MAX_SEND,
                  "a %d-byte datagram was sent, over the %d-byte send cap",
                  largest, UDP_MAX_SEND);
    return 0;
}
