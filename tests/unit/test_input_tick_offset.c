/*
 * The producer's forward input-tick offset is adopted once and kept.
 *
 * The server consumes one tick number per starved half-step: when a slot's
 * input queue runs dry it substitutes the held buttons and advances
 * lastProcessedInput past the substituted tick. A producer whose counter has
 * fallen behind that consumption would emit tick numbers the server has
 * already passed, and every one of them would arrive stale.
 *
 * clientBuildInputPacket answers that by jumping the packet's tick past
 * lastProcessedInput — with a measured round trip, out to where the server
 * will be when the packet lands (the round trip in half-steps, plus a
 * margin). The jump has to be *kept* as a per-ClientSim offset rather than
 * recomputed per packet: a recomputed jump renumbers every packet to the same
 * value until the next snapshot moves lastProcessedInput, so the client only
 * ever supplies one usable input per snapshot. Held as an offset, the
 * producer's own counter keeps running on wall time and the whole stream lands
 * ahead of the server after one packet.
 *
 * This case drives clientBuildInputPacket directly rather than through a
 * frontend, on a ClientSim connected to ut_make_running_sim over the local
 * transport, and reads T2 state off the ClientSim struct (the unittests
 * profile permits internal access). serverLastProcessedInput is set by hand
 * to stand for a slot the server has stall-advanced past.
 *
 * The no-measured-ping case is pinned separately because it is a different
 * population: a local-transport producer supplies one input per net tick
 * while the server runs two half-steps, so it lands here in perfect
 * conditions with no flight time to cover, and anything past the next
 * unprocessed tick number is input latency the bot did not have before.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "client_sim.h"
#include "client_sim_internal.h"   /* inputTickOffset, clientState, MY_TANK */
#include "client_net.h"            /* clientBuildInputPacket */
#include "input_packet.h"
#include "test_harness.h"

#define OFS_WARMUP_TICKS 8

/* Where the server's consumption sits, well past anything the producer below
 * has supplied. */
#define OFS_SERVER_LPI   500u

int run_input_tick_offset_adopts_jump(void) {
    ServerSim *sim = ut_make_running_sim("Offset");
    ClientSim *cs;
    InputPacket pkt;
    uint32_t input_tick = 1;
    uint32_t firstJump, secondPkt;
    int w;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    UT_ASSERT(clientSimConnectLocal(cs, sim, "Offset", "", 0, 0));

    /* Warm up until the local tank is live. The active local transport runs
     * two server half-steps per clientSimNetTick, so feed two tick numbers per
     * net tick to keep the stream from starving. */
    for (w = 0; w < OFS_WARMUP_TICKS; w++) {
        InputPacket a, b;
        memset(&a, 0, sizeof(a)); a.tick = input_tick;     a.playerNum = 0;
        memset(&b, 0, sizeof(b)); b.tick = input_tick + 1; b.playerNum = 0;
        clientSimNetSendInput(cs, &a);
        clientSimNetSendInput(cs, &b);
        clientSimNetTick(cs);
        input_tick += 2;
    }

    /* Without a local tank clientBuildInputPacket returns an empty packet
     * before it reaches the tick logic and every assertion below is vacuous. */
    UT_ASSERT_MSG(MY_TANK(cs) != NULL, "own tank missing after warmup");

    /* 100ms round trip = 10 half-steps at 100Hz. */
    clientSimSetProjectionPing(cs, 100);
    cs->clientState.serverLastProcessedInput = OFS_SERVER_LPI;
    cs->inputTickOffset = 0;

    /* --- The jump, game parity ------------------------------------------ */
    /* Supplied tick 10 is far behind the server's 500: the packet must land at
     * 500 + 10 (RTT) + 4 (margin) = 514, which is even, as a game tick wants. */
    clientBuildInputPacket(cs, &pkt, 0, false, false, false, true, 0, 10);
    firstJump = OFS_SERVER_LPI + 10u + CLIENT_INPUT_JUMP_MARGIN_HALFSTEPS;
    UT_ASSERT_MSG(pkt.tick == firstJump,
                  "game-parity jump landed at %u, expected %u",
                  (unsigned)pkt.tick, (unsigned)firstJump);
    UT_ASSERT_MSG((pkt.tick % 2) == 0,
                  "game tick %u is odd", (unsigned)pkt.tick);
    UT_ASSERT_MSG(cs->inputTickOffset == firstJump - 10u,
                  "offset %u does not carry the jump (expected %u)",
                  (unsigned)cs->inputTickOffset, (unsigned)(firstJump - 10u));

    /* --- The offset sticks ---------------------------------------------- */
    /* The producer's counter advances by one and the half flips, while the
     * server's consumption stays where it was. The packet must land one past
     * the jump, not be renumbered to the same value again. */
    clientBuildInputPacket(cs, &pkt, 0, false, false, false, false, 0, 11);
    secondPkt = pkt.tick;
    UT_ASSERT_MSG(secondPkt == firstJump + 1u,
                  "second packet landed at %u, expected %u — the offset was "
                  "recomputed instead of kept",
                  (unsigned)secondPkt, (unsigned)(firstJump + 1u));
    UT_ASSERT_MSG((secondPkt % 2) == 1,
                  "keys tick %u is even", (unsigned)secondPkt);
    UT_ASSERT_MSG(cs->inputTickOffset == firstJump - 10u,
                  "offset moved on a packet that needed no jump (now %u)",
                  (unsigned)cs->inputTickOffset);

    /* --- A caught-up producer is untouched ------------------------------ */
    /* Supplied tick already past the server's consumption on its own: the held
     * offset is added and nothing else happens. */
    {
        uint32_t held = cs->inputTickOffset;
        clientBuildInputPacket(cs, &pkt, 0, false, false, false, true, 0, 600);
        UT_ASSERT_MSG(pkt.tick == 600u + held,
                      "caught-up producer renumbered to %u, expected %u",
                      (unsigned)pkt.tick, (unsigned)(600u + held));
        UT_ASSERT_MSG(cs->inputTickOffset == held,
                      "offset changed for a caught-up producer: %u -> %u",
                      (unsigned)held, (unsigned)cs->inputTickOffset);
    }

    /* --- The jump, keys parity ------------------------------------------ */
    /* Clear the offset so this is a fresh producer falling behind; the target
     * lands on 514 again and must be bumped to the odd 515 for a keys tick. */
    cs->inputTickOffset = 0;
    clientBuildInputPacket(cs, &pkt, 0, false, false, false, false, 0, 11);
    UT_ASSERT_MSG(pkt.tick == firstJump + 1u,
                  "keys-parity jump landed at %u, expected %u",
                  (unsigned)pkt.tick, (unsigned)(firstJump + 1u));
    UT_ASSERT_MSG((pkt.tick % 2) == 1,
                  "keys tick %u is even", (unsigned)pkt.tick);
    UT_ASSERT_MSG(cs->inputTickOffset == firstJump + 1u - 11u,
                  "offset %u does not carry the keys-parity jump (expected %u)",
                  (unsigned)cs->inputTickOffset,
                  (unsigned)(firstJump + 1u - 11u));

    /* --- The round trip is capped --------------------------------------- */
    /* A 2s ping is 200 half-steps, most of the 256-entry input history. The
     * jump takes the cap instead, so the gap it leaves in the ring stays
     * short. */
    cs->inputTickOffset = 0;
    clientSimSetProjectionPing(cs, 2000);
    clientBuildInputPacket(cs, &pkt, 0, false, false, false, true, 0, 10);
    {
        uint32_t capped = OFS_SERVER_LPI + CLIENT_INPUT_JUMP_MAX_RTT_HALFSTEPS +
                          CLIENT_INPUT_JUMP_MARGIN_HALFSTEPS;
        UT_ASSERT_MSG(pkt.tick == capped,
                      "uncapped ping jumped to %u, expected the capped %u",
                      (unsigned)pkt.tick, (unsigned)capped);
    }

    /* --- The local producer, no measured round trip -------------------- */
    /* projectionPingMs 0 is the local transport (headless, the gym, an
     * in-process host), which under-supplies structurally rather than being
     * locked out. The target is the next tick number the server has not
     * processed — 501 — parity-corrected: already odd for a keys tick, bumped
     * to the even 502 for a game tick. */
    cs->inputTickOffset = 0;
    clientSimSetProjectionPing(cs, 0);
    clientBuildInputPacket(cs, &pkt, 0, false, false, false, false, 0, 10);
    UT_ASSERT_MSG(pkt.tick == OFS_SERVER_LPI + 1u,
                  "ping-0 keys tick landed at %u, expected %u — an "
                  "under-supplying local producer waits in the queue for the "
                  "difference",
                  (unsigned)pkt.tick, (unsigned)(OFS_SERVER_LPI + 1u));
    UT_ASSERT_MSG(cs->inputTickOffset == OFS_SERVER_LPI + 1u - 10u,
                  "offset %u does not carry the ping-0 keys jump (expected %u)",
                  (unsigned)cs->inputTickOffset,
                  (unsigned)(OFS_SERVER_LPI + 1u - 10u));

    cs->inputTickOffset = 0;
    clientBuildInputPacket(cs, &pkt, 0, false, false, false, true, 0, 10);
    UT_ASSERT_MSG(pkt.tick == OFS_SERVER_LPI + 2u,
                  "ping-0 game tick landed at %u, expected %u",
                  (unsigned)pkt.tick, (unsigned)(OFS_SERVER_LPI + 2u));

    clientSimDisconnect(cs);
    clientSimDestroy(cs);
    serverSimDestroy(sim);
    return 0;
}
