/*
 * Team start sides from the lobby to the spawned tanks
 * (test_starts_side_integration.c).
 *
 * The lobby reservations picked on join and team change, the per-team
 * side table handed to startsAssignBatch at game start, and the
 * tank-aware spawn scatter together put every tank of a north team in the
 * northern half of the start bounding box and every tank of a south team
 * in the southern half, on distinct squares, even when the south team has
 * more players than starts. The game starts through
 * serverSimStartGameInPlace: the full serverSimStartGame reloads the
 * starts from the cached map and would discard the injected layout.
 *
 *   (21) sixteen corner starts, four players north against twelve south,
 *        with the sides set before the players join and again after.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "types.h"
#include "game_sim.h"
#include "bolo_map.h"              /* mapSetPos */
#include "bolo_rand.h"             /* bolo_srand */
#include "everard_map.h"
#include "starts.h"                /* startsGetMaxs */
#include "start_sides.h"           /* START_SIDE_* */
#include "tank.h"                  /* tankGetWorld */
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim.ss, sim->sim.tanks */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, serverSimSetTeam,
                                    * serverSimSetTeamMeta, serverSimStartGameInPlace */
#include "test_harness.h"

typedef struct {
    BYTE x;
    BYTE y;
} LayoutStart;

/* Four clusters of four, one per corner, every start a diagonal. bbox
 * 40..210 on both axes, so the halves split at y=125. */
static const LayoutStart k_corners[16] = {
    {  40,  40 }, {  50,  40 }, {  40,  50 }, {  50,  50 },
    { 200,  40 }, { 210,  40 }, { 200,  50 }, { 210,  50 },
    {  40, 200 }, {  50, 200 }, {  40, 210 }, {  50, 210 },
    { 200, 200 }, { 210, 200 }, { 200, 210 }, { 210, 210 },
};

/* Lobby-enabled ServerSim on Everard Island with the corner layout in
 * place of the map's own starts, every start square deep sea and the
 * map's pills and bases cleared so only the layout drives placement. */
static ServerSim *make_corner_lobby(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    GameSim *gs;
    int i;
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    gs = &sim->sim;
    gs->pb->numPills = 0;
    gs->bs->numBases = 0;
    gs->ss->numStarts = 0;
    for (i = 0; i < 16; i++) {
        BYTE idx = gs->ss->numStarts++;
        mapSetPos(gs, &gs->mp, k_corners[i].x, k_corners[i].y, DEEP_SEA, FALSE, TRUE);
        gs->ss->item[idx].x = k_corners[i].x;
        gs->ss->item[idx].y = k_corners[i].y;
        gs->ss->item[idx].dir = 0;
    }
    return sim;
}

/* Slots 0..3 on team 1, slots 4..15 on team 2. */
static void add_players(ServerSim *sim) {
    BYTE s;
    for (s = 0; s < MAX_TANKS; s++) {
        char name[16];
        snprintf(name, sizeof(name), "P%u", (unsigned)s);
        serverSimAddPlayer(sim, s, name, false);
        serverSimSetTeam(sim, s, (BYTE)(s < 4 ? 1 : 2));
    }
}

static void set_sides(ServerSim *sim) {
    serverSimSetTeamMeta(sim, 1, 0, 0, START_SIDE_N, NULL, 0);
    serverSimSetTeamMeta(sim, 2, 0, 0, START_SIDE_S, NULL, 0);
}

/* Map square a live tank sits on. */
static void tank_square(GameSim *gs, BYTE slot, int *mx, int *my) {
    WORLD wx;
    WORLD wy;
    tankGetWorld(&gs->tanks[slot], &wx, &wy);
    *mx = (int)(wx >> M_W_SHIFT_SIZE);
    *my = (int)(wy >> M_W_SHIFT_SIZE);
}

/* How many of the slots first..first+count-1 sit west of midX. */
static int count_west(const int *mx, int first, int count, int midX) {
    int west = 0;
    int i;
    for (i = first; i < first + count; i++) {
        if (mx[i] < midX) west++;
    }
    return west;
}

/* Every team-1 tank north of the bbox midline, every team-2 tank south
 * of it, all sixteen on distinct squares — and each team spread across
 * both of its side's corners rather than piled into one of them. */
static int check_split(ServerSim *sim, const char *what) {
    GameSim *gs = &sim->sim;
    int leftPos;
    int rightPos;
    int topPos;
    int bottomPos;
    int midY;
    int midX;
    int west;
    int mx[MAX_TANKS];
    int my[MAX_TANKS];
    int i;
    int j;

    startsGetMaxs(&gs->ss, &leftPos, &rightPos, &topPos, &bottomPos);
    midY = (topPos + bottomPos) / 2;
    midX = (leftPos + rightPos) / 2;

    for (i = 0; i < MAX_TANKS; i++) {
        UT_ASSERT_MSG(gs->tanks[i] != NULL, "%s: slot %d has no tank", what, i);
        tank_square(gs, (BYTE)i, &mx[i], &my[i]);
    }
    for (i = 0; i < 4; i++) {
        UT_ASSERT_MSG(my[i] < midY,
                      "%s: team 1 (north) slot %d spawned at (%d,%d), below the midline %d",
                      what, i, mx[i], my[i], midY);
    }
    for (i = 4; i < MAX_TANKS; i++) {
        UT_ASSERT_MSG(my[i] > midY,
                      "%s: team 2 (south) slot %d spawned at (%d,%d), above the midline %d",
                      what, i, mx[i], my[i], midY);
    }
    for (i = 0; i < MAX_TANKS; i++) {
        for (j = 0; j < i; j++) {
            UT_ASSERT_MSG(mx[i] != mx[j] || my[i] != my[j],
                          "%s: slots %d and %d both spawned at (%d,%d)",
                          what, j, i, mx[i], my[i]);
        }
    }

    /* Team 1 has four players and eight north starts, four in each north
       corner, so its side can hold every one of them: two go north-west
       and two north-east. Piling all four into one corner is the bug this
       pins — the side rules alone are happy with it. */
    west = count_west(mx, 0, 4, midX);
    UT_ASSERT_MSG(west == 2,
                  "%s: team 1 (north) put %d of its 4 tanks west of %d, expected 2 in each north corner",
                  what, west, midX);

    /* Team 2 has twelve players for eight south starts, so four of them
       ride a start a team-mate already holds. However the riders land, the
       team still has to use both south corners; an even 6/6 is what the
       spread and the rider balance give, but the corners only have to be
       used, not matched exactly. */
    west = count_west(mx, 4, 12, midX);
    UT_ASSERT_MSG(west >= 4 && west <= 8,
                  "%s: team 2 (south) put %d of its 12 tanks west of %d, expected both south corners used",
                  what, west, midX);
    return 0;
}

/* (21) Four north against twelve south on sixteen corner starts, started
 *      in place. First with the sides set before anyone joins, then with
 *      the sides set after every player is on a team. */
int run_starts_side_end_to_end_four_v_twelve(void) {
    ServerSim *sim;

    sim = make_corner_lobby();
    UT_ASSERT(sim != NULL);
    set_sides(sim);
    add_players(sim);
    bolo_srand(11);
    serverSimStartGameInPlace(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
    UT_ASSERT(check_split(sim, "sides first") == 0);
    serverSimDestroy(sim);

    sim = make_corner_lobby();
    UT_ASSERT(sim != NULL);
    add_players(sim);
    set_sides(sim);
    bolo_srand(11);
    serverSimStartGameInPlace(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
    UT_ASSERT(check_split(sim, "sides after") == 0);
    serverSimDestroy(sim);
    return 0;
}
