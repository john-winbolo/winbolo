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
 *     number they used to hold. The tank's caps through the HUD accessor and
 *     the base's through its own, since src/gui/ cannot see the sim.
 *
 *   sim_rules_base_empties_without_wrapping — a base hit for more than it has
 *     left empties rather than wrapping past the cap, so what a shell takes
 *     and what a base may hold are no longer tied together by arithmetic.
 *
 *   sim_rules_pill_empties_without_wrapping — the same for a pill caught by
 *     an explosion, where the old shape could leave a wrapped remainder in
 *     the living range instead of emptying it.
 *
 *   sim_rules_terrain_caps_follow — mapGetSpeed and mapGetTurnRate return the
 *     terrain rules this sim runs on, and moving one terrain's rule leaves the
 *     others where they were.
 *
 *   sim_rules_river_cap_moves_drowning — the river cap is also the wading
 *     test, so a tank driving a raised river at a speed the classic cap would
 *     never have allowed still loses stock to the water.
 *
 *   sim_rules_base_regen_seed_follows — the base regeneration timer is seeded
 *     from its rule, one player's slot at a time.
 *
 *   sim_rules_shell_flight_follows — the per-tick step and the tick budget a
 *     fired shell carries are built from shell_speed and shell_life, read off
 *     the shell record shellsAddItem just made.
 *
 *   sim_rules_brain_shot_follows — the brain's shot simulator, which has no
 *     sim to read, flies the shell rules pushed onto its pathfinder, and a
 *     pathfinder nobody has pushed to flies the classic one.
 *
 *   sim_rules_terrain_life_follows — how many hits a building takes before it
 *     is rubble follows its life rule.
 *
 *   sim_rules_pairs — what one rule allows another. One refusal per pair with
 *     a reason naming both sides, the boundary where the two are equal
 *     accepted, and a check that a pair refuses a table without rewriting it.
 *
 *   sim_rules_capture_threshold_moves — moving base_capture_armour moves the
 *     armour at which a base can be driven onto and taken, and the status the
 *     panel draws moves with it.
 *
 *   sim_rules_builder_cost_follows — what a boat costs the builder is the
 *     rule, so raising it refuses an order the same wood used to buy.
 *
 *   sim_rules_are_classic — a table simRulesClassic filled is reported
 *     classic, and one with a single rule moved is not: an integer rule, a
 *     float rule, the last field in the struct, and a rule the brain's own
 *     view does not carry, which is the one a check written against that
 *     view would wave through.
 *
 *   sim_rules_obs_refuses_non_classic — the observation builder refuses to
 *     build on a sim whose rules are not classic and builds on one whose
 *     rules are, so a model is never handed a vector on a scale it was not
 *     trained against.
 *
 *   sim_rules_worldsim_pill_follows — the forward model's four pill numbers
 *     carry the classic values it was built with rather than constants of its
 *     own, and a clear keeps them the way it keeps the terrain speeds beside
 *     them. brainWorldSimCreate takes no rules and always reads the classic
 *     table, so a model built on a moved pill rule cannot be made from here;
 *     the fields, what they hold, and what a clear does to them are what this
 *     pins.
 *
 *   sim_rules_boat_speed_follows — the five places the pathfinder moves a
 *     tank at boat speed read its terrain speed table like every other speed
 *     it reads, so a boat speed pushed through the setter moves the travel
 *     estimate, the straight-line cost estimate, the searched cost, and both
 *     lookup tables the navigation search builds — the last two being the
 *     ones the search actually expands.
 *
 *   sim_rules_obs_reload_follows — the observation's reload scalar is the
 *     ticks still to wait over the rule that set them, not over a constant
 *     15 that the classic 13 could never reach. The other two builders
 *     already scale it this way. Only the classic case is drivable:
 *     obsBuildMultiView refuses a table that is not classic, so neither a
 *     moved tank_reload_ticks nor the rule of 0 the divide guards against
 *     can be put in front of it from here.
 */

#include <stddef.h>
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
#include "tankexp.h"     /* TK_DAMAGE — the splash the explosion path deals */
#include "floodfill.h"  /* FLOOD_FILL_WAIT */
#include "sounddist.h"  /* SDIST_SOFT / SDIST_NONE */
#include "starts.h"     /* START_* */
#include "treegrow.h"
#include "sim_rules.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "brain.h"
#include "brain_data.h"
#include "brain_pathfinder.h"
#include "brain_worldsim.h"
#include "obs_builder.h"  /* obsBuildMultiView — the refusal a case can drive */
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
    SR_EQ(mine_damage_range, MINE_DAMAGE_RANGE);
    SR_EQ(mine_fatal_divisor, MINE_FATAL_DIVISOR);
    SR_EQ(water_loss_shells, TANK_WATER_LOSS_SHELLS);
    SR_EQ(water_loss_mines, TANK_WATER_LOSS_MINES);
    SR_EQ(just_fired_ticks, JUST_FIRED_TICKS);
    SR_EQ(tree_hide_distance, MIN_TREEHIDE_DIST);
    SR_EQ(gunsight_min, GUNSIGHT_MIN);
    SR_EQ(gunsight_max, GUNSIGHT_MAX);
    SR_FEQ(tank_accel_rate, TANK_ACCELERATE_RATE);
    SR_FEQ(tank_decel_rate, TANK_TERRAIN_DECEL_RATE);
    SR_FEQ(tank_brake_rate, TANK_SLOWKEY_RATE);
    SR_FEQ(tank_autoslow_rate, TANK_AUTOSLOW_SPEED);
    SR_EQ(tank_min_move, TANK_MIN_MOVE_SPEED);

    /* Tank collision geometry */
    SR_EQ(tank_hit_radius, TANK_HIT_RADIUS);
    SR_EQ(tank_collision_distance, TANK_COLLISION_DISTANCE);
    SR_EQ(tank_nudge_threshold, TANK_NUDGE_THRESHOLD);
    SR_EQ(tank_nudge_amount, TANK_NUDGE_AMOUNT);
    SR_EQ(tank_nudge_iterations, TANK_MAX_NUDGE_ITERATIONS);
    SR_EQ(tank_bump_decay_shift, TANK_BUMP_DECAY_SHIFT);
    SR_EQ(tank_pill_pickup_inset, TANK_PILL_PICKUP_INSET);
    SR_EQ(tank_boat_exit_inset, TANK_MOVE_BOAT_SUB);
    SR_EQ(tank_slide_step, TANK_SLIDE);
    SR_FEQ(tank_wall_glide, TANK_WALL_GLIDE);

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

    /* Builder walk speeds */
    SR_EQ(man_speed_road, MAP_MANSPEED_TROAD);
    SR_EQ(man_speed_grass, MAP_MANSPEED_TGRASS);
    SR_EQ(man_speed_forest, MAP_MANSPEED_TFOREST);
    SR_EQ(man_speed_river, MAP_MANSPEED_TRIVER);
    SR_EQ(man_speed_swamp, MAP_MANSPEED_TSWAMP);
    SR_EQ(man_speed_crater, MAP_MANSPEED_TCRATER);
    SR_EQ(man_speed_rubble, MAP_MANSPEED_TRUBBLE);
    SR_EQ(man_speed_boat, MAP_MANSPEED_TBOAT);
    SR_EQ(man_speed_deep_sea, MAP_MANSPEED_TDEEPSEA);
    SR_EQ(man_speed_refuel_base, MAP_MANSPEED_TREFBASE);

    /* Shells */
    SR_EQ(shell_life, SHELL_LIFE);
    SR_EQ(shell_speed, SHELL_SPEED);
    SR_EQ(shell_start_add, SHELL_START_ADD);

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
    SR_EQ(lgm_arrive_tolerance, LGM_MAX_GOAL);
    SR_EQ(lgm_return_tolerance, LGM_RETURN_MAX_GOAL);
    SR_EQ(lgm_pill_drop_search, LGM_PILL_DROP_SEARCH);
    SR_EQ(lgm_boat_leave_offset, LGM_TANKBOAT_LEAVE);
    SR_EQ(lgm_boat_return_offset, LGM_TANKBOAT_RETURN);

    /* Pillbox */
    SR_EQ(pill_max_armour, PILLS_MAX_ARMOUR);
    SR_EQ(pill_attack_ticks, PILLBOX_ATTACK_NORMAL);
    SR_EQ(pill_attack_min_ticks, PILLBOX_MAX_FIRERATE);
    SR_EQ(pill_cooldown_ticks, PILLBOX_COOLDOWN_TIME);
    SR_EQ(pill_repair_amount, PILL_REPAIR_AMOUNT);
    SR_EQ(pill_range, PILLBOX_RANGE);
    SR_EQ(pill_shell_damage, PILLBOX_SHELL_DAMAGE);
    SR_EQ(pill_angry_divisor, PILLBOX_ANGRY_DIVISOR);
    SR_FEQ(pill_fire_length, PILLBOX_FIRE_DISTANCE);
    SR_EQ(pill_base_defend_range, PILL_BASE_HIT_RANGE);
    SR_EQ(pill_aim_iterations, MAX_AIM_ITERATE);

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
    SR_EQ(base_status_range, BASE_STATUS_RANGE);
    SR_EQ(base_reveal_range, BASE_PREDICT_REVEAL_RANGE);

    /* Terrain destruction and explosions */
    SR_EQ(building_life, BUILDING_LIFE);
    SR_EQ(rubble_life, RUBBLE_LIFE);
    SR_EQ(grass_life, GRASS_LIFE);
    SR_EQ(swamp_life, SWAMP_LIFE);
    SR_EQ(mine_fuse_ticks, MINES_EXPLOSION_WAIT);
    SR_EQ(big_explosion_threshold, TANK_BIG_EXPLOSION_THRESHOLD);
    SR_EQ(tank_explosion_damage, TK_DAMAGE);
    SR_EQ(tank_explosion_length, TK_EXPLODE_LENGTH);
    SR_EQ(tank_explosion_move, TK_MOVE_AMOUNT);
    SR_EQ(tank_explosion_update_ticks, TK_UPDATE_TIME);
    SR_EQ(tank_explosion_width, TK_WIDTH_CHECK);
    SR_EQ(tank_explosion_height, TK_HEIGHT_CHECK);

    /* Tree growth */
    SR_EQ(start_tank_range, START_TANK_RANGE);
    SR_EQ(start_pill_range, START_PILL_RANGE);
    SR_EQ(start_base_range, START_BASE_RANGE);
    SR_EQ(start_spawn_separation, START_SPAWN_SEPARATION);
    SR_EQ(start_scatter_max, START_SCATTER_MAX);
    SR_EQ(start_neutral_threshold_pct, START_NEUTRAL_THRESHOLD_PCT);
    SR_EQ(sound_soft_range, SDIST_SOFT);
    SR_EQ(sound_none_range, SDIST_NONE);
    SR_EQ(flood_fill_ticks, FLOOD_FILL_WAIT);
    SR_EQ(tree_grow_ticks, TREEGROW_TIME);
    SR_EQ(tree_grow_initial_ticks, TREEGROW_INITIAL_TIME);
    SR_EQ(tree_grow_initial_score, TREEGROW_INITIAL_SCORE);
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
 * refusal is always about the field under test.
 *
 * loFix and hiFix are the partners that have to move with it. A field's own
 * range is one question and what it allows another field is a different one,
 * so a field that sits inside its range can still break a pair while the
 * other side of that pair is still classic — tank_full_mines at 0 with a mine
 * costing 1 is a sim where no mine can ever be laid, and the pairs block is
 * right to refuse it. The bounds that need company say so here, and the ones
 * that take no fix-up are ranges that stand on their own.
 *
 * Only the acceptance half takes the fix-ups. simRulesValidate checks every
 * range before any pair, so a value one past the end is still refused by the
 * field's own row, naming the field, whatever the partners hold. Each of the
 * four assertions starts from a fresh classic table, so a fix-up made for one
 * bound cannot leak into the other. */
#define SR_RANGE_INT_WITH(field, lo, hi, loFix, hiFix)                       \
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
        loFix;                                                               \
        why[0] = '\0';                                                       \
        UT_ASSERT_MSG(simRulesValidate(&t, why, sizeof why),                 \
                      #field " at its low bound was refused: %s", why);      \
        simRulesClassic(&t);                                                 \
        t.field = (int32_t) (hi);                                            \
        hiFix;                                                               \
        why[0] = '\0';                                                       \
        UT_ASSERT_MSG(simRulesValidate(&t, why, sizeof why),                 \
                      #field " at its high bound was refused: %s", why);     \
    } while (0)

/* A field in no pair: both bounds stand on their own. */
#define SR_RANGE_INT(field, lo, hi)                                          \
    SR_RANGE_INT_WITH(field, lo, hi, (void) 0, (void) 0)

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
    /* A capacity of nothing needs the cost of the thing it carries to be
       nothing too, or the sim is one where that order can never be paid. */
    SR_RANGE_INT_WITH(tank_full_mines, 0, 255,
                      t.lgm_cost_mine = 0,
                      (void) 0);
    SR_RANGE_INT_WITH(tank_full_trees, 0, 255,
                      t.lgm_cost_road = 0;
                      t.lgm_cost_building = 0;
                      t.lgm_cost_repair_building = 0;
                      t.lgm_cost_boat = 0;
                      t.lgm_cost_pill_new = 0;
                      t.lgm_cost_pill_repair = 0,
                      (void) 0);
    SR_RANGE_INT(tank_full_armour, 0, 255);
    SR_RANGE_INT(tank_death_ticks, 0, 65535);
    SR_RANGE_INT(tank_water_ticks, 1, 255);
    SR_RANGE_INT(shell_damage, 1, 255);
    SR_RANGE_INT(mine_damage, 1, 255);
    SR_RANGE_INT(just_fired_ticks, 0, 255);
    SR_RANGE_INT(tank_min_move, 0, 255);
    SR_RANGE_INT(shell_speed, 1, 255);

    /* The gunsight ends and the two shell numbers the budget is built from
       all sit in one arithmetic, so each bound needs the others brought
       along. At the low end the range collapses onto gunsight_min, and the
       start offset has to come down to what one tick of that shortest shot
       can pay for. At the high end the budget has to stay inside the BYTE
       the shell record keeps it in, which pins shell_life down as the
       gunsight opens and pins the gunsight in as shell_life grows. */
    SR_RANGE_INT_WITH(gunsight_min, 1, 255,
                      t.shell_start_add = 0,
                      t.gunsight_max = 255; t.shell_life = 1);
    SR_RANGE_INT_WITH(gunsight_max, 1, 255,
                      t.gunsight_min = 1; t.shell_start_add = 0,
                      t.shell_life = 1; t.shell_start_add = 0);
    SR_RANGE_INT_WITH(shell_life, 1, 255,
                      t.shell_start_add = 0,
                      t.gunsight_max = 1; t.gunsight_min = 1);
    /* shell_start_add has no ceiling of its own — RULE_INT_MIN gives it only
       a floor — so the sweep uses the largest offset any legal table can
       carry, which is what the shortest-shot pair allows at the top of every
       partner's range: shell_life 255 and gunsight_min 255, halved. Sweeping
       to INT32_MAX instead would only re-test the pair, since above this
       number no partners exist that accept it. The one past the end is still
       refused against the classic partners, and by the pair rather than by a
       row of its own — which is the honest answer for a field whose ceiling
       is another field. */
    SR_RANGE_INT_WITH(shell_start_add, 0, 255 * 255 / 2,
                      (void) 0,
                      t.shell_life = 255; t.gunsight_min = 255;
                      t.gunsight_max = 255);
    SR_RANGE_FLT(tank_accel_rate, 0.01, 16.0, 0.005);
    SR_RANGE_FLT(tank_decel_rate, 0.01, 16.0, 0.005);
    SR_RANGE_FLT(tank_brake_rate, 0.01, 16.0, 0.005);
    SR_RANGE_FLT(tank_autoslow_rate, 0.01, 16.0, 0.005);
    /* A tree cannot be worth more than a whole pill, and one load still has
       to finish a pill on nothing — which at the top of the range means a
       tree worth far more than the classic four. */
    SR_RANGE_INT_WITH(pill_max_armour, 1, 255,
                      t.pill_repair_amount = 1,
                      t.pill_repair_amount = 255);
    SR_RANGE_INT(pill_cooldown_ticks, 0, 255);
    /* A base that holds nothing keeps nothing back, hands nothing out, and
       is taken the moment it is touched — and the hit threshold comes down
       with the capture threshold, since it sits under it. */
    SR_RANGE_INT_WITH(base_full_armour, 0, 255,
                      t.base_min_armour = 0; t.base_armour_give = 0;
                      t.base_capture_armour = 0; t.base_hit_armour = 0,
                      (void) 0);
    SR_RANGE_INT_WITH(base_full_shells, 0, 255,
                      t.base_shells_give = 0,
                      (void) 0);
    SR_RANGE_INT_WITH(base_full_mines, 0, 255,
                      t.base_mines_give = 0,
                      (void) 0);

    /* The attack interval is a pair, so neither end goes through the macro
     * above: moving one alone to the far side of the other is refused by the
     * pair check rather than by its own row. Each end against its own range
     * first, with the other end moved to keep the pair legal. */
    simRulesClassic(&r);
    r.pill_attack_ticks = 0;
    why[0] = '\0';
    UT_ASSERT_MSG(!simRulesValidate(&r, why, sizeof why),
                  "pill_attack_ticks below its range was accepted");
    UT_ASSERT_MSG(strstr(why, "pill_attack_ticks") != NULL,
                  "pill_attack_ticks below its range gave \"%s\"", why);
    simRulesClassic(&r);
    r.pill_attack_ticks = 256;
    why[0] = '\0';
    UT_ASSERT_MSG(!simRulesValidate(&r, why, sizeof why),
                  "pill_attack_ticks above its range was accepted");
    UT_ASSERT_MSG(strstr(why, "pill_attack_ticks") != NULL,
                  "pill_attack_ticks above its range gave \"%s\"", why);
    simRulesClassic(&r);
    r.pill_attack_min_ticks = 0;
    why[0] = '\0';
    UT_ASSERT_MSG(!simRulesValidate(&r, why, sizeof why),
                  "pill_attack_min_ticks below its range was accepted");
    UT_ASSERT_MSG(strstr(why, "pill_attack_min_ticks") != NULL,
                  "pill_attack_min_ticks below its range gave \"%s\"", why);
    simRulesClassic(&r);
    r.pill_attack_ticks = 1;
    r.pill_attack_min_ticks = 1;
    why[0] = '\0';
    UT_ASSERT_MSG(simRulesValidate(&r, why, sizeof why),
                  "the pair at its low bound was refused: %s", why);
    simRulesClassic(&r);
    r.pill_attack_ticks = 255;
    why[0] = '\0';
    UT_ASSERT_MSG(simRulesValidate(&r, why, sizeof why),
                  "pill_attack_ticks at its high bound was refused: %s", why);

    /* Then the one against the other. */
    simRulesClassic(&r);
    r.pill_attack_min_ticks = r.pill_attack_ticks + 1;
    why[0] = '\0';
    UT_ASSERT_MSG(!simRulesValidate(&r, why, sizeof why),
                  "a minimum above the normal interval was accepted");
    UT_ASSERT_MSG(strstr(why, "pill_attack_min_ticks") != NULL &&
                      strstr(why, "pill_attack_ticks") != NULL,
                  "the refusal does not name the pair: %s", why);

    /* Equal ends are the tightest legal pair, not a refusal: a sim where a
     * hurt pill fires no faster than a calm one is odd but coherent. */
    simRulesClassic(&r);
    r.pill_attack_min_ticks = r.pill_attack_ticks;
    why[0] = '\0';
    UT_ASSERT_MSG(simRulesValidate(&r, why, sizeof why),
                  "a pair with equal ends was refused: %s", why);

    /* And a pair moved together, both away from classic, is accepted. */
    simRulesClassic(&r);
    r.pill_attack_ticks = 40;
    r.pill_attack_min_ticks = 12;
    why[0] = '\0';
    UT_ASSERT_MSG(simRulesValidate(&r, why, sizeof why),
                  "a valid pair was refused: %s", why);

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
    h.cs->sim.rules.base_full_shells = 61;
    h.cs->sim.rules.pill_max_armour = 23;

    /* The display copy, through the accessor the HUD bars read. */
    clientSimGetTankFullStats(h.cs, &fullShells, &fullMines, &fullArmour,
                              &fullTrees);
    UT_ASSERT_MSG(fullShells == 77,
                  "the HUD accessor reports a shell cap of %u, not the rule's 77",
                  (unsigned) fullShells);
    UT_ASSERT_MSG(fullMines == TANK_FULL_MINES,
                  "changing one cap moved another: mines reads %u",
                  (unsigned) fullMines);

    /* The base bars read their own accessor, for the same reason. */
    {
        BYTE baseShells = 0, baseMines = 0, baseArmour = 0;
        clientSimGetBaseFullStats(h.cs, &baseShells, &baseMines, &baseArmour);
        UT_ASSERT_MSG(baseShells == 61,
                      "the base accessor reports a shell cap of %u, not the "
                      "rule's 61", (unsigned) baseShells);
        UT_ASSERT_MSG(baseMines == BASE_FULL_MINES &&
                          baseArmour == BASE_FULL_ARMOUR,
                      "changing one base cap moved another: %u mines, %u armour",
                      (unsigned) baseMines, (unsigned) baseArmour);
    }

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
    UT_ASSERT_MSG(bi.rules.base_full_shells == 61,
                  "the brain view reports a base shell cap of %ld, not the "
                  "rule's 61", (long) bi.rules.base_full_shells);
    UT_ASSERT_MSG(bi.rules.pill_max_armour == 23,
                  "the brain view reports a pill armour cap of %ld, not the "
                  "rule's 23", (long) bi.rules.pill_max_armour);
    UT_ASSERT_MSG(bi.rules.base_full_mines == BASE_FULL_MINES,
                  "the brain view moved a rule nobody changed: base mines "
                  "reads %ld", (long) bi.rules.base_full_mines);
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

/* ---- the timings and counts -------------------------------------------- */

/* The timer a base's regeneration runs on is seeded from the rule wherever it
 * is set, so a sim that was given a different one counts from that. */
int run_sim_rules_base_regen_seed_follows(void) {
    ServerSim *sim = ut_make_running_sim("Rules");
    UT_ASSERT(sim != NULL);

    /* Classic first, so the change below is what moves the reading. */
    basesUpdateTimer(&sim->sim, 0);
    UT_ASSERT_MSG(sim->sim.baseTimer[0] == BASE_TICKS_BETWEEN_REFUEL,
                  "a classic sim seeds the base timer at %d, not %d",
                  sim->sim.baseTimer[0], BASE_TICKS_BETWEEN_REFUEL);

    /* A value no other timer in the engine holds. */
    sim->sim.rules.base_regen_ticks = 777;
    basesUpdateTimer(&sim->sim, 0);
    UT_ASSERT_MSG(sim->sim.baseTimer[0] == 777,
                  "the base timer seeded at %d, not the rule's 777",
                  sim->sim.baseTimer[0]);

    /* One player's timer is not every player's. */
    basesUpdateTimer(&sim->sim, 1);
    sim->sim.rules.base_regen_ticks = BASE_TICKS_BETWEEN_REFUEL;
    basesUpdateTimer(&sim->sim, 2);
    UT_ASSERT_MSG(sim->sim.baseTimer[1] == 777 &&
                      sim->sim.baseTimer[2] == BASE_TICKS_BETWEEN_REFUEL,
                  "seeding one slot moved another: slots read %d and %d",
                  sim->sim.baseTimer[1], sim->sim.baseTimer[2]);

    serverSimDestroy(sim);
    return 0;
}

/* How many hits a building takes before it is rubble is the life rule plus
 * the hit that puts it on the list. Counted here through buildingAddItem,
 * which is where a shell landing on a building ends up. */
static int hitsToRubble(ServerSim *sim, BYTE mx, BYTE my) {
    int hits = 0;
    while (hits < 64) {
        BYTE terrain = buildingAddItem(&sim->sim, &sim->sim.blds, mx, my);
        hits++;
        if (terrain == RUBBLE) {
            return hits;
        }
    }
    return -1;
}

int run_sim_rules_terrain_life_follows(void) {
    ServerSim *sim = ut_make_running_sim("Rules");
    int classicHits, shortHits;
    UT_ASSERT(sim != NULL);

    /* Two squares, because the first is spent once it turns to rubble and
       the count is about how long a fresh building lasts. */
    classicHits = hitsToRubble(sim, 40, 40);
    UT_ASSERT_MSG(classicHits == BUILDING_LIFE + 1,
                  "a classic building took %d hits to rubble, expected %d",
                  classicHits, BUILDING_LIFE + 1);

    sim->sim.rules.building_life = 1;
    shortHits = hitsToRubble(sim, 42, 40);
    UT_ASSERT_MSG(shortHits == 2,
                  "a building with a life of 1 took %d hits to rubble, "
                  "expected 2", shortHits);

    /* The other three life rules are read at the same point in their own
       add paths; changing the building's moved none of them. */
    UT_ASSERT_MSG(sim->sim.rules.rubble_life == RUBBLE_LIFE &&
                      sim->sim.rules.grass_life == GRASS_LIFE &&
                      sim->sim.rules.swamp_life == SWAMP_LIFE,
                  "changing the building life moved another terrain's");

    serverSimDestroy(sim);
    return 0;
}

/* ---- the base's armour ---------------------------------------------------
 *
 * A base used to die by subtracting the shell's damage from its BYTE armour
 * and asking whether the result had climbed past the cap — true only because
 * the subtraction had wrapped. That tied the cap and the damage together
 * through arithmetic nobody declared, which a rules table can no longer be
 * trusted to keep true. basesDamagePos now asks whether the shell takes more
 * than is left, so the outcome is the same for every armour a base can hold
 * and the one it used to get wrong — a value above the cap, which the wrap
 * test read as "was full" and emptied — is damaged instead. */
int run_sim_rules_base_empties_without_wrapping(void) {
    ServerSim *sim = ut_make_running_sim("Gunner");
    GameSim *gs;
    base item;
    BYTE bx, by;
    int armour;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) >= 1,
                  "Everard Island should carry at least one base");
    memset(&item, 0, sizeof(item));
    basesGetBase(&gs->bs, &item, 1);
    bx = item.x;
    by = item.y;

    /* Armour to spare: exactly the shell's damage comes off. */
    gs->bs->item[0].armour = (BYTE) (DAMAGE + 3);
    basesDamagePos(gs, bx, by, NEUTRAL);
    UT_ASSERT_MSG(gs->bs->item[0].armour == 3,
                  "a hit with armour to spare left %u, expected 3",
                  (unsigned) gs->bs->item[0].armour);

    /* Less left than the shell takes: empty, not a wrap. */
    for (armour = 1; armour < DAMAGE; armour++) {
        gs->bs->item[0].armour = (BYTE) armour;
        basesDamagePos(gs, bx, by, NEUTRAL);
        UT_ASSERT_MSG(gs->bs->item[0].armour == 0,
                      "a base at %d armour hit for %d read back %u, expected 0",
                      armour, DAMAGE, (unsigned) gs->bs->item[0].armour);
    }

    /* Exactly emptied is zero too, and the base reads as capturable rather
       than as a full one. */
    gs->bs->item[0].armour = (BYTE) DAMAGE;
    basesDamagePos(gs, bx, by, NEUTRAL);
    UT_ASSERT_MSG(gs->bs->item[0].armour == 0,
                  "a base emptied exactly read back %u, expected 0",
                  (unsigned) gs->bs->item[0].armour);
    UT_ASSERT_MSG(baseIsCapturable(gs, &gs->bs, bx, by) == TRUE,
                  "an emptied base did not read as capturable");

    /* The case the wrap test got wrong: armour above the cap is damaged, not
       taken for a base that was full and has just been emptied. */
    gs->bs->item[0].armour = 255;
    basesDamagePos(gs, bx, by, NEUTRAL);
    UT_ASSERT_MSG(gs->bs->item[0].armour == (BYTE) (255 - DAMAGE),
                  "a base above the cap hit for %d read back %u, expected %u",
                  DAMAGE, (unsigned) gs->bs->item[0].armour,
                  (unsigned) (255 - DAMAGE));

    serverSimDestroy(sim);
    return 0;
}

/* The pill's armour, the same way. pillsGetDamagePos is the explosion path,
 * and it used to subtract the splash from a BYTE and ask whether the result
 * had climbed past the armour cap. That answered "yes" only because the
 * subtraction had wrapped, and only while the wrap landed above the cap: a
 * blow big enough wraps back down into the living range, and a raised cap
 * widens that window. It now asks whether the blow takes more than is left. */
int run_sim_rules_pill_empties_without_wrapping(void) {
    ServerSim *sim = ut_make_running_sim("Splasher");
    GameSim *gs;
    pillbox item;
    BYTE px, py;
    int armour;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1,
                  "Everard Island should carry at least one pillbox");
    memset(&item, 0, sizeof(item));
    pillsGetPill(&gs->pb, &item, 1);
    px = item.x;
    py = item.y;

    /* Armour to spare: exactly the splash comes off. */
    gs->pb->item[0].armour = (BYTE) (TK_DAMAGE + 3);
    pillsGetDamagePos(gs, &gs->pb, px, py, TK_DAMAGE);
    UT_ASSERT_MSG(gs->pb->item[0].armour == 3,
                  "a hit with armour to spare left %u, expected 3",
                  (unsigned) gs->pb->item[0].armour);

    /* Less left than the splash takes: empty, not a wrap. */
    for (armour = 1; armour < TK_DAMAGE; armour++) {
        gs->pb->item[0].armour = (BYTE) armour;
        pillsGetDamagePos(gs, &gs->pb, px, py, TK_DAMAGE);
        UT_ASSERT_MSG(gs->pb->item[0].armour == 0,
                      "a pill at %d armour hit for %d read back %u, expected 0",
                      armour, TK_DAMAGE, (unsigned) gs->pb->item[0].armour);
    }

    /* Exactly emptied is zero too, and the pill reads as dead on the ground
       rather than as one still worth shooting. */
    gs->pb->item[0].armour = (BYTE) TK_DAMAGE;
    pillsGetDamagePos(gs, &gs->pb, px, py, TK_DAMAGE);
    UT_ASSERT_MSG(gs->pb->item[0].armour == 0,
                  "a pill emptied exactly read back %u, expected 0",
                  (unsigned) gs->pb->item[0].armour);
    UT_ASSERT_MSG(pillsDeadPos(&gs->pb, px, py) == TRUE,
                  "an emptied pill did not read as dead");

    /* The case the wrap test got wrong: a blow far bigger than the cap wraps
       back down into the living range, so the old shape left the pill alive
       on the hit that should have finished it. */
    gs->pb->item[0].armour = 1;
    pillsGetDamagePos(gs, &gs->pb, px, py, 255);
    UT_ASSERT_MSG(gs->pb->item[0].armour == 0,
                  "a pill at 1 armour hit for 255 read back %u, expected 0",
                  (unsigned) gs->pb->item[0].armour);

    /* An explosion landing on a pill already dead leaves it dead rather than
       raising it, which the same wrap used to do. */
    gs->pb->item[0].armour = 0;
    pillsGetDamagePos(gs, &gs->pb, px, py, 255);
    UT_ASSERT_MSG(gs->pb->item[0].armour == 0,
                  "an explosion on a dead pill left it at %u, expected 0",
                  (unsigned) gs->pb->item[0].armour);

    serverSimDestroy(sim);
    return 0;
}

/* What a shell takes off a pillbox. A shell takes shell_damage off a tank and
 * off a base; against a pill it took one, written as a decrement, so the
 * amount could not be moved without moving what a shell does to everything
 * else. pillsDamagePos now reads its own rule, and reads it the way the base
 * path reads its own — asking whether the blow takes more than is left rather
 * than subtracting into a BYTE and reading the wrap. */
int run_sim_rules_pill_shell_damage_follows(void) {
    ServerSim *sim = ut_make_running_sim("Sapper");
    GameSim *gs;
    pillbox item;
    BYTE px, py;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1,
                  "Everard Island should carry at least one pillbox");
    memset(&item, 0, sizeof(item));
    pillsGetPill(&gs->pb, &item, 1);
    px = item.x;
    py = item.y;

    /* The classic rule is one, which is what the decrement used to do. */
    UT_ASSERT_MSG(gs->rules.pill_shell_damage == PILLBOX_SHELL_DAMAGE,
                  "a running sim starts at pill_shell_damage %ld, expected %d",
                  (long) gs->rules.pill_shell_damage, PILLBOX_SHELL_DAMAGE);
    gs->pb->item[0].armour = 10;
    pillsDamagePos(gs, px, py, TRUE, FALSE, NEUTRAL);
    UT_ASSERT_MSG(gs->pb->item[0].armour == 9,
                  "a classic shell left the pill at %u, expected 9",
                  (unsigned) gs->pb->item[0].armour);

    /* Moved, and the shell takes the rule rather than one. */
    gs->rules.pill_shell_damage = 4;
    gs->pb->item[0].armour = 10;
    pillsDamagePos(gs, px, py, TRUE, FALSE, NEUTRAL);
    UT_ASSERT_MSG(gs->pb->item[0].armour == 6,
                  "a shell taking 4 left the pill at %u, expected 6",
                  (unsigned) gs->pb->item[0].armour);

    /* Less left than the shell takes: empty, not a wrap. */
    gs->pb->item[0].armour = 2;
    pillsDamagePos(gs, px, py, TRUE, FALSE, NEUTRAL);
    UT_ASSERT_MSG(gs->pb->item[0].armour == 0,
                  "a pill at 2 armour hit for 4 read back %u, expected 0",
                  (unsigned) gs->pb->item[0].armour);
    UT_ASSERT_MSG(pillsDeadPos(&gs->pb, px, py) == TRUE,
                  "a pill emptied by a raised shell did not read as dead");

    /* A shell that takes the whole cap is a pill killed by one hit, which the
       range check allows and which the path has to actually carry out. */
    gs->rules.pill_shell_damage = gs->rules.pill_max_armour;
    gs->pb->item[0].armour = (BYTE) gs->rules.pill_max_armour;
    pillsDamagePos(gs, px, py, TRUE, FALSE, NEUTRAL);
    UT_ASSERT_MSG(gs->pb->item[0].armour == 0,
                  "a full pill hit for the whole cap read back %u, expected 0",
                  (unsigned) gs->pb->item[0].armour);

    serverSimDestroy(sim);
    return 0;
}

/* How fast a hurt pillbox angers. The interval starts at pill_attack_ticks and
 * is floored at pill_attack_min_ticks, both rules already; the step between
 * them was a literal halving, so the two ends could be moved and the ramp
 * between them could not. A divisor of one is a pill that never angers, which
 * is why the range check's floor is one rather than two. */
int run_sim_rules_pill_angry_divisor_follows(void) {
    ServerSim *sim = ut_make_running_sim("Stoker");
    GameSim *gs;
    pillbox item;
    BYTE px, py;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1,
                  "Everard Island should carry at least one pillbox");
    memset(&item, 0, sizeof(item));
    pillsGetPill(&gs->pb, &item, 1);
    px = item.x;
    py = item.y;

    UT_ASSERT_MSG(gs->rules.pill_angry_divisor == PILLBOX_ANGRY_DIVISOR,
                  "a running sim starts at pill_angry_divisor %ld, expected %d",
                  (long) gs->rules.pill_angry_divisor, PILLBOX_ANGRY_DIVISOR);

    /* Classic: one hit halves the interval. */
    gs->pb->item[0].armour = 10;
    gs->pb->item[0].speed = (BYTE) gs->rules.pill_attack_ticks;
    pillsDamagePos(gs, px, py, TRUE, FALSE, NEUTRAL);
    UT_ASSERT_MSG(gs->pb->item[0].speed ==
                      (BYTE) (gs->rules.pill_attack_ticks / 2),
                  "a classic hit left the interval at %u, expected %ld",
                  (unsigned) gs->pb->item[0].speed,
                  (long) (gs->rules.pill_attack_ticks / 2));

    /* A divisor of one is a pill that never angers. */
    gs->rules.pill_angry_divisor = 1;
    gs->pb->item[0].armour = 10;
    gs->pb->item[0].speed = (BYTE) gs->rules.pill_attack_ticks;
    pillsDamagePos(gs, px, py, TRUE, FALSE, NEUTRAL);
    UT_ASSERT_MSG(gs->pb->item[0].speed ==
                      (BYTE) gs->rules.pill_attack_ticks,
                  "a divisor of one moved the interval to %u, expected %ld",
                  (unsigned) gs->pb->item[0].speed,
                  (long) gs->rules.pill_attack_ticks);

    /* A divisor past the whole span angers the pill fully on the first shell,
       and the floor is what catches it rather than the division. */
    gs->rules.pill_angry_divisor = 255;
    gs->pb->item[0].armour = 10;
    gs->pb->item[0].speed = (BYTE) gs->rules.pill_attack_ticks;
    pillsDamagePos(gs, px, py, TRUE, FALSE, NEUTRAL);
    UT_ASSERT_MSG(gs->pb->item[0].speed ==
                      (BYTE) gs->rules.pill_attack_min_ticks,
                  "a divisor of 255 left the interval at %u, expected the "
                  "floor %ld",
                  (unsigned) gs->pb->item[0].speed,
                  (long) gs->rules.pill_attack_min_ticks);

    serverSimDestroy(sim);
    return 0;
}

/* The splash a dying tank lays on a pillbox, and the shape of the wreck that
 * carries it. tkExplosionUpdate drove the wreck on TK_MOVE_AMOUNT every
 * TK_UPDATE_TIME ticks and tested a box of TK_WIDTH_CHECK by TK_HEIGHT_CHECK
 * around it; the four splash sites passed TK_DAMAGE. All six are rules now,
 * so a scenario can make a death throw a longer, wider or harder wreck. The
 * splash is checked through pillsGetDamagePos, which is the path the wreck
 * itself calls. */
int run_sim_rules_tank_explosion_follows(void) {
    ServerSim *sim = ut_make_running_sim("Pyre");
    GameSim *gs;
    pillbox item;
    BYTE px, py;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1,
                  "Everard Island should carry at least one pillbox");
    memset(&item, 0, sizeof(item));
    pillsGetPill(&gs->pb, &item, 1);
    px = item.x;
    py = item.y;

    /* The six defaults are the constants the sites used to read. */
    UT_ASSERT_MSG(gs->rules.tank_explosion_damage == TK_DAMAGE,
                  "tank_explosion_damage is %ld, expected %d",
                  (long) gs->rules.tank_explosion_damage, TK_DAMAGE);
    UT_ASSERT_MSG(gs->rules.tank_explosion_length == TK_EXPLODE_LENGTH,
                  "tank_explosion_length is %ld, expected %d",
                  (long) gs->rules.tank_explosion_length, TK_EXPLODE_LENGTH);
    UT_ASSERT_MSG(gs->rules.tank_explosion_move == TK_MOVE_AMOUNT,
                  "tank_explosion_move is %ld, expected %d",
                  (long) gs->rules.tank_explosion_move, TK_MOVE_AMOUNT);
    UT_ASSERT_MSG(gs->rules.tank_explosion_update_ticks == TK_UPDATE_TIME,
                  "tank_explosion_update_ticks is %ld, expected %d",
                  (long) gs->rules.tank_explosion_update_ticks, TK_UPDATE_TIME);
    UT_ASSERT_MSG(gs->rules.tank_explosion_width == TK_WIDTH_CHECK,
                  "tank_explosion_width is %ld, expected %d",
                  (long) gs->rules.tank_explosion_width, TK_WIDTH_CHECK);
    UT_ASSERT_MSG(gs->rules.tank_explosion_height == TK_HEIGHT_CHECK,
                  "tank_explosion_height is %ld, expected %d",
                  (long) gs->rules.tank_explosion_height, TK_HEIGHT_CHECK);

    /* The splash takes the rule rather than the constant. */
    gs->rules.tank_explosion_damage = 2;
    gs->pb->item[0].armour = 10;
    pillsGetDamagePos(gs, &gs->pb, px, py,
                      (BYTE) gs->rules.tank_explosion_damage);
    UT_ASSERT_MSG(gs->pb->item[0].armour == 8,
                  "a splash of 2 left the pill at %u, expected 8",
                  (unsigned) gs->pb->item[0].armour);

    /* Zero is a wreck that scorches nothing, which the range check allows. */
    gs->rules.tank_explosion_damage = 0;
    gs->pb->item[0].armour = 10;
    pillsGetDamagePos(gs, &gs->pb, px, py,
                      (BYTE) gs->rules.tank_explosion_damage);
    UT_ASSERT_MSG(gs->pb->item[0].armour == 10,
                  "a splash of 0 moved the pill to %u, expected 10",
                  (unsigned) gs->pb->item[0].armour);

    serverSimDestroy(sim);
    return 0;
}

/* What the water takes off a wading tank. tank_water_ticks says how often
 * tankInWater runs; what it takes each time was a pair of decrements, so the
 * interval could be moved and the loss could not. Driven through tankInWater
 * directly, which is the call the wading test in tankUpdate makes. */
int run_sim_rules_water_loss_follows(void) {
    ServerSim *sim = ut_make_running_sim("Wader");
    GameSim *gs;
    tank *tnk;
    BYTE shells, mines, armour, trees;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    tnk = &gs->tanks[0];
    UT_ASSERT(tnk != NULL && *tnk != NULL);

    UT_ASSERT_MSG(gs->rules.water_loss_shells == TANK_WATER_LOSS_SHELLS &&
                      gs->rules.water_loss_mines == TANK_WATER_LOSS_MINES,
                  "a running sim starts at %ld/%ld, expected %d/%d",
                  (long) gs->rules.water_loss_shells,
                  (long) gs->rules.water_loss_mines,
                  TANK_WATER_LOSS_SHELLS, TANK_WATER_LOSS_MINES);

    /* Classic: one of each. */
    tankSetStats(tnk, 20, 20, 20, 20);
    tankInWater(gs, tnk);
    tankGetStats(tnk, &shells, &mines, &armour, &trees);
    UT_ASSERT_MSG(shells == 19 && mines == 19,
                  "a classic ducking left %u shells and %u mines, expected 19 "
                  "and 19", (unsigned) shells, (unsigned) mines);

    /* Moved, and the water takes the rule rather than one. */
    gs->rules.water_loss_shells = 5;
    gs->rules.water_loss_mines  = 3;
    tankSetStats(tnk, 20, 20, 20, 20);
    tankInWater(gs, tnk);
    tankGetStats(tnk, &shells, &mines, &armour, &trees);
    UT_ASSERT_MSG(shells == 15 && mines == 17,
                  "a ducking taking 5 and 3 left %u shells and %u mines, "
                  "expected 15 and 17", (unsigned) shells, (unsigned) mines);

    /* Less carried than the water takes: empty, not a wrap. */
    tankSetStats(tnk, 2, 1, 20, 20);
    tankInWater(gs, tnk);
    tankGetStats(tnk, &shells, &mines, &armour, &trees);
    UT_ASSERT_MSG(shells == 0 && mines == 0,
                  "a tank carrying less than the water takes read back %u "
                  "shells and %u mines, expected 0 and 0",
                  (unsigned) shells, (unsigned) mines);

    /* Zero is water that takes nothing, which the range check allows and
       which has to leave the tank alone rather than empty it. */
    gs->rules.water_loss_shells = 0;
    gs->rules.water_loss_mines  = 0;
    tankSetStats(tnk, 20, 20, 20, 20);
    tankInWater(gs, tnk);
    tankGetStats(tnk, &shells, &mines, &armour, &trees);
    UT_ASSERT_MSG(shells == 20 && mines == 20,
                  "a ducking taking nothing left %u shells and %u mines, "
                  "expected 20 and 20", (unsigned) shells, (unsigned) mines);

    serverSimDestroy(sim);
    return 0;
}

/* ---- the pairs ------------------------------------------------------------
 *
 * What one rule allows another. Each arm is broken one field at a time from a
 * fresh classic table, so a refusal is always about the pair under test, and
 * the reason has to name both sides — a controller reads it to see which of
 * the two has to move. Each is then given a value that moves but still holds,
 * the boundary where the two are equal, which must be accepted. */

#define SR_PAIR_REFUSED(setup, a, b)                                         \
    do {                                                                     \
        SimRules t;                                                          \
        char why[SIM_RULES_WHY_LEN];                                         \
        simRulesClassic(&t);                                                 \
        setup;                                                               \
        why[0] = '\0';                                                       \
        UT_ASSERT_MSG(!simRulesValidate(&t, why, sizeof why),                \
                      a " against " b ": a table that breaks the pair was "  \
                      "accepted");                                           \
        UT_ASSERT_MSG(strstr(why, a) != NULL && strstr(why, b) != NULL,      \
                      "the refusal does not name " a " and " b ": \"%s\"",   \
                      why);                                                  \
    } while (0)

#define SR_PAIR_OK(setup, label)                                             \
    do {                                                                     \
        SimRules t;                                                          \
        char why[SIM_RULES_WHY_LEN];                                         \
        simRulesClassic(&t);                                                 \
        setup;                                                               \
        why[0] = '\0';                                                       \
        UT_ASSERT_MSG(simRulesValidate(&t, why, sizeof why),                 \
                      label " was refused: %s", why);                        \
    } while (0)

int run_sim_rules_pairs(void) {
    SimRules r;
    char why[SIM_RULES_WHY_LEN];

    /* The classic table satisfies every pair. Stated first and on its own,
     * because a pair written the wrong way round refuses the classic game and
     * nothing else here would say which one did it. */
    simRulesClassic(&r);
    why[0] = '\0';
    UT_ASSERT_MSG(simRulesValidate(&r, why, sizeof why),
                  "the classic table failed a pair check: %s", why);

    /* A base soaks shots down to the hit threshold and is taken at the
     * capture threshold, so the first cannot sit above the second. */
    SR_PAIR_REFUSED(t.base_hit_armour = t.base_capture_armour + 1,
                    "base_hit_armour", "base_capture_armour");
    SR_PAIR_OK(t.base_hit_armour = t.base_capture_armour,
               "a hit threshold equal to the capture threshold");

    /* And a base taken at armour it could never hold is one that is always
       there for the taking, so the threshold cannot sit above full. */
    SR_PAIR_REFUSED(t.base_capture_armour = t.base_full_armour + 1,
                    "base_capture_armour", "base_full_armour");
    SR_PAIR_OK(t.base_capture_armour = t.base_full_armour,
               "a capture threshold equal to what a base holds");

    /* A reserve bigger than the tank is what the base is allowed to hold. */
    SR_PAIR_REFUSED(t.base_min_armour = t.base_full_armour + 1,
                    "base_min_armour", "base_full_armour");
    SR_PAIR_REFUSED(t.base_min_shells = t.base_full_shells + 1,
                    "base_min_shells", "base_full_shells");
    SR_PAIR_REFUSED(t.base_min_mines = t.base_full_mines + 1,
                    "base_min_mines", "base_full_mines");
    /* A reserve equal to what the base holds is legal, and then the give
       has to be nothing: there is no room above the reserve to hand out. */
    SR_PAIR_OK(t.base_min_armour = t.base_full_armour; t.base_armour_give = 0,
               "a reserve equal to what a base holds, giving nothing");

    /* A give bigger than the gap between full and the reserve. */
    SR_PAIR_REFUSED(t.base_armour_give =
                        t.base_full_armour - t.base_min_armour + 1,
                    "base_armour_give", "base_full_armour");
    SR_PAIR_REFUSED(t.base_shells_give =
                        t.base_full_shells - t.base_min_shells + 1,
                    "base_shells_give", "base_full_shells");
    SR_PAIR_REFUSED(t.base_mines_give =
                        t.base_full_mines - t.base_min_mines + 1,
                    "base_mines_give", "base_full_mines");
    SR_PAIR_OK(t.base_armour_give = t.base_full_armour - t.base_min_armour,
               "a give equal to the gap above the reserve");

    /* A job the man could never pay for. */
    SR_PAIR_REFUSED(t.lgm_cost_road = t.tank_full_trees + 1,
                    "lgm_cost_road", "tank_full_trees");
    SR_PAIR_REFUSED(t.lgm_cost_building = t.tank_full_trees + 1,
                    "lgm_cost_building", "tank_full_trees");
    SR_PAIR_REFUSED(t.lgm_cost_repair_building = t.tank_full_trees + 1,
                    "lgm_cost_repair_building", "tank_full_trees");
    SR_PAIR_REFUSED(t.lgm_cost_boat = t.tank_full_trees + 1,
                    "lgm_cost_boat", "tank_full_trees");
    SR_PAIR_REFUSED(t.lgm_cost_pill_new = t.tank_full_trees + 1,
                    "lgm_cost_pill_new", "tank_full_trees");
    SR_PAIR_OK(t.lgm_cost_boat = t.tank_full_trees,
               "a boat costing a full load of trees");

    /* The repair order takes cost times load at once, so the pair is the
     * product. A cost inside the tree cap on its own is still refused when
     * the load multiplies it past one. */
    SR_PAIR_REFUSED(t.lgm_cost_pill_repair =
                        (t.tank_full_trees / t.lgm_pill_repair_load) + 1,
                    "lgm_cost_pill_repair", "lgm_pill_repair_load");
    SR_PAIR_OK(t.lgm_cost_pill_repair =
                   t.tank_full_trees / t.lgm_pill_repair_load,
               "a repair load that exactly fills the tank");

    SR_PAIR_REFUSED(t.lgm_cost_mine = t.tank_full_mines + 1,
                    "lgm_cost_mine", "tank_full_mines");
    SR_PAIR_OK(t.lgm_cost_mine = t.tank_full_mines,
               "a mine costing every mine a tank carries");

    /* One delivery has to finish a pill on nothing. */
    SR_PAIR_REFUSED(t.pill_repair_amount = 1; t.pill_max_armour = 255,
                    "lgm_pill_repair_load", "pill_repair_amount");
    SR_PAIR_OK(t.pill_repair_amount = 3; t.pill_max_armour = 12,
               "a load that exactly covers a pill on zero");

    /* And a single tree cannot be worth more than a whole pill. */
    SR_PAIR_REFUSED(t.pill_repair_amount = t.pill_max_armour + 1,
                    "pill_repair_amount", "pill_max_armour");
    SR_PAIR_OK(t.pill_repair_amount = t.pill_max_armour,
               "one tree worth a whole pill");

    /* The gunsight range needs somewhere to stand: a minimum above the
       maximum is a range with nothing in it. */
    SR_PAIR_REFUSED(t.gunsight_min = t.gunsight_max + 1,
                    "gunsight_min", "gunsight_max");
    SR_PAIR_OK(t.gunsight_min = t.gunsight_max,
               "a gunsight pinned to one length");

    /* The start offset comes off the life budget, so at the shortest shot a
       player can take — gunsight_min — it cannot swallow the whole budget.
       Classic is 5 against 8*2/2, so 9 is the first refusal. */
    SR_PAIR_REFUSED(t.shell_start_add =
                        t.shell_life * t.gunsight_min / 2 + 1,
                    "shell_start_add", "shell_life");
    SR_PAIR_OK(t.shell_start_add = t.shell_life * t.gunsight_min / 2,
               "an offset that exactly spends the shortest shot's budget");

    /* And at the longest shot the budget still has to fit the BYTE the shell
       record stores it in. At the classic gunsight_max of 14 the budget is
       7 per point of shell_life, so 37 lands on 255 exactly and 38 is one
       past it. */
    SR_PAIR_REFUSED(t.shell_life = 38, "shell_life", "gunsight_max");
    SR_PAIR_OK(t.shell_life = 37,
               "the longest shot whose budget still fits a byte");

    /* A pair refuses a table; it never rewrites one. The classic values come
     * back unchanged from a call that passes and from one that does not. */
    {
        SimRules before;
        SimRules after;
        simRulesClassic(&before);
        after = before;
        UT_ASSERT(simRulesValidate(&after, NULL, 0));
        UT_ASSERT_MSG(memcmp(&before, &after, sizeof after) == 0,
                      "a passing check changed the table");
        after.base_min_armour = after.base_full_armour + 1;
        before = after;
        UT_ASSERT(!simRulesValidate(&after, NULL, 0));
        UT_ASSERT_MSG(memcmp(&before, &after, sizeof after) == 0,
                      "a refusing check changed the table");
    }

    return 0;
}

/* A base is taken by driving onto it once its armour is down to the capture
 * threshold. Move the rule and the square a tank can take moves with it —
 * baseIsCapturable is the test tankMoveUnified makes on the drive-over path,
 * and basesGetStatusNum is what draws the X, so the two have to agree. */
int run_sim_rules_capture_threshold_moves(void) {
    ServerSim *sim = ut_make_running_sim("Capturer");
    GameSim *gs;
    base item;
    BYTE bx, by;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT(basesGetNumBases(&gs->bs) >= 1);
    memset(&item, 0, sizeof(item));
    basesGetBase(&gs->bs, &item, 1);
    bx = item.x;
    by = item.y;

    /* Owned, so the capturable test turns on armour rather than on the base
     * being unclaimed, and holding well above the classic threshold. */
    gs->bs->item[0].owner = 0;
    gs->bs->item[0].armour = 20;
    gs->viewPlayer = 0;

    UT_ASSERT_MSG(baseIsCapturable(gs, &gs->bs, bx, by) == FALSE,
                  "a base at 20 armour is not capturable at the classic "
                  "threshold of %ld", (long) gs->rules.base_capture_armour);
    UT_ASSERT_MSG(basesGetStatusNum(gs, 1) != baseDead,
                  "a base at 20 armour should not draw as dead classically");

    /* Raise the threshold past the base's armour and the same base is now
     * one a tank can drive onto and take. */
    gs->rules.base_capture_armour = 25;
    UT_ASSERT_MSG(baseIsCapturable(gs, &gs->bs, bx, by) == TRUE,
                  "a base at 20 armour is capturable once the threshold is 25");
    UT_ASSERT_MSG(basesGetStatusNum(gs, 1) == baseDead,
                  "the status must follow the threshold the capture test uses");

    /* And lowering it below the armour again puts the base out of reach. */
    gs->rules.base_capture_armour = 5;
    UT_ASSERT_MSG(baseIsCapturable(gs, &gs->bs, bx, by) == FALSE,
                  "a base at 20 armour is out of reach at a threshold of 5");

    serverSimDestroy(sim);
    return 0;
}

/* What the builder spends comes off the rules table. A boat on a river costs
 * lgm_cost_boat trees, so the order the man will accept moves with the rule
 * rather than with the constant it replaced. */
int run_sim_rules_builder_cost_follows(void) {
    ServerSim *sim = ut_make_running_sim("Builder");
    GameSim *gs;
    tank *tnk;
    BYTE rx = 0, ry = 0;
    int x, y;
    bool found = false;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    tnk = &gs->tanks[0];

    for (y = 0; y < 256 && !found; y++) {
        for (x = 0; x < 256 && !found; x++) {
            if (mapGetPos(&gs->mp, (BYTE) x, (BYTE) y) == RIVER &&
                !basesExistPos(&gs->bs, (BYTE) x, (BYTE) y) &&
                !pillsExistPos(&gs->pb, (BYTE) x, (BYTE) y)) {
                rx = (BYTE) x;
                ry = (BYTE) y;
                found = true;
            }
        }
    }
    UT_ASSERT_MSG(found, "no plain river tile found on the map");

    /* Exactly the classic price buys a boat. */
    tankSetTrees(gs, tnk, (BYTE) gs->rules.lgm_cost_boat);
    UT_ASSERT_MSG(lgmRequestIsValid(gs, &gs->lgmen[0], tnk, rx, ry,
                                    LGM_BOAT_REQUEST),
                  "a boat should be affordable with exactly %ld trees",
                  (long) gs->rules.lgm_cost_boat);

    /* Raise the price by one with the wood unchanged and the same order is
     * refused, so the spend is the rule's and not the old constant's. */
    gs->rules.lgm_cost_boat += 1;
    UT_ASSERT_MSG(!lgmRequestIsValid(gs, &gs->lgmen[0], tnk, rx, ry,
                                     LGM_BOAT_REQUEST),
                  "a dearer boat should be refused on the same wood");

    /* Pay the new price and it goes ahead again. */
    tankSetTrees(gs, tnk, (BYTE) gs->rules.lgm_cost_boat);
    UT_ASSERT_MSG(lgmRequestIsValid(gs, &gs->lgmen[0], tnk, rx, ry,
                                    LGM_BOAT_REQUEST),
                  "a boat should be affordable at the raised price with the "
                  "wood to match");

    serverSimDestroy(sim);
    return 0;
}

/* A shell is built from shell_speed and shell_life at the moment it is fired:
 * the speed becomes the 24.8 per-tick step it carries for its whole flight,
 * and the life becomes the tick budget on its record. Both are read here off
 * the shell shellsAddItem just made, which is the same record shellsUpdate
 * walks, so a rule that moves moves the shell rather than only the number. */
int run_sim_rules_shell_flight_follows(void) {
    ServerSim *sim = ut_make_running_sim("Gunner");
    GameSim *gs;
    int32_t classicStep, fastStep;
    BYTE classicLen, longLen;
    const TURNTYPE east = (TURNTYPE) BRADIANS_EAST;
    /* Half the widest gunsight, which is what the tank hands shellsAddItem. */
    TURNTYPE len;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    len = (TURNTYPE) (gs->rules.gunsight_max / 2.0f);

    /* Fired east, so the whole step lands on X and the comparison is the
       speed itself, in 24.8 fixed point. */
    shellsAddItem(gs, &gs->shs, 10000, 20000, east, len, 0, FALSE);
    UT_ASSERT_MSG(gs->shs != NULL, "the classic shot produced no shell");
    classicStep = gs->shs->xStep;
    classicLen = gs->shs->length;
    UT_ASSERT_MSG(classicStep == gs->rules.shell_speed * 256,
                  "a shell east steps %ld per tick, not the rule's %ld",
                  (long) classicStep, (long) (gs->rules.shell_speed * 256));
    UT_ASSERT_MSG(classicLen ==
                      (BYTE) (1 + gs->rules.shell_life *
                                      (gs->rules.gunsight_max / 2) -
                              gs->rules.shell_start_add),
                  "a classic shell lives %u ticks, not the budget the rules "
                  "give it", (unsigned) classicLen);

    /* Double the speed and the next shell steps twice as far per tick. */
    gs->rules.shell_speed *= 2;
    shellsAddItem(gs, &gs->shs, 10000, 20000, east, len, 0, FALSE);
    fastStep = gs->shs->xStep;
    UT_ASSERT_MSG(fastStep == classicStep * 2,
                  "doubling shell_speed gave a step of %ld, not %ld",
                  (long) fastStep, (long) (classicStep * 2));

    /* And a longer life is a longer budget, on the same gunsight. */
    gs->rules.shell_speed /= 2;
    gs->rules.shell_life *= 2;
    shellsAddItem(gs, &gs->shs, 10000, 20000, east, len, 0, FALSE);
    longLen = gs->shs->length;
    UT_ASSERT_MSG(longLen > classicLen,
                  "doubling shell_life left the shell living %u ticks, not "
                  "more than %u", (unsigned) longLen, (unsigned) classicLen);
    UT_ASSERT_MSG(longLen ==
                      (BYTE) (1 + gs->rules.shell_life *
                                      (gs->rules.gunsight_max / 2) -
                              gs->rules.shell_start_add),
                  "a longer-lived shell lives %u ticks, not the budget the "
                  "raised rule gives it", (unsigned) longLen);

    serverSimDestroy(sim);
    return 0;
}

/* The brain's shot simulator has no sim to read, so the shell rules reach it
 * on the pathfinder the bot manager pushes to each think — the same carrier
 * the movement rules already ride. A pathfinder nobody has pushed to holds
 * the classic shell, and one that has been pushed flies what it was given. */
int run_sim_rules_brain_shot_follows(void) {
    BrainPathfinder *pf = brainPathfinderCreate();
    SimRules classic;
    BrainShotTile seeded[64];
    BrainShotTile pushed[64];
    BrainShotTile shortLife[64];
    BrainShotTile fast[64];
    int nSeeded, nPushed, nShort, nFast;
    const int cap = (int) (sizeof(seeded) / sizeof(seeded[0]));

    UT_ASSERT(pf != NULL);
    simRulesClassic(&classic);

    /* A fresh pathfinder is seeded from the defaults table, so its walk is
       the classic one — the same answer as pushing the classic rules in. */
    nSeeded = brainPathfinderSimulateShot(pf, 10000, 20000, 14000, 20000,
                                          BRAIN_SHOT_SHOOTER_TANK, 0,
                                          seeded, cap);
    UT_ASSERT_MSG(nSeeded > 0, "a classic shot walked no tiles");

    brainPathfinderSetShellRules(pf, classic.shell_life, classic.shell_speed,
                                 classic.shell_start_add, classic.gunsight_max);
    nPushed = brainPathfinderSimulateShot(pf, 10000, 20000, 14000, 20000,
                                          BRAIN_SHOT_SHOOTER_TANK, 0,
                                          pushed, cap);
    UT_ASSERT_MSG(nPushed == nSeeded &&
                      memcmp(seeded, pushed, (size_t) nSeeded * sizeof(seeded[0])) == 0,
                  "a fresh pathfinder walked %d tiles and one pushed the "
                  "classic rules walked %d", nSeeded, nPushed);

    /* A shorter-lived shell dies sooner, so the walk is shorter. */
    brainPathfinderSetShellRules(pf, 2, classic.shell_speed,
                                 classic.shell_start_add, classic.gunsight_max);
    nShort = brainPathfinderSimulateShot(pf, 10000, 20000, 14000, 20000,
                                         BRAIN_SHOT_SHOOTER_TANK, 0,
                                         shortLife, cap);
    UT_ASSERT_MSG(nShort < nSeeded,
                  "cutting shell_life walked %d tiles, not fewer than %d",
                  nShort, nSeeded);

    /* And a faster shell covers more ground in the same budget. */
    brainPathfinderSetShellRules(pf, classic.shell_life,
                                 classic.shell_speed * 2,
                                 classic.shell_start_add, classic.gunsight_max);
    nFast = brainPathfinderSimulateShot(pf, 10000, 20000, 14000, 20000,
                                        BRAIN_SHOT_SHOOTER_TANK, 0,
                                        fast, cap);
    UT_ASSERT_MSG(nFast > nSeeded,
                  "doubling shell_speed walked %d tiles, not more than %d",
                  nFast, nSeeded);

    brainPathfinderDestroy(pf);
    return 0;
}

/* brainDataMakeInfo allocates the variable-length members; the cases below
 * only read them, so they are freed the way the headless loggers free them
 * and the way run_sim_rules_copies_follow does above. */
static void srFreeBrainInfo(BrainInfo *bi) {
    free(bi->allies);
    free(bi->base);
    free(bi->pillview);
    free(bi->viewdata);
    free(bi->events);
    if (bi->message != NULL) {
        free(bi->message->receivers);
        free(bi->message->message);
        free(bi->message);
    }
}

/* ---- against the classic table ------------------------------------------
 *
 * What the observation builders ask before they will build. The cases below
 * move one rule at a time, because the question is whether any field moving
 * is noticed rather than whether a particular one is.
 */

int run_sim_rules_are_classic(void) {
    SimRules r;
    int      at;

    simRulesClassic(&r);
    UT_ASSERT_MSG(simRulesAreClassic(&r),
                  "a table simRulesClassic has just filled is not reported "
                  "classic");
    UT_ASSERT_MSG(simRulesFirstDifference(&r) < 0,
                  "a classic table reports field %d as differing",
                  simRulesFirstDifference(&r));

    /* An integer rule. */
    simRulesClassic(&r);
    r.tank_full_armour = TANK_FULL_ARMOUR + 1;
    UT_ASSERT_MSG(!simRulesAreClassic(&r),
                  "a table with tank_full_armour moved is reported classic");
    at = simRulesFirstDifference(&r);
    UT_ASSERT_MSG(at == (int) (offsetof(SimRules, tank_full_armour) /
                               sizeof(int32_t)),
                  "the first difference is field %d, expected the one "
                  "tank_full_armour sits at (%d)", at,
                  (int) (offsetof(SimRules, tank_full_armour) /
                         sizeof(int32_t)));

    /* A float rule. The comparison is over the bits, so a rate that is not
       the classic rate is not classic however close it lands: 0.3125 is as
       exact in a float as the 0.25 it replaces, and neither is a NaN. */
    simRulesClassic(&r);
    r.tank_accel_rate = r.tank_accel_rate + 0.0625f;
    UT_ASSERT_MSG(!simRulesAreClassic(&r),
                  "a table with tank_accel_rate moved to %g is reported "
                  "classic", (double) r.tank_accel_rate);

    /* A rule the brain's own view does not carry. BrainRules holds the tank,
       terrain, pill and base numbers and none of the builder's costs, so a
       check written against that view would call this table classic and let
       an observation out on a sim that is not the classic game. */
    simRulesClassic(&r);
    r.lgm_cost_road = r.lgm_cost_road + 1;
    UT_ASSERT_MSG(!simRulesAreClassic(&r),
                  "a table with lgm_cost_road moved is reported classic — a "
                  "rule outside the brain's view still makes a sim not the "
                  "classic game");

    /* The last field, so the walk is not stopping short of the end. */
    simRulesClassic(&r);
    r.tree_weight_mine = r.tree_weight_mine + 1;
    UT_ASSERT_MSG(!simRulesAreClassic(&r),
                  "a table with its last field moved is reported classic");

    /* No table is not a classic table: a caller with nothing to show cannot
       be told its numbers are the right ones. */
    UT_ASSERT_MSG(!simRulesAreClassic(NULL), "NULL is reported classic");

    return 0;
}

/* ---- the refusal, through an observation path ----------------------------
 *
 * obsBuildMultiView is the one of the three paths that answers rather than
 * ends the process — it runs inside a live client, where killing the game
 * over a rule change would be worse than the rescaled vector it is avoiding.
 * That makes it the path a case can drive: the gym's and the headless
 * logger's refusals call exit(), so a case over either would take the test
 * runner down with it and there is no case here for them.
 */

int run_sim_rules_obs_refuses_non_classic(void) {
    LoopbackHarness h;
    BrainInfo       bi;
    WinBoloObs     *obs;
    WinBoloObs     *zero;
    bool            built;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "ObsScale", false, NULL, 4322),
                  "loopback start failed");
    loopbackHarnessPumpUntil(&h, 40, NULL, NULL);
    UT_ASSERT_MSG(h.cs != NULL, "the harness produced no client");

    obs  = (WinBoloObs *) malloc(sizeof(*obs));
    zero = (WinBoloObs *) calloc(1, sizeof(*zero));
    UT_ASSERT_MSG(obs != NULL && zero != NULL, "could not allocate an observation");

    /* Classic first: the observation is built, and it is not the zeroed
       buffer a refusal leaves behind. */
    UT_ASSERT_MSG(simRulesAreClassic(&h.cs->sim.rules),
                  "setup: a fresh client is not running the classic rules, so "
                  "the build below would prove nothing");
    memset(&bi, 0, sizeof(bi));
    brainDataMakeInfo(h.cs, &bi, true, aiNone);
    built = obsBuildMultiView(h.cs, &bi, obs);
    UT_ASSERT_MSG(built,
                  "the builder refused a sim running the classic rules");
    UT_ASSERT_MSG(memcmp(obs, zero, sizeof(*obs)) != 0,
                  "the builder said it built an observation and left it "
                  "zeroed");
    srFreeBrainInfo(&bi);

    /* Now one rule away from classic — a pill's armour cap, which is one of
       the numbers the observation normalises against. */
    h.cs->sim.rules.pill_max_armour = 23;
    memset(&bi, 0, sizeof(bi));
    brainDataMakeInfo(h.cs, &bi, true, aiNone);
    built = obsBuildMultiView(h.cs, &bi, obs);
    UT_ASSERT_MSG(!built,
                  "the builder produced an observation on a sim whose pill "
                  "armour cap is 23, not the classic 15 — the vector would be "
                  "on a scale nothing was trained against");
    UT_ASSERT_MSG(memcmp(obs, zero, sizeof(*obs)) == 0,
                  "a refused build left something behind in the observation");
    srFreeBrainInfo(&bi);

    /* And a rule the observation does not read at all still refuses: the
       question is whether this is the classic game, not whether the entries
       this vector happens to use have moved. */
    h.cs->sim.rules.pill_max_armour = PILLS_MAX_ARMOUR;
    h.cs->sim.rules.lgm_cost_road = h.cs->sim.rules.lgm_cost_road + 1;
    memset(&bi, 0, sizeof(bi));
    brainDataMakeInfo(h.cs, &bi, true, aiNone);
    built = obsBuildMultiView(h.cs, &bi, obs);
    UT_ASSERT_MSG(!built,
                  "the builder produced an observation on a sim whose "
                  "builder costs are not the classic ones");
    srFreeBrainInfo(&bi);

    free(obs);
    free(zero);
    loopbackHarnessStop(&h);
    return 0;
}

/* The forward model's pill numbers. brainWorldSimCreate takes no rules, so a
   model built on a moved pill rule cannot be made here; what is checkable is
   that the numbers live on the model, that they are the classic ones, and
   that a clear leaves them alone. */
int run_sim_rules_worldsim_pill_follows(void) {
    BrainWorldSim *sim = brainWorldSimCreate();
    SimRules classic;

    UT_ASSERT(sim != NULL);
    simRulesClassic(&classic);

    UT_ASSERT_MSG(sim->pill_range == (int) classic.pill_range,
                  "the model fires pills within %d, not the classic %d",
                  sim->pill_range, (int) classic.pill_range);
    UT_ASSERT_MSG(sim->pill_attack_ticks == (int) classic.pill_attack_ticks,
                  "the model's calm fire interval is %d, not the classic %d",
                  sim->pill_attack_ticks, (int) classic.pill_attack_ticks);
    UT_ASSERT_MSG(sim->pill_attack_min_ticks == (int) classic.pill_attack_min_ticks,
                  "the model's angriest fire interval is %d, not the classic %d",
                  sim->pill_attack_min_ticks, (int) classic.pill_attack_min_ticks);
    UT_ASSERT_MSG(sim->pill_cooldown_ticks == (int) classic.pill_cooldown_ticks,
                  "the model's anger chain runs %d ticks, not the classic %d",
                  sim->pill_cooldown_ticks, (int) classic.pill_cooldown_ticks);

    /* A calm pill is added at the calm interval, which is one of the places
       the model reads the field rather than a constant of its own. */
    brainWorldSimAddPill(sim, 10, 10, 15, 0.0f, 0xFF, 0);
    UT_ASSERT_MSG(sim->num_pills == 1, "the pill was not added");
    UT_ASSERT_MSG(sim->pills[0].speed == (uint8_t) classic.pill_attack_ticks,
                  "a calm pill fires every %d ticks, not the rule's %d",
                  (int) sim->pills[0].speed, (int) classic.pill_attack_ticks);

    /* A clear drops the world and keeps the numbers the world runs on. */
    brainWorldSimClear(sim);
    UT_ASSERT_MSG(sim->num_pills == 0, "a clear kept the world");
    UT_ASSERT_MSG(sim->pill_range == (int) classic.pill_range &&
                      sim->pill_attack_ticks == (int) classic.pill_attack_ticks &&
                      sim->pill_attack_min_ticks == (int) classic.pill_attack_min_ticks &&
                      sim->pill_cooldown_ticks == (int) classic.pill_cooldown_ticks,
                  "a clear lost the pill numbers the model was built with");

    brainWorldSimDestroy(sim);
    return 0;
}

/* A flat per-tile pill contribution. The subtract table scales whatever this
   returns by the tile's inverse speed, which is the slot under test, so a
   constant here leaves the speed as the only thing that can move the result. */
static float srTileContrib(void *user, int tile_key) {
    (void) user;
    (void) tile_key;
    return 1.0f;
}

/* Run one navigation slate to completion and read the destination back both
   ways: the cost the search settled on, and that cost with a pill's
   contribution taken off the path it realized. */
static void srRunBoatDijkstra(BrainPathfinder *pf, int sx, int sy,
                              int dx, int dy, float *outCost, float *outLess) {
    int step;
    int done = 0;
    brainPathfinderDijkstraStart(pf, 0, 1, sx, sy, 1, 40, 40, 40, 40,
                                 0.0f, 0, 1.0f, 1, 1);
    for (step = 0; step < 4000 && !done; step++) {
        done = brainPathfinderDijkstraStep(pf, 0, (uint32_t) (2 + step), 100000);
    }
    *outCost = brainPathfinderDijkstraLookupByKind(pf, 1, dx, dy, 1);
    *outLess = brainPathfinderDijkstraLookupSubtractByKind(pf, 1, dx, dy, 1,
                                                           srTileContrib, NULL);
}

/* The five places the pathfinder moves a tank at boat speed, each driven
   through its own entry point on an all-river map with the tank afloat: every
   tile it crosses is water it stays afloat on, so the speed it moves at is the
   table's boat slot and nothing else. */
int run_sim_rules_boat_speed_follows(void) {
    BrainPathfinder *pf = brainPathfinderCreate();
    BYTE *map = (BYTE *) malloc((size_t) MAP_ARRAY_SIZE * MAP_ARRAY_SIZE);
    SimRules classic;
    int x, y;
    int ticksClassic, ticksSlow, ticksFast;
    float estClassic, estSlow;
    float costClassic, costSlow;
    float dijClassic, dijSlow;
    float lessClassic, lessSlow;
    float subClassic, subSlow;
    const int sx = 100, sy = 100, dx = 120, dy = 100;

    UT_ASSERT(pf != NULL && map != NULL);
    simRulesClassic(&classic);

    memset(map, BRIVER, (size_t) MAP_ARRAY_SIZE * MAP_ARRAY_SIZE);
    brainPathfinderSetMap(pf, map);

    /* The travel estimate, which moves the tank a tick at a time. */
    ticksClassic = brainPathfinderEstimateTankTravelTicks(pf, sx, sy, dx, dy,
                                                          1, 20000, 200);
    UT_ASSERT_MSG(ticksClassic > 0,
                  "a boat crossing river arrived in %d ticks", ticksClassic);

    brainPathfinderSetTerrainSpeed(pf, BBOAT, (float) classic.speed_boat / 2.0f);
    ticksSlow = brainPathfinderEstimateTankTravelTicks(pf, sx, sy, dx, dy,
                                                       1, 20000, 200);
    UT_ASSERT_MSG(ticksSlow > ticksClassic,
                  "halving the boat speed crossed in %d ticks, not more than "
                  "the %d the classic speed took", ticksSlow, ticksClassic);

    brainPathfinderSetTerrainSpeed(pf, BBOAT, (float) classic.speed_boat * 2.0f);
    ticksFast = brainPathfinderEstimateTankTravelTicks(pf, sx, sy, dx, dy,
                                                       1, 20000, 200);
    UT_ASSERT_MSG(ticksFast < ticksClassic,
                  "doubling the boat speed crossed in %d ticks, not fewer than "
                  "the %d the classic speed took", ticksFast, ticksClassic);

    /* The other two weigh danger by how long the tank is in it, so it has to
       be there for the boat speed to reach the cost at all. Stamp it over the
       whole map rather than a corridor: a corridor is something the search can
       route around, and then neither cost would move. Small enough that the
       armour drain it also feeds still rounds to nothing. */
    for (y = 0; y < MAP_ARRAY_SIZE; y++) {
        for (x = 0; x < MAP_ARRAY_SIZE; x++) {
            brainPathfinderSetDanger(pf, x, y, 6.0f);
        }
    }

    brainPathfinderSetTerrainSpeed(pf, BBOAT, (float) classic.speed_boat);
    estClassic = brainPathfinderEstimateCost(pf, sx, sy, dx, dy, 1);
    costClassic = brainPathfinderCostTo(pf, sx, sy, dx, dy, 1,
                                        40, 40, 40, 40, 2000000);
    UT_ASSERT_MSG(estClassic > 0.0f,
                  "the straight-line estimate across river was %f", estClassic);
    UT_ASSERT_MSG(costClassic > 0.0f,
                  "the searched cost across river was %f", costClassic);

    brainPathfinderSetTerrainSpeed(pf, BBOAT, (float) classic.speed_boat / 2.0f);
    estSlow = brainPathfinderEstimateCost(pf, sx, sy, dx, dy, 1);
    costSlow = brainPathfinderCostTo(pf, sx, sy, dx, dy, 1,
                                     40, 40, 40, 40, 2000000);

    /* The straight-line cost estimate. */
    UT_ASSERT_MSG(estSlow > estClassic,
                  "halving the boat speed estimated %f across the danger, not "
                  "more than the %f the classic speed estimated",
                  estSlow, estClassic);

    /* And the cost the search itself works out. */
    UT_ASSERT_MSG(costSlow > costClassic,
                  "halving the boat speed searched out %f across the danger, "
                  "not more than the %f the classic speed searched out",
                  costSlow, costClassic);

    /* Last the two tables the navigation search builds for itself, which are
       what it expands — a boat speed that reached the estimates above but not
       these would steer the bot by one number and time it by another. */
    brainPathfinderSetTerrainSpeed(pf, BBOAT, (float) classic.speed_boat);
    srRunBoatDijkstra(pf, sx, sy, dx, dy, &dijClassic, &lessClassic);
    UT_ASSERT_MSG(dijClassic > 0.0f,
                  "the navigation search costed the crossing at %f", dijClassic);

    brainPathfinderSetTerrainSpeed(pf, BBOAT, (float) classic.speed_boat / 2.0f);
    srRunBoatDijkstra(pf, sx, sy, dx, dy, &dijSlow, &lessSlow);

    UT_ASSERT_MSG(dijSlow > dijClassic,
                  "halving the boat speed cost the navigation search %f, not "
                  "more than the %f the classic speed cost it",
                  dijSlow, dijClassic);

    /* The second table is the one the subtraction is scaled by, so read it as
       the size of the subtraction rather than the cost it came off — the cost
       itself already moved with the table above. */
    subClassic = dijClassic - lessClassic;
    subSlow    = dijSlow    - lessSlow;
    UT_ASSERT_MSG(subClassic > 0.0f,
                  "a pill's contribution came off the classic-speed path as "
                  "%f, so the subtraction table was never reached", subClassic);
    UT_ASSERT_MSG(subSlow > subClassic,
                  "halving the boat speed took %f off the path, not more than "
                  "the %f the classic speed took off", subSlow, subClassic);

    brainPathfinderDestroy(pf);
    free(map);
    return 0;
}

/* The observation's reload scalar. obsBuildMultiView refuses a table that is
   not classic, so the only case reachable from here is the classic one — which
   is the one that moved: the old divisor of 15 put a tank one tick from firing
   at 12/15, and the rule that set the counter is 13. */
int run_sim_rules_obs_reload_follows(void) {
    LoopbackHarness h;
    BrainInfo       bi;
    WinBoloObs     *obs;
    GameSim        *gs;
    BYTE            me;
    bool            built;
    float           want, wasBefore;
    const BYTE      reload = 6;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "ObsReload", false, NULL, 4323),
                  "loopback start failed");
    loopbackHarnessPumpUntil(&h, 40, NULL, NULL);
    UT_ASSERT_MSG(h.cs != NULL, "the harness produced no client");

    obs = (WinBoloObs *) malloc(sizeof(*obs));
    UT_ASSERT_MSG(obs != NULL, "could not allocate an observation");

    gs = clientSimGetGameSim(h.cs);
    me = clientSimGetMyPlayerNum(h.cs);
    UT_ASSERT_MSG(gs != NULL, "the harness produced no sim");
    UT_ASSERT_MSG(simRulesAreClassic(&gs->rules),
                  "setup: a fresh client is not running the classic rules, so "
                  "the builder below would refuse and prove nothing");
    UT_ASSERT_MSG(gs->rules.tank_reload_ticks == TANK_RELOAD_TIME,
                  "setup: the classic reload rule is %d, not %d",
                  (int) gs->rules.tank_reload_ticks, TANK_RELOAD_TIME);

    /* Park the gun mid-reload so the scalar has something to scale. */
    tankSetReload(&gs->tanks[me], reload);
    memset(&bi, 0, sizeof(bi));
    brainDataMakeInfo(h.cs, &bi, true, aiNone);
    UT_ASSERT_MSG(bi.reload == reload,
                  "the tank went into the builder at reload %d, not %d",
                  (int) bi.reload, (int) reload);

    built = obsBuildMultiView(h.cs, &bi, obs);
    UT_ASSERT_MSG(built, "the builder refused a sim running the classic rules");

    want      = (float) reload / (float) gs->rules.tank_reload_ticks;
    wasBefore = (float) reload / 15.0f;

    UT_ASSERT_MSG(obs->scalar[7] > want - 0.0005f &&
                      obs->scalar[7] < want + 0.0005f,
                  "a tank %d ticks off firing reads %f, not the %f its reload "
                  "rule of %d makes it",
                  (int) reload, obs->scalar[7], want,
                  (int) gs->rules.tank_reload_ticks);

    /* And it is no longer on the scale that could not reach 1.0. */
    UT_ASSERT_MSG(obs->scalar[7] > wasBefore + 0.0005f,
                  "the scalar reads %f, which is still the %f a divisor of 15 "
                  "gives — the rule is not being read",
                  obs->scalar[7], wasBefore);

    srFreeBrainInfo(&bi);
    free(obs);
    loopbackHarnessStop(&h);
    return 0;
}
