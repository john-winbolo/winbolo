/*
 * SP input-to-shot tick-count baseline.
 *
 * Drives a ServerSim + ClientSim wired together with the active
 * transport_local — the same shape SP uses today. Sends warmup
 * inputs to fill the server's jitter buffer, then a FIRE input,
 * then ticks the active transport (which advances serverSimTick
 * inside localTick) until a shell appears in the server's shell
 * snapshot. Records the tick count.
 *
 * Captured BEFORE the SP → passive-local unification. After the
 * unification the server's tick lives on the host timer thread,
 * so the same fire input takes ~1 more sim tick to be observed
 * server-side. EXPECTED_TICKS_TO_SHELL is the captured pre-shift
 * value — Phase 6 should adjust it once the latency change lands.
 *
 * This is a measured number, not a derived bound.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "client_sim.h"
#include "client_net.h"
#include "input_packet.h"
#include "test_harness.h"

/* Allow a generous ceiling — the SP fire-to-shell path empirically
 * lands in single digits today, but the test's job is to record the
 * actual count, not assert a tight bound. */
#define EXPECTED_TICKS_TO_SHELL 12

#define WARMUP_TICKS 4

int run_sp_input_to_shot_baseline(void) {
    ServerSim *sim = ut_make_running_sim("Shooter");
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed returned NULL");

    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs, gameOpen, false, 0, -1);
    clientSimSetPlayerNum(cs, 0);
    clientSimConnectLocal(cs, sim, 0);

    /* Pre-fire warmup. Each input carries a strictly-increasing tick
     * so the server's jitter buffer reaches its initial target depth
     * and lastProcessedInput advances past 0 before we drop the
     * fire packet. */
    uint32_t input_tick;
    for (input_tick = 1; input_tick <= WARMUP_TICKS; input_tick++) {
        InputPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.tick      = input_tick;
        pkt.playerNum = 0;
        clientSimNetSendInput(cs, &pkt);
        clientSimNetTick(cs);
    }

    /* Fire input on a game-tick (even) input.tick — the keys/game
     * split inside serverSimTick is driven by the dequeued input's
     * tick parity, and only the game-tick branch actually fires a
     * shell. */
    if ((input_tick & 1u) == 1u) {
        input_tick++;
    }
    InputPacket firePkt;
    memset(&firePkt, 0, sizeof(firePkt));
    firePkt.tick      = input_tick;
    firePkt.playerNum = 0;
    firePkt.actions   = INPUT_ACTION_FIRE;
    clientSimNetSendInput(cs, &firePkt);

    /* Spin the active local transport until the server's shell
     * snapshot shows at least one live shell. localTick → serverSimTick
     * inside transport_local.c is what advances the sim. */
    int ticks_to_shell = -1;
    int i;
    for (i = 1; i <= 200; i++) {
        clientSimNetTick(cs);
        ShellRender shells[8];
        int nshells = serverSimGetShellSnapshot(sim, shells, 8);
        if (nshells > 0) {
            ticks_to_shell = i;
            break;
        }
    }

    fprintf(stderr,
            "  input-to-shell baseline: %d ticks (cap %d)\n",
            ticks_to_shell, EXPECTED_TICKS_TO_SHELL);

    UT_ASSERT_MSG(ticks_to_shell > 0,
                  "no shell observed within tick budget");
    UT_ASSERT_MSG(ticks_to_shell <= EXPECTED_TICKS_TO_SHELL,
                  "baseline shifted: %d > EXPECTED %d (update after Phase 6)",
                  ticks_to_shell, EXPECTED_TICKS_TO_SHELL);

    clientSimDestroy(cs);
    serverSimDestroy(sim);
    return 0;
}
