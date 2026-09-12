/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Simulation Rules
 *Filename:      sim_rules.h
 *Author:        John Morrison
 *Purpose:
 *  The per-simulation table of gameplay numbers that were
 *  compile-time constants. One SimRules sits on GameSim,
 *  so the server's sim and every ClientSim hold the same
 *  table and shared code reads sim->rules.<field> where it
 *  read the constant.
 *
 *  A field's name is the name a scenario uses for it, so a
 *  rule has exactly one spelling. Integer fields are
 *  int32_t whatever the engine stores the value in; the
 *  four tank rates, the ten terrain turn rates and the two
 *  half-tick refuel intervals are float.
 *
 *  Every field is declared here, including the ones whose
 *  use sites still read their constant. The table is
 *  defined once and in full so nothing has to widen it as
 *  each group converts; simRulesClassic fills all of them
 *  with the value the constant holds today, so a field
 *  nothing reads yet still carries the right number when
 *  its sites arrive.
 *********************************************************/

#ifndef SIM_RULES_H
#define SIM_RULES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Length of the buffer simRulesValidate writes its reason into. Long enough
 * for the longest field name plus the value and the bound it missed. */
#define SIM_RULES_WHY_LEN 128

typedef struct SimRules {
    /* ---- Tank ---- */
    int32_t tank_reload_ticks;   /* ticks between shots; 0 fires every tick */
    int32_t tank_full_shells;
    int32_t tank_full_mines;
    int32_t tank_full_trees;
    int32_t tank_full_armour;
    int32_t tank_death_ticks;    /* respawn wait */
    int32_t tank_water_ticks;    /* ticks per unit of drowning drain */
    int32_t shell_damage;
    int32_t mine_damage;
    int32_t just_fired_ticks;    /* how long a shot keeps a tank out of the trees */
    int32_t gunsight_min;
    int32_t gunsight_max;
    float   tank_accel_rate;
    float   tank_decel_rate;     /* the drag terrain applies above its cap */
    float   tank_brake_rate;     /* the slow key */
    float   tank_autoslow_rate;
    int32_t tank_min_move;       /* residual speed a tank needs to move a tick */

    /* ---- Terrain: the cap a tank's speed clamps to ----
     * Building, half-building and pillbox are absent on purpose: they are
     * impassable because tankBuildingCollision tests the terrain type, not
     * because their speed is zero, so a rule for them would do nothing. */
    int32_t speed_road;
    int32_t speed_grass;
    int32_t speed_forest;
    int32_t speed_river;
    int32_t speed_swamp;
    int32_t speed_crater;
    int32_t speed_rubble;
    int32_t speed_boat;
    int32_t speed_deep_sea;
    int32_t speed_refuel_base;

    /* ---- Terrain: bradians turned per tick; 256 is a full circle ---- */
    float   turn_road;
    float   turn_grass;
    float   turn_forest;
    float   turn_river;
    float   turn_swamp;
    float   turn_crater;
    float   turn_rubble;
    float   turn_boat;
    float   turn_deep_sea;
    float   turn_refuel_base;

    /* ---- Shells ---- */
    int32_t shell_life;
    int32_t shell_speed;
    int32_t shell_start_add;

    /* ---- Builder ---- */
    int32_t lgm_build_ticks;
    int32_t lgm_cost_road;
    int32_t lgm_cost_building;
    int32_t lgm_cost_repair_building;
    int32_t lgm_cost_pill_repair;
    int32_t lgm_cost_boat;
    int32_t lgm_cost_pill_new;
    int32_t lgm_cost_mine;
    int32_t lgm_pill_repair_load;
    int32_t lgm_gather_trees;
    int32_t lgm_helicopter_speed;  /* also the parachute delay: distance / speed */

    /* ---- Pillbox ---- */
    int32_t pill_max_armour;
    int32_t pill_attack_ticks;      /* the interval, so lower is angrier */
    int32_t pill_attack_min_ticks;
    int32_t pill_cooldown_ticks;
    int32_t pill_repair_amount;
    int32_t pill_range;

    /* ---- Base ---- */
    int32_t base_full_armour;
    int32_t base_full_shells;
    int32_t base_full_mines;
    int32_t base_capture_armour;
    int32_t base_hit_armour;
    int32_t base_min_armour;
    int32_t base_min_shells;
    int32_t base_min_mines;
    int32_t base_armour_give;
    int32_t base_shells_give;
    int32_t base_mines_give;
    int32_t base_refuel_armour_ticks;
    float   base_refuel_shells_ticks;  /* halves: basesHalfTickCalulator alternates */
    float   base_refuel_mines_ticks;
    int32_t base_regen_ticks;

    /* ---- Terrain destruction and explosions ---- */
    int32_t building_life;
    int32_t rubble_life;
    int32_t grass_life;
    int32_t swamp_life;
    int32_t mine_fuse_ticks;
    int32_t big_explosion_threshold;

    /* ---- Tree growth ---- */
    int32_t tree_grow_ticks;
    int32_t tree_grow_initial_ticks;
    int32_t tree_weight_forest;
    int32_t tree_weight_grass;
    int32_t tree_weight_river;
    int32_t tree_weight_boat;
    int32_t tree_weight_deep_sea;
    int32_t tree_weight_swamp;
    int32_t tree_weight_rubble;
    int32_t tree_weight_building;
    int32_t tree_weight_half_building;
    int32_t tree_weight_crater;
    int32_t tree_weight_road;
    int32_t tree_weight_mine;
} SimRules;

/*********************************************************
 *NAME:          simRulesClassic
 *PURPOSE:
 *  Fills every field with the value its constant holds, so
 *  a sim built from this table plays the classic game.
 *
 *ARGUMENTS:
 *  out - table to fill
 *********************************************************/
void simRulesClassic(SimRules *out);

/* Which kind of check a table failed. A caller that has to answer for the
 * table in its own vocabulary — the scenario funnel's set-rule arm answers
 * SCN_OP_RANGE or SCN_OP_PAIR — needs to know which of the two happened,
 * which the reason string says in words and nothing said in a value. */
typedef enum {
    SIM_RULES_OK = 0,
    SIM_RULES_FAULT_RANGE,    /* a field outside the range its own row allows */
    SIM_RULES_FAULT_PAIR,     /* a field against another field */
    SIM_RULES_FAULT_NO_TABLE  /* there was no table to check */
} SimRulesFault;

/*********************************************************
 *NAME:          simRulesCheck
 *PURPOSE:
 *  simRulesValidate, answering with the kind of check that
 *  failed rather than with a bool. Every range and every
 *  pair is asked in the same order and the reason string is
 *  the same one.
 *
 *ARGUMENTS:
 *  rules  - table to check
 *  why    - buffer the reason is written into, or NULL
 *  whyLen - bytes available at why
 *********************************************************/
SimRulesFault simRulesCheck(const SimRules *rules, char *why, size_t whyLen);

/*********************************************************
 *NAME:          simRulesValidate
 *PURPOSE:
 *  Checks every field against the range its row allows.
 *  Returns true when all of them are inside it; otherwise
 *  false, with a reason naming the field, the value it
 *  holds and the bound it missed written into why.
 *
 *  Rows whose bound is another field — a builder cost
 *  against the tank's carrying capacity, a base's give
 *  against its reserve — have their own numbers checked
 *  with the ranges and are then asked against each other
 *  by the pair arms that follow, once every range is known
 *  to hold.
 *
 *ARGUMENTS:
 *  rules  - table to check
 *  why    - buffer the reason is written into, or NULL
 *  whyLen - bytes available at why
 *********************************************************/
bool simRulesValidate(const SimRules *rules, char *why, size_t whyLen);

#endif /* SIM_RULES_H */
