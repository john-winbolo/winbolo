/*
 * Batch start-assignment reservation tests (test_starts_assign_batch.c).
 *
 * startsAssignBatch computes every connected slot's start in one pass.
 * Its optional reservedStartIdx0 argument locks a 0-based start per slot
 * (MAX_STARTS = none): a reserved slot is excluded from the cluster /
 * farthest-first placement and emitted at exactly its reserved index,
 * while the unreserved slots fill the remaining free starts. These tests
 * pin that contract:
 *
 *   (a) a reserved slot lands on exactly its reserved start;
 *   (b) an unreserved slot avoids a reserved start;
 *   (c) a stale (out-of-range) reservation falls through to placement;
 *   (d) duplicate reservations: all honored when LOBBY_SHARED_STARTS is
 *       on, else the first honored and the rest placed normally;
 *   (e) NULL reservedStartIdx0 preserves the original (no-reservation) path.
 *
 * The 5-start layout is built on a running sim's map with each start square
 * forced to deep sea (no mine) so startsIsValidSquare accepts it and the
 * distance math drives placement. All players are solo (team 0) so Step 5's
 * farthest-first pass is deterministic and needs no base setup.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "types.h"
#include "game_sim.h"
#include "bolo_map.h"     /* mapSetPos */
#include "starts.h"       /* startsAssignBatch */
#include "bolo_rand.h"    /* bolo_srand */
#include "lobby_shared_starts.h" /* lobbySharedStartsEnabled — duplicate reservations */
#include "server_sim.h"   /* ut_make_running_sim, serverSimGetGameSim */
#include "test_harness.h"

/* Start layout. Chebyshev distances (startsMapDistance) from start 0 at
 * (100,100): 1 (102,100)=2, 2 (200,200)=100, 3 (130,100)=30, 4 (50,50)=50.
 * From start 3 at (130,100): 0=30, 1=28, 2=100, 4=80. */
static const BYTE k_sx[5] = {100, 102, 200, 130, 50};
static const BYTE k_sy[5] = {100, 100, 200, 100, 50};
#define K_NUM_STARTS 5

static void build_starts(GameSim *gs) {
    int i;
    for (i = 0; i < K_NUM_STARTS; i++) {
        mapSetPos(gs, &gs->mp, k_sx[i], k_sy[i], DEEP_SEA, FALSE, TRUE);
        gs->ss->item[i].x = k_sx[i];
        gs->ss->item[i].y = k_sy[i];
        gs->ss->item[i].dir = 0;
    }
    startsSetNumStarts(&gs->ss, K_NUM_STARTS);
}

/* Reset the per-slot inputs to "nobody connected, no reservations". */
static void reset_inputs(bool *connected, BYTE *team, BYTE *reserved) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        connected[i] = false;
        team[i] = 0;                 /* solo */
        reserved[i] = MAX_STARTS;    /* none */
    }
}

/* (a) A reserved slot lands on exactly its reserved start. */
int run_starts_batch_reserved_lands_exact(void) {
    ServerSim *sim = ut_make_running_sim("Reserve");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    build_starts(gs);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE out[MAX_TANKS];
    reset_inputs(connected, team, reserved);
    connected[0] = true;
    reserved[0] = 3;                 /* lock 0-based start 3 */

    startsAssignBatch(gs, &gs->ss, connected, team, out, reserved, NULL);

    UT_ASSERT_MSG(out[0] == 3,
                  "reserved slot should land on start 3, got %u",
                  (unsigned)out[0]);
    serverSimDestroy(sim);
    return 0;
}

/* (b) An unreserved slot avoids a reserved start. Slot 0 locks start 0;
 *     solo slot 1 then takes the farthest free start (2). */
int run_starts_batch_unreserved_avoids_reserved(void) {
    ServerSim *sim = ut_make_running_sim("Avoid");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    build_starts(gs);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE out[MAX_TANKS];
    reset_inputs(connected, team, reserved);
    connected[0] = true;
    connected[1] = true;
    reserved[0] = 0;                 /* lock 0-based start 0 */

    startsAssignBatch(gs, &gs->ss, connected, team, out, reserved, NULL);

    UT_ASSERT_MSG(out[0] == 0,
                  "reserved slot should land on start 0, got %u",
                  (unsigned)out[0]);
    UT_ASSERT_MSG(out[1] != 0 && out[1] == 2,
                  "unreserved slot should avoid start 0 and take farthest (2), got %u",
                  (unsigned)out[1]);
    serverSimDestroy(sim);
    return 0;
}

/* (c) A stale (out-of-range) reservation is ignored; the slot is placed
 *     normally rather than locked or left unplaced. */
int run_starts_batch_stale_reservation_falls_through(void) {
    ServerSim *sim = ut_make_running_sim("Stale");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    build_starts(gs);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE out[MAX_TANKS];
    reset_inputs(connected, team, reserved);
    connected[0] = true;
    reserved[0] = 7;                 /* >= numStarts: stale, treat as none */

    startsAssignBatch(gs, &gs->ss, connected, team, out, reserved, NULL);

    UT_ASSERT_MSG(out[0] != MAX_STARTS && out[0] < K_NUM_STARTS,
                  "stale reservation should fall through to placement, got %u",
                  (unsigned)out[0]);
    serverSimDestroy(sim);
    return 0;
}

/* (d) Duplicate reservations. With shared starts on both slots keep the
 *     start they named; with it off the first keeps it and the second is
 *     placed as unreserved. */
int run_starts_batch_duplicate_honors_first(void) {
    ServerSim *sim = ut_make_running_sim("Dup");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    build_starts(gs);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE out[MAX_TANKS];
    reset_inputs(connected, team, reserved);
    connected[0] = true;
    connected[1] = true;
    reserved[0] = 3;                 /* both name start 3 */
    reserved[1] = 3;

    startsAssignBatch(gs, &gs->ss, connected, team, out, reserved, NULL);

    UT_ASSERT_MSG(out[0] == 3,
                  "first slot should keep reserved start 3, got %u",
                  (unsigned)out[0]);
    if (lobbySharedStartsEnabled()) {
        /* Shared starts: both slots asked for start 3 and both get it —
         * startsGetStart's scatter puts the second tank a few squares
         * behind the first. */
        UT_ASSERT_MSG(out[1] == 3,
                      "duplicate slot should share reserved start 3, got %u",
                      (unsigned)out[1]);
    } else {
        UT_ASSERT_MSG(out[1] != 3 && out[1] == 2,
                      "duplicate slot should be placed unreserved (farthest, 2), got %u",
                      (unsigned)out[1]);
    }
    serverSimDestroy(sim);
    return 0;
}

/* (e) NULL reservedStartIdx0 preserves the original no-reservation path:
 *     the solo slot is placed by the existing farthest-first pass. */
int run_starts_batch_null_reservations_place_normally(void) {
    ServerSim *sim = ut_make_running_sim("NullRes");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    build_starts(gs);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE out[MAX_TANKS];
    reset_inputs(connected, team, reserved);
    connected[0] = true;

    startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, NULL);

    UT_ASSERT_MSG(out[0] != MAX_STARTS && out[0] < K_NUM_STARTS,
                  "NULL reservations should place the slot normally, got %u",
                  (unsigned)out[0]);
    serverSimDestroy(sim);
    return 0;
}

/* (f) A lone solo with nothing else claimed must not always pick the
 *     lowest-index start. Step 5's farthest-first pass ties every valid
 *     start when nothing is claimed yet; choosing randomly among the ties
 *     is the single-player fix. Re-seeding and re-running should spread the
 *     placement across several starts. */
int run_starts_batch_solo_random_seed(void) {
    ServerSim *sim = ut_make_running_sim("SoloRand");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    build_starts(gs);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE out[MAX_TANKS];
    reset_inputs(connected, team, reserved);
    connected[0] = true;

    bool seen[K_NUM_STARTS] = {false};
    int distinct = 0;
    int s;
    for (s = 0; s < 64; s++) {
        bolo_srand((uint64_t)(s + 1));
        startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, NULL);
        UT_ASSERT_MSG(out[0] < K_NUM_STARTS,
                      "solo should be placed, got %u", (unsigned)out[0]);
        if (!seen[out[0]]) { seen[out[0]] = true; distinct++; }
    }
    UT_ASSERT_MSG(distinct >= 2,
                  "lone solo placement should vary across seeds, saw %d distinct",
                  distinct);
    serverSimDestroy(sim);
    return 0;
}

/* Six starts: a left cluster (x<=60) and a right cluster (x>=190), all on
 * y=100. The map's own pills/bases are cleared so only this layout drives
 * placement; two base-less teams then fall to the stripe grid, which splits
 * the wide bbox along X — one cluster per team. */
static void build_two_clusters(GameSim *gs) {
    static const BYTE cx[6] = {40, 50, 60, 190, 200, 210};
    int i;
    gs->pb->numPills = 0;   /* no pills near the synthetic starts */
    gs->bs->numBases = 0;   /* no owned/neutral bases to steer anchors */
    for (i = 0; i < 6; i++) {
        mapSetPos(gs, &gs->mp, cx[i], 100, DEEP_SEA, FALSE, TRUE);
        gs->ss->item[i].x = cx[i];
        gs->ss->item[i].y = 100;
        gs->ss->item[i].dir = 0;
    }
    startsSetNumStarts(&gs->ss, 6);
}

/* (g) Two base-less teams split across the map: each team's members cluster
 *     on one side, and the two teams land on opposite sides. */
int run_starts_batch_teams_cluster_and_separate(void) {
    ServerSim *sim = ut_make_running_sim("Teams");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    build_two_clusters(gs);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE out[MAX_TANKS];
    int i;
    int xa0;
    int xa1;
    int xb0;
    reset_inputs(connected, team, reserved);
    connected[0] = connected[1] = connected[2] = connected[3] = true;
    team[0] = team[1] = 1;   /* team 1: slots 0,1 */
    team[2] = team[3] = 2;   /* team 2: slots 2,3 */

    bolo_srand(12345);
    startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, NULL);

    for (i = 0; i < 4; i++) {
        UT_ASSERT_MSG(out[i] < 6, "slot %d unplaced (%u)", i, (unsigned)out[i]);
    }
    xa0 = gs->ss->item[out[0]].x;
    xa1 = gs->ss->item[out[1]].x;
    xb0 = gs->ss->item[out[2]].x;
    /* Each team's two members sit on the same side of the x=125 midline... */
    UT_ASSERT_MSG((xa0 < 125) == (xa1 < 125),
                  "team 1 split across sides: %d,%d", xa0, xa1);
    UT_ASSERT_MSG((gs->ss->item[out[2]].x < 125) == (gs->ss->item[out[3]].x < 125),
                  "team 2 split across sides: %d,%d",
                  gs->ss->item[out[2]].x, gs->ss->item[out[3]].x);
    /* ...and the two teams sit on opposite sides. */
    UT_ASSERT_MSG((xa0 < 125) != (xb0 < 125),
                  "teams landed on the same side: a=%d b=%d", xa0, xb0);
    serverSimDestroy(sim);
    return 0;
}

/* (h) The unanchored-team anchor is jittered, so the exact pair of starts a
 *     team claims within its cluster varies across seeds. Without the jitter
 *     the anchor is the fixed cluster centroid and the pair is constant. */
int run_starts_batch_team_anchor_jitter_varies(void) {
    ServerSim *sim = ut_make_running_sim("Jitter");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    build_two_clusters(gs);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE out[MAX_TANKS];
    int seenPair[16];
    int nSeen = 0;
    int s;
    reset_inputs(connected, team, reserved);
    connected[0] = connected[1] = connected[2] = connected[3] = true;
    team[0] = team[1] = 1;
    team[2] = team[3] = 2;

    for (s = 0; s < 64; s++) {
        int lo;
        int hi;
        int key;
        int j;
        int found = 0;
        bolo_srand((uint64_t)(s * 7 + 1));
        startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, NULL);
        lo = out[0] < out[1] ? out[0] : out[1];
        hi = out[0] < out[1] ? out[1] : out[0];
        key = lo * 100 + hi;
        for (j = 0; j < nSeen; j++) {
            if (seenPair[j] == key) { found = 1; break; }
        }
        if (!found && nSeen < 16) { seenPair[nSeen++] = key; }
    }
    UT_ASSERT_MSG(nSeen >= 2,
                  "team 1's claimed pair should vary across seeds, saw %d", nSeen);
    serverSimDestroy(sim);
    return 0;
}
