/*
 * Base restock timers must not outlive the players they belong to.
 *
 * basesUpdate walks all MAX_TANKS slots and treats every baseTimer that is
 * not BASE_TIMER_OFF as a live restock cycle; each cycle that reaches zero
 * restocks EVERY base on the map and rearms. So the bases' refuel rate is
 * simply how many slots are armed, which is how it comes to scale with the
 * number of players.
 *
 * The bug these tests pin: nothing ever disarmed a slot. basesRemoveTimer
 * existed but had no callers, serverSimRemovePlayer left the leaver's timer
 * running, and the round-start stagger only wrote timers for players who
 * were connected — skipping, rather than clearing, the slots left armed by
 * an earlier round. A server that had once held eight players therefore kept
 * refuelling at eight players' rate for every later game, for the life of
 * the process.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "game_sim.h"
#include "bases.h"                 /* BASE_TIMER_OFF, BASE_TICKS_BETWEEN_REFUEL */
#include "everard_map.h"           /* E_MAP */
#include "server_sim.h"
#include "server_sim_internal.h"   /* serverSimGetGameSim — baseTimer lives on GameSim */
#include "server_sim_lifecycle.h"  /* serverSimStartGame / SetLobbyEnabled */
#include "test_harness.h"

static ServerSim *make_lobby_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

/* How many slots would restock the bases on the next pass of basesUpdate. */
static int armed_timer_count(ServerSim *sim) {
    GameSim *gs = serverSimGetGameSim(sim);
    int n = 0;
    BYTE i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (gs->baseTimer[i] != BASE_TIMER_OFF) n++;
    }
    return n;
}

/* ---- 1. A player who leaves mid-round stops restocking the bases. ---- */
int run_base_timer_cleared_on_leave(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimAddPlayer(sim, 1, "Guest", false);
    serverSimStartGame(sim);
    UT_ASSERT_MSG(armed_timer_count(sim) == 2,
                  "two players in the round should arm two timers (got %d)",
                  armed_timer_count(sim));

    serverSimRemovePlayer(sim, 1);

    UT_ASSERT_MSG(serverSimGetGameSim(sim)->baseTimer[1] == BASE_TIMER_OFF,
                  "the leaver's timer must be disarmed");
    UT_ASSERT_MSG(armed_timer_count(sim) == 1,
                  "one player left in the round should leave one timer armed "
                  "(got %d)", armed_timer_count(sim));

    serverSimDestroy(sim);
    return 0;
}

/* ---- 2. The next round arms one timer per player IN IT, not per player
 *         who was ever on the server. This is the reported bug. ---- */
int run_base_timer_not_inherited_next_round(void) {
    ServerSim *sim = make_lobby_sim();
    BYTE i;
    UT_ASSERT(sim != NULL);

    /* A busy first round: six players. */
    for (i = 0; i < 6; i++) {
        char name[16];
        snprintf(name, sizeof(name), "P%u", (unsigned)i);
        serverSimAddPlayer(sim, i, name, false);
    }
    serverSimStartGame(sim);
    UT_ASSERT_MSG(armed_timer_count(sim) == 6,
                  "six players should arm six timers (got %d)",
                  armed_timer_count(sim));

    /* Everyone but two leaves, and a new round starts. */
    for (i = 2; i < 6; i++) {
        serverSimRemovePlayer(sim, i);
    }
    serverSimStartGame(sim);

    UT_ASSERT_MSG(armed_timer_count(sim) == 2,
                  "a two-player round must refuel at two players' rate, not "
                  "at the high-water mark of six (got %d armed timers)",
                  armed_timer_count(sim));
    for (i = 2; i < MAX_TANKS; i++) {
        UT_ASSERT_MSG(serverSimGetGameSim(sim)->baseTimer[i] == BASE_TIMER_OFF,
                      "empty slot %u must hold no live restock cycle",
                      (unsigned)i);
    }

    serverSimDestroy(sim);
    return 0;
}
