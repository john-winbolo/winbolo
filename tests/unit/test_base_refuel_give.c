/*
 * A base refuel moves the same amount off the base as onto the tank
 * (test_base_refuel_give.c).
 *
 * basesRefueling took a whole base_*_give off the base and then asked
 * tankAdd* to put the same amount on the tank. tankAdd* never adds past the
 * tank's full amount, so when the tank was less than a give short of full it
 * refused the lot: the base lost a give every refuel interval and the tank
 * stayed where it was until the base hit its minimum. The classic rules hide
 * this for a tank that is a multiple of five short, since 40 divides by 5,
 * but a modified rule set with a give that does not divide the full amount
 * shows it on every refuel, and a tank at 37 shows it even at classic rules.
 *
 * The cases:
 *   base_refuel_partial_give_armour — give 8, tank at 35 of 40: the tank
 *     reaches 40 and the base loses 5, then a full tank draws nothing more.
 *   base_refuel_partial_give_shells_and_mines — the same for a shell give
 *     of 3 with the tank at 39, and a mine give of 3 with the tank at 38.
 *   base_refuel_partial_give_classic_rules — classic give of 5 with the
 *     tank at 37: it reaches 40 and the base loses 3.
 *   base_refuel_whole_give_unchanged — give 8, tank at 20: a whole give
 *     still moves, so the usual refuel is not slowed.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "game_sim.h"
#include "bases.h"
#include "tank.h"
#include "server_sim.h"
#include "test_harness.h"

/* Base 1 stocked to the full, owned by the tank in slot 0 and ready to refuel
 * this tick. */
static void stage_base(GameSim *gs) {
    gs->bs->item[0].owner = 0;
    gs->bs->item[0].armour = (BYTE) gs->rules.base_full_armour;
    gs->bs->item[0].shells = (BYTE) gs->rules.base_full_shells;
    gs->bs->item[0].mines = (BYTE) gs->rules.base_full_mines;
    gs->bs->item[0].refuelTime = 0;
}

/* The tank in slot 0, with every stock at full so only the one a case lowers
 * is short. */
static tank *stage_tank(GameSim *gs) {
    tank *tnk = &gs->tanks[0];
    if (*tnk == NULL) return NULL;
    tankSetArmour(tnk, (BYTE) gs->rules.tank_full_armour);
    tankSetShells(gs, tnk, (BYTE) gs->rules.tank_full_shells);
    tankSetMines(gs, tnk, (BYTE) gs->rules.tank_full_mines);
    return tnk;
}

int run_base_refuel_partial_give_armour(void) {
    ServerSim *sim = ut_make_running_sim("Refueller");
    GameSim *gs;
    tank *tnk;
    BYTE baseBefore;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) >= 1,
                  "Everard Island should carry at least one base");

    gs->rules.base_armour_give = 8;
    stage_base(gs);
    tnk = stage_tank(gs);
    UT_ASSERT_MSG(tnk != NULL, "slot 0 has no tank to refuel");
    tankSetArmour(tnk, 35);
    baseBefore = gs->bs->item[0].armour;

    basesRefueling(gs, tnk, 1);
    UT_ASSERT_MSG(tankGetArmour(tnk) == 40,
                  "tank at 35 with a give of 8 should reach 40, got %u",
                  (unsigned) tankGetArmour(tnk));
    UT_ASSERT_MSG(gs->bs->item[0].armour == baseBefore - 5,
                  "the base should lose the 5 the tank took, lost %u",
                  (unsigned) (baseBefore - gs->bs->item[0].armour));

    /* A full tank draws nothing: the base must not keep paying. */
    gs->bs->item[0].refuelTime = 0;
    baseBefore = gs->bs->item[0].armour;
    basesRefueling(gs, tnk, 1);
    UT_ASSERT_MSG(tankGetArmour(tnk) == 40,
                  "a full tank should stay at 40, got %u",
                  (unsigned) tankGetArmour(tnk));
    UT_ASSERT_MSG(gs->bs->item[0].armour == baseBefore,
                  "a full tank should take nothing, the base lost %u",
                  (unsigned) (baseBefore - gs->bs->item[0].armour));

    serverSimDestroy(sim);
    return 0;
}

int run_base_refuel_partial_give_shells_and_mines(void) {
    ServerSim *sim = ut_make_running_sim("Refueller");
    GameSim *gs;
    tank *tnk;
    BYTE baseBefore;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) >= 1,
                  "Everard Island should carry at least one base");

    gs->rules.base_shells_give = 3;
    gs->rules.base_mines_give = 3;
    stage_base(gs);
    tnk = stage_tank(gs);
    UT_ASSERT_MSG(tnk != NULL, "slot 0 has no tank to refuel");

    /* Shells: 39 of 40 with a give of 3. */
    tankSetShells(gs, tnk, 39);
    baseBefore = gs->bs->item[0].shells;
    basesRefueling(gs, tnk, 1);
    UT_ASSERT_MSG(tankGetShells(tnk) == 40,
                  "tank at 39 shells with a give of 3 should reach 40, got %u",
                  (unsigned) tankGetShells(tnk));
    UT_ASSERT_MSG(gs->bs->item[0].shells == baseBefore - 1,
                  "the base should lose the 1 shell the tank took, lost %u",
                  (unsigned) (baseBefore - gs->bs->item[0].shells));

    /* Mines: 38 of 40 with a give of 3. Shells are full now, so the refuel
       falls through to mines. */
    gs->bs->item[0].refuelTime = 0;
    tankSetMines(gs, tnk, 38);
    baseBefore = gs->bs->item[0].mines;
    basesRefueling(gs, tnk, 1);
    UT_ASSERT_MSG(tankGetMines(tnk) == 40,
                  "tank at 38 mines with a give of 3 should reach 40, got %u",
                  (unsigned) tankGetMines(tnk));
    UT_ASSERT_MSG(gs->bs->item[0].mines == baseBefore - 2,
                  "the base should lose the 2 mines the tank took, lost %u",
                  (unsigned) (baseBefore - gs->bs->item[0].mines));

    serverSimDestroy(sim);
    return 0;
}

int run_base_refuel_partial_give_classic_rules(void) {
    ServerSim *sim = ut_make_running_sim("Refueller");
    GameSim *gs;
    tank *tnk;
    BYTE baseBefore;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT_MSG(gs->rules.base_armour_give == 5 && gs->rules.tank_full_armour == 40,
                  "this case is about the classic give of 5 into 40");

    stage_base(gs);
    tnk = stage_tank(gs);
    UT_ASSERT_MSG(tnk != NULL, "slot 0 has no tank to refuel");
    tankSetArmour(tnk, 37);
    baseBefore = gs->bs->item[0].armour;

    basesRefueling(gs, tnk, 1);
    UT_ASSERT_MSG(tankGetArmour(tnk) == 40,
                  "tank at 37 with the classic give of 5 should reach 40, got %u",
                  (unsigned) tankGetArmour(tnk));
    UT_ASSERT_MSG(gs->bs->item[0].armour == baseBefore - 3,
                  "the base should lose the 3 the tank took, lost %u",
                  (unsigned) (baseBefore - gs->bs->item[0].armour));

    serverSimDestroy(sim);
    return 0;
}

int run_base_refuel_whole_give_unchanged(void) {
    ServerSim *sim = ut_make_running_sim("Refueller");
    GameSim *gs;
    tank *tnk;
    BYTE baseBefore;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    gs->rules.base_armour_give = 8;
    stage_base(gs);
    tnk = stage_tank(gs);
    UT_ASSERT_MSG(tnk != NULL, "slot 0 has no tank to refuel");
    tankSetArmour(tnk, 20);
    baseBefore = gs->bs->item[0].armour;

    basesRefueling(gs, tnk, 1);
    UT_ASSERT_MSG(tankGetArmour(tnk) == 28,
                  "tank at 20 with a give of 8 should reach 28, got %u",
                  (unsigned) tankGetArmour(tnk));
    UT_ASSERT_MSG(gs->bs->item[0].armour == baseBefore - 8,
                  "the base should lose the whole give of 8, lost %u",
                  (unsigned) (baseBefore - gs->bs->item[0].armour));

    serverSimDestroy(sim);
    return 0;
}
