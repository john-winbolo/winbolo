/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * A tank moving slower than tank_min_move is predicted without corrections.
 *
 * Such a tank does not move every tick: its speed is saved up as residual
 * speed and it moves on the tick the total reaches tank_min_move. No snapshot
 * carries that total. A client that replays its unacknowledged inputs from a
 * snapshot therefore has to start the replay from the total it had after the
 * acknowledged input; started from the total it has now, the replayed tank
 * moves on different ticks from the server's and every snapshot corrects it
 * by a tick's travel one way and then the other.
 *
 * Any tank pulling away from rest spends its first two dozen ticks under
 * tank_min_move, so open ground is enough: the client connects over the real
 * transport with 80ms of delay each way, which leaves about sixteen inputs
 * unacknowledged at any moment, then speeds up from rest and brakes to a
 * stop several times. Nothing else on the map touches the tank, so every
 * snapshot must find it where it was predicted.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* lastProcessedInput */
#include "client_sim.h"
#include "client_sim_internal.h"   /* MY_TANK, projectionPingMs */
#include "client_net.h"            /* clientBuildInputPacket */
#include "client_connect_state.h"
#include "client_enums.h"          /* tankButton */
#include "input_packet.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define SLOW_SPEC "delay=80"
#define SLOW_SEED 0x51A7u
#define SLOW_CONNECT_MAX 4000
#define SLOW_ESTABLISH_MAX 600
#define SLOW_SETTLE 80
/* Frames of each button. Thirty of accelerate takes the tank from rest past
 * tank_min_move, and thirty of brake brings it back to rest. */
#define SLOW_PHASE 30
#define SLOW_CYCLES 4

static uint32_t slowClientTick;

static bool slowConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool slowHaveTank(LoopbackHarness *h, void *user) {
    BYTE mx = 0, my = 0;
    (void)user;
    return clientSimGetMyTankMapPos(h->cs, &mx, &my) == TRUE;
}

/* One frame the way a frontend runs it: the keys half then the game half,
 * each built by clientBuildInputPacket, predicted, and sent. */
static void slowFeedFrame(LoopbackHarness *h, tankButton tb) {
    int half;
    for (half = 0; half < 2; half++) {
        InputPacket pkt;
        bool isGameTick = (half == 1);
        clientBuildInputPacket(h->cs, &pkt, tb, /*isShoot*/ false,
                               /*isMine*/ false, /*isBrain*/ false, isGameTick,
                               clientSimGetMyPlayerNum(h->cs), slowClientTick);
        if (isGameTick) {
            clientSimGameTick(h->cs, &pkt, /*isBrain*/ false);
        } else {
            clientSimKeysTick(h->cs, &pkt);
        }
        clientSimNetSendInput(h->cs, &pkt);
        slowClientTick++;
    }
}

int run_prediction_slow_tank_no_corrections(void) {
    LoopbackHarness h;
    BYTE slot;
    int at, i, cycle;
    uint32_t before, after;
    WORLD x0, y0, x1, y1;
    WORLD predX, predY, postX, postY;

    slowClientTick = 1;
    if (!loopbackHarnessStart(&h, "Slow", /*lobbyMode*/ false, SLOW_SPEC,
                              SLOW_SEED)) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start failed");
    }
    loopbackHarnessUseVirtualClock(&h, true);

    at = loopbackHarnessPumpUntil(&h, SLOW_CONNECT_MAX, slowConnected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never reached CONNECTED");
    }
    at = loopbackHarnessPumpUntil(&h, SLOW_ESTABLISH_MAX, slowHaveTank, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never got a tank");
    }
    slot = clientSimGetMyPlayerNum(h.cs);

    /* Stand still until the stream is established and a round trip has been
     * measured, then a while longer so the input pipeline has settled. */
    at = -1;
    for (i = 1; i <= SLOW_ESTABLISH_MAX; i++) {
        slowFeedFrame(&h, TNONE);
        loopbackHarnessPump(&h);
        if (h.cs->projectionPingMs != 0 && h.sim->lastProcessedInput[slot] > 0) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("no established input stream within %d pumps", SLOW_ESTABLISH_MAX);
    }
    for (i = 0; i < SLOW_SETTLE; i++) {
        slowFeedFrame(&h, TNONE);
        loopbackHarnessPump(&h);
    }
    if (MY_TANK(h.cs) == NULL) {
        loopbackHarnessStop(&h);
        UT_FAIL("own tank missing after the stream established");
    }

    before = clientSimGetPredictionCorrections(h.cs, NULL, NULL, NULL, NULL);
    tankGetWorld(&MY_TANK(h.cs), &x0, &y0);

    for (cycle = 0; cycle < SLOW_CYCLES; cycle++) {
        for (i = 0; i < SLOW_PHASE; i++) {
            slowFeedFrame(&h, TACCEL);
            loopbackHarnessPump(&h);
        }
        for (i = 0; i < SLOW_PHASE; i++) {
            slowFeedFrame(&h, TDECEL);
            loopbackHarnessPump(&h);
        }
    }
    /* Long enough for every input above to be acknowledged. */
    for (i = 0; i < SLOW_SETTLE; i++) {
        slowFeedFrame(&h, TNONE);
        loopbackHarnessPump(&h);
    }

    after = clientSimGetPredictionCorrections(h.cs, &predX, &predY, &postX, &postY);
    tankGetWorld(&MY_TANK(h.cs), &x1, &y1);
    fprintf(stderr, "  slow tank: moved (%u,%u) -> (%u,%u), %u correction(s)\n",
            (unsigned)x0, (unsigned)y0, (unsigned)x1, (unsigned)y1,
            (unsigned)(after - before));

    if (x0 == x1 && y0 == y1) {
        loopbackHarnessStop(&h);
        UT_FAIL("the tank never moved, so nothing was predicted");
    }
    if (after != before) {
        loopbackHarnessStop(&h);
        UT_FAIL("%u snapshot(s) moved the tank off its predicted position; "
                "the last from (%u,%u) to (%u,%u)",
                (unsigned)(after - before), (unsigned)predX, (unsigned)predY,
                (unsigned)postX, (unsigned)postY);
    }

    loopbackHarnessStop(&h);
    return 0;
}
