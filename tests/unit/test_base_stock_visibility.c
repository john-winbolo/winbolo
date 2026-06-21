/*
 * Base-visibility integration tests (the per-recipient stock cull).
 *
 * Two off-line CODE-VERIFIABLE pins for plans/network-optimization.md §2
 * ("Bases"):
 *
 *   1. basesGetClosestForPlayer returns the correct neutral/allied-in-range
 *      base per player, excludes enemy bases (BASE_NOT_FOUND), re-includes a
 *      base the moment it flips neutral, and drops a base that is out of
 *      BASE_STATUS_RANGE.
 *
 *   2. serverSimBuildSnapshot's full-sync base block sends each human
 *      recipient real stock only for *its own* closest base and zeroes every
 *      other base's stock — owner is kept for all — so enemy / non-closest
 *      stock never leaks across the wire.
 *
 * The bot-exemption arm of the same cull is covered by the baseline.ds_*
 * tests and is not re-tested here.
 *
 * Both tests drive ut_make_running_sim (slot 0) plus a second human at slot 1
 * and read base / snapshot state directly off the GameSim and ServerSim
 * structs (the unittests profile permits T2-internal access). The cull reads
 * the recipient's tank position via serverSimGetTankState, so each test
 * positions the tanks on the base centres it wants to be "closest".
 */

#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* lastFullSyncTick direct access */
#include "game_sim.h"              /* GameSim.bs / .tanks / .plyrs */
#include "bases.h"                 /* basesGetClosestForPlayer, BASE_NOT_FOUND */
#include "tank.h"                  /* tankSetWorld */
#include "players.h"               /* playersIsAllie */
#include "input_packet.h"
#include "test_harness.h"

/* A base's world centre, the same formula basesGetClosestForPlayer uses. */
static WORLD bv_base_world(BYTE mapCoord) {
    return (WORLD)(((int)mapCoord << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
}

/* 1. Per-player closest, enemy exclusion, inclusion-after-flip, out-of-range. */
int run_bases_closest_for_player(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) >= 2,
                  "Everard map has < 2 bases (%u) — test needs two",
                  basesGetNumBases(&gs->bs));
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[1] != NULL,
                  "slot-0/slot-1 tanks not both valid for positioning");

    const BYTE a = 0, b = 1;
    WORLD aCx = bv_base_world((*gs->bs).item[a].x);
    WORLD aCy = bv_base_world((*gs->bs).item[a].y);
    WORLD bCx = bv_base_world((*gs->bs).item[b].x);
    WORLD bCy = bv_base_world((*gs->bs).item[b].y);

    /* Players 0 and 1 must start un-allied for the enemy-exclusion arm. */
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1) != TRUE,
                  "players 0 and 1 are allied by default — breaks exclusion setup");

    /* Per-player closest: both bases neutral, each tank on its own base. */
    (*gs->bs).item[a].owner = NEUTRAL;
    (*gs->bs).item[b].owner = NEUTRAL;
    tankSetWorld(gs, &gs->tanks[0], aCx, aCy, 0, false);
    tankSetWorld(gs, &gs->tanks[1], bCx, bCy, 0, false);
    UT_ASSERT_MSG(basesGetClosestForPlayer(gs, 0, aCx, aCy) == (BYTE)(a + 1),
                  "player 0 at base a should see base a (%u), got %u",
                  (BYTE)(a + 1), basesGetClosestForPlayer(gs, 0, aCx, aCy));
    UT_ASSERT_MSG(basesGetClosestForPlayer(gs, 1, bCx, bCy) == (BYTE)(b + 1),
                  "player 1 at base b should see base b (%u), got %u",
                  (BYTE)(b + 1), basesGetClosestForPlayer(gs, 1, bCx, bCy));

    /* Enemy exclusion: every base owned by player 1 → invisible to player 0. */
    {
        BYTE n = basesGetNumBases(&gs->bs), i;
        for (i = 0; i < n; i++) {
            (*gs->bs).item[i].owner = 1;
        }
    }
    UT_ASSERT_MSG(basesGetClosestForPlayer(gs, 0, aCx, aCy) == BASE_NOT_FOUND,
                  "all-enemy bases must be invisible to player 0, got %u",
                  basesGetClosestForPlayer(gs, 0, aCx, aCy));

    /* Inclusion after flip: base a turns neutral (others stay enemy). */
    (*gs->bs).item[a].owner = NEUTRAL;
    UT_ASSERT_MSG(basesGetClosestForPlayer(gs, 0, aCx, aCy) == (BYTE)(a + 1),
                  "flipping base a neutral should re-include it, got %u",
                  basesGetClosestForPlayer(gs, 0, aCx, aCy));

    /* Out of range: only base a is neutral; a point ≥8 squares from base a
     * (16 squares here, well past BASE_STATUS_RANGE's 7) sees nothing. The
     * shift direction is chosen to stay within the [0,255] map. */
    {
        int aMapX = (*gs->bs).item[a].x;
        int farMapX = (aMapX >= 16) ? (aMapX - 16) : (aMapX + 16);
        WORLD farX = bv_base_world((BYTE)farMapX);
        WORLD farY = aCy;
        UT_ASSERT_MSG(basesGetClosestForPlayer(gs, 0, farX, farY) == BASE_NOT_FOUND,
                      "base a should be out of range from the shifted point, got %u",
                      basesGetClosestForPlayer(gs, 0, farX, farY));
    }

    serverSimDestroy(sim);
    return 0;
}

/* 2. Full-sync per-recipient stock cull: each recipient sees real stock only
 *    for its own closest base, zeroed stock for every other base, and owner
 *    preserved for all. */
int run_base_stock_visibility(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) >= 2,
                  "Everard map has < 2 bases (%u) — test needs two",
                  basesGetNumBases(&gs->bs));
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[1] != NULL,
                  "slot-0/slot-1 tanks not both valid for positioning");

    const BYTE a = 0, b = 1;

    /* Both neutral, distinct nonzero stock so a leak is unmistakable. */
    (*gs->bs).item[a].owner  = NEUTRAL;
    (*gs->bs).item[a].armour = 20;
    (*gs->bs).item[a].shells = 30;
    (*gs->bs).item[a].mines  = 40;
    (*gs->bs).item[b].owner  = NEUTRAL;
    (*gs->bs).item[b].armour = 21;
    (*gs->bs).item[b].shells = 31;
    (*gs->bs).item[b].mines  = 41;

    WORLD aCx = bv_base_world((*gs->bs).item[a].x);
    WORLD aCy = bv_base_world((*gs->bs).item[a].y);
    WORLD bCx = bv_base_world((*gs->bs).item[b].x);
    WORLD bCy = bv_base_world((*gs->bs).item[b].y);
    tankSetWorld(gs, &gs->tanks[0], aCx, aCy, 0, false);  /* recipient 0 → base a */
    tankSetWorld(gs, &gs->tanks[1], bCx, bCy, 0, false);  /* recipient 1 → base b */

    SnapshotHeader hdr;
    TankSnapshot tk[MAX_TANKS];
    ShellSnapshot sh[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot te[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot bo[MAX_SNAPSHOT_BASES];
    PillSnapshot po[MAX_SNAPSHOT_PILLS];
    GameEvent ev[MAX_SNAPSHOT_EVENTS];

    /* Recipient 0: base a is closest → real stock; base b zeroed; owners kept.
     * Reset lastFullSyncTick so this build forces a full base sync. */
    sim->lastFullSyncTick = 0;
    serverSimBuildSnapshot(sim, 0, &hdr, tk, MAX_TANKS, sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS, bo, MAX_SNAPSHOT_BASES,
                           po, MAX_SNAPSHOT_PILLS, ev, MAX_SNAPSHOT_EVENTS, false);
    UT_ASSERT_MSG(hdr.baseCount >= 2, "expected a full-sync base block, got %u",
                  hdr.baseCount);
    UT_ASSERT_MSG(bo[a].armour == 20 && bo[a].shells == 30 && bo[a].mines == 40,
                  "recipient 0: closest base a stock wrong (%u/%u/%u)",
                  bo[a].armour, bo[a].shells, bo[a].mines);
    UT_ASSERT_MSG(bo[b].armour == 0 && bo[b].shells == 0 && bo[b].mines == 0,
                  "recipient 0: non-closest base b stock leaked (%u/%u/%u)",
                  bo[b].armour, bo[b].shells, bo[b].mines);
    UT_ASSERT_MSG(bo[a].owner == NEUTRAL && bo[b].owner == NEUTRAL,
                  "recipient 0: owners must be kept for all bases (a=%u b=%u)",
                  bo[a].owner, bo[b].owner);

    /* Recipient 1: base b is closest → real stock; base a zeroed; owners kept. */
    sim->lastFullSyncTick = 0;
    serverSimBuildSnapshot(sim, 1, &hdr, tk, MAX_TANKS, sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS, bo, MAX_SNAPSHOT_BASES,
                           po, MAX_SNAPSHOT_PILLS, ev, MAX_SNAPSHOT_EVENTS, false);
    UT_ASSERT_MSG(hdr.baseCount >= 2, "expected a full-sync base block, got %u",
                  hdr.baseCount);
    UT_ASSERT_MSG(bo[b].armour == 21 && bo[b].shells == 31 && bo[b].mines == 41,
                  "recipient 1: closest base b stock wrong (%u/%u/%u)",
                  bo[b].armour, bo[b].shells, bo[b].mines);
    UT_ASSERT_MSG(bo[a].armour == 0 && bo[a].shells == 0 && bo[a].mines == 0,
                  "recipient 1: non-closest base a stock leaked (%u/%u/%u)",
                  bo[a].armour, bo[a].shells, bo[a].mines);
    UT_ASSERT_MSG(bo[a].owner == NEUTRAL && bo[b].owner == NEUTRAL,
                  "recipient 1: owners must be kept for all bases (a=%u b=%u)",
                  bo[a].owner, bo[b].owner);

    serverSimDestroy(sim);
    return 0;
}
