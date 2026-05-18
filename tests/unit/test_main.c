/*
 * WinBoloUnitTests entry point. Dispatches one named test per
 * invocation so each CTest entry maps to a single test case.
 *
 *   WinBoloUnitTests --test <name>      run a single test
 *   WinBoloUnitTests --list             print test names
 *   WinBoloUnitTests                    run every test in sequence
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "test_harness.h"

#include "everard_map.h"
#include "server_sim.h"

typedef struct {
    const char *name;
    int (*fn)(void);
} UnitTestEntry;

static const UnitTestEntry s_tests[] = {
    { "transport_local_passive_threads", run_transport_local_passive_threads },
    { "sp_subscriber_delivery",          run_sp_subscriber_delivery          },
    { "active_local_input_to_shot",      run_active_local_input_to_shot      },
    { "sp_shoot_through_timer",          run_sp_shoot_through_timer          },
    { "ini_writer_persistence",          run_ini_writer_persistence          },
    { "ini_writer_insertion_point",      run_ini_writer_insertion_point      },
    { "ini_writer_security",             run_ini_writer_security             },
    { "wbn_bearer_state",                run_wbn_bearer_state                },
    { "wbn_rekey_codec",                 run_wbn_rekey_codec                 },
};
#define NUM_TESTS ((int)(sizeof(s_tests) / sizeof(s_tests[0])))

/* Defined in test_stubs.c — every binary that links ClientSim needs to
 * own this symbol because the engine reads it from menu-aware paths. */
extern bool isInMenu;

ServerSim *ut_make_running_sim(const char *player_name) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, player_name, false);
    return sim;
}

static int run_one(const char *name) {
    int i;
    for (i = 0; i < NUM_TESTS; i++) {
        if (strcmp(s_tests[i].name, name) == 0) {
            fprintf(stderr, "RUN  %s\n", s_tests[i].name);
            int rc = s_tests[i].fn();
            fprintf(stderr, "%s %s\n", rc == 0 ? "PASS" : "FAIL",
                    s_tests[i].name);
            return rc;
        }
    }
    fprintf(stderr, "unknown test: %s\n", name);
    return 2;
}

int main(int argc, char **argv) {
    /* SDL_Init(0) brings up the SDL3 runtime so SDL_CreateMutex /
     * SDL_CreateThread are usable from the unit tests without
     * dragging in any subsystem-specific init. */
    if (!SDL_Init(0)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 3;
    }
    (void)isInMenu;

    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        int i;
        for (i = 0; i < NUM_TESTS; i++) {
            printf("%s\n", s_tests[i].name);
        }
        SDL_Quit();
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "--test") == 0) {
        int rc = run_one(argv[2]);
        SDL_Quit();
        return rc;
    }

    /* Default: run them all in sequence. */
    int fail = 0;
    int i;
    for (i = 0; i < NUM_TESTS; i++) {
        if (run_one(s_tests[i].name) != 0) {
            fail = 1;
        }
    }
    SDL_Quit();
    return fail;
}
