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

#include "global.h"      /* DAMAGE */
#include "gametype.h"    /* TANK_FULL_* */
#include "tank.h"        /* the tank timings, rates and MINE_DAMAGE */
#include "bolo_map.h"    /* MAP_SPEED_T* / MAP_TURN_T* */
#include "shells.h"      /* SHELL_LIFE / SHELL_SPEED */
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

    /* ---- Shells ----
     * shell_start_add is 5, not shells.h's SHELL_START_ADD. That header
     * declares 6 and both users override it to 5 with their own #define
     * (shells.c and client_sim.c), so 5 is the number a shell is actually
     * fired with and 6 is a value nothing reads. */
    out->shell_life          = SHELL_LIFE;
    out->shell_speed         = SHELL_SPEED;
    out->shell_start_add     = 5;

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

    /* ---- Pillbox ---- */
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
        return false;                                                        \
    }

/* Only the fixed end of a row bounded by another field. */
#define RULE_INT_MIN(field, lo)                                              \
    if (rules->field < (int32_t) (lo)) {                                     \
        simRulesWhyInt(why, whyLen, #field, rules->field, (int32_t) (lo),    \
                       INT32_MAX);                                           \
        return false;                                                        \
    }

/* Written as a negated in-range test so a NaN fails rather than passing. */
#define RULE_FLT(field, lo, hi)                                              \
    if (!(rules->field >= (float) (lo) && rules->field <= (float) (hi))) {   \
        simRulesWhyFloat(why, whyLen, #field, rules->field, (float) (lo),    \
                         (float) (hi));                                      \
        return false;                                                        \
    }

bool simRulesValidate(const SimRules *rules, char *why, size_t whyLen) {
    if (why != NULL && whyLen > 0) {
        why[0] = '\0';
    }
    if (rules == NULL) {
        if (why != NULL && whyLen > 0) {
            snprintf(why, whyLen, "no rules table");
        }
        return false;
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

    return true;
}

#undef RULE_INT
#undef RULE_INT_MIN
#undef RULE_FLT
