/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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
#include "sounddist.h"   /* SDIST_SOFT / SDIST_NONE */
#include "starts.h"      /* START_* — the spawn-safety defaults */

/* The rules event leaves building_life off the wire while it is classic and
   decodes a body without it as this value, so the two must agree. */
BOLO_STATIC_ASSERT(CTRL_SIM_RULES_BUILDING_LIFE_CLASSIC == BUILDING_LIFE,
                   ctrl_sim_rules_building_life_classic_matches);

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
    out->pill_massage_range    = PILLBOX_MASSAGE_RANGE;
    out->pill_massage_cosine   = (float) PILLBOX_MASSAGE_COSINE;

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
    out->base_status_range        = BASE_STATUS_RANGE;
    out->base_reveal_range        = BASE_PREDICT_REVEAL_RANGE;

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

    /* ---- Spawning ---- */
    out->start_tank_range          = START_TANK_RANGE;
    out->start_pill_range          = START_PILL_RANGE;
    out->start_base_range          = START_BASE_RANGE;
    out->start_spawn_separation    = START_SPAWN_SEPARATION;
    out->start_scatter_max         = START_SCATTER_MAX;
    out->start_neutral_threshold_pct = START_NEUTRAL_THRESHOLD_PCT;

    /* ---- Hearing ---- */
    out->sound_soft_range          = SDIST_SOFT;
    out->sound_none_range          = SDIST_NONE;

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

    /* ---- Pillbox shell cap ---- */
    out->pill_shell_cap          = PILLBOX_SHELL_CAP;
    out->pill_max_shells_at_tank = PILLBOX_MAX_SHELLS_AT_TANK;

    /* ---- Base defence shape ---- */
    out->pill_base_defend_shape  = PILL_BASE_HIT_SHAPE;

    /* ---- Mac Bolo shell push ---- */
    out->tank_slide_mac          = TANK_SLIDE_MAC;
    out->tank_slide_armour_bonus = TANK_SLIDE_ARMOUR_BONUS;
    out->pill_aim_mac            = 0;
    out->tank_collision_mac      = 0;

    /* ---- Deep sea ---- */
    out->tank_deep_sea_safe      = 0;
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

/* ---- The bounds ---------------------------------------------------------
 *
 * Every fixed bound, one row per rule, in the order SIM_RULE_LIST names
 * them. The arms below are generated from this list and so is the table
 * simRulesRuleRange reads, so a number is written down once and a refusal
 * and the range a form shows cannot say different things.
 *
 * The shape says how a row is read: INT is a whole-number row with both
 * ends fixed, FLT a rate row with both ends fixed, and INTMIN a row with a
 * floor and no fixed ceiling — what caps it is another rule, or nothing. An
 * INTMIN row carries INT32_MAX in the hi column because that is the ceiling
 * simRulesWhyInt prints for it; the shape rather than that number is what
 * tells simRulesRuleRange the row has no fixed ceiling of its own. */
#define SIM_RULE_BOUND_LIST(X)                                               \
    /* Tank */                                                               \
    X(tank_reload_ticks,         INT,    0,      255)                        \
    X(tank_full_shells,          INT,    0,      255)                        \
    X(tank_full_mines,           INT,    0,      255)                        \
    X(tank_full_trees,           INT,    0,      255)                        \
    X(tank_full_armour,          INT,    0,      255)                        \
    X(tank_death_ticks,          INT,    0,      65535)                      \
    X(tank_water_ticks,          INT,    1,      255)                        \
    X(shell_damage,              INT,    1,      255)                        \
    X(mine_damage,               INT,    1,      255)                        \
    /* Zero is a mine that only hurts a tank standing exactly on its centre, \
       which is a table worth being able to write. */                        \
    X(mine_damage_range,         INT,    0,      65535)                      \
    /* The blow keeps all but one part in this many, so a big divisor        \
       leaves a fatal hit at nearly full strength and one takes the whole    \
       of it off. */                                                         \
    X(mine_fatal_divisor,        INT,    1,      255)                        \
    X(water_loss_shells,         INT,    0,      255)                        \
    X(water_loss_mines,          INT,    0,      255)                        \
    X(just_fired_ticks,          INT,    0,      255)                        \
    /* Zero is a wood that hides nothing, which is a coherent table. */      \
    X(tree_hide_distance,        INT,    0,      65535)                      \
    X(gunsight_min,              INT,    1,      255)                        \
    X(gunsight_max,              INT,    1,      255)                        \
    X(tank_accel_rate,           FLT,    0.01,   16.0)                       \
    X(tank_decel_rate,           FLT,    0.01,   16.0)                       \
    X(tank_brake_rate,           FLT,    0.01,   16.0)                       \
    X(tank_autoslow_rate,        FLT,    0.01,   16.0)                       \
    X(tank_min_move,             INT,    0,      255)                        \
    /* Tank collision geometry */                                            \
    X(tank_hit_radius,           INT,    1,      255)                        \
    X(tank_collision_distance,   INT,    0,      65535)                      \
    X(tank_nudge_threshold,      INT,    0,      65535)                      \
    X(tank_nudge_amount,         INT,    1,      255)                        \
    X(tank_nudge_iterations,     INT,    1,      255)                        \
    /* A shift, so its ceiling is what an int32_t can be shifted by. */      \
    X(tank_bump_decay_shift,     INT,    0,      31)                         \
    X(tank_pill_pickup_inset,    INT,    0,      255)                        \
    X(tank_boat_exit_inset,      INT,    0,      255)                        \
    /* The push moves the tank without a wall check, and at 64 or more its   \
       first tick carries it a quarter of a map square or further before     \
       the building nudge looks. */                                          \
    X(tank_slide_step,           INT,    0,      63)                         \
    X(tank_wall_glide,           FLT,    0.0,    1.0)                        \
    /* Terrain speed caps: the players[].speed packing saturates at 63 */    \
    X(speed_road,                INT,    0,      63)                         \
    X(speed_grass,               INT,    0,      63)                         \
    X(speed_forest,              INT,    0,      63)                         \
    X(speed_river,               INT,    0,      63)                         \
    X(speed_swamp,               INT,    0,      63)                         \
    X(speed_crater,              INT,    0,      63)                         \
    X(speed_rubble,              INT,    0,      63)                         \
    X(speed_boat,                INT,    0,      63)                         \
    X(speed_deep_sea,            INT,    0,      63)                         \
    X(speed_refuel_base,         INT,    0,      63)                         \
    /* Terrain turn rates */                                                 \
    X(turn_road,                 FLT,    0.0,    16.0)                       \
    X(turn_grass,                FLT,    0.0,    16.0)                       \
    X(turn_forest,               FLT,    0.0,    16.0)                       \
    X(turn_river,                FLT,    0.0,    16.0)                       \
    X(turn_swamp,                FLT,    0.0,    16.0)                       \
    X(turn_crater,               FLT,    0.0,    16.0)                       \
    X(turn_rubble,               FLT,    0.0,    16.0)                       \
    X(turn_boat,                 FLT,    0.0,    16.0)                       \
    X(turn_deep_sea,             FLT,    0.0,    16.0)                       \
    X(turn_refuel_base,          FLT,    0.0,    16.0)                       \
    /* Terrain: the builder's walk, in the window the tank's own caps use */ \
    X(man_speed_road,            INT,    0,      63)                         \
    X(man_speed_grass,           INT,    0,      63)                         \
    X(man_speed_forest,          INT,    0,      63)                         \
    X(man_speed_river,           INT,    0,      63)                         \
    X(man_speed_swamp,           INT,    0,      63)                         \
    X(man_speed_crater,          INT,    0,      63)                         \
    X(man_speed_rubble,          INT,    0,      63)                         \
    X(man_speed_boat,            INT,    0,      63)                         \
    X(man_speed_deep_sea,        INT,    0,      63)                         \
    X(man_speed_refuel_base,     INT,    0,      63)                         \
    /* Shells */                                                             \
    X(shell_life,                INT,    1,      255)                        \
    X(shell_speed,               INT,    1,      255)                        \
    X(shell_start_add,           INTMIN, 0,      INT32_MAX)                  \
    /* Builder */                                                            \
    X(lgm_build_ticks,           INT,    0,      255)                        \
    X(lgm_cost_road,             INTMIN, 0,      INT32_MAX)                  \
    X(lgm_cost_building,         INTMIN, 0,      INT32_MAX)                  \
    X(lgm_cost_repair_building,  INTMIN, 0,      INT32_MAX)                  \
    X(lgm_cost_pill_repair,      INTMIN, 0,      INT32_MAX)                  \
    X(lgm_cost_boat,             INTMIN, 0,      INT32_MAX)                  \
    X(lgm_cost_pill_new,         INTMIN, 0,      INT32_MAX)                  \
    X(lgm_cost_mine,             INTMIN, 0,      INT32_MAX)                  \
    X(lgm_pill_repair_load,      INT,    1,      255)                        \
    X(lgm_gather_trees,          INT,    1,      255)                        \
    X(lgm_helicopter_speed,      INT,    1,      255)                        \
    /* The tolerances are half-widths: the test takes each either way round  \
       the goal, which is what the two-signed constants used to spell. */    \
    X(lgm_arrive_tolerance,      INT,    1,      255)                        \
    X(lgm_return_tolerance,      INT,    1,      65535)                      \
    X(lgm_pill_drop_search,      INT,    1,      255)                        \
    X(lgm_boat_leave_offset,     INT,    0,      255)                        \
    X(lgm_boat_return_offset,    INT,    0,      255)                        \
    /* Pillbox */                                                            \
    X(pill_max_armour,           INT,    1,      255)                        \
    X(pill_attack_ticks,         INT,    1,      255)                        \
    X(pill_attack_min_ticks,     INTMIN, 1,      INT32_MAX)                  \
    X(pill_cooldown_ticks,       INT,    0,      255)                        \
    X(pill_repair_amount,        INTMIN, 1,      INT32_MAX)                  \
    X(pill_range,                INT,    0,      65535)                      \
    /* Capped by what a pill can hold, so only its own end is fixed here. A  \
       shell that takes the whole cap is a pill killed by one hit, which is  \
       a table a scenario may want; one that takes more is the same thing    \
       said twice. */                                                        \
    X(pill_shell_damage,         INTMIN, 1,      INT32_MAX)                  \
    /* One is a pill that never angers, which is why the floor is one        \
       rather than two. */                                                   \
    X(pill_angry_divisor,        INT,    1,      255)                        \
    /* The shell's own length, in half map squares, the way a tank's is:     \
       the gunsight rows are bounded the same way. */                        \
    X(pill_fire_length,          FLT,    0.5,    127.0)                      \
    /* Zero is a pill that only answers for the square it stands on, or      \
       with the circle's exclusive edge, one that never answers at all. */   \
    X(pill_base_defend_range,    INT,    0,      255)                        \
    /* The aim solver's step budget. One is a pill that never leads a        \
       target and fires straight at where it is standing now. */             \
    X(pill_aim_iterations,       INT,    1,      65535)                      \
    /* Zero is a pillbox that leads every target with the solver, so it is   \
       also the switch that takes the sloppy close-range aim out. */         \
    X(pill_massage_range,        INT,    0,      65535)                      \
    /* A cosine, so zero is a pillbox that aims true at any close tank and   \
       one is a pillbox that aims sloppily at all of them. */                \
    X(pill_massage_cosine,       FLT,    0.0,    1.0)                        \
    /* Base */                                                               \
    X(base_full_armour,          INT,    0,      255)                        \
    X(base_full_shells,          INT,    0,      255)                        \
    X(base_full_mines,           INT,    0,      255)                        \
    X(base_capture_armour,       INTMIN, 0,      INT32_MAX)                  \
    X(base_hit_armour,           INTMIN, 0,      INT32_MAX)                  \
    X(base_min_armour,           INTMIN, 0,      INT32_MAX)                  \
    X(base_min_shells,           INTMIN, 0,      INT32_MAX)                  \
    X(base_min_mines,            INTMIN, 0,      INT32_MAX)                  \
    X(base_armour_give,          INTMIN, 0,      INT32_MAX)                  \
    X(base_shells_give,          INTMIN, 0,      INT32_MAX)                  \
    X(base_mines_give,           INTMIN, 0,      INT32_MAX)                  \
    X(base_refuel_armour_ticks,  INT,    1,      255)                        \
    X(base_refuel_shells_ticks,  FLT,    0.5,    255.0)                      \
    X(base_refuel_mines_ticks,   FLT,    0.5,    255.0)                      \
    X(base_regen_ticks,          INTMIN, 1,      INT32_MAX)                  \
    X(base_status_range,         INT,    0,      65535)                      \
    X(base_reveal_range,         INT,    0,      65535)                      \
    /* Terrain destruction and explosions */                                 \
    X(building_life,             INT,    1,      255)                        \
    X(rubble_life,               INT,    1,      255)                        \
    X(grass_life,                INT,    1,      255)                        \
    X(swamp_life,                INT,    1,      255)                        \
    X(mine_fuse_ticks,           INT,    1,      255)                        \
    X(big_explosion_threshold,   INT,    0,      510)                        \
    /* Capped by what a pill can hold, so only its own end is fixed here.    \
       Zero is a wreck that scorches nothing, which is a table a scenario    \
       may want. */                                                          \
    X(tank_explosion_damage,     INTMIN, 0,      INT32_MAX)                  \
    X(tank_explosion_length,     INT,    1,      255)                        \
    X(tank_explosion_move,       INT,    0,      255)                        \
    X(tank_explosion_update_ticks, INT,    1,      255)                      \
    X(tank_explosion_width,      INT,    0,      255)                        \
    X(tank_explosion_height,     INT,    0,      255)                        \
    /* Spawning. Zero on any of the three ranges is a spawn that does not    \
       care what is standing there. */                                       \
    X(start_tank_range,          INT,    0,      255)                        \
    X(start_pill_range,          INT,    0,      255)                        \
    X(start_base_range,          INT,    0,      255)                        \
    X(start_spawn_separation,    INT,    0,      255)                        \
    X(start_scatter_max,         INT,    1,      65535)                      \
    X(start_neutral_threshold_pct, INT,    0,      100)                      \
    /* Hearing */                                                            \
    X(sound_soft_range,          INT,    0,      255)                        \
    X(sound_none_range,          INT,    0,      255)                        \
    /* Terrain flooding */                                                   \
    X(flood_fill_ticks,          INT,    1,      255)                        \
    /* Tree growth */                                                        \
    X(tree_grow_ticks,           INTMIN, 1,      INT32_MAX)                  \
    X(tree_grow_initial_ticks,   INTMIN, 1,      INT32_MAX)                  \
    /* The score the weighted draw starts and resets from, so how long the   \
       map waits for its first tree. Negative by design, and the weights     \
       below share its window. */                                            \
    X(tree_grow_initial_score,   INT,    -32768, 32767)                      \
    X(tree_weight_forest,        INT,    -32768, 32767)                      \
    X(tree_weight_grass,         INT,    -32768, 32767)                      \
    X(tree_weight_river,         INT,    -32768, 32767)                      \
    X(tree_weight_boat,          INT,    -32768, 32767)                      \
    X(tree_weight_deep_sea,      INT,    -32768, 32767)                      \
    X(tree_weight_swamp,         INT,    -32768, 32767)                      \
    X(tree_weight_rubble,        INT,    -32768, 32767)                      \
    X(tree_weight_building,      INT,    -32768, 32767)                      \
    X(tree_weight_half_building, INT,    -32768, 32767)                      \
    X(tree_weight_crater,        INT,    -32768, 32767)                      \
    X(tree_weight_road,          INT,    -32768, 32767)                      \
    X(tree_weight_mine,          INT,    -32768, 32767)                      \
    /* Pillbox shell cap */                                                 \
    X(pill_shell_cap,            INT,    0,      1)                          \
    X(pill_max_shells_at_tank,   INT,    1,      255)                        \
    /* Base defence shape: PILL_BASE_HIT_SQUARE or PILL_BASE_HIT_CIRCLE. */  \
    X(pill_base_defend_shape,    INT,    0,      1)                          \
    /* Mac Bolo shell push */                                               \
    X(tank_slide_mac,            INT,    0,      1)                          \
    X(tank_slide_armour_bonus,   INT,    0,      255)                        \
    X(pill_aim_mac,              INT,    0,      1) \
    X(tank_collision_mac,        INT,    0,      1)                          \
    /* Deep sea */                                                          \
    X(tank_deep_sea_safe,        INT,    0,      1)

/* The rules a single other rule also caps, as the pairs below hold them.
 * Only a direct field against other_field test is here: where a ceiling is
 * an expression of more than one rule — shell_start_add against shell_life
 * times gunsight_min halved, a give row against a full row less a min row —
 * no one rule names it, and those rows answer cappedBy -1 with the reason
 * left to the words the refusal itself carries. */
#define SIM_RULE_CAPPED_BY_LIST(X)                                           \
    X(pill_attack_min_ticks,    pill_attack_ticks)                           \
    X(base_capture_armour,      base_full_armour)                            \
    X(base_hit_armour,          base_capture_armour)                         \
    X(base_min_armour,          base_full_armour)                            \
    X(base_min_shells,          base_full_shells)                            \
    X(base_min_mines,           base_full_mines)                             \
    X(lgm_cost_road,            tank_full_trees)                             \
    X(lgm_cost_building,        tank_full_trees)                             \
    X(lgm_cost_repair_building, tank_full_trees)                             \
    X(lgm_cost_boat,            tank_full_trees)                             \
    X(lgm_cost_pill_new,        tank_full_trees)                             \
    X(lgm_cost_mine,            tank_full_mines)                             \
    X(pill_repair_amount,       pill_max_armour)                             \
    X(pill_shell_damage,        pill_max_armour)                             \
    X(tank_explosion_damage,    pill_max_armour)                             \
    X(sound_soft_range,         sound_none_range)                            \
    X(gunsight_min,             gunsight_max)

/* A row's arm, chosen by its shape: the three above are the three bodies,
 * and the one for a row with no fixed ceiling takes only the floor. */
#define SIM_RULE_ARM_INT(field, lo, hi)    RULE_INT(field, lo, hi)
#define SIM_RULE_ARM_INTMIN(field, lo, hi) RULE_INT_MIN(field, lo)
#define SIM_RULE_ARM_FLT(field, lo, hi)    RULE_FLT(field, lo, hi)
#define SIM_RULE_BOUND_ARM(field, shape, lo, hi)                             \
    SIM_RULE_ARM_##shape(field, lo, hi)

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

    SIM_RULE_BOUND_LIST(SIM_RULE_BOUND_ARM)

    /* ---- Pairs ----------------------------------------------------------
     *
     * What one field allows another, now that both sides of each of these is
     * a rule. The rows above check each field against its own fixed bounds;
     * these check the rows against each other, in an order that lets a later
     * arm rely on an earlier one — base_min_* is known to be inside
     * base_full_* before the give rows subtract the two. The two products are
     * computed in 64 bits because a cost and a load are each bounded below
     * and not above, so multiplying them in int32_t could overflow before the
     * comparison. */

    /* Both ends of the attack interval are converted, so the pair can be
     * asked here: the fastest a hurt pill fires cannot be slower than the
     * rate an untouched one sits at, or the clamp has no window to land in.
     * It leads the block because it stands on its own, where the rows under
     * it are ordered so that a later one can take an earlier one as read. */
    RULE_PAIR(PAIR_ASKED2(pill_attack_min_ticks, pill_attack_ticks),
              rules->pill_attack_min_ticks <= rules->pill_attack_ticks,
              "pill_attack_min_ticks is %ld, above pill_attack_ticks %ld",
              (long) rules->pill_attack_min_ticks,
              (long) rules->pill_attack_ticks)

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

    /* What a pill can be hit by, against what it can hold. A shot taking the
     * whole cap is a pill killed by one hit, which is a table a scenario may
     * want; one taking more is the same thing said twice. */
    RULE_PAIR(PAIR_ASKED2(pill_shell_damage, pill_max_armour),
              rules->pill_shell_damage <= rules->pill_max_armour,
              "pill_shell_damage is %ld, above pill_max_armour %ld",
              (long) rules->pill_shell_damage, (long) rules->pill_max_armour)
    RULE_PAIR(PAIR_ASKED2(tank_explosion_damage, pill_max_armour),
              rules->tank_explosion_damage <= rules->pill_max_armour,
              "tank_explosion_damage is %ld, above pill_max_armour %ld",
              (long) rules->tank_explosion_damage,
              (long) rules->pill_max_armour)

    /* The near band sits inside the audible one, or there is no far band
     * for a sound to land in. */
    RULE_PAIR(PAIR_ASKED2(sound_soft_range, sound_none_range),
              rules->sound_soft_range <= rules->sound_none_range,
              "sound_soft_range is %ld, above sound_none_range %ld",
              (long) rules->sound_soft_range, (long) rules->sound_none_range)

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
#undef SIM_RULE_ARM_INT
#undef SIM_RULE_ARM_INTMIN
#undef SIM_RULE_ARM_FLT
#undef SIM_RULE_BOUND_ARM

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

/* The bounds, off the same list the arms are generated from. An INTMIN row
 * carries the ceiling simRulesWhyInt prints and answers hasHi false, since
 * what caps it is another rule or nothing. cappedBy is -1 in every row here
 * and the pair table below is what fills it in. */
#define SIM_RULE_RANGE_HAS_HI_INT    true
#define SIM_RULE_RANGE_HAS_HI_INTMIN false
#define SIM_RULE_RANGE_HAS_HI_FLT    true
#define SIM_RULE_RANGE_ROW(field, shape, lo, hi)                             \
    { (double) (lo), (double) (hi), SIM_RULE_RANGE_HAS_HI_##shape, -1 },
static const SimRuleRange simRuleRanges[] = {
    SIM_RULE_BOUND_LIST(SIM_RULE_RANGE_ROW)
};
#undef SIM_RULE_RANGE_ROW
#undef SIM_RULE_RANGE_HAS_HI_INT
#undef SIM_RULE_RANGE_HAS_HI_INTMIN
#undef SIM_RULE_RANGE_HAS_HI_FLT

/* A rule given a row in SIM_RULE_LIST and none in the bound list leaves
 * this table short of the others, which is what this catches. */
BOLO_STATIC_ASSERT(sizeof(simRuleRanges) / sizeof(simRuleRanges[0]) ==
                       (size_t)SIM_RULE_COUNT,
                   sim_rule_bounds_cover_every_rule);

/* The rows a second rule caps, walked rather than indexed: seventeen rows,
 * read by a form rather than by the tick. */
#define SIM_RULE_CAPPED_BY_ROW(field, cap)                                   \
    { SIM_RULE_##field, SIM_RULE_##cap },
static const struct {
    int rule;
    int cap;
} simRuleCappedBy[] = {
    SIM_RULE_CAPPED_BY_LIST(SIM_RULE_CAPPED_BY_ROW)
};
#undef SIM_RULE_CAPPED_BY_ROW

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

bool simRulesRuleRange(int rule, SimRuleRange *out) {
    size_t       i;
    const size_t pairs = sizeof(simRuleCappedBy) / sizeof(simRuleCappedBy[0]);

    if (out == NULL) {
        return false;
    }
    out->lo       = 0.0;
    out->hi       = 0.0;
    out->hasHi    = false;
    out->cappedBy = -1;
    if (!simRulesIndexInRange(rule)) {
        return false;
    }

    *out = simRuleRanges[rule];
    for (i = 0; i < pairs; i++) {
        if (simRuleCappedBy[i].rule == rule) {
            out->cappedBy = simRuleCappedBy[i].cap;
            break;
        }
    }
    return true;
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
