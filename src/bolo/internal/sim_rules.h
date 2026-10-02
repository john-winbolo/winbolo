/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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
    int32_t mine_damage_range;   /* world units off centre a mine still hurts */
    int32_t mine_fatal_divisor;  /* what a blow bigger than the armour left is cut by */
    int32_t water_loss_shells;   /* what a wading tank loses each interval */
    int32_t water_loss_mines;
    int32_t just_fired_ticks;    /* how long a shot keeps a tank out of the trees */
    int32_t tree_hide_distance;  /* how far off a tank in trees stops being seen */
    int32_t gunsight_min;
    int32_t gunsight_max;
    float   tank_accel_rate;
    float   tank_decel_rate;     /* the drag terrain applies above its cap */
    float   tank_brake_rate;     /* the slow key */
    float   tank_autoslow_rate;
    int32_t tank_min_move;       /* residual speed a tank needs to move a tick */

    /* ---- Tank collision geometry ----
     * What the tank is, as a shape, to a shell and to the world it drives
     * through. tank_hit_radius is the circle both the shell test and the
     * building resolver use; the rest is how two tanks shove each other
     * apart and how one comes off a wall. */
    int32_t tank_hit_radius;         /* the circle a shell has to reach */
    int32_t tank_collision_distance; /* how close two tanks shove */
    int32_t tank_nudge_threshold;    /* which axis a shove takes */
    int32_t tank_nudge_amount;       /* how far one shove moves a tank */
    int32_t tank_nudge_iterations;   /* shoves tried in a tick */
    int32_t tank_bump_decay_shift;   /* how fast a bump dies away */
    int32_t tank_pill_pickup_inset;  /* the reach a tank picks a pill up from */
    int32_t tank_boat_exit_inset;    /* how far inside a bank a boat is held */
    int32_t tank_slide_step;         /* world units a knocked tank slides */
    float   tank_wall_glide;         /* 0 slides along a wall, 1 glides free */

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

    /* ---- Terrain: the cap the builder's walk clamps to ----
     * The tank's own caps are the speed_* rows above. A man walks a
     * different table - he crosses swamp and rubble faster than a tank
     * does and cannot cross a river at all - so the two are separate rows
     * rather than one set read twice. Building, half-building and pillbox
     * are absent for the reason they are absent from speed_*. */
    int32_t man_speed_road;
    int32_t man_speed_grass;
    int32_t man_speed_forest;
    int32_t man_speed_river;
    int32_t man_speed_swamp;
    int32_t man_speed_crater;
    int32_t man_speed_rubble;
    int32_t man_speed_boat;
    int32_t man_speed_deep_sea;
    int32_t man_speed_refuel_base;

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
    int32_t lgm_arrive_tolerance;  /* how near his goal counts as arrived */
    int32_t lgm_return_tolerance;  /* the same, coming back to the tank */
    int32_t lgm_pill_drop_search;  /* squares a column of the drop search walks */
    int32_t lgm_boat_leave_offset; /* how far out he steps onto a boat */
    int32_t lgm_boat_return_offset;

    /* ---- Pillbox ---- */
    int32_t pill_max_armour;
    int32_t pill_attack_ticks;      /* the interval, so lower is angrier */
    int32_t pill_attack_min_ticks;
    int32_t pill_cooldown_ticks;
    int32_t pill_repair_amount;
    int32_t pill_range;
    int32_t pill_shell_damage;      /* what one shell takes off a pill */
    int32_t pill_angry_divisor;     /* the step from normal toward min */
    float   pill_fire_length;       /* how far the shell a pill fires flies */
    int32_t pill_base_defend_range; /* map squares around a shot base */
    int32_t pill_aim_iterations;    /* how hard a pill works to lead a target */
    int32_t pill_massage_range;     /* how near the old sloppy aim takes over */
    float   pill_massage_cosine;    /* how straight at the pill still aims true */

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
    int32_t base_status_range;   /* how near a base has to be to read its stock */
    int32_t base_reveal_range;   /* how near before its armour is worth predicting */

    /* ---- Terrain destruction and explosions ---- */
    int32_t building_life;
    int32_t rubble_life;
    int32_t grass_life;
    int32_t swamp_life;
    int32_t mine_fuse_ticks;
    int32_t big_explosion_threshold;
    int32_t tank_explosion_damage;        /* the splash a dying tank deals a pill */
    int32_t tank_explosion_length;        /* how long the wreck travels */
    int32_t tank_explosion_move;          /* world units a wreck moves a step */
    int32_t tank_explosion_update_ticks;  /* ticks between those steps */
    int32_t tank_explosion_width;         /* half the wreck's collision box */
    int32_t tank_explosion_height;

    /* ---- Spawning ----
     * How a respawn picks its start. The three ranges are what counts as
     * too near a tank, a pillbox or a base; the separation is how far a
     * scattered spawn keeps from another live tank, the scatter cap how
     * long the spiral search looks, and the threshold the share of neutral
     * bases below which a player's own base is preferred to a neutral. */
    int32_t start_tank_range;
    int32_t start_pill_range;
    int32_t start_base_range;
    int32_t start_spawn_separation;
    int32_t start_scatter_max;
    int32_t start_neutral_threshold_pct;

    /* ---- Hearing ----
     * How far a sound carries, in map squares. Inside the soft range it is
     * played near, past the none range it is dropped, and between the two
     * it is played far. */
    int32_t sound_soft_range;
    int32_t sound_none_range;

    /* ---- Terrain flooding ---- */
    int32_t flood_fill_ticks;    /* how long water takes to claim a square */

    /* ---- Tree growth ---- */
    int32_t tree_grow_ticks;
    int32_t tree_grow_initial_ticks;
    int32_t tree_grow_initial_score; /* what the weighted draw starts and resets from */
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

    /* ---- Pillbox shell cap ---- last, like its rows in SIM_RULE_LIST */
    int32_t pill_shell_cap;         /* whether pill_max_shells_at_tank applies */
    int32_t pill_max_shells_at_tank; /* pill shells one tank can have coming */

    /* ---- Base defence shape ---- last, like its row in SIM_RULE_LIST */
    int32_t pill_base_defend_shape; /* 0 a square, 1 a circle */

    /* ---- Mac Bolo shell push ---- last, like their rows in SIM_RULE_LIST */
    int32_t tank_slide_mac;          /* whether a shell hit pushes the Mac Bolo way */
    int32_t tank_slide_armour_bonus; /* extra Mac Bolo push at zero armour */

    /* ---- Mac Bolo pill aiming ---- appended to preserve rule indices */
    int32_t pill_aim_mac;            /* Mac Bolo lead, including pill massage */
    int32_t tank_collision_mac;      /* Mac sprite boxes and solid-tile nudges */

    /* ---- Deep sea ---- appended to preserve rule indices */
    int32_t tank_deep_sea_safe;      /* a tank with no boat floats on deep sea */
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

/*********************************************************
 *NAME:          simRulesAreClassic
 *PURPOSE:
 *  Whether a table holds the classic game's numbers, every
 *  field of it. Asked by code whose own numbers describe the
 *  classic game and which has nothing sensible to do on a
 *  sim running anything else — the observation builders,
 *  whose normalisation is a fixed scale a model was trained
 *  against.
 *
 *  Answered against a table simRulesClassic has just filled,
 *  so the answer follows those values and there is no second
 *  list of them here to drift from them.
 *
 *  A NULL table is not classic: a caller with no table to
 *  show cannot be told its numbers are the right ones.
 *
 *ARGUMENTS:
 *  rules - table to test, or NULL
 *********************************************************/
bool simRulesAreClassic(const SimRules *rules);

/*********************************************************
 *NAME:          simRulesFirstDifference
 *PURPOSE:
 *  simRulesAreClassic, answering with which field differs
 *  rather than whether one does: the index of the first
 *  field that is not the classic value, or -1 when the whole
 *  table is classic. A NULL table answers 0.
 *
 *  The index counts fields from the front of the struct, so
 *  it is the same index the scenario surface names a rule by,
 *  and simRulesRuleName (public/sim_rules_names.h) turns it
 *  into that rule's name.
 *
 *  A refusal that has to tell an operator what is wrong
 *  wants this rather than the bool — "the rules are not
 *  classic" leaves them hunting through a scenario, a server
 *  setting and a saved config for which number moved.
 *
 *ARGUMENTS:
 *  rules - table to test, or NULL
 *********************************************************/
int simRulesFirstDifference(const SimRules *rules);

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
 *NAME:          simRulesCheckCarried
 *PURPOSE:
 *  simRulesCheck over the part of a table a CTRL_SIM_RULES
 *  event can have set: the range row of every rule the event
 *  carries, and every pair whose fields are all carried. The
 *  rows and pairs are the same ones simRulesCheck asks and
 *  the reasons are the same strings — one body answers both,
 *  and which arms this pass skips is read off the event's own
 *  field list rather than written down a second time.
 *
 *  For the client, which has to refuse a table a hostile or
 *  broken server sends it before that table reaches the tank
 *  code. It cannot ask the whole check: a pair holding a
 *  carried rule against one the event leaves on the server —
 *  lgm_cost_pill_new against tank_full_trees, say — would
 *  refuse a server that legitimately lowered both, because
 *  the client never hears about the half it is not sent.
 *
 *ARGUMENTS:
 *  rules  - table to check
 *  why    - buffer the reason is written into, or NULL
 *  whyLen - bytes available at why
 *********************************************************/
SimRulesFault simRulesCheckCarried(const SimRules *rules, char *why,
                                   size_t whyLen);

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
