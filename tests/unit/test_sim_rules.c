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
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "gametype.h"
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

    /* Three values nothing else in the engine holds, so a reading that matches
     * cannot have come from a literal that happens to agree. */
    h.cs->sim.rules.tank_full_shells = 77;
    h.cs->sim.rules.tank_reload_ticks = 29;
    h.cs->sim.rules.tank_death_ticks = 900;

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
