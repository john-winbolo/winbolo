/*
 * Active local-transport input-to-shot regression.
 *
 * Constructs ServerSim + ClientSim wired via the active variant
 * (clientSimConnectLocal — the path gym, headless, and wasm use),
 * drives ticks single-threaded through clientSimNetTick, sends a
 * FIRE input, and counts ticks until a shell appears server-side.
 * Asserts the tick count stays at or below EXPECTED_TICKS_TO_SHELL;
 * bumps of this value need an explanation.
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

/* Allow a generous ceiling — the fire-to-shell path empirically
 * lands in single digits today, but the test's job is to record the
 * actual count, not assert a tight bound. */
#define EXPECTED_TICKS_TO_SHELL 12

#define WARMUP_TICKS 4

int run_active_local_input_to_shot(void) {
    ServerSim *sim = ut_make_running_sim("Shooter");
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed returned NULL");

    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    /* New signature: connect runs serverSimLocalJoin internally, which
     * picks slot 0 in this empty harness; playerName is the input
     * the join validator accepts. */
    UT_ASSERT(clientSimConnectLocal(cs, sim, "Shooter", "", 0, 0));

    /* Pre-fire warmup. The active local transport runs TWO server
     * half-steps per clientSimNetTick, so the producer must supply two
     * tick numbers per net tick or the stream starves into substitutes
     * (stall-advance) and the producer's numbering falls permanently
     * behind. Feed pairs so the jitter buffer fills and lastProcessedInput
     * advances past 0 without skew. */
    uint32_t input_tick = 1;
    int w;
    for (w = 0; w < WARMUP_TICKS; w++) {
        InputPacket a, b;
        memset(&a, 0, sizeof(a)); a.tick = input_tick;     a.playerNum = 0;
        memset(&b, 0, sizeof(b)); b.tick = input_tick + 1; b.playerNum = 0;
        clientSimNetSendInput(cs, &a);
        clientSimNetSendInput(cs, &b);
        clientSimNetTick(cs);
        input_tick += 2;
    }

    /* Fire on a fresh even (game-tick) number computed from the server's
     * live lastProcessedInput rather than an assumed counter — the
     * keys/game split is driven by the dequeued input's parity, and only
     * the game-tick branch fires a shell. Substitutes may have advanced
     * the server past our warmup numbering, so read it back. Pair the
     * fire with a keys-tick companion so the frame still supplies two
     * numbers. */
    uint32_t lpi = serverSimGetLastProcessedInput(sim, 0);
    uint32_t fireTick = lpi + 2;
    if ((fireTick & 1u) == 1u) {
        fireTick++;                     /* even = game tick */
    }
    uint32_t keysTick = fireTick - 1;   /* odd, still > lpi */

    InputPacket keysPkt, firePkt;
    memset(&keysPkt, 0, sizeof(keysPkt));
    keysPkt.tick      = keysTick;
    keysPkt.playerNum = 0;
    memset(&firePkt, 0, sizeof(firePkt));
    firePkt.tick      = fireTick;
    firePkt.playerNum = 0;
    firePkt.actions   = INPUT_ACTION_FIRE;
    clientSimNetSendInput(cs, &keysPkt);
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
            "  input-to-shell: %d ticks (cap %d)\n",
            ticks_to_shell, EXPECTED_TICKS_TO_SHELL);

    UT_ASSERT_MSG(ticks_to_shell > 0,
                  "no shell observed within tick budget");
    UT_ASSERT_MSG(ticks_to_shell <= EXPECTED_TICKS_TO_SHELL,
                  "ticks-to-shell increased: %d > EXPECTED %d",
                  ticks_to_shell, EXPECTED_TICKS_TO_SHELL);

    clientSimDestroy(cs);
    serverSimDestroy(sim);
    return 0;
}
