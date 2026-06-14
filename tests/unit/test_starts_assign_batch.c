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
 *   (d) duplicate reservations honor the first, place the rest normally;
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
    gs->ss->numStarts = K_NUM_STARTS;
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

    startsAssignBatch(gs, &gs->ss, connected, team, out, reserved);

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

    startsAssignBatch(gs, &gs->ss, connected, team, out, reserved);

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

    startsAssignBatch(gs, &gs->ss, connected, team, out, reserved);

    UT_ASSERT_MSG(out[0] != MAX_STARTS && out[0] < K_NUM_STARTS,
                  "stale reservation should fall through to placement, got %u",
                  (unsigned)out[0]);
    serverSimDestroy(sim);
    return 0;
}

/* (d) Duplicate reservations: the first slot keeps the start, the second
 *     is placed as unreserved. */
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

    startsAssignBatch(gs, &gs->ss, connected, team, out, reserved);

    UT_ASSERT_MSG(out[0] == 3,
                  "first slot should keep reserved start 3, got %u",
                  (unsigned)out[0]);
    UT_ASSERT_MSG(out[1] != 3 && out[1] == 2,
                  "duplicate slot should be placed unreserved (farthest, 2), got %u",
                  (unsigned)out[1]);
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

    startsAssignBatch(gs, &gs->ss, connected, team, out, NULL);

    UT_ASSERT_MSG(out[0] != MAX_STARTS && out[0] < K_NUM_STARTS,
                  "NULL reservations should place the slot normally, got %u",
                  (unsigned)out[0]);
    serverSimDestroy(sim);
    return 0;
}
