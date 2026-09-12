/*
 * The per-simulation rules table: its defaults, its range checks, and the
 * copies that used to hold the same numbers.
 *
 *   sim_rules_classic_defaults — every field of simRulesClassic() against the
 *     constant it replaced, named one at a time. This is the case that catches
 *     a default typed wrong or a field left at zero, and it is deliberately
 *     exhaustive rather than a sample: a wrong default changes how the classic
 *     game plays and nothing else here would notice. Fields whose use sites
 *     still read their constant are checked the same way, so the table cannot
 *     drift from the engine ahead of its conversion.
 *
 *   sim_rules_validate_ranges — simRulesValidate accepts the classic table,
 *     and for each field this change converted, refuses a value one past each
 *     end of its range with a reason that names the field, while accepting a
 *     value inside it.
 *
 *   sim_rules_copies_follow — a display copy and a brain copy report a rule
 *     that has been changed away from its classic value, rather than the
 *     number they used to hold.
 *
 *   sim_rules_terrain_caps_follow — mapGetSpeed and mapGetTurnRate return the
 *     terrain rules this sim runs on, and moving one terrain's rule leaves the
 *     others where they were.
 *
 *   sim_rules_river_cap_moves_drowning — the river cap is also the wading
 *     test, so a tank driving a raised river at a speed the classic cap would
 *     never have allowed still loses stock to the water.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "gametype.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "game_sim.h"
#include "tank.h"
#include "bolo_map.h"
#include "shells.h"
#include "lgm.h"
#include "pillbox.h"
#include "bases.h"
#include "building.h"
#include "rubble.h"
#include "grass.h"
#include "swamp.h"
#include "minesexp.h"
#include "treegrow.h"
#include "sim_rules.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "brain.h"
#include "brain_data.h"
#include "loopback_harness.h"
#include "test_harness.h"

/* ---- defaults ----------------------------------------------------------- */

#define SR_EQ(field, want)                                                   \
    UT_ASSERT_MSG((long) r.field == (long) (want),                           \
                  "rules." #field " defaults to %ld, not " #want " (%ld)",   \
                  (long) r.field, (long) (want))

/* Floats compare exactly on purpose: every default is a value the constant
 * spells out, so an exact match is the only right answer and a tolerance
 * would hide a default that was retyped. */
#define SR_FEQ(field, want)                                                  \
    UT_ASSERT_MSG((double) r.field == (double) (want),                       \
                  "rules." #field " defaults to %g, not " #want " (%g)",     \
                  (double) r.field, (double) (want))

int run_sim_rules_classic_defaults(void) {
    SimRules r;

    memset(&r, 0xEE, sizeof(r));   /* so a field the table skips is not 0 by luck */
    simRulesClassic(&r);

    /* Tank */
    SR_EQ(tank_reload_ticks, TANK_RELOAD_TIME);
    SR_EQ(tank_full_shells, TANK_FULL_SHELLS);
    SR_EQ(tank_full_mines, TANK_FULL_MINES);
    SR_EQ(tank_full_trees, TANK_FULL_TREES);
    SR_EQ(tank_full_armour, TANK_FULL_ARMOUR);
    SR_EQ(tank_death_ticks, TANK_DEATH_WAIT);
    SR_EQ(tank_water_ticks, TANK_WATER_TIME);
    SR_EQ(shell_damage, DAMAGE);
    SR_EQ(mine_damage, MINE_DAMAGE);
    SR_EQ(just_fired_ticks, JUST_FIRED_TICKS);
    SR_EQ(gunsight_min, GUNSIGHT_MIN);
    SR_EQ(gunsight_max, GUNSIGHT_MAX);
    SR_FEQ(tank_accel_rate, TANK_ACCELERATE_RATE);
    SR_FEQ(tank_decel_rate, TANK_TERRAIN_DECEL_RATE);
    SR_FEQ(tank_brake_rate, TANK_SLOWKEY_RATE);
    SR_FEQ(tank_autoslow_rate, TANK_AUTOSLOW_SPEED);
    SR_EQ(tank_min_move, TANK_MIN_MOVE_SPEED);

    /* Terrain speed caps */
    SR_EQ(speed_road, MAP_SPEED_TROAD);
    SR_EQ(speed_grass, MAP_SPEED_TGRASS);
    SR_EQ(speed_forest, MAP_SPEED_TFOREST);
    SR_EQ(speed_river, MAP_SPEED_TRIVER);
    SR_EQ(speed_swamp, MAP_SPEED_TSWAMP);
    SR_EQ(speed_crater, MAP_SPEED_TCRATER);
    SR_EQ(speed_rubble, MAP_SPEED_TRUBBLE);
    SR_EQ(speed_boat, MAP_SPEED_TBOAT);
    SR_EQ(speed_deep_sea, MAP_SPEED_TDEEPSEA);
    SR_EQ(speed_refuel_base, MAP_SPEED_TREFBASE);

    /* Terrain turn rates */
    SR_FEQ(turn_road, MAP_TURN_TROAD);
    SR_FEQ(turn_grass, MAP_TURN_TGRASS);
    SR_FEQ(turn_forest, MAP_TURN_TFOREST);
    SR_FEQ(turn_river, MAP_TURN_TRIVER);
    SR_FEQ(turn_swamp, MAP_TURN_TSWAMP);
    SR_FEQ(turn_crater, MAP_TURN_TCRATER);
    SR_FEQ(turn_rubble, MAP_TURN_TRUBBLE);
    SR_FEQ(turn_boat, MAP_TURN_TBOAT);
    SR_FEQ(turn_deep_sea, MAP_TURN_TDEEPSEA);
    SR_FEQ(turn_refuel_base, MAP_TURN_TREFBASE);

    /* Shells. shell_start_add is the one field with no constant to check
     * against: shells.h declares SHELL_START_ADD as 6 and both users override
     * it to 5 with their own #define (shells.c and client_sim.c), so 5 is what
     * a shell is actually fired with and the header's 6 is read by nothing. */
    SR_EQ(shell_life, SHELL_LIFE);
    SR_EQ(shell_speed, SHELL_SPEED);
    SR_EQ(shell_start_add, 5);

    /* Builder */
    SR_EQ(lgm_build_ticks, LGM_BUILD_TIME);
    SR_EQ(lgm_cost_road, LGM_COST_ROAD);
    SR_EQ(lgm_cost_building, LGM_COST_BUILDING);
    SR_EQ(lgm_cost_repair_building, LGM_COST_REPAIRBUILDING);
    SR_EQ(lgm_cost_pill_repair, LGM_COST_PILLREPAIR);
    SR_EQ(lgm_cost_boat, LGM_COST_BOAT);
    SR_EQ(lgm_cost_pill_new, LGM_COST_PILLNEW);
    SR_EQ(lgm_cost_mine, LGM_COST_MINE);
    SR_EQ(lgm_pill_repair_load, LGM_LOAD_PILLREPAIR);
    SR_EQ(lgm_gather_trees, LGM_GATHER_TREE);
    SR_EQ(lgm_helicopter_speed, LGM_HELICOPTER_SPEED);

    /* Pillbox */
    SR_EQ(pill_max_armour, PILLS_MAX_ARMOUR);
    SR_EQ(pill_attack_ticks, PILLBOX_ATTACK_NORMAL);
    SR_EQ(pill_attack_min_ticks, PILLBOX_MAX_FIRERATE);
    SR_EQ(pill_cooldown_ticks, PILLBOX_COOLDOWN_TIME);
    SR_EQ(pill_repair_amount, PILL_REPAIR_AMOUNT);
    SR_EQ(pill_range, PILLBOX_RANGE);

    /* Base */
    SR_EQ(base_full_armour, BASE_FULL_ARMOUR);
    SR_EQ(base_full_shells, BASE_FULL_SHELLS);
    SR_EQ(base_full_mines, BASE_FULL_MINES);
    SR_EQ(base_capture_armour, MIN_ARMOUR_CAPTURE);
    SR_EQ(base_hit_armour, BASE_MIN_CAN_HIT);
    SR_EQ(base_min_armour, BASE_MIN_ARMOUR);
    SR_EQ(base_min_shells, BASE_MIN_SHELLS);
    SR_EQ(base_min_mines, BASE_MIN_MINES);
    SR_EQ(base_armour_give, BASE_ARMOUR_GIVE);
    SR_EQ(base_shells_give, BASE_SHELLS_GIVE);
    SR_EQ(base_mines_give, BASE_MINES_GIVE);
    SR_EQ(base_refuel_armour_ticks, BASE_REFUEL_ARMOUR);
    SR_FEQ(base_refuel_shells_ticks, BASE_REFUEL_SHELLS);
    SR_FEQ(base_refuel_mines_ticks, BASE_REFUEL_MINES);
    SR_EQ(base_regen_ticks, BASE_TICKS_BETWEEN_REFUEL);

    /* Terrain destruction and explosions */
    SR_EQ(building_life, BUILDING_LIFE);
    SR_EQ(rubble_life, RUBBLE_LIFE);
    SR_EQ(grass_life, GRASS_LIFE);
    SR_EQ(swamp_life, SWAMP_LIFE);
    SR_EQ(mine_fuse_ticks, MINES_EXPLOSION_WAIT);
    SR_EQ(big_explosion_threshold, TANK_BIG_EXPLOSION_THRESHOLD);

    /* Tree growth */
    SR_EQ(tree_grow_ticks, TREEGROW_TIME);
    SR_EQ(tree_grow_initial_ticks, TREEGROW_INITIAL_TIME);
    SR_EQ(tree_weight_forest, TREE_GROW_FOREST);
    SR_EQ(tree_weight_grass, TREE_GROW_GRASS);
    SR_EQ(tree_weight_river, TREE_GROW_RIVER);
    SR_EQ(tree_weight_boat, TREE_GROW_BOAT);
    SR_EQ(tree_weight_deep_sea, TREE_GROW_DEEP_SEA);
    SR_EQ(tree_weight_swamp, TREE_GROW_DEEP_SWAMP);
    SR_EQ(tree_weight_rubble, TREE_GROW_DEEP_RUBBLE);
    SR_EQ(tree_weight_building, TREE_GROW_BUILDING);
    SR_EQ(tree_weight_half_building, TREE_GROW_HALF_BUILDING);
    SR_EQ(tree_weight_crater, TREE_GROW_CRATER);
    SR_EQ(tree_weight_road, TREE_GROW_ROAD);
    SR_EQ(tree_weight_mine, TREE_GROW_MINE);

    return 0;
}

/* ---- ranges ------------------------------------------------------------- */

/* One field's range, exercised from a fresh classic table each time so a
 * refusal is always about the field under test. */
#define SR_RANGE_INT(field, lo, hi)                                          \
    do {                                                                     \
        SimRules t;                                                          \
        char why[SIM_RULES_WHY_LEN];                                         \
        simRulesClassic(&t);                                                 \
        t.field = (int32_t) (lo) - 1;                                        \
        why[0] = '\0';                                                       \
        UT_ASSERT_MSG(!simRulesValidate(&t, why, sizeof why),                \
                      #field " below its range was accepted");               \
        UT_ASSERT_MSG(strstr(why, #field) != NULL,                           \
                      #field " below its range gave the reason \"%s\"", why);\
        simRulesClassic(&t);                                                 \
        t.field = (int32_t) (hi) + 1;                                        \
        why[0] = '\0';                                                       \
        UT_ASSERT_MSG(!simRulesValidate(&t, why, sizeof why),                \
                      #field " above its range was accepted");               \
        UT_ASSERT_MSG(strstr(why, #field) != NULL,                           \
                      #field " above its range gave the reason \"%s\"", why);\
        simRulesClassic(&t);                                                 \
        t.field = (int32_t) (lo);                                            \
        UT_ASSERT_MSG(simRulesValidate(&t, why, sizeof why),                 \
                      #field " at its low bound was refused: %s", why);      \
        t.field = (int32_t) (hi);                                            \
        UT_ASSERT_MSG(simRulesValidate(&t, why, sizeof why),                 \
                      #field " at its high bound was refused: %s", why);     \
    } while (0)

#define SR_RANGE_FLT(field, lo, hi, step)                                    \
    do {                                                                     \
        SimRules t;                                                          \
        char why[SIM_RULES_WHY_LEN];                                         \
        simRulesClassic(&t);                                                 \
        t.field = (float) (lo) - (float) (step);                             \
        why[0] = '\0';                                                       \
        UT_ASSERT_MSG(!simRulesValidate(&t, why, sizeof why),                \
                      #field " below its range was accepted");               \
        UT_ASSERT_MSG(strstr(why, #field) != NULL,                           \
                      #field " below its range gave the reason \"%s\"", why);\
        simRulesClassic(&t);                                                 \
        t.field = (float) (hi) + (float) (step);                             \
        why[0] = '\0';                                                       \
        UT_ASSERT_MSG(!simRulesValidate(&t, why, sizeof why),                \
                      #field " above its range was accepted");               \
        UT_ASSERT_MSG(strstr(why, #field) != NULL,                           \
                      #field " above its range gave the reason \"%s\"", why);\
        simRulesClassic(&t);                                                 \
        t.field = (float) (lo);                                              \
        UT_ASSERT_MSG(simRulesValidate(&t, why, sizeof why),                 \
                      #field " at its low bound was refused: %s", why);      \
        t.field = (float) (hi);                                              \
        UT_ASSERT_MSG(simRulesValidate(&t, why, sizeof why),                 \
                      #field " at its high bound was refused: %s", why);     \
    } while (0)

int run_sim_rules_validate_ranges(void) {
    SimRules r;
    char why[SIM_RULES_WHY_LEN];

    /* The classic table passes its own checks. Stated outright because a
     * range or an invariant written down wrong is caught here and nowhere
     * else — every other case starts from this table. */
    simRulesClassic(&r);
    why[0] = '\0';
    UT_ASSERT_MSG(simRulesValidate(&r, why, sizeof why),
                  "the classic table failed its own checks: %s", why);

    /* One row per field this change converted. */
    SR_RANGE_INT(tank_reload_ticks, 0, 255);
    SR_RANGE_INT(tank_full_shells, 0, 255);
    SR_RANGE_INT(tank_full_mines, 0, 255);
    SR_RANGE_INT(tank_full_trees, 0, 255);
    SR_RANGE_INT(tank_full_armour, 0, 255);
    SR_RANGE_INT(tank_death_ticks, 0, 65535);
    SR_RANGE_INT(tank_water_ticks, 1, 255);
    SR_RANGE_INT(mine_damage, 1, 255);
    SR_RANGE_INT(just_fired_ticks, 0, 255);
    SR_RANGE_INT(tank_min_move, 0, 255);
    SR_RANGE_FLT(tank_accel_rate, 0.01, 16.0, 0.005);
    SR_RANGE_FLT(tank_decel_rate, 0.01, 16.0, 0.005);
    SR_RANGE_FLT(tank_brake_rate, 0.01, 16.0, 0.005);
    SR_RANGE_FLT(tank_autoslow_rate, 0.01, 16.0, 0.005);

    /* A NULL table is refused rather than read. */
    why[0] = '\0';
    UT_ASSERT_MSG(!simRulesValidate(NULL, why, sizeof why),
                  "a NULL table was accepted");

    /* A caller that wants no reason gets the same answer. */
    simRulesClassic(&r);
    r.tank_full_shells = 256;
    UT_ASSERT_MSG(!simRulesValidate(&r, NULL, 0),
                  "the reason buffer changed the answer");

    return 0;
}

/* ---- the copies --------------------------------------------------------- */

/* Set a rule away from its classic value and ask the two surfaces that used
 * to hold their own copy of it what they now report. The HUD reads the T1
 * accessor because src/gui/ cannot see the sim; a brain reads the view
 * brain_data.c fills each tick. */
int run_sim_rules_copies_follow(void) {
    LoopbackHarness h;
    BrainInfo bi;
    BYTE fullShells = 0, fullMines = 0, fullArmour = 0, fullTrees = 0;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Rules", false, NULL, 4321),
                  "loopback start failed");
    loopbackHarnessPumpUntil(&h, 40, NULL, NULL);
    UT_ASSERT_MSG(h.cs != NULL, "the harness produced no client");

    /* Classic first, so the change below is what moves the readings. */
    clientSimGetTankFullStats(h.cs, &fullShells, &fullMines, &fullArmour,
                              &fullTrees);
    UT_ASSERT_MSG(fullShells == TANK_FULL_SHELLS && fullMines == TANK_FULL_MINES &&
                      fullArmour == TANK_FULL_ARMOUR && fullTrees == TANK_FULL_TREES,
                  "a fresh client reports caps %u/%u/%u/%u, not the classic ones",
                  (unsigned) fullShells, (unsigned) fullMines,
                  (unsigned) fullArmour, (unsigned) fullTrees);

    /* Values nothing else in the engine holds, so a reading that matches
     * cannot have come from a literal that happens to agree. */
    h.cs->sim.rules.tank_full_shells = 77;
    h.cs->sim.rules.tank_reload_ticks = 29;
    h.cs->sim.rules.tank_death_ticks = 900;
    h.cs->sim.rules.speed_forest = 9;
    h.cs->sim.rules.turn_forest = 0.125f;

    /* The display copy, through the accessor the HUD bars read. */
    clientSimGetTankFullStats(h.cs, &fullShells, &fullMines, &fullArmour,
                              &fullTrees);
    UT_ASSERT_MSG(fullShells == 77,
                  "the HUD accessor reports a shell cap of %u, not the rule's 77",
                  (unsigned) fullShells);
    UT_ASSERT_MSG(fullMines == TANK_FULL_MINES,
                  "changing one cap moved another: mines reads %u",
                  (unsigned) fullMines);

    /* The brain copy, through the view brain_data.c fills. */
    memset(&bi, 0, sizeof(bi));
    brainDataMakeInfo(h.cs, &bi, true, aiNone);
    UT_ASSERT_MSG(bi.rules.tank_full_shells == 77,
                  "the brain view reports a shell cap of %ld, not the rule's 77",
                  (long) bi.rules.tank_full_shells);
    UT_ASSERT_MSG(bi.rules.tank_reload_ticks == 29,
                  "the brain view reports a reload of %ld, not the rule's 29",
                  (long) bi.rules.tank_reload_ticks);
    UT_ASSERT_MSG(bi.rules.tank_death_ticks == 900,
                  "the brain view reports a death wait of %ld, not the rule's 900",
                  (long) bi.rules.tank_death_ticks);
    UT_ASSERT_MSG(bi.rules.tank_full_mines == TANK_FULL_MINES,
                  "the brain view moved a rule nobody changed: mines reads %ld",
                  (long) bi.rules.tank_full_mines);
    UT_ASSERT_MSG(bi.rules.speed_forest == 9,
                  "the brain view reports a forest cap of %ld, not the rule's 9",
                  (long) bi.rules.speed_forest);
    UT_ASSERT_MSG(bi.rules.turn_forest == 0.125f,
                  "the brain view reports a forest turn rate of %g, not the "
                  "rule's 0.125", (double) bi.rules.turn_forest);
    UT_ASSERT_MSG(bi.rules.speed_road == MAP_SPEED_TROAD &&
                      bi.rules.turn_road == (float) MAP_TURN_TROAD,
                  "the brain view moved a terrain nobody changed: road reads "
                  "%ld at %g", (long) bi.rules.speed_road,
                  (double) bi.rules.turn_road);

    /* brainDataMakeInfo allocates the variable-length members; this case only
     * reads them, so it frees them the way the headless loggers do. */
    free(bi.allies);
    free(bi.base);
    free(bi.pillview);
    free(bi.viewdata);
    free(bi.events);
    if (bi.message != NULL) {
        free(bi.message->receivers);
        free(bi.message->message);
        free(bi.message);
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* ---- the terrain rules at their use sites ------------------------------- */

/* Paint a patch of one terrain around a square, so what the engine reads
 * there is what this case put there and a tank that drifts off the middle
 * square is still on the same ground. */
static void paintTerrain(ServerSim *sim, BYTE terrain, BYTE mx, BYTE my,
                         int radius) {
    int dx, dy;
    for (dy = -radius; dy <= radius; dy++) {
        for (dx = -radius; dx <= radius; dx++) {
            mapSetPos(&sim->sim, &sim->sim.mp, (BYTE) (mx + dx),
                      (BYTE) (my + dy), terrain, FALSE, FALSE);
        }
    }
}

static BYTE speedAt(ServerSim *sim, BYTE mx, BYTE my) {
    return mapGetSpeed(&sim->sim, &sim->sim.mp, &sim->sim.pb, &sim->sim.bs,
                       mx, my, FALSE, 0);
}

static TURNTYPE turnAt(ServerSim *sim, BYTE mx, BYTE my) {
    return mapGetTurnRate(&sim->sim, &sim->sim.mp, &sim->sim.pb, &sim->sim.bs,
                          mx, my, FALSE, 0);
}

/* The same square is read three times: as classic grass, as grass with both
 * of its rules moved, and as road, which nothing here touched. One square
 * throughout, so a reading cannot be about a base or a pillbox that happens
 * to sit somewhere else on the map. */
int run_sim_rules_terrain_caps_follow(void) {
    ServerSim *sim = ut_make_running_sim("Rules");
    UT_ASSERT(sim != NULL);

    paintTerrain(sim, GRASS, 40, 40, 2);
    UT_ASSERT_MSG(speedAt(sim, 40, 40) == MAP_SPEED_TGRASS,
                  "classic grass caps at %u, not %d",
                  (unsigned) speedAt(sim, 40, 40), MAP_SPEED_TGRASS);
    UT_ASSERT_MSG(turnAt(sim, 40, 40) == (TURNTYPE) MAP_TURN_TGRASS,
                  "classic grass turns at %g, not %g",
                  (double) turnAt(sim, 40, 40), (double) MAP_TURN_TGRASS);

    /* Neither value belongs to any terrain, so a reading that matches can
       only have come from the rule. */
    sim->sim.rules.speed_grass = 9;
    sim->sim.rules.turn_grass = 0.125f;
    UT_ASSERT_MSG(speedAt(sim, 40, 40) == 9,
                  "grass caps at %u, not the rule's 9",
                  (unsigned) speedAt(sim, 40, 40));
    UT_ASSERT_MSG(turnAt(sim, 40, 40) == (TURNTYPE) 0.125f,
                  "grass turns at %g, not the rule's 0.125",
                  (double) turnAt(sim, 40, 40));

    /* The same square as road: a grass rule moved no other terrain. */
    paintTerrain(sim, ROAD, 40, 40, 2);
    UT_ASSERT_MSG(speedAt(sim, 40, 40) == MAP_SPEED_TROAD,
                  "road caps at %u after a grass rule moved, not %d",
                  (unsigned) speedAt(sim, 40, 40), MAP_SPEED_TROAD);
    UT_ASSERT_MSG(turnAt(sim, 40, 40) == (TURNTYPE) MAP_TURN_TROAD,
                  "road turns at %g after a grass rule moved, not %g",
                  (double) turnAt(sim, 40, 40), (double) MAP_TURN_TROAD);

    serverSimDestroy(sim);
    return 0;
}

/* The river cap is also the test for whether a tank is in the water rather
 * than on top of it. Raise the cap and a tank driving faster than the classic
 * 3 is still wading, which is the behaviour that would quietly disappear if
 * the wading test kept reading the constant. */
int run_sim_rules_river_cap_moves_drowning(void) {
    ServerSim *sim = ut_make_running_sim("Rules");
    BYTE shellsBefore = 0, minesBefore = 0, armour = 0, trees = 0;
    BYTE shellsAfter = 0, minesAfter = 0;
    int tick;
    UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL);

    sim->sim.rules.speed_river = 8;
    paintTerrain(sim, RIVER, 40, 40, 2);
    tankSetWorld(&sim->sim, &sim->sim.tanks[0],
                 (WORLD) ((40 << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE),
                 (WORLD) ((40 << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE),
                 (TURNTYPE) 0, FALSE);
    tankSetOnBoat(&sim->sim.tanks[0], FALSE);

    /* Speed 6: faster than the classic river allowed, slower than this one
       does. Autoslowdown off and no button held, so the speed stays put and
       the only thing under test is what the wading check makes of it. A
       residual step nothing can reach holds the tank on the square it
       started on, because crossing tiles is not what this case is about. */
    sim->sim.tanks[0]->autoSlowdown = FALSE;
    sim->sim.rules.tank_min_move = 255;
    tankSetSpeed(&sim->sim.tanks[0], (SPEEDTYPE) 6);

    tankGetStats(&sim->sim.tanks[0], &shellsBefore, &minesBefore, &armour,
                 &trees);
    UT_ASSERT_MSG(shellsBefore > 1 && minesBefore > 1,
                  "the tank spawned with %u shells and %u mines, too few to "
                  "watch the water take any",
                  (unsigned) shellsBefore, (unsigned) minesBefore);

    /* Two full drain intervals, so the case does not turn on which tick the
       count started from. */
    for (tick = 0; tick < 2 * TANK_WATER_TIME; tick++) {
        tankUpdate(&sim->sim, &sim->sim.tanks[0], TNONE, FALSE, FALSE);
    }

    UT_ASSERT_MSG(tankGetActualSpeed(&sim->sim.tanks[0]) >
                      (SPEEDTYPE) MAP_SPEED_TRIVER,
                  "the tank slowed to %g, so the drain below proves nothing "
                  "about the raised cap",
                  (double) tankGetActualSpeed(&sim->sim.tanks[0]));
    tankGetStats(&sim->sim.tanks[0], &shellsAfter, &minesAfter, &armour,
                 &trees);
    UT_ASSERT_MSG(shellsAfter < shellsBefore && minesAfter < minesBefore,
                  "a tank wading a raised river kept %u shells and %u mines "
                  "of its %u and %u",
                  (unsigned) shellsAfter, (unsigned) minesAfter,
                  (unsigned) shellsBefore, (unsigned) minesBefore);

    serverSimDestroy(sim);
    return 0;
}
