/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Simulation Rules
 *Filename:      sim_rules.c
 *Author:        John Morrison
 *Purpose:
 *  The classic defaults and the range checks for SimRules.
 *
 *  Every default here is the value its constant holds, and
 *  the constants stay defined so this file is the one place
 *  the two are written down together. A default that drifts
 *  from its constant changes how the classic game plays, so
 *  the unit case walks this table field by field.
 *********************************************************/

#include "sim_rules.h"

#include <stdio.h>
#include <string.h>   /* memcmp — the comparison against the classic table */

#include "global.h"      /* DAMAGE */
#include "gametype.h"    /* TANK_FULL_* */
#include "tank.h"        /* the tank timings, rates and MINE_DAMAGE */
#include "bolo_map.h"    /* MAP_SPEED_T* / MAP_TURN_T* */
#include "shells.h"      /* SHELL_LIFE / SHELL_SPEED / SHELL_START_ADD */
#include "lgm.h"         /* LGM_* */
#include "pillbox.h"     /* PILLS_MAX_ARMOUR / PILLBOX_* */
#include "bases.h"       /* BASE_* / MIN_ARMOUR_CAPTURE */
#include "building.h"    /* BUILDING_LIFE */
#include "rubble.h"      /* RUBBLE_LIFE */
#include "grass.h"       /* GRASS_LIFE */
#include "swamp.h"       /* SWAMP_LIFE */
#include "minesexp.h"    /* MINES_EXPLOSION_WAIT */
#include "treegrow.h"    /* TREEGROW_* / TREE_GROW_* */

void simRulesClassic(SimRules *out) {
    if (out == NULL) {
        return;
    }

    /* ---- Tank ---- */
    out->tank_reload_ticks   = TANK_RELOAD_TIME;
    out->tank_full_shells    = TANK_FULL_SHELLS;
    out->tank_full_mines     = TANK_FULL_MINES;
    out->tank_full_trees     = TANK_FULL_TREES;
    out->tank_full_armour    = TANK_FULL_ARMOUR;
    out->tank_death_ticks    = TANK_DEATH_WAIT;
    out->tank_water_ticks    = TANK_WATER_TIME;
    out->shell_damage        = DAMAGE;
    out->mine_damage         = MINE_DAMAGE;
    out->just_fired_ticks    = JUST_FIRED_TICKS;
    out->gunsight_min        = GUNSIGHT_MIN;
    out->gunsight_max        = GUNSIGHT_MAX;
    out->tank_accel_rate     = (float) TANK_ACCELERATE_RATE;
    out->tank_decel_rate     = (float) TANK_TERRAIN_DECEL_RATE;
    out->tank_brake_rate     = (float) TANK_SLOWKEY_RATE;
    out->tank_autoslow_rate  = (float) TANK_AUTOSLOW_SPEED;
    out->tank_min_move       = TANK_MIN_MOVE_SPEED;

    /* ---- Terrain speed caps ---- */
    out->speed_road          = MAP_SPEED_TROAD;
    out->speed_grass         = MAP_SPEED_TGRASS;
    out->speed_forest        = MAP_SPEED_TFOREST;
    out->speed_river         = MAP_SPEED_TRIVER;
    out->speed_swamp         = MAP_SPEED_TSWAMP;
    out->speed_crater        = MAP_SPEED_TCRATER;
    out->speed_rubble        = MAP_SPEED_TRUBBLE;
    out->speed_boat          = MAP_SPEED_TBOAT;
    out->speed_deep_sea      = MAP_SPEED_TDEEPSEA;
    out->speed_refuel_base   = MAP_SPEED_TREFBASE;

    /* ---- Terrain turn rates ---- */
    out->turn_road           = (float) MAP_TURN_TROAD;
    out->turn_grass          = (float) MAP_TURN_TGRASS;
    out->turn_forest         = (float) MAP_TURN_TFOREST;
    out->turn_river          = (float) MAP_TURN_TRIVER;
    out->turn_swamp          = (float) MAP_TURN_TSWAMP;
    out->turn_crater         = (float) MAP_TURN_TCRATER;
    out->turn_rubble         = (float) MAP_TURN_TRUBBLE;
    out->turn_boat           = (float) MAP_TURN_TBOAT;
    out->turn_deep_sea       = (float) MAP_TURN_TDEEPSEA;
    out->turn_refuel_base    = (float) MAP_TURN_TREFBASE;

    /* ---- Shells ---- */
    out->shell_life          = SHELL_LIFE;
    out->shell_speed         = SHELL_SPEED;
    out->shell_start_add     = SHELL_START_ADD;

    /* ---- Builder ---- */
    out->lgm_build_ticks          = LGM_BUILD_TIME;
    out->lgm_cost_road            = LGM_COST_ROAD;
    out->lgm_cost_building        = LGM_COST_BUILDING;
    out->lgm_cost_repair_building = LGM_COST_REPAIRBUILDING;
    out->lgm_cost_pill_repair     = LGM_COST_PILLREPAIR;
    out->lgm_cost_boat            = LGM_COST_BOAT;
    out->lgm_cost_pill_new        = LGM_COST_PILLNEW;
    out->lgm_cost_mine            = LGM_COST_MINE;
    out->lgm_pill_repair_load     = LGM_LOAD_PILLREPAIR;
    out->lgm_gather_trees         = LGM_GATHER_TREE;
    out->lgm_helicopter_speed     = LGM_HELICOPTER_SPEED;

    /* ---- Pillbox ----
     * pill_max_armour was PILLS_MAX_ARMOUR and PILL_MAX_HEALTH, two names for
     * the same 15; the second is gone and this field is the one spelling. */
    out->pill_max_armour       = PILLS_MAX_ARMOUR;
    out->pill_attack_ticks     = PILLBOX_ATTACK_NORMAL;
    out->pill_attack_min_ticks = PILLBOX_MAX_FIRERATE;
    out->pill_cooldown_ticks   = PILLBOX_COOLDOWN_TIME;
    out->pill_repair_amount    = PILL_REPAIR_AMOUNT;
    out->pill_range            = PILLBOX_RANGE;

    /* ---- Base ---- */
    out->base_full_armour         = BASE_FULL_ARMOUR;
    out->base_full_shells         = BASE_FULL_SHELLS;
    out->base_full_mines          = BASE_FULL_MINES;
    out->base_capture_armour      = MIN_ARMOUR_CAPTURE;
    out->base_hit_armour          = BASE_MIN_CAN_HIT;
    out->base_min_armour          = BASE_MIN_ARMOUR;
    out->base_min_shells          = BASE_MIN_SHELLS;
    out->base_min_mines           = BASE_MIN_MINES;
    out->base_armour_give         = BASE_ARMOUR_GIVE;
    out->base_shells_give         = BASE_SHELLS_GIVE;
    out->base_mines_give          = BASE_MINES_GIVE;
    out->base_refuel_armour_ticks = BASE_REFUEL_ARMOUR;
    out->base_refuel_shells_ticks = (float) BASE_REFUEL_SHELLS;
    out->base_refuel_mines_ticks  = (float) BASE_REFUEL_MINES;
    out->base_regen_ticks         = BASE_TICKS_BETWEEN_REFUEL;

    /* ---- Terrain destruction and explosions ---- */
    out->building_life           = BUILDING_LIFE;
    out->rubble_life             = RUBBLE_LIFE;
    out->grass_life              = GRASS_LIFE;
    out->swamp_life              = SWAMP_LIFE;
    out->mine_fuse_ticks         = MINES_EXPLOSION_WAIT;
    out->big_explosion_threshold = TANK_BIG_EXPLOSION_THRESHOLD;

    /* ---- Tree growth ---- */
    out->tree_grow_ticks           = TREEGROW_TIME;
    out->tree_grow_initial_ticks   = TREEGROW_INITIAL_TIME;
    out->tree_weight_forest        = TREE_GROW_FOREST;
    out->tree_weight_grass         = TREE_GROW_GRASS;
    out->tree_weight_river         = TREE_GROW_RIVER;
    out->tree_weight_boat          = TREE_GROW_BOAT;
    out->tree_weight_deep_sea      = TREE_GROW_DEEP_SEA;
    out->tree_weight_swamp         = TREE_GROW_DEEP_SWAMP;
    out->tree_weight_rubble        = TREE_GROW_DEEP_RUBBLE;
    out->tree_weight_building      = TREE_GROW_BUILDING;
    out->tree_weight_half_building = TREE_GROW_HALF_BUILDING;
    out->tree_weight_crater        = TREE_GROW_CRATER;
    out->tree_weight_road          = TREE_GROW_ROAD;
    out->tree_weight_mine          = TREE_GROW_MINE;
}

/* ---- Against the classic table -------------------------------------------
 *
 * Answered by filling a classic table and comparing the whole struct, so the
 * answer follows simRulesClassic wherever its values are written down and
 * there is no second list of them to keep in step with it.
 *
 * A whole-struct comparison is exact here because the struct has no padding
 * to read: every field is four bytes wide, and the static assertion in
 * server_sim_scenario.c holds sizeof(SimRules) to SCN_RULE_COUNT times four,
 * which cannot be true of a struct carrying a pad byte. Floats compare by
 * their bits rather than numerically, which is the answer this question
 * wants — a rate written to a value that is not the classic one is not
 * classic however close to it it lands, and no classic value is a NaN. */

int simRulesFirstDifference(const SimRules *rules) {
    SimRules        classic;
    const int32_t  *a;
    const int32_t  *b;
    int             i;
    const int       fields = (int) (sizeof(SimRules) / sizeof(int32_t));

    if (rules == NULL) {
        return 0;
    }
    simRulesClassic(&classic);
    if (memcmp(rules, &classic, sizeof(classic)) == 0) {
        return -1;
    }

    /* Which one. Walked as four-byte words rather than by name: the fields
       are all four bytes and the list that names them is the scenario's,
       which sits above this file. The index is that list's own index, so a
       caller holding SCN_RULE_LIST can turn it into the rule's name. */
    a = (const int32_t *) (const void *) rules;
    b = (const int32_t *) (const void *) &classic;
    for (i = 0; i < fields; i++) {
        if (memcmp(&a[i], &b[i], sizeof(int32_t)) != 0) {
            return i;
        }
    }
    /* The whole-struct compare above said they differ, so a word does. */
    return 0;
}

bool simRulesAreClassic(const SimRules *rules) {
    return simRulesFirstDifference(rules) < 0;
}

/* ---- Range checks ---------------------------------------------------------
 *
 * One arm per field, in the order the struct declares them. A row whose bound
 * is another field is checked only as far as its own number goes — the fixed
 * end — because what one field allows another is a different question. */

static void simRulesWhyInt(char *why, size_t whyLen, const char *field,
                           int32_t value, int32_t lo, int32_t hi) {
    if (why == NULL || whyLen == 0) {
        return;
    }
    snprintf(why, whyLen, "%s is %ld, outside %ld..%ld", field, (long) value,
             (long) lo, (long) hi);
}

static void simRulesWhyFloat(char *why, size_t whyLen, const char *field,
                             float value, float lo, float hi) {
    if (why == NULL || whyLen == 0) {
        return;
    }
    snprintf(why, whyLen, "%s is %g, outside %g..%g", field, (double) value,
             (double) lo, (double) hi);
}

#define RULE_INT(field, lo, hi)                                              \
    if (rules->field < (int32_t) (lo) || rules->field > (int32_t) (hi)) {    \
        simRulesWhyInt(why, whyLen, #field, rules->field, (int32_t) (lo),    \
                       (int32_t) (hi));                                      \
        return SIM_RULES_FAULT_RANGE;                                        \
    }

/* Only the fixed end of a row bounded by another field. */
#define RULE_INT_MIN(field, lo)                                              \
    if (rules->field < (int32_t) (lo)) {                                     \
        simRulesWhyInt(why, whyLen, #field, rules->field, (int32_t) (lo),    \
                       INT32_MAX);                                           \
        return SIM_RULES_FAULT_RANGE;                                        \
    }

/* One arm per pair of fields. The reason names both sides and the numbers
 * they hold, so a caller reading a refusal can see which of the two has to
 * move rather than only that something is wrong. A pair never changes a
 * value: it refuses the table or it passes it. */
#define RULE_PAIR(cond, ...)                                                 \
    if (!(cond)) {                                                           \
        if (why != NULL && whyLen > 0) {                                     \
            snprintf(why, whyLen, __VA_ARGS__);                              \
        }                                                                    \
        return SIM_RULES_FAULT_PAIR;                                         \
    }

/* Written as a negated in-range test so a NaN fails rather than passing. */
#define RULE_FLT(field, lo, hi)                                              \
    if (!(rules->field >= (float) (lo) && rules->field <= (float) (hi))) {   \
        simRulesWhyFloat(why, whyLen, #field, rules->field, (float) (lo),    \
                         (float) (hi));                                      \
        return SIM_RULES_FAULT_RANGE;                                        \
    }

SimRulesFault simRulesCheck(const SimRules *rules, char *why, size_t whyLen) {
    if (why != NULL && whyLen > 0) {
        why[0] = '\0';
    }
    if (rules == NULL) {
        if (why != NULL && whyLen > 0) {
            snprintf(why, whyLen, "no rules table");
        }
        return SIM_RULES_FAULT_NO_TABLE;
    }

    /* ---- Tank ---- */
    RULE_INT(tank_reload_ticks, 0, 255)
    RULE_INT(tank_full_shells, 0, 255)
    RULE_INT(tank_full_mines, 0, 255)
    RULE_INT(tank_full_trees, 0, 255)
    RULE_INT(tank_full_armour, 0, 255)
    RULE_INT(tank_death_ticks, 0, 65535)
    RULE_INT(tank_water_ticks, 1, 255)
    RULE_INT(shell_damage, 1, 255)
    RULE_INT(mine_damage, 1, 255)
    RULE_INT(just_fired_ticks, 0, 255)
    RULE_INT(gunsight_min, 1, 255)
    RULE_INT(gunsight_max, 1, 255)
    RULE_FLT(tank_accel_rate, 0.01, 16.0)
    RULE_FLT(tank_decel_rate, 0.01, 16.0)
    RULE_FLT(tank_brake_rate, 0.01, 16.0)
    RULE_FLT(tank_autoslow_rate, 0.01, 16.0)
    RULE_INT(tank_min_move, 0, 255)

    /* ---- Terrain speed caps: the players[].speed packing saturates at 63 ---- */
    RULE_INT(speed_road, 0, 63)
    RULE_INT(speed_grass, 0, 63)
    RULE_INT(speed_forest, 0, 63)
    RULE_INT(speed_river, 0, 63)
    RULE_INT(speed_swamp, 0, 63)
    RULE_INT(speed_crater, 0, 63)
    RULE_INT(speed_rubble, 0, 63)
    RULE_INT(speed_boat, 0, 63)
    RULE_INT(speed_deep_sea, 0, 63)
    RULE_INT(speed_refuel_base, 0, 63)

    /* ---- Terrain turn rates ---- */
    RULE_FLT(turn_road, 0.0, 16.0)
    RULE_FLT(turn_grass, 0.0, 16.0)
    RULE_FLT(turn_forest, 0.0, 16.0)
    RULE_FLT(turn_river, 0.0, 16.0)
    RULE_FLT(turn_swamp, 0.0, 16.0)
    RULE_FLT(turn_crater, 0.0, 16.0)
    RULE_FLT(turn_rubble, 0.0, 16.0)
    RULE_FLT(turn_boat, 0.0, 16.0)
    RULE_FLT(turn_deep_sea, 0.0, 16.0)
    RULE_FLT(turn_refuel_base, 0.0, 16.0)

    /* ---- Shells ---- */
    RULE_INT(shell_life, 1, 255)
    RULE_INT(shell_speed, 1, 255)
    RULE_INT_MIN(shell_start_add, 0)

    /* ---- Builder ---- */
    RULE_INT(lgm_build_ticks, 0, 255)
    RULE_INT_MIN(lgm_cost_road, 0)
    RULE_INT_MIN(lgm_cost_building, 0)
    RULE_INT_MIN(lgm_cost_repair_building, 0)
    RULE_INT_MIN(lgm_cost_pill_repair, 0)
    RULE_INT_MIN(lgm_cost_boat, 0)
    RULE_INT_MIN(lgm_cost_pill_new, 0)
    RULE_INT_MIN(lgm_cost_mine, 0)
    RULE_INT(lgm_pill_repair_load, 1, 255)
    RULE_INT(lgm_gather_trees, 1, 255)
    RULE_INT(lgm_helicopter_speed, 1, 255)

    /* ---- Pillbox ---- */
    RULE_INT(pill_max_armour, 1, 255)
    RULE_INT(pill_attack_ticks, 1, 255)
    RULE_INT_MIN(pill_attack_min_ticks, 1)
    /* Both ends of the attack interval are converted, so the pair can be
     * asked here: the fastest a hurt pill fires cannot be slower than the
     * rate an untouched one sits at, or the clamp has no window to land in. */
    if (rules->pill_attack_min_ticks > rules->pill_attack_ticks) {
        if (why != NULL && whyLen > 0) {
            snprintf(why, whyLen,
                     "pill_attack_min_ticks is %ld, above pill_attack_ticks %ld",
                     (long) rules->pill_attack_min_ticks,
                     (long) rules->pill_attack_ticks);
        }
        /* A pair, though it is asked here rather than in the pairs block
           below: both its sides are pillbox rows and it sits with them. */
        return SIM_RULES_FAULT_PAIR;
    }
    RULE_INT(pill_cooldown_ticks, 0, 255)
    RULE_INT_MIN(pill_repair_amount, 1)
    RULE_INT(pill_range, 0, 65535)

    /* ---- Base ---- */
    RULE_INT(base_full_armour, 0, 255)
    RULE_INT(base_full_shells, 0, 255)
    RULE_INT(base_full_mines, 0, 255)
    RULE_INT_MIN(base_capture_armour, 0)
    RULE_INT_MIN(base_hit_armour, 0)
    RULE_INT_MIN(base_min_armour, 0)
    RULE_INT_MIN(base_min_shells, 0)
    RULE_INT_MIN(base_min_mines, 0)
    RULE_INT_MIN(base_armour_give, 0)
    RULE_INT_MIN(base_shells_give, 0)
    RULE_INT_MIN(base_mines_give, 0)
    RULE_INT(base_refuel_armour_ticks, 1, 255)
    RULE_FLT(base_refuel_shells_ticks, 0.5, 255.0)
    RULE_FLT(base_refuel_mines_ticks, 0.5, 255.0)
    RULE_INT(base_regen_ticks, 1, INT32_MAX)

    /* ---- Terrain destruction and explosions ---- */
    RULE_INT(building_life, 1, 255)
    RULE_INT(rubble_life, 1, 255)
    RULE_INT(grass_life, 1, 255)
    RULE_INT(swamp_life, 1, 255)
    RULE_INT(mine_fuse_ticks, 1, 255)
    RULE_INT(big_explosion_threshold, 0, 510)

    /* ---- Tree growth ---- */
    RULE_INT(tree_grow_ticks, 1, INT32_MAX)
    RULE_INT(tree_grow_initial_ticks, 1, INT32_MAX)
    RULE_INT(tree_weight_forest, -32768, 32767)
    RULE_INT(tree_weight_grass, -32768, 32767)
    RULE_INT(tree_weight_river, -32768, 32767)
    RULE_INT(tree_weight_boat, -32768, 32767)
    RULE_INT(tree_weight_deep_sea, -32768, 32767)
    RULE_INT(tree_weight_swamp, -32768, 32767)
    RULE_INT(tree_weight_rubble, -32768, 32767)
    RULE_INT(tree_weight_building, -32768, 32767)
    RULE_INT(tree_weight_half_building, -32768, 32767)
    RULE_INT(tree_weight_crater, -32768, 32767)
    RULE_INT(tree_weight_road, -32768, 32767)
    RULE_INT(tree_weight_mine, -32768, 32767)

    /* ---- Pairs ----------------------------------------------------------
     *
     * What one field allows another, now that both sides of each of these is
     * a rule. The rows above check each field against its own fixed bounds;
     * these check the rows against each other, in an order that lets a later
     * arm rely on an earlier one — base_min_* is known to be inside
     * base_full_* before the give rows subtract the two. The attack-interval
     * pair is not here: both its sides are pillbox rows and it sits with
     * them. The two products are computed in 64 bits because a cost and a
     * load are each bounded below and not above, so multiplying them in
     * int32_t could overflow before the comparison. */

    /* The three base armour thresholds read hit, capture, full, in that
     * order. A capture threshold above what a base can ever hold would make
     * every base on the map permanently capturable, since its armour could
     * never climb past the threshold. Checked before the hit arm below, so a
     * table breaking both is told about the outer one first. */
    RULE_PAIR(rules->base_capture_armour <= rules->base_full_armour,
              "base_capture_armour is %ld, above base_full_armour %ld",
              (long) rules->base_capture_armour,
              (long) rules->base_full_armour)

    RULE_PAIR(rules->base_hit_armour <= rules->base_capture_armour,
              "base_hit_armour is %ld, above base_capture_armour %ld",
              (long) rules->base_hit_armour,
              (long) rules->base_capture_armour)

    RULE_PAIR(rules->base_min_armour <= rules->base_full_armour,
              "base_min_armour is %ld, above base_full_armour %ld",
              (long) rules->base_min_armour, (long) rules->base_full_armour)
    RULE_PAIR(rules->base_min_shells <= rules->base_full_shells,
              "base_min_shells is %ld, above base_full_shells %ld",
              (long) rules->base_min_shells, (long) rules->base_full_shells)
    RULE_PAIR(rules->base_min_mines <= rules->base_full_mines,
              "base_min_mines is %ld, above base_full_mines %ld",
              (long) rules->base_min_mines, (long) rules->base_full_mines)

    /* A base hands out what it holds above its reserve, so a give bigger
     * than that gap would take the base under its own floor. */
    RULE_PAIR(rules->base_armour_give <=
                  rules->base_full_armour - rules->base_min_armour,
              "base_armour_give is %ld, above base_full_armour %ld less "
              "base_min_armour %ld",
              (long) rules->base_armour_give, (long) rules->base_full_armour,
              (long) rules->base_min_armour)
    RULE_PAIR(rules->base_shells_give <=
                  rules->base_full_shells - rules->base_min_shells,
              "base_shells_give is %ld, above base_full_shells %ld less "
              "base_min_shells %ld",
              (long) rules->base_shells_give, (long) rules->base_full_shells,
              (long) rules->base_min_shells)
    RULE_PAIR(rules->base_mines_give <=
                  rules->base_full_mines - rules->base_min_mines,
              "base_mines_give is %ld, above base_full_mines %ld less "
              "base_min_mines %ld",
              (long) rules->base_mines_give, (long) rules->base_full_mines,
              (long) rules->base_min_mines)

    /* A job the man can never pay for is one no player can order. */
    RULE_PAIR(rules->lgm_cost_road <= rules->tank_full_trees,
              "lgm_cost_road is %ld, above tank_full_trees %ld",
              (long) rules->lgm_cost_road, (long) rules->tank_full_trees)
    RULE_PAIR(rules->lgm_cost_building <= rules->tank_full_trees,
              "lgm_cost_building is %ld, above tank_full_trees %ld",
              (long) rules->lgm_cost_building, (long) rules->tank_full_trees)
    RULE_PAIR(rules->lgm_cost_repair_building <= rules->tank_full_trees,
              "lgm_cost_repair_building is %ld, above tank_full_trees %ld",
              (long) rules->lgm_cost_repair_building,
              (long) rules->tank_full_trees)
    RULE_PAIR(rules->lgm_cost_boat <= rules->tank_full_trees,
              "lgm_cost_boat is %ld, above tank_full_trees %ld",
              (long) rules->lgm_cost_boat, (long) rules->tank_full_trees)
    RULE_PAIR(rules->lgm_cost_pill_new <= rules->tank_full_trees,
              "lgm_cost_pill_new is %ld, above tank_full_trees %ld",
              (long) rules->lgm_cost_pill_new, (long) rules->tank_full_trees)

    /* The repair order takes a whole load at once, so what it costs is the
     * cost times the load and that is what the tank has to be able to
     * carry — a script setting the two apart multiplies them. */
    RULE_PAIR((int64_t) rules->lgm_cost_pill_repair *
                  rules->lgm_pill_repair_load <= rules->tank_full_trees,
              "lgm_cost_pill_repair %ld times lgm_pill_repair_load %ld is "
              "above tank_full_trees %ld",
              (long) rules->lgm_cost_pill_repair,
              (long) rules->lgm_pill_repair_load,
              (long) rules->tank_full_trees)

    RULE_PAIR(rules->lgm_cost_mine <= rules->tank_full_mines,
              "lgm_cost_mine is %ld, above tank_full_mines %ld",
              (long) rules->lgm_cost_mine, (long) rules->tank_full_mines)

    /* One delivery has to be able to finish a pill on nothing, or the man
     * walks out, spends the trees and leaves the pill short. */
    RULE_PAIR((int64_t) rules->lgm_pill_repair_load *
                  rules->pill_repair_amount >= rules->pill_max_armour,
              "lgm_pill_repair_load %ld times pill_repair_amount %ld is "
              "below pill_max_armour %ld",
              (long) rules->lgm_pill_repair_load,
              (long) rules->pill_repair_amount,
              (long) rules->pill_max_armour)
    RULE_PAIR(rules->pill_repair_amount <= rules->pill_max_armour,
              "pill_repair_amount is %ld, above pill_max_armour %ld",
              (long) rules->pill_repair_amount,
              (long) rules->pill_max_armour)

    /* The gunsight is a range the player walks between two ends, so a
     * minimum above the maximum leaves nowhere to stand: tankCreate would
     * start the tank outside it and neither the increase nor the decrease
     * key could bring it back. */
    RULE_PAIR(rules->gunsight_min <= rules->gunsight_max,
              "gunsight_min is %ld, above gunsight_max %ld",
              (long) rules->gunsight_min, (long) rules->gunsight_max)

    /* shellLifeTicks takes the start offset off the life budget, so an
     * offset bigger than the shortest shot's whole budget gives a shell
     * that dies where it is born. Measured at gunsight_min, the shortest
     * shot a player can take. The product is 64-bit because neither field
     * is bounded above by the other. */
    RULE_PAIR((int64_t) rules->shell_start_add <=
                  (int64_t) rules->shell_life * rules->gunsight_min / 2,
              "shell_start_add is %ld, above shell_life %ld times "
              "gunsight_min %ld halved",
              (long) rules->shell_start_add, (long) rules->shell_life,
              (long) rules->gunsight_min)

    /* And the other end: the tank fires at sightLen / 2 and shellLifeTicks
     * stores the answer in the shell record's BYTE, so the longest shot a
     * player can take has to still fit. Computed the way shellLifeTicks
     * computes it, so the check and the code cannot disagree. */
    RULE_PAIR((int64_t) rules->shell_life * rules->gunsight_max / 2 -
                      rules->shell_start_add + 1 <= 255,
              "shell_life %ld times gunsight_max %ld halved, less "
              "shell_start_add %ld, plus 1, is above 255",
              (long) rules->shell_life, (long) rules->gunsight_max,
              (long) rules->shell_start_add)

    return SIM_RULES_OK;
}

#undef RULE_INT
#undef RULE_INT_MIN
#undef RULE_FLT
#undef RULE_PAIR

/* For the callers that only want to know whether the table is usable. */
bool simRulesValidate(const SimRules *rules, char *why, size_t whyLen) {
    return simRulesCheck(rules, why, whyLen) == SIM_RULES_OK;
}
