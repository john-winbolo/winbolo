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

#include <math.h>     /* floor / fabs — the ratio a change is snapped to */
#include <stddef.h>   /* offsetof — which rules CTRL_SIM_RULES carries */
#include <stdio.h>
#include <string.h>   /* memcmp — the comparison against the classic table */

#include "control_event.h"  /* CTRL_SIM_RULES_ALL_FIELDS */
#include "platform_types.h"  /* BOLO_STATIC_ASSERT */
#include "sim_rules_names.h" /* SIM_RULE_LIST and the describe kinds */
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
#include "tankexp.h"     /* TK_DAMAGE and the wreck's shape */
#include "treegrow.h"    /* TREEGROW_* / TREE_GROW_* */
#include "floodfill.h"   /* FLOOD_FILL_WAIT */

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
    out->mine_damage_range   = MINE_DAMAGE_RANGE;
    out->mine_fatal_divisor  = MINE_FATAL_DIVISOR;
    out->water_loss_shells   = TANK_WATER_LOSS_SHELLS;
    out->water_loss_mines    = TANK_WATER_LOSS_MINES;
    out->just_fired_ticks    = JUST_FIRED_TICKS;
    out->tree_hide_distance  = MIN_TREEHIDE_DIST;
    out->gunsight_min        = GUNSIGHT_MIN;
    out->gunsight_max        = GUNSIGHT_MAX;
    out->tank_accel_rate     = (float) TANK_ACCELERATE_RATE;
    out->tank_decel_rate     = (float) TANK_TERRAIN_DECEL_RATE;
    out->tank_brake_rate     = (float) TANK_SLOWKEY_RATE;
    out->tank_autoslow_rate  = (float) TANK_AUTOSLOW_SPEED;
    out->tank_min_move       = TANK_MIN_MOVE_SPEED;

    /* ---- Tank collision geometry ---- */
    out->tank_hit_radius         = TANK_HIT_RADIUS;
    out->tank_collision_distance = TANK_COLLISION_DISTANCE;
    out->tank_nudge_threshold    = TANK_NUDGE_THRESHOLD;
    out->tank_nudge_amount       = TANK_NUDGE_AMOUNT;
    out->tank_nudge_iterations   = TANK_MAX_NUDGE_ITERATIONS;
    out->tank_bump_decay_shift   = TANK_BUMP_DECAY_SHIFT;
    out->tank_pill_pickup_inset  = TANK_PILL_PICKUP_INSET;
    out->tank_boat_exit_inset    = TANK_MOVE_BOAT_SUB;
    out->tank_slide_step         = TANK_SLIDE;
    out->tank_wall_glide         = (float) TANK_WALL_GLIDE;

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

    /* ---- Builder walk speeds ---- */
    out->man_speed_road         = MAP_MANSPEED_TROAD;
    out->man_speed_grass        = MAP_MANSPEED_TGRASS;
    out->man_speed_forest       = MAP_MANSPEED_TFOREST;
    out->man_speed_river        = MAP_MANSPEED_TRIVER;
    out->man_speed_swamp        = MAP_MANSPEED_TSWAMP;
    out->man_speed_crater       = MAP_MANSPEED_TCRATER;
    out->man_speed_rubble       = MAP_MANSPEED_TRUBBLE;
    out->man_speed_boat         = MAP_MANSPEED_TBOAT;
    out->man_speed_deep_sea     = MAP_MANSPEED_TDEEPSEA;
    out->man_speed_refuel_base  = MAP_MANSPEED_TREFBASE;

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
    out->lgm_arrive_tolerance     = LGM_MAX_GOAL;
    out->lgm_return_tolerance     = LGM_RETURN_MAX_GOAL;
    out->lgm_pill_drop_search     = LGM_PILL_DROP_SEARCH;
    out->lgm_boat_leave_offset    = LGM_TANKBOAT_LEAVE;
    out->lgm_boat_return_offset   = LGM_TANKBOAT_RETURN;

    /* ---- Pillbox ----
     * pill_max_armour was PILLS_MAX_ARMOUR and PILL_MAX_HEALTH, two names for
     * the same 15; the second is gone and this field is the one spelling. */
    out->pill_max_armour       = PILLS_MAX_ARMOUR;
    out->pill_attack_ticks     = PILLBOX_ATTACK_NORMAL;
    out->pill_attack_min_ticks = PILLBOX_MAX_FIRERATE;
    out->pill_cooldown_ticks   = PILLBOX_COOLDOWN_TIME;
    out->pill_repair_amount    = PILL_REPAIR_AMOUNT;
    out->pill_range            = PILLBOX_RANGE;
    out->pill_shell_damage     = PILLBOX_SHELL_DAMAGE;
    out->pill_angry_divisor    = PILLBOX_ANGRY_DIVISOR;
    out->pill_fire_length      = (float) PILLBOX_FIRE_DISTANCE;
    out->pill_base_defend_range = PILL_BASE_HIT_RANGE;
    out->pill_aim_iterations   = MAX_AIM_ITERATE;

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
    out->tank_explosion_damage       = TK_DAMAGE;
    out->tank_explosion_length       = TK_EXPLODE_LENGTH;
    out->tank_explosion_move         = TK_MOVE_AMOUNT;
    out->tank_explosion_update_ticks = TK_UPDATE_TIME;
    out->tank_explosion_width        = TK_WIDTH_CHECK;
    out->tank_explosion_height       = TK_HEIGHT_CHECK;

    /* ---- Terrain flooding ---- */
    out->flood_fill_ticks          = FLOOD_FILL_WAIT;

    /* ---- Tree growth ---- */
    out->tree_grow_ticks           = TREEGROW_TIME;
    out->tree_grow_initial_ticks   = TREEGROW_INITIAL_TIME;
    out->tree_grow_initial_score   = TREEGROW_INITIAL_SCORE;
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
       are all four bytes and SIM_RULE_LIST names them in this order. The
       index is that list's own index, so simRulesRuleName turns it into the
       rule's name. */
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
 * end — because what one field allows another is a different question.
 *
 * Every arm is written once and asked twice: the server asks all of them, and
 * a client checking a CTRL_SIM_RULES event asks only the ones whose fields
 * that event carries. The two callers share this one body rather than each
 * keeping its own copy of the numbers, because a second copy is a second
 * place a bound can be changed and forgotten.
 *
 * Which arms the carried pass asks is read off CTRL_SIM_RULES' own field
 * list, so a rule added to or dropped from the event changes the answer with
 * it. The client cannot ask every arm: some pairs hold a carried field
 * against one the event leaves behind, and a server that lowered both would
 * be refused by a client still reading the classic value for the half it was
 * never sent. So a pair is asked on the client only when the event carries
 * every field it names. */

/* Where each carried rule sits in the struct. offsetof identifies a field
 * without naming its type, which is what lets one list cover the integer
 * rows and the float ones together. */
#define SIM_RULES_CARRIED_OFFSET(name) offsetof(SimRules, name),
static const size_t simRulesCarriedOffsets[] = {
    CTRL_SIM_RULES_ALL_FIELDS(SIM_RULES_CARRIED_OFFSET)
};
#undef SIM_RULES_CARRIED_OFFSET

static bool simRulesOffsetCarried(size_t offset) {
    size_t i;
    const size_t count =
        sizeof(simRulesCarriedOffsets) / sizeof(simRulesCarriedOffsets[0]);

    for (i = 0; i < count; i++) {
        if (simRulesCarriedOffsets[i] == offset) {
            return true;
        }
    }
    return false;
}

/* Whether CTRL_SIM_RULES carries this rule. */
#define SIM_RULES_CARRIES(field) simRulesOffsetCarried(offsetof(SimRules, field))

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

/* Whether this pass asks a row at all: the server asks every one, the
 * carried pass only the rows whose field rides the event. */
#define RULE_ASKED(field) (!carriedOnly || SIM_RULES_CARRIES(field))

#define RULE_INT(field, lo, hi)                                              \
    if (RULE_ASKED(field) &&                                                 \
        (rules->field < (int32_t) (lo) || rules->field > (int32_t) (hi))) {  \
        simRulesWhyInt(why, whyLen, #field, rules->field, (int32_t) (lo),    \
                       (int32_t) (hi));                                      \
        return SIM_RULES_FAULT_RANGE;                                        \
    }

/* Only the fixed end of a row bounded by another field. */
#define RULE_INT_MIN(field, lo)                                              \
    if (RULE_ASKED(field) && rules->field < (int32_t) (lo)) {                \
        simRulesWhyInt(why, whyLen, #field, rules->field, (int32_t) (lo),    \
                       INT32_MAX);                                           \
        return SIM_RULES_FAULT_RANGE;                                        \
    }

/* One arm per pair of fields. The reason names both sides and the numbers
 * they hold, so a caller reading a refusal can see which of the two has to
 * move rather than only that something is wrong. A pair never changes a
 * value: it refuses the table or it passes it.
 *
 * The first argument says when the pair is asked — PAIR_ASKED2 or
 * PAIR_ASKED3 naming the fields it reads, so the carried pass skips a pair
 * whose other side the event does not carry. */
#define RULE_PAIR(asked, cond, ...)                                          \
    if ((asked) && !(cond)) {                                                \
        if (why != NULL && whyLen > 0) {                                     \
            snprintf(why, whyLen, __VA_ARGS__);                              \
        }                                                                    \
        return SIM_RULES_FAULT_PAIR;                                         \
    }

#define PAIR_ASKED2(a, b)                                                    \
    (!carriedOnly || (SIM_RULES_CARRIES(a) && SIM_RULES_CARRIES(b)))
#define PAIR_ASKED3(a, b, c)                                                 \
    (!carriedOnly || (SIM_RULES_CARRIES(a) && SIM_RULES_CARRIES(b) &&        \
                      SIM_RULES_CARRIES(c)))

/* Written as a negated in-range test so a NaN fails rather than passing. */
#define RULE_FLT(field, lo, hi)                                              \
    if (RULE_ASKED(field) &&                                                 \
        !(rules->field >= (float) (lo) && rules->field <= (float) (hi))) {   \
        simRulesWhyFloat(why, whyLen, #field, rules->field, (float) (lo),    \
                         (float) (hi));                                      \
        return SIM_RULES_FAULT_RANGE;                                        \
    }

/* The one body both passes run. carriedOnly false is the server's whole
 * check; true is the client's, asking only what a CTRL_SIM_RULES event can
 * have brought. */
static SimRulesFault simRulesCheckRows(const SimRules *rules, bool carriedOnly,
                                       char *why, size_t whyLen) {
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
    /* Zero is a mine that only hurts a tank standing exactly on its
       centre, which is a table worth being able to write. */
    RULE_INT(mine_damage_range, 0, 65535)
    /* One leaves a fatal hit at full strength. */
    RULE_INT(mine_fatal_divisor, 1, 255)
    RULE_INT(water_loss_shells, 0, 255)
    RULE_INT(water_loss_mines, 0, 255)
    RULE_INT(just_fired_ticks, 0, 255)
    /* Zero is a wood that hides nothing, which is a coherent table. */
    RULE_INT(tree_hide_distance, 0, 65535)
    RULE_INT(gunsight_min, 1, 255)
    RULE_INT(gunsight_max, 1, 255)
    RULE_FLT(tank_accel_rate, 0.01, 16.0)
    RULE_FLT(tank_decel_rate, 0.01, 16.0)
    RULE_FLT(tank_brake_rate, 0.01, 16.0)
    RULE_FLT(tank_autoslow_rate, 0.01, 16.0)
    RULE_INT(tank_min_move, 0, 255)

    /* The hit circle. Squared at its use sites, which is why the ceiling is
       255 rather than a world-unit range: 255 squared still fits an int. */
    RULE_INT(tank_hit_radius, 1, 255)
    RULE_INT(tank_collision_distance, 0, 65535)
    RULE_INT(tank_nudge_threshold, 0, 65535)
    RULE_INT(tank_nudge_amount, 1, 255)
    RULE_INT(tank_nudge_iterations, 1, 255)
    /* A shift, so its ceiling is what an int32_t can be shifted by. */
    RULE_INT(tank_bump_decay_shift, 0, 31)
    RULE_INT(tank_pill_pickup_inset, 0, 255)
    RULE_INT(tank_boat_exit_inset, 0, 255)
    RULE_INT(tank_slide_step, 0, 255)
    RULE_FLT(tank_wall_glide, 0.0, 1.0)

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

    /* The builder's walk, in the window the tank's own caps use: the
       players[].speed packing saturates at 63 and the man is stored the
       same way. */
    RULE_INT(man_speed_road, 0, 63)
    RULE_INT(man_speed_grass, 0, 63)
    RULE_INT(man_speed_forest, 0, 63)
    RULE_INT(man_speed_river, 0, 63)
    RULE_INT(man_speed_swamp, 0, 63)
    RULE_INT(man_speed_crater, 0, 63)
    RULE_INT(man_speed_rubble, 0, 63)
    RULE_INT(man_speed_boat, 0, 63)
    RULE_INT(man_speed_deep_sea, 0, 63)
    RULE_INT(man_speed_refuel_base, 0, 63)

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
    /* The tolerances are half-widths: the test takes each either way round
       the goal, which is what the two-signed constants used to spell. */
    RULE_INT(lgm_arrive_tolerance, 1, 255)
    RULE_INT(lgm_return_tolerance, 1, 65535)
    RULE_INT(lgm_pill_drop_search, 1, 255)
    RULE_INT(lgm_boat_leave_offset, 0, 255)
    RULE_INT(lgm_boat_return_offset, 0, 255)

    /* ---- Pillbox ---- */
    RULE_INT(pill_max_armour, 1, 255)
    RULE_INT(pill_attack_ticks, 1, 255)
    RULE_INT_MIN(pill_attack_min_ticks, 1)
    /* Both ends of the attack interval are converted, so the pair can be
     * asked here: the fastest a hurt pill fires cannot be slower than the
     * rate an untouched one sits at, or the clamp has no window to land in. */
    /* A pair, though it is asked here rather than in the pairs block below:
       both its sides are pillbox rows and it sits with them. */
    RULE_PAIR(PAIR_ASKED2(pill_attack_min_ticks, pill_attack_ticks),
              rules->pill_attack_min_ticks <= rules->pill_attack_ticks,
              "pill_attack_min_ticks is %ld, above pill_attack_ticks %ld",
              (long) rules->pill_attack_min_ticks,
              (long) rules->pill_attack_ticks)
    RULE_INT(pill_cooldown_ticks, 0, 255)
    RULE_INT_MIN(pill_repair_amount, 1)
    RULE_INT(pill_range, 0, 65535)
    /* Capped by what a pill can hold, so only its own end is fixed here. A
       shell that takes the whole cap is a pill killed by one hit, which is a
       table a scenario may want; one that takes more is the same thing said
       twice. */
    RULE_INT_MIN(pill_shell_damage, 1)
    /* One is a pill that never angers, which is a coherent setting and why
       the floor is one rather than two. */
    RULE_INT(pill_angry_divisor, 1, 255)
    /* The shell's own length, in half map squares, the way a tank's is: the
       gunsight rows are bounded the same way. */
    RULE_FLT(pill_fire_length, 0.5, 127.0)
    /* Zero is a pill that only answers for the square it stands on. */
    RULE_INT(pill_base_defend_range, 0, 255)
    /* The aim solver's step budget. One is a pill that never leads a
       target and fires straight at where it is standing now. */
    RULE_INT(pill_aim_iterations, 1, 65535)
    RULE_PAIR(PAIR_ASKED2(pill_shell_damage, pill_max_armour),
              rules->pill_shell_damage <= rules->pill_max_armour,
              "pill_shell_damage is %ld, above pill_max_armour %ld",
              (long) rules->pill_shell_damage, (long) rules->pill_max_armour)

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
    /* Capped by what a pill can hold, so only its own end is fixed here.
       Zero is a wreck that scorches nothing, which is a table a scenario
       may want. */
    RULE_INT_MIN(tank_explosion_damage, 0)
    RULE_INT(tank_explosion_length, 1, 255)
    RULE_INT(tank_explosion_move, 0, 255)
    RULE_INT(tank_explosion_update_ticks, 1, 255)
    RULE_INT(tank_explosion_width, 0, 255)
    RULE_INT(tank_explosion_height, 0, 255)
    RULE_PAIR(PAIR_ASKED2(tank_explosion_damage, pill_max_armour),
              rules->tank_explosion_damage <= rules->pill_max_armour,
              "tank_explosion_damage is %ld, above pill_max_armour %ld",
              (long) rules->tank_explosion_damage,
              (long) rules->pill_max_armour)

    /* ---- Terrain flooding ---- */
    RULE_INT(flood_fill_ticks, 1, 255)

    /* ---- Tree growth ---- */
    RULE_INT(tree_grow_ticks, 1, INT32_MAX)
    RULE_INT(tree_grow_initial_ticks, 1, INT32_MAX)
    /* The score the weighted draw starts and resets from, so how long the
       map waits for its first tree. Negative by design, and the weights
       below share its window. */
    RULE_INT(tree_grow_initial_score, -32768, 32767)
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
    RULE_PAIR(PAIR_ASKED2(base_capture_armour, base_full_armour),
              rules->base_capture_armour <= rules->base_full_armour,
              "base_capture_armour is %ld, above base_full_armour %ld",
              (long) rules->base_capture_armour,
              (long) rules->base_full_armour)

    RULE_PAIR(PAIR_ASKED2(base_hit_armour, base_capture_armour),
              rules->base_hit_armour <= rules->base_capture_armour,
              "base_hit_armour is %ld, above base_capture_armour %ld",
              (long) rules->base_hit_armour,
              (long) rules->base_capture_armour)

    RULE_PAIR(PAIR_ASKED2(base_min_armour, base_full_armour),
              rules->base_min_armour <= rules->base_full_armour,
              "base_min_armour is %ld, above base_full_armour %ld",
              (long) rules->base_min_armour, (long) rules->base_full_armour)
    RULE_PAIR(PAIR_ASKED2(base_min_shells, base_full_shells),
              rules->base_min_shells <= rules->base_full_shells,
              "base_min_shells is %ld, above base_full_shells %ld",
              (long) rules->base_min_shells, (long) rules->base_full_shells)
    RULE_PAIR(PAIR_ASKED2(base_min_mines, base_full_mines),
              rules->base_min_mines <= rules->base_full_mines,
              "base_min_mines is %ld, above base_full_mines %ld",
              (long) rules->base_min_mines, (long) rules->base_full_mines)

    /* A base hands out what it holds above its reserve, so a give bigger
     * than that gap would take the base under its own floor. */
    RULE_PAIR(PAIR_ASKED3(base_armour_give, base_full_armour, base_min_armour),
              rules->base_armour_give <=
                  rules->base_full_armour - rules->base_min_armour,
              "base_armour_give is %ld, above base_full_armour %ld less "
              "base_min_armour %ld",
              (long) rules->base_armour_give, (long) rules->base_full_armour,
              (long) rules->base_min_armour)
    RULE_PAIR(PAIR_ASKED3(base_shells_give, base_full_shells, base_min_shells),
              rules->base_shells_give <=
                  rules->base_full_shells - rules->base_min_shells,
              "base_shells_give is %ld, above base_full_shells %ld less "
              "base_min_shells %ld",
              (long) rules->base_shells_give, (long) rules->base_full_shells,
              (long) rules->base_min_shells)
    RULE_PAIR(PAIR_ASKED3(base_mines_give, base_full_mines, base_min_mines),
              rules->base_mines_give <=
                  rules->base_full_mines - rules->base_min_mines,
              "base_mines_give is %ld, above base_full_mines %ld less "
              "base_min_mines %ld",
              (long) rules->base_mines_give, (long) rules->base_full_mines,
              (long) rules->base_min_mines)

    /* A job the man can never pay for is one no player can order. */
    RULE_PAIR(PAIR_ASKED2(lgm_cost_road, tank_full_trees),
              rules->lgm_cost_road <= rules->tank_full_trees,
              "lgm_cost_road is %ld, above tank_full_trees %ld",
              (long) rules->lgm_cost_road, (long) rules->tank_full_trees)
    RULE_PAIR(PAIR_ASKED2(lgm_cost_building, tank_full_trees),
              rules->lgm_cost_building <= rules->tank_full_trees,
              "lgm_cost_building is %ld, above tank_full_trees %ld",
              (long) rules->lgm_cost_building, (long) rules->tank_full_trees)
    RULE_PAIR(PAIR_ASKED2(lgm_cost_repair_building, tank_full_trees),
              rules->lgm_cost_repair_building <= rules->tank_full_trees,
              "lgm_cost_repair_building is %ld, above tank_full_trees %ld",
              (long) rules->lgm_cost_repair_building,
              (long) rules->tank_full_trees)
    RULE_PAIR(PAIR_ASKED2(lgm_cost_boat, tank_full_trees),
              rules->lgm_cost_boat <= rules->tank_full_trees,
              "lgm_cost_boat is %ld, above tank_full_trees %ld",
              (long) rules->lgm_cost_boat, (long) rules->tank_full_trees)
    RULE_PAIR(PAIR_ASKED2(lgm_cost_pill_new, tank_full_trees),
              rules->lgm_cost_pill_new <= rules->tank_full_trees,
              "lgm_cost_pill_new is %ld, above tank_full_trees %ld",
              (long) rules->lgm_cost_pill_new, (long) rules->tank_full_trees)

    /* The repair order takes a whole load at once, so what it costs is the
     * cost times the load and that is what the tank has to be able to
     * carry — a script setting the two apart multiplies them. */
    RULE_PAIR(PAIR_ASKED3(lgm_cost_pill_repair, lgm_pill_repair_load,
                        tank_full_trees),
              (int64_t) rules->lgm_cost_pill_repair *
                  rules->lgm_pill_repair_load <= rules->tank_full_trees,
              "lgm_cost_pill_repair %ld times lgm_pill_repair_load %ld is "
              "above tank_full_trees %ld",
              (long) rules->lgm_cost_pill_repair,
              (long) rules->lgm_pill_repair_load,
              (long) rules->tank_full_trees)

    RULE_PAIR(PAIR_ASKED2(lgm_cost_mine, tank_full_mines),
              rules->lgm_cost_mine <= rules->tank_full_mines,
              "lgm_cost_mine is %ld, above tank_full_mines %ld",
              (long) rules->lgm_cost_mine, (long) rules->tank_full_mines)

    /* One delivery has to be able to finish a pill on nothing, or the man
     * walks out, spends the trees and leaves the pill short. */
    RULE_PAIR(PAIR_ASKED3(lgm_pill_repair_load, pill_repair_amount,
                        pill_max_armour),
              (int64_t) rules->lgm_pill_repair_load *
                  rules->pill_repair_amount >= rules->pill_max_armour,
              "lgm_pill_repair_load %ld times pill_repair_amount %ld is "
              "below pill_max_armour %ld",
              (long) rules->lgm_pill_repair_load,
              (long) rules->pill_repair_amount,
              (long) rules->pill_max_armour)
    RULE_PAIR(PAIR_ASKED2(pill_repair_amount, pill_max_armour),
              rules->pill_repair_amount <= rules->pill_max_armour,
              "pill_repair_amount is %ld, above pill_max_armour %ld",
              (long) rules->pill_repair_amount,
              (long) rules->pill_max_armour)

    /* The gunsight is a range the player walks between two ends, so a
     * minimum above the maximum leaves nowhere to stand: tankCreate would
     * start the tank outside it and neither the increase nor the decrease
     * key could bring it back. */
    RULE_PAIR(PAIR_ASKED2(gunsight_min, gunsight_max),
              rules->gunsight_min <= rules->gunsight_max,
              "gunsight_min is %ld, above gunsight_max %ld",
              (long) rules->gunsight_min, (long) rules->gunsight_max)

    /* shellLifeTicks takes the start offset off the life budget, so an
     * offset bigger than the shortest shot's whole budget gives a shell
     * that dies where it is born. Measured at gunsight_min, the shortest
     * shot a player can take. The product is 64-bit because neither field
     * is bounded above by the other. */
    RULE_PAIR(PAIR_ASKED3(shell_start_add, shell_life, gunsight_min),
              (int64_t) rules->shell_start_add <=
                  (int64_t) rules->shell_life * rules->gunsight_min / 2,
              "shell_start_add is %ld, above shell_life %ld times "
              "gunsight_min %ld halved",
              (long) rules->shell_start_add, (long) rules->shell_life,
              (long) rules->gunsight_min)

    /* And the other end: the tank fires at sightLen / 2 and shellLifeTicks
     * stores the answer in the shell record's BYTE, so the longest shot a
     * player can take has to still fit. Computed the way shellLifeTicks
     * computes it, so the check and the code cannot disagree. */
    RULE_PAIR(PAIR_ASKED3(shell_life, gunsight_max, shell_start_add),
              (int64_t) rules->shell_life * rules->gunsight_max / 2 -
                      rules->shell_start_add + 1 <= 255,
              "shell_life %ld times gunsight_max %ld halved, less "
              "shell_start_add %ld, plus 1, is above 255",
              (long) rules->shell_life, (long) rules->gunsight_max,
              (long) rules->shell_start_add)

    return SIM_RULES_OK;
}

#undef RULE_ASKED
#undef RULE_INT
#undef RULE_INT_MIN
#undef RULE_FLT
#undef RULE_PAIR
#undef PAIR_ASKED2
#undef PAIR_ASKED3

SimRulesFault simRulesCheck(const SimRules *rules, char *why, size_t whyLen) {
    return simRulesCheckRows(rules, false, why, whyLen);
}

SimRulesFault simRulesCheckCarried(const SimRules *rules, char *why,
                                   size_t whyLen) {
    return simRulesCheckRows(rules, true, why, whyLen);
}

/* For the callers that only want to know whether the table is usable. */
bool simRulesValidate(const SimRules *rules, char *why, size_t whyLen) {
    return simRulesCheck(rules, why, whyLen) == SIM_RULES_OK;
}

/* ---- The rules by index --------------------------------------------------
 *
 * Name, unit and value kind come straight off SIM_RULE_LIST, so the three
 * tables are three readings of the one list and cannot fall out of step with
 * each other or with the enum.
 *
 * A value is read out of a filled classic table as a four-byte word rather
 * than by name: every field is four bytes and the list's order is the
 * struct's, which is what simRulesFirstDifference already relies on. The
 * assertion below is what keeps that true — a field added to SimRules
 * without a row in the list makes the sizes disagree and does not compile. */

BOLO_STATIC_ASSERT(sizeof(SimRules) == (size_t)SIM_RULE_COUNT * 4,
                   sim_rules_struct_is_the_whole_rule_list);

#define SIM_RULE_NAME_ROW(name, kind, unit) #name,
static const char *const simRuleNames[] = {
    SIM_RULE_LIST(SIM_RULE_NAME_ROW)
};
#undef SIM_RULE_NAME_ROW

#define SIM_RULE_UNIT_ROW(name, kind, unit) unit,
static const SimRuleUnit simRuleUnits[] = {
    SIM_RULE_LIST(SIM_RULE_UNIT_ROW)
};
#undef SIM_RULE_UNIT_ROW

#define SIM_RULE_KIND_ROW(name, kind, unit) kind,
static const SimRuleValueKind simRuleValueKinds[] = {
    SIM_RULE_LIST(SIM_RULE_KIND_ROW)
};
#undef SIM_RULE_KIND_ROW

/* Whether an index names a rule at all. Every accessor asks this first: a
 * caller holding a rule number off a wire or out of a manifest has one that
 * may name nothing, and each of them answers that case rather than reading
 * past the end of a table. */
static bool simRulesIndexInRange(int rule) {
    return rule >= 0 && rule < (int)SIM_RULE_COUNT;
}

int simRulesRuleCount(void) {
    return (int)SIM_RULE_COUNT;
}

const char *simRulesRuleName(int rule) {
    if (!simRulesIndexInRange(rule)) {
        return "";
    }
    return simRuleNames[rule];
}

int simRulesRuleIndex(const char *name) {
    int i;

    if (name == NULL) {
        return -1;
    }
    for (i = 0; i < (int)SIM_RULE_COUNT; i++) {
        if (strcmp(simRuleNames[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

SimRuleUnit simRulesRuleUnit(int rule) {
    if (!simRulesIndexInRange(rule)) {
        return SIM_RULE_UNIT_COUNT;
    }
    return simRuleUnits[rule];
}

SimRuleValueKind simRulesRuleValueKind(int rule) {
    if (!simRulesIndexInRange(rule)) {
        return SIM_RULE_VALUE_INT;
    }
    return simRuleValueKinds[rule];
}

double simRulesClassicValue(int rule) {
    SimRules       classic;
    const int32_t *words;

    if (!simRulesIndexInRange(rule)) {
        return 0.0;
    }
    simRulesClassic(&classic);
    words = (const int32_t *) (const void *) &classic;
    if (simRuleValueKinds[rule] == SIM_RULE_VALUE_FLOAT) {
        float rate;
        /* Copied rather than cast through a pointer: the word is an int32_t
           here and the field a float, and only a copy reads the bits as the
           type the field holds without depending on how the two alias. */
        memcpy(&rate, &words[rule], sizeof(rate));
        return (double) rate;
    }
    return (double) words[rule];
}

/* ---- What a value does to a rule -----------------------------------------
 *
 * A kind and a number, never a string: the frontends that ask this question
 * each have their own way of drawing it and their own language, and this
 * file is below both. src/gui/sim_rules_phrase.h renders the pair.
 *
 * A ratio is only offered where it says something true. Below 1.5x a
 * difference is the clearer answer — "+2" rather than "1.2x as many" — and a
 * default or a value at or below zero has no ratio to take at all, which is
 * what keeps a weighted-draw entry from being described as a multiple of
 * itself. Above that the ratio snaps to a whole number when it lands within
 * five percent of one, so a rate that works out at 2.02 reads as twice
 * rather than as 2.0, and otherwise keeps one decimal. */

#define SIM_RULE_RATIO_MIN   1.5   /* below this a difference says more */
#define SIM_RULE_SNAP_BAND   0.05  /* how near a whole multiple has to be */

SimRuleChange simRulesDescribeValue(SimRuleUnit unit, double classic,
                                    double value) {
    SimRuleChange out;
    double        ratio;
    double        whole;

    out.kind   = SIM_RULE_CHANGE_UNCHANGED;
    out.number = 0.0;

    if (value == classic) {
        return out;
    }
    if (unit == SIM_RULE_UNIT_FLAG) {
        out.kind = (value != 0.0) ? SIM_RULE_CHANGE_ON : SIM_RULE_CHANGE_OFF;
        return out;
    }
    if (unit == SIM_RULE_UNIT_CONSTANT_BY_DESIGN) {
        out.kind   = SIM_RULE_CHANGE_RAW;
        out.number = value;
        return out;
    }
    if (classic <= 0.0 || value <= 0.0) {
        out.kind   = SIM_RULE_CHANGE_DELTA;
        out.number = value - classic;
        return out;
    }

    ratio = (value > classic) ? value / classic : classic / value;
    if (ratio < SIM_RULE_RATIO_MIN) {
        out.kind   = SIM_RULE_CHANGE_DELTA;
        out.number = value - classic;
        return out;
    }

    whole = floor(ratio + 0.5);
    if (whole >= 2.0 && fabs(ratio - whole) <= whole * SIM_RULE_SNAP_BAND) {
        ratio = whole;
    } else {
        ratio = floor(ratio * 10.0 + 0.5) / 10.0;
    }

    switch (unit) {
        case SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER:
            out.kind = (value < classic) ? SIM_RULE_CHANGE_FASTER
                                         : SIM_RULE_CHANGE_SLOWER;
            break;
        case SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER:
            out.kind = (value > classic) ? SIM_RULE_CHANGE_FASTER
                                         : SIM_RULE_CHANGE_SLOWER;
            break;
        case SIM_RULE_UNIT_COUNT:
        case SIM_RULE_UNIT_PERCENT:
        default:
            out.kind = (value > classic) ? SIM_RULE_CHANGE_MORE
                                         : SIM_RULE_CHANGE_FEWER;
            break;
    }
    out.number = ratio;
    return out;
}

SimRuleChange simRulesDescribeChange(int rule, double value) {
    SimRuleChange out;

    if (!simRulesIndexInRange(rule)) {
        /* No rule, so nothing has changed about one. */
        out.kind   = SIM_RULE_CHANGE_UNCHANGED;
        out.number = 0.0;
        return out;
    }
    if (simRuleValueKinds[rule] == SIM_RULE_VALUE_FLOAT) {
        /* What the field would hold, not what was typed: the field is a
           float and the classic value is read out of one, so a value that
           only differs past float precision is the same value. */
        value = (double) (float) value;
    }
    return simRulesDescribeValue(simRuleUnits[rule], simRulesClassicValue(rule),
                                 value);
}
