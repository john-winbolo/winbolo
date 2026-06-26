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
 *   2. basesBaseInStockRange's neutral/allied + distance filter (the per-base
 *      predicate the stock cull gates on).
 *
 *   3. serverSimBuildSnapshot's full-sync base block sends each human recipient
 *      real stock for every neutral/allied base within send range of its tank
 *      and zeroes ammo for out-of-range and enemy bases — owner is kept for all
 *      — so enemy / out-of-range stock never leaks across the wire.
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
    UT_ASSERT_MSG(basesGetClosestForPlayer(gs, 0, aCx, aCy, BASE_STATUS_RANGE) == (BYTE)(a + 1),
                  "player 0 at base a should see base a (%u), got %u",
                  (BYTE)(a + 1), basesGetClosestForPlayer(gs, 0, aCx, aCy, BASE_STATUS_RANGE));
    UT_ASSERT_MSG(basesGetClosestForPlayer(gs, 1, bCx, bCy, BASE_STATUS_RANGE) == (BYTE)(b + 1),
                  "player 1 at base b should see base b (%u), got %u",
                  (BYTE)(b + 1), basesGetClosestForPlayer(gs, 1, bCx, bCy, BASE_STATUS_RANGE));

    /* Enemy exclusion: every base owned by player 1 → invisible to player 0. */
    {
        BYTE n = basesGetNumBases(&gs->bs), i;
        for (i = 0; i < n; i++) {
            (*gs->bs).item[i].owner = 1;
        }
    }
    UT_ASSERT_MSG(basesGetClosestForPlayer(gs, 0, aCx, aCy, BASE_STATUS_RANGE) == BASE_NOT_FOUND,
                  "all-enemy bases must be invisible to player 0, got %u",
                  basesGetClosestForPlayer(gs, 0, aCx, aCy, BASE_STATUS_RANGE));

    /* Inclusion after flip: base a turns neutral (others stay enemy). */
    (*gs->bs).item[a].owner = NEUTRAL;
    UT_ASSERT_MSG(basesGetClosestForPlayer(gs, 0, aCx, aCy, BASE_STATUS_RANGE) == (BYTE)(a + 1),
                  "flipping base a neutral should re-include it, got %u",
                  basesGetClosestForPlayer(gs, 0, aCx, aCy, BASE_STATUS_RANGE));

    /* Out of range: only base a is neutral; a point ≥8 squares from base a
     * (16 squares here, well past BASE_STATUS_RANGE's 7) sees nothing. The
     * shift direction is chosen to stay within the [0,255] map. */
    {
        int aMapX = (*gs->bs).item[a].x;
        int farMapX = (aMapX >= 16) ? (aMapX - 16) : (aMapX + 16);
        WORLD farX = bv_base_world((BYTE)farMapX);
        WORLD farY = aCy;
        UT_ASSERT_MSG(basesGetClosestForPlayer(gs, 0, farX, farY, BASE_STATUS_RANGE) == BASE_NOT_FOUND,
                      "base a should be out of range from the shifted point, got %u",
                      basesGetClosestForPlayer(gs, 0, farX, farY, BASE_STATUS_RANGE));
    }

    /* Asymmetric send margin: a neutral base sitting just past
     * BASE_STATUS_RANGE (here 1920 world units ≈ 7.5 map squares from base a)
     * is rejected by the bare display range but selected by the widened server
     * send ceiling (BASE_STATUS_RANGE + 256 = 2048). This pins that the margin
     * reveals a base's stock at a radius the client's own display range would
     * not yet switch to. Base a is the only neutral base here (every other base
     * is still enemy-owned), so it is the sole candidate. Offset along X only
     * (gapY = 0) so the Euclidean distance equals the offset exactly; the sign
     * is chosen to keep the probe point inside the WORLD range. */
    {
        const WORLD marginProbe = 1920; /* 1792 <= d < 2048 */
        WORLD nearX = (aCx >= marginProbe) ? (WORLD)(aCx - marginProbe)
                                           : (WORLD)(aCx + marginProbe);
        UT_ASSERT_MSG(basesGetClosestForPlayer(gs, 0, nearX, aCy, BASE_STATUS_RANGE) == BASE_NOT_FOUND,
                      "base a just past BASE_STATUS_RANGE must be rejected by the bare range, got %u",
                      basesGetClosestForPlayer(gs, 0, nearX, aCy, BASE_STATUS_RANGE));
        UT_ASSERT_MSG(basesGetClosestForPlayer(gs, 0, nearX, aCy, (WORLD)(BASE_STATUS_RANGE + 256)) == (BYTE)(a + 1),
                      "base a within BASE_STATUS_RANGE+256 must be selected by the widened range (%u), got %u",
                      (BYTE)(a + 1),
                      basesGetClosestForPlayer(gs, 0, nearX, aCy, (WORLD)(BASE_STATUS_RANGE + 256)));
    }

    serverSimDestroy(sim);
    return 0;
}

/* 2. basesBaseInStockRange predicate: a neutral base within range is true; the
 *    same base evaluated from a far point is false; an enemy base in range is
 *    false (only neutral/allied bases reveal stock). */
int run_bases_base_in_stock_range(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) >= 1,
                  "Everard map has no bases (%u)", basesGetNumBases(&gs->bs));
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1) != TRUE,
                  "players 0 and 1 allied by default — breaks the enemy case");

    const BYTE a = 0;
    WORLD aCx = bv_base_world((*gs->bs).item[a].x);
    WORLD aCy = bv_base_world((*gs->bs).item[a].y);

    /* Neutral base, tank on its centre → in range. */
    (*gs->bs).item[a].owner = NEUTRAL;
    UT_ASSERT_MSG(basesBaseInStockRange(gs, 0, aCx, aCy, a, BASE_STATUS_RANGE) == true,
                  "neutral base a at d=0 should be in stock range");

    /* Same neutral base, evaluated from a point 16 squares away → out of range.
     * Shift along X only (gapY = 0) and pick the sign that stays in [0,255]. */
    {
        int aMapX = (*gs->bs).item[a].x;
        int farMapX = (aMapX >= 16) ? (aMapX - 16) : (aMapX + 16);
        WORLD farX = bv_base_world((BYTE)farMapX);
        UT_ASSERT_MSG(basesBaseInStockRange(gs, 0, farX, aCy, a, BASE_STATUS_RANGE) == false,
                      "neutral base a 16 squares away should be out of stock range");
    }

    /* Enemy base in range → false (stock is only revealed for neutral/allied). */
    (*gs->bs).item[a].owner = 1;
    UT_ASSERT_MSG(basesBaseInStockRange(gs, 0, aCx, aCy, a, BASE_STATUS_RANGE) == false,
                  "enemy base a in range must not be in stock range");

    serverSimDestroy(sim);
    return 0;
}

/* 3. Full-sync per-recipient stock cull, in-range rule: a neutral/allied base
 *    within send range of the recipient keeps real stock; an out-of-range
 *    neutral base and an in-range enemy base have ammo zeroed; owner is kept
 *    for all. */
int run_base_stock_visibility(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) >= 2,
                  "Everard map has < 2 bases (%u) — test needs two",
                  basesGetNumBases(&gs->bs));
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid for positioning");
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1) != TRUE,
                  "players 0 and 1 allied by default — breaks the enemy case");

    const BYTE a = 0, b = 1;

    /* Base a stays put (recipient sits on it → in range); base b is relocated
     * 16 squares from base a so it is clearly out of range. Distinct nonzero
     * stock so a leak is unmistakable. Shift along X only, sign kept in
     * [0,255]. */
    int aMapX = (*gs->bs).item[a].x;
    int aMapY = (*gs->bs).item[a].y;
    int farMapX = (aMapX >= 16) ? (aMapX - 16) : (aMapX + 16);

    (*gs->bs).item[a].owner  = NEUTRAL;
    (*gs->bs).item[a].armour = 20;
    (*gs->bs).item[a].shells = 30;
    (*gs->bs).item[a].mines  = 40;
    (*gs->bs).item[b].x      = (BYTE)farMapX;
    (*gs->bs).item[b].y      = (BYTE)aMapY;
    (*gs->bs).item[b].owner  = NEUTRAL;
    (*gs->bs).item[b].armour = 21;
    (*gs->bs).item[b].shells = 31;
    (*gs->bs).item[b].mines  = 41;

    WORLD aCx = bv_base_world((*gs->bs).item[a].x);
    WORLD aCy = bv_base_world((*gs->bs).item[a].y);
    tankSetWorld(gs, &gs->tanks[0], aCx, aCy, 0, false);  /* recipient 0 → on base a */

    SnapshotHeader hdr;
    TankSnapshot tk[MAX_TANKS];
    ShellSnapshot sh[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot te[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot bo[MAX_SNAPSHOT_BASES];
    PillSnapshot po[MAX_SNAPSHOT_PILLS];
    GameEvent ev[MAX_SNAPSHOT_EVENTS];

    /* In-range neutral base a → real stock; out-of-range neutral base b → ammo
     * zeroed, armour + owner kept. Reset lastFullSyncTick to force a full sync. */
    memset(sim->lastFullSyncTick, 0, sizeof(sim->lastFullSyncTick));
    serverSimBuildSnapshot(sim, 0, &hdr, tk, MAX_TANKS, sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS, bo, MAX_SNAPSHOT_BASES,
                           po, MAX_SNAPSHOT_PILLS, ev, MAX_SNAPSHOT_EVENTS, false);
    UT_ASSERT_MSG(hdr.baseCount >= 2, "expected a full-sync base block, got %u",
                  hdr.baseCount);
    UT_ASSERT_MSG(bo[a].armour == 20 && bo[a].shells == 30 && bo[a].mines == 40,
                  "recipient 0: in-range neutral base a must keep real stock (%u/%u/%u)",
                  bo[a].armour, bo[a].shells, bo[a].mines);
    UT_ASSERT_MSG(bo[b].armour == 21 && bo[b].shells == 0 && bo[b].mines == 0,
                  "recipient 0: out-of-range neutral base b — armour kept, ammo culled (%u/%u/%u)",
                  bo[b].armour, bo[b].shells, bo[b].mines);
    UT_ASSERT_MSG(bo[a].owner == NEUTRAL && bo[b].owner == NEUTRAL,
                  "recipient 0: owners must be kept for all bases (a=%u b=%u)",
                  bo[a].owner, bo[b].owner);

    /* Now move base b in range (1 square from base a) but make it enemy: even
     * in range its ammo stays zeroed (only neutral/allied reveal stock), its
     * live armour is masked, and its owner is kept. Base a unchanged → still
     * real stock. */
    {
        int nearMapX = (aMapX >= 1) ? (aMapX - 1) : (aMapX + 1);
        (*gs->bs).item[b].x      = (BYTE)nearMapX;
        (*gs->bs).item[b].y      = (BYTE)aMapY;
        (*gs->bs).item[b].owner  = 1;   /* enemy, alive */
        (*gs->bs).item[b].armour = 21;
        (*gs->bs).item[b].shells = 31;
        (*gs->bs).item[b].mines  = 41;
    }
    memset(sim->lastFullSyncTick, 0, sizeof(sim->lastFullSyncTick));
    serverSimBuildSnapshot(sim, 0, &hdr, tk, MAX_TANKS, sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS, bo, MAX_SNAPSHOT_BASES,
                           po, MAX_SNAPSHOT_PILLS, ev, MAX_SNAPSHOT_EVENTS, false);
    UT_ASSERT_MSG(hdr.baseCount >= 2, "expected a full-sync base block, got %u",
                  hdr.baseCount);
    UT_ASSERT_MSG(bo[a].armour == 20 && bo[a].shells == 30 && bo[a].mines == 40,
                  "recipient 0: in-range neutral base a stock wrong on 2nd build (%u/%u/%u)",
                  bo[a].armour, bo[a].shells, bo[a].mines);
    UT_ASSERT_MSG(bo[b].shells == 0 && bo[b].mines == 0,
                  "recipient 0: in-range enemy base b ammo must be zeroed (%u/%u)",
                  bo[b].shells, bo[b].mines);
    UT_ASSERT_MSG(bo[b].armour == BASE_FULL_ARMOUR,
                  "recipient 0: in-range live enemy base b armour must be masked (%u)",
                  bo[b].armour);
    UT_ASSERT_MSG(bo[a].owner == NEUTRAL && bo[b].owner == 1,
                  "recipient 0: owners must be kept (a=%u b=%u)",
                  bo[a].owner, bo[b].owner);

    serverSimDestroy(sim);
    return 0;
}

/* 4. Enemy/own armour fog-of-war in the full-sync: an enemy base reads
 *    BASE_FULL_ARMOUR while alive (exact value hidden) and its true armour
 *    once dead; a friendly (own) base always reads true armour. */
int run_base_armour_fog_of_war(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) >= 2,
                  "Everard map has < 2 bases (%u)", basesGetNumBases(&gs->bs));
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid for positioning");
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1) != TRUE,
                  "players 0 and 1 allied by default — breaks the enemy case");

    const BYTE a = 0, b = 1;
    WORLD aCx = bv_base_world((*gs->bs).item[a].x);
    WORLD aCy = bv_base_world((*gs->bs).item[a].y);
    (*gs->bs).item[a].owner = NEUTRAL;
    tankSetWorld(gs, &gs->tanks[0], aCx, aCy, 0, false);

    SnapshotHeader hdr;
    TankSnapshot tk[MAX_TANKS];
    ShellSnapshot sh[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot te[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot bo[MAX_SNAPSHOT_BASES];
    PillSnapshot po[MAX_SNAPSHOT_PILLS];
    GameEvent ev[MAX_SNAPSHOT_EVENTS];

    #define BV_BUILD0() do { memset(sim->lastFullSyncTick, 0, sizeof(sim->lastFullSyncTick)); \
        serverSimBuildSnapshot(sim, 0, &hdr, tk, MAX_TANKS, sh, MAX_SNAPSHOT_SHELLS, \
            te, MAX_SNAPSHOT_TK_EXPLOSIONS, bo, MAX_SNAPSHOT_BASES, \
            po, MAX_SNAPSHOT_PILLS, ev, MAX_SNAPSHOT_EVENTS, false); } while (0)

    (*gs->bs).item[b].owner  = 1;   /* enemy, alive */
    (*gs->bs).item[b].armour = 50;
    BV_BUILD0();
    UT_ASSERT_MSG(bo[b].armour == BASE_FULL_ARMOUR,
                  "enemy alive base should read BASE_FULL_ARMOUR (%u), got %u",
                  (unsigned)BASE_FULL_ARMOUR, bo[b].armour);

    (*gs->bs).item[b].owner  = 1;   /* enemy, dead/capturable */
    (*gs->bs).item[b].armour = 5;
    BV_BUILD0();
    UT_ASSERT_MSG(bo[b].armour == 5,
                  "enemy dead base should read its true value (5), got %u", bo[b].armour);

    (*gs->bs).item[b].owner  = 0;   /* own base */
    (*gs->bs).item[b].armour = 50;
    BV_BUILD0();
    UT_ASSERT_MSG(bo[b].armour == 50,
                  "own base should read its true value (50), got %u", bo[b].armour);

    #undef BV_BUILD0
    serverSimDestroy(sim);
    return 0;
}

/* 5. Per-client full-sync clock: two recipients each receive their own full
 *    base sync on the same tick. The full-sync cadence is tracked per client
 *    in lastFullSyncTick[MAX_TANKS]; before that it was a single shared scalar
 *    that the first client built each tick set, so a second client built on
 *    the same tick saw the clock already advanced and got baseCount == 0 — its
 *    off-screen base/pill/map state then only refreshed when it happened to be
 *    the first build of an interval. */
int run_two_clients_full_sync_independent(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) >= 2,
                  "Everard map has < 2 bases (%u) — test needs two",
                  basesGetNumBases(&gs->bs));

    SnapshotHeader hdr;
    TankSnapshot tk[MAX_TANKS];
    ShellSnapshot sh[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot te[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot bo[MAX_SNAPSHOT_BASES];
    PillSnapshot po[MAX_SNAPSHOT_PILLS];
    GameEvent ev[MAX_SNAPSHOT_EVENTS];

    /* Reset the clock once, then build for both clients on the same tick. The
     * tick must be nonzero: at tick 0 the clock's zero-value special case fires
     * a full sync for everyone and masks the shared-scalar bug. */
    memset(sim->lastFullSyncTick, 0, sizeof(sim->lastFullSyncTick));
    sim->tick = 100;

    serverSimBuildSnapshot(sim, 0, &hdr, tk, MAX_TANKS, sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS, bo, MAX_SNAPSHOT_BASES,
                           po, MAX_SNAPSHOT_PILLS, ev, MAX_SNAPSHOT_EVENTS, false);
    UT_ASSERT_MSG(hdr.baseCount >= 2,
                  "client 0 should get a full-sync base block, got %u", hdr.baseCount);

    serverSimBuildSnapshot(sim, 1, &hdr, tk, MAX_TANKS, sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS, bo, MAX_SNAPSHOT_BASES,
                           po, MAX_SNAPSHOT_PILLS, ev, MAX_SNAPSHOT_EVENTS, false);
    UT_ASSERT_MSG(hdr.baseCount >= 2,
                  "client 1 must get its own full-sync base block on the same tick "
                  "(pre-fix this was 0 from the shared full-sync clock), got %u",
                  hdr.baseCount);

    serverSimDestroy(sim);
    return 0;
}
