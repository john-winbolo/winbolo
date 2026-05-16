/*
 * SP control-event delivery across threads.
 *
 * Mirrors the SP wiring at gamefront.c:1122: a ClientSim is
 * registered as a subscriber on a ServerSim that is being ticked
 * from a worker thread. The test thread publishes a CTRL_PLAYER_NAME
 * event (via serverSimSetPlayerName) under threadsMutex and verifies
 * the subscriber's ClientSim mirrored the rename — both ends are
 * holding the same mutex, so the dispatcher fan-out and the
 * ClientSim read are serialised even though the sim tick is on a
 * separate thread.
 *
 * Sanitizers should not flag the subscriber dispatch path. The test
 * is the canary the §3.4 / §3.0 audit hinges on — if a future change
 * starts reaching into ClientSim state from a worker thread without
 * the lock, this is where it surfaces.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "client_sim.h"
#include "client_net.h"
#include "control_event.h"
#include "game_sim.h"
#include "players.h"
#include "threads.h"
#include "test_harness.h"

#define SIM_TICKS_TARGET 500
#define NAME_CHANGES 50

typedef struct {
    ServerSim *sim;
    SDL_AtomicInt done;
} TickerArgs;

static int SDLCALL ticker_fn(void *opaque) {
    TickerArgs *args = (TickerArgs *)opaque;
    int ticks = 0;
    while (SDL_GetAtomicInt(&args->done) == 0 || ticks < SIM_TICKS_TARGET) {
        threadsWaitForMutex();
        serverSimTick(args->sim);
        threadsReleaseMutex();
        ticks++;
        /* Match real timer cadence loosely so we get genuine
         * interleaving with the main-thread publisher. */
        SDL_DelayNS(20000);
    }
    return ticks;
}

int run_sp_subscriber_delivery(void) {
    if (!threadsCreate(TRUE)) {
        UT_FAIL("threadsCreate failed");
    }

    ServerSim *sim = ut_make_running_sim("InitialName");
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed returned NULL");

    /* Allocate the human-side ClientSim the same way gamefront does
     * before connecting it: alloc + create + slot-0 + subscribe. */
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs, gameOpen, false, 0, -1);
    clientSimSetPlayerNum(cs, 0);
    /* Seed the client's player table so playersSetPlayerName routed
     * through CTRL_PLAYER_NAME has a slot to overwrite. */
    GameSim *cgs = clientSimGetGameSim(cs);
    playersSetPlayer(cs, &cgs->plyrs, NEUTRAL, 0,
                     (char *)"InitialName", "??",
                     0, 0, 0, 0, 0, FALSE, 0, NULL, TRUE);

    SubscriberHandle h = serverSimRegisterClientSubscriber(sim, cs);
    UT_ASSERT_MSG(h != SUBSCRIBER_HANDLE_INVALID,
                  "serverSimRegisterClientSubscriber failed");

    TickerArgs targs;
    targs.sim = sim;
    SDL_SetAtomicInt(&targs.done, 0);
    SDL_Thread *ticker = SDL_CreateThread(ticker_fn, "subscriber-ticker", &targs);
    UT_ASSERT(ticker != NULL);

    int i;
    for (i = 1; i <= NAME_CHANGES; i++) {
        char nameBuf[PACKET_MAX_PLAYER_NAME];
        snprintf(nameBuf, sizeof(nameBuf), "Player%03d", i);

        threadsWaitForMutex();
        serverSimSetPlayerName(sim, 0, nameBuf);
        /* Read the client-side mirror back inside the same critical
         * section so we observe the in-process delivery the dispatcher
         * performed synchronously inside serverSimPublishControl. */
        char observed[PACKET_MAX_PLAYER_NAME];
        observed[0] = '\0';
        playersGetPlayerName(&cgs->plyrs, 0, observed, FALSE);
        threadsReleaseMutex();

        UT_ASSERT_MSG(strcmp(observed, nameBuf) == 0,
                      "iter %d: subscriber missed delivery; got '%s' want '%s'",
                      i, observed, nameBuf);

        SDL_Delay(1);
    }

    SDL_SetAtomicInt(&targs.done, 1);
    int ticked = 0;
    SDL_WaitThread(ticker, &ticked);
    fprintf(stderr, "  ticker thread completed %d serverSimTicks\n", ticked);

    serverSimUnregisterSubscriber(sim, h);
    clientSimDestroy(cs);
    serverSimDestroy(sim);
    threadsDestroy();
    return 0;
}
