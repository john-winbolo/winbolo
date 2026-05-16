/*
 * SP shoot-through-timer end-to-end.
 *
 * Mirrors the unified SP wiring: serverInstanceStartup with
 * acceptRemoteClients=false, a ClientSim wired via the passive local
 * transport, and a worker thread driving serverInstanceTick at the SP
 * cadence. The main thread sends a FIRE input via clientSimNetSendInput
 * and the test waits for a shell to appear in the server's shell
 * snapshot. Exercises the recursive-mutex wrapper, the passive
 * transport's input queue serialisation, the acceptRemoteClients=false
 * gates in serverInstanceTick, and the SP startup ordering all in one
 * shot.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_lifecycle.h"
#include "client_sim.h"
#include "client_net.h"
#include "input_packet.h"
#include "threads.h"
#include "test_harness.h"

#define WARMUP_INPUTS    4
#define POLL_ITERS       50
#define POLL_INTERVAL_MS 20
#define WORKER_TICK_MS   20

typedef struct {
    ServerSim    *sim;
    SDL_AtomicInt stop;
} WorkerCtx;

static int SDLCALL worker_fn(void *arg) {
    WorkerCtx *ctx = (WorkerCtx *)arg;
    while (SDL_GetAtomicInt(&ctx->stop) == 0) {
        serverInstanceTick(ctx->sim);
        SDL_Delay(WORKER_TICK_MS);
    }
    return 0;
}

int run_sp_shoot_through_timer(void) {
    if (!threadsCreate(TRUE)) {
        UT_FAIL("threadsCreate failed");
    }

    ServerSim *sim = ut_make_running_sim("Shooter");
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed returned NULL");

    ServerInstanceConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.udpPort             = 0;
    cfg.bindAddr            = "";
    cfg.password            = "";
    cfg.maxPlayers          = MAX_TANKS;
    cfg.acceptRemoteClients = false;
    cfg.useWbn              = false;
    cfg.compTanks           = aiNone;
    cfg.useTracker          = false;
    cfg.trackerAddr         = "";
    cfg.trackerPort         = 0;
    cfg.useNatKeepalive     = false;
    cfg.useNatPortmap       = false;
    UT_ASSERT(serverInstanceStartup(sim, &cfg));

    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs, gameOpen, false, 0, -1);
    clientSimSetPlayerNum(cs, 0);
    UT_ASSERT(clientSimConnectLocalPassive(cs, sim, 0));

    WorkerCtx ctx;
    ctx.sim = sim;
    SDL_SetAtomicInt(&ctx.stop, 0);
    SDL_Thread *worker = SDL_CreateThread(worker_fn, "sp-shoot-worker", &ctx);
    UT_ASSERT(worker != NULL);

    /* Let the worker tick a couple of cycles before any input arrives so
     * the server is unambiguously in its running-state tick loop by the
     * time we send a fire input. */
    SDL_Delay(50);

    /* Warmup inputs to fill the jitter buffer / advance the server's
     * input pointer past 0, mirroring test_active_local_input_to_shot. */
    uint32_t input_tick;
    for (input_tick = 1; input_tick <= WARMUP_INPUTS; input_tick++) {
        InputPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.tick      = input_tick;
        pkt.playerNum = 0;
        clientSimNetSendInput(cs, &pkt);
    }

    /* Fire on a game-tick (even input.tick) — the server's keys/game
     * split is driven by input.tick parity and only the game-tick branch
     * actually fires a shell. */
    if ((input_tick & 1u) == 1u) {
        input_tick++;
    }
    InputPacket firePkt;
    memset(&firePkt, 0, sizeof(firePkt));
    firePkt.tick      = input_tick;
    firePkt.playerNum = 0;
    firePkt.actions   = INPUT_ACTION_FIRE;
    clientSimNetSendInput(cs, &firePkt);

    /* Poll for a shell in the server's snapshot. Hold threadsMutex
     * around the read so the linked-list walk doesn't race the
     * worker's serverSimTick. POLL_ITERS × 20 ms gives a ~1 s budget,
     * well above the handful-of-ticks SP fire-to-shell baseline. */
    int observed_at = -1;
    int n_shells = 0;
    int i;
    for (i = 1; i <= POLL_ITERS; i++) {
        ShellRender shells[8];
        threadsWaitForMutex();
        n_shells = serverSimGetShellSnapshot(sim, shells, 8);
        threadsReleaseMutex();
        if (n_shells > 0) {
            observed_at = i;
            break;
        }
        SDL_Delay(POLL_INTERVAL_MS);
    }

    fprintf(stderr,
            "  shell observed after %d poll iter(s); n_shells=%d\n",
            observed_at, n_shells);

    UT_ASSERT_MSG(n_shells > 0,
                  "shell did not appear within %d timer cycles (~%d ms)",
                  POLL_ITERS, POLL_ITERS * POLL_INTERVAL_MS);

    SDL_SetAtomicInt(&ctx.stop, 1);
    SDL_WaitThread(worker, NULL);

    clientSimDisconnect(cs);
    clientSimDestroy(cs);
    serverInstanceShutdown(sim);
    serverSimDestroy(sim);
    threadsDestroy();
    return 0;
}
