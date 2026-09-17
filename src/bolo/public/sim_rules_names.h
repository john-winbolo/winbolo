/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Simulation Rule Names
 *Filename:      sim_rules_names.h
 *Author:        John Morrison
 *Purpose:
 *  The one list of the rules a simulation runs on: each
 *  field's name, whether it holds a whole number or a rate,
 *  and the unit it is read in. The index enum is generated
 *  from the list, and that index is what the wire, the
 *  scenario surface and a recording all name a rule by.
 *
 *  Here rather than beside the table itself because a
 *  frontend has to be able to name a rule and say what a
 *  value does to it: the editor's rules form, the lobby's
 *  popup and the viewer's rules display see public/ and
 *  none of them sees internal/sim_rules.h or
 *  scenario_api/. Names and units only — the values stay
 *  in simRulesClassic and the ranges stay with the checks
 *  in sim_rules.c, which is the one place a bound is
 *  written down.
 *
 *  Row order is the field order of SimRules, and that order
 *  is the rule index, so a row and a field cannot be read
 *  apart. The static assertion in sim_rules.c holds the two
 *  together: a field added to the struct without a row here
 *  does not compile.
 *********************************************************/

#ifndef SIM_RULES_NAMES_H
#define SIM_RULES_NAMES_H

#ifdef __cplusplus
extern "C" {
#endif

/* What a row's field holds: SimRules is int32_t or float per field, and a
 * caller reading a value out of the table has to know which. */
typedef enum {
    SIM_RULE_VALUE_INT,
    SIM_RULE_VALUE_FLOAT
} SimRuleValueKind;

/* How a row is read, which is what decides the words a change is described
 * in. The tag says which direction is "better" and whether a ratio means
 * anything at all for the row:
 *
 *   TICKS_LOWER_IS_FASTER  an interval — a reload, a regeneration, a build,
 *                          a cooldown, a respawn. A smaller number is faster.
 *   COUNT                  a stock, a cap, a damage, a range, a threshold.
 *                          More is more.
 *   SPEED_HIGHER_IS_FASTER a speed, a turn rate or an acceleration. A bigger
 *                          number is faster.
 *   PERCENT                a row whose value is a percentage.
 *   FLAG                   a row that is only on or off.
 *   CONSTANT_BY_DESIGN     a row where a ratio and a difference both say
 *                          nothing true — the tree weights are twelve
 *                          entries in a weighted draw, six of them negative
 *                          and one zero — so the raw number is the answer.
 *
 * PERCENT and FLAG name no row today. They are here because the describe
 * arms that read them are here: the first rule that wants one takes the tag
 * rather than inventing a seventh. */
typedef enum {
    SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER,
    SIM_RULE_UNIT_COUNT,
    SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER,
    SIM_RULE_UNIT_PERCENT,
    SIM_RULE_UNIT_FLAG,
    SIM_RULE_UNIT_CONSTANT_BY_DESIGN
} SimRuleUnit;

#define SIM_RULE_LIST(X)                                                     \
    /* Tank */                                                               \
    X(tank_reload_ticks, SIM_RULE_VALUE_INT,                                 \
      SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER)                                   \
    X(tank_full_shells, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)             \
    X(tank_full_mines, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)              \
    X(tank_full_trees, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)              \
    X(tank_full_armour, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)             \
    X(tank_death_ticks, SIM_RULE_VALUE_INT,                                  \
      SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER)                                   \
    X(tank_water_ticks, SIM_RULE_VALUE_INT,                                  \
      SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER)                                   \
    X(shell_damage, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)                 \
    X(mine_damage, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)                  \
    X(mine_damage_range, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)            \
    X(mine_fatal_divisor, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)           \
    X(water_loss_shells, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)            \
    X(water_loss_mines, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)             \
    X(just_fired_ticks, SIM_RULE_VALUE_INT,                                  \
      SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER)                                   \
    X(tree_hide_distance, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)           \
    X(gunsight_min, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)                 \
    X(gunsight_max, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)                 \
    X(tank_accel_rate, SIM_RULE_VALUE_FLOAT,                                 \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                                  \
    X(tank_decel_rate, SIM_RULE_VALUE_FLOAT,                                 \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                                  \
    X(tank_brake_rate, SIM_RULE_VALUE_FLOAT,                                 \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                                  \
    X(tank_autoslow_rate, SIM_RULE_VALUE_FLOAT,                              \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                                  \
    X(tank_min_move, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)                \
    /* Tank collision geometry */                                           \
    X(tank_hit_radius, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)              \
    X(tank_collision_distance, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)      \
    X(tank_nudge_threshold, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)         \
    X(tank_nudge_amount, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)            \
    X(tank_nudge_iterations, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)        \
    X(tank_bump_decay_shift, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)        \
    X(tank_pill_pickup_inset, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)       \
    X(tank_boat_exit_inset, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)         \
    X(tank_slide_step, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)              \
    X(tank_wall_glide, SIM_RULE_VALUE_FLOAT, SIM_RULE_UNIT_COUNT)            \
    /* Terrain: the cap a tank's speed clamps to */                          \
    X(speed_road, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)  \
    X(speed_grass, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER) \
    X(speed_forest, SIM_RULE_VALUE_INT,                                      \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                                  \
    X(speed_river, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER) \
    X(speed_swamp, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER) \
    X(speed_crater, SIM_RULE_VALUE_INT,                                      \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                                  \
    X(speed_rubble, SIM_RULE_VALUE_INT,                                      \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                                  \
    X(speed_boat, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)  \
    X(speed_deep_sea, SIM_RULE_VALUE_INT,                                    \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                                  \
    X(speed_refuel_base, SIM_RULE_VALUE_INT,                                 \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                                  \
    /* Terrain: bradians turned per tick */                                  \
    X(turn_road, SIM_RULE_VALUE_FLOAT, SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER) \
    X(turn_grass, SIM_RULE_VALUE_FLOAT,                                      \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                                  \
    X(turn_forest, SIM_RULE_VALUE_FLOAT,                                     \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                                  \
    X(turn_river, SIM_RULE_VALUE_FLOAT,                                      \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                                  \
    X(turn_swamp, SIM_RULE_VALUE_FLOAT,                                      \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                                  \
    X(turn_crater, SIM_RULE_VALUE_FLOAT,                                     \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                                  \
    X(turn_rubble, SIM_RULE_VALUE_FLOAT,                                     \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                                  \
    X(turn_boat, SIM_RULE_VALUE_FLOAT, SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER) \
    X(turn_deep_sea, SIM_RULE_VALUE_FLOAT,                                   \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                                  \
    X(turn_refuel_base, SIM_RULE_VALUE_FLOAT,                                \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                                  \
    /* Terrain: the cap the builder's walk clamps to */                      \
    X(man_speed_road, SIM_RULE_VALUE_INT,                                \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                              \
    X(man_speed_grass, SIM_RULE_VALUE_INT,                               \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                              \
    X(man_speed_forest, SIM_RULE_VALUE_INT,                              \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                              \
    X(man_speed_river, SIM_RULE_VALUE_INT,                               \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                              \
    X(man_speed_swamp, SIM_RULE_VALUE_INT,                               \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                              \
    X(man_speed_crater, SIM_RULE_VALUE_INT,                              \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                              \
    X(man_speed_rubble, SIM_RULE_VALUE_INT,                              \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                              \
    X(man_speed_boat, SIM_RULE_VALUE_INT,                                \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                              \
    X(man_speed_deep_sea, SIM_RULE_VALUE_INT,                            \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                              \
    X(man_speed_refuel_base, SIM_RULE_VALUE_INT,                         \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                              \
    /* Shells */                                                             \
    X(shell_life, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)                   \
    X(shell_speed, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER) \
    X(shell_start_add, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)              \
    /* Builder */                                                            \
    X(lgm_build_ticks, SIM_RULE_VALUE_INT,                                   \
      SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER)                                   \
    X(lgm_cost_road, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)                \
    X(lgm_cost_building, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)            \
    X(lgm_cost_repair_building, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)     \
    X(lgm_cost_pill_repair, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)         \
    X(lgm_cost_boat, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)                \
    X(lgm_cost_pill_new, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)            \
    X(lgm_cost_mine, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)                \
    X(lgm_pill_repair_load, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)         \
    X(lgm_gather_trees, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)             \
    X(lgm_helicopter_speed, SIM_RULE_VALUE_INT,                              \
      SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER)                                  \
    /* Pillbox */                                                            \
    X(pill_max_armour, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)              \
    X(pill_attack_ticks, SIM_RULE_VALUE_INT,                                 \
      SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER)                                   \
    X(pill_attack_min_ticks, SIM_RULE_VALUE_INT,                             \
      SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER)                                   \
    X(pill_cooldown_ticks, SIM_RULE_VALUE_INT,                               \
      SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER)                                   \
    X(pill_repair_amount, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)           \
    X(pill_range, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)                   \
    X(pill_shell_damage, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)            \
    X(pill_angry_divisor, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)           \
    X(pill_fire_length, SIM_RULE_VALUE_FLOAT, SIM_RULE_UNIT_COUNT)           \
    X(pill_base_defend_range, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)       \
    X(pill_aim_iterations, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)          \
    /* Base */                                                               \
    X(base_full_armour, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)             \
    X(base_full_shells, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)             \
    X(base_full_mines, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)              \
    X(base_capture_armour, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)          \
    X(base_hit_armour, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)              \
    X(base_min_armour, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)              \
    X(base_min_shells, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)              \
    X(base_min_mines, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)               \
    X(base_armour_give, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)             \
    X(base_shells_give, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)             \
    X(base_mines_give, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)              \
    X(base_refuel_armour_ticks, SIM_RULE_VALUE_INT,                          \
      SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER)                                   \
    X(base_refuel_shells_ticks, SIM_RULE_VALUE_FLOAT,                        \
      SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER)                                   \
    X(base_refuel_mines_ticks, SIM_RULE_VALUE_FLOAT,                         \
      SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER)                                   \
    X(base_regen_ticks, SIM_RULE_VALUE_INT,                                  \
      SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER)                                   \
    /* Terrain destruction and explosions */                                 \
    X(building_life, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)                \
    X(rubble_life, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)                  \
    X(grass_life, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)                   \
    X(swamp_life, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)                   \
    X(mine_fuse_ticks, SIM_RULE_VALUE_INT,                                   \
      SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER)                                   \
    X(big_explosion_threshold, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)      \
    X(tank_explosion_damage, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)        \
    X(tank_explosion_length, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)        \
    X(tank_explosion_move, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)          \
    X(tank_explosion_update_ticks, SIM_RULE_VALUE_INT,                       \
      SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER)                                   \
    X(tank_explosion_width, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)         \
    X(tank_explosion_height, SIM_RULE_VALUE_INT, SIM_RULE_UNIT_COUNT)        \
    /* Tree growth */                                                        \
    X(tree_grow_ticks, SIM_RULE_VALUE_INT,                                   \
      SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER)                                   \
    X(tree_grow_initial_ticks, SIM_RULE_VALUE_INT,                           \
      SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER)                                   \
    X(tree_weight_forest, SIM_RULE_VALUE_INT,                                \
      SIM_RULE_UNIT_CONSTANT_BY_DESIGN)                                      \
    X(tree_weight_grass, SIM_RULE_VALUE_INT,                                 \
      SIM_RULE_UNIT_CONSTANT_BY_DESIGN)                                      \
    X(tree_weight_river, SIM_RULE_VALUE_INT,                                 \
      SIM_RULE_UNIT_CONSTANT_BY_DESIGN)                                      \
    X(tree_weight_boat, SIM_RULE_VALUE_INT,                                  \
      SIM_RULE_UNIT_CONSTANT_BY_DESIGN)                                      \
    X(tree_weight_deep_sea, SIM_RULE_VALUE_INT,                              \
      SIM_RULE_UNIT_CONSTANT_BY_DESIGN)                                      \
    X(tree_weight_swamp, SIM_RULE_VALUE_INT,                                 \
      SIM_RULE_UNIT_CONSTANT_BY_DESIGN)                                      \
    X(tree_weight_rubble, SIM_RULE_VALUE_INT,                                \
      SIM_RULE_UNIT_CONSTANT_BY_DESIGN)                                      \
    X(tree_weight_building, SIM_RULE_VALUE_INT,                              \
      SIM_RULE_UNIT_CONSTANT_BY_DESIGN)                                      \
    X(tree_weight_half_building, SIM_RULE_VALUE_INT,                         \
      SIM_RULE_UNIT_CONSTANT_BY_DESIGN)                                      \
    X(tree_weight_crater, SIM_RULE_VALUE_INT,                                \
      SIM_RULE_UNIT_CONSTANT_BY_DESIGN)                                      \
    X(tree_weight_road, SIM_RULE_VALUE_INT,                                  \
      SIM_RULE_UNIT_CONSTANT_BY_DESIGN)                                      \
    X(tree_weight_mine, SIM_RULE_VALUE_INT,                                  \
      SIM_RULE_UNIT_CONSTANT_BY_DESIGN)

/* One member per rule, in the struct's own field order. SIM_RULE_COUNT is
 * one past the last, and an index at or above it names no rule. A rule's
 * name is the name a scenario, a manifest and an operator line all spell it
 * with, so the enumerator carries that name verbatim rather than an
 * upper-case respelling of it: one spelling, and no second column to get
 * wrong. */
typedef enum {
#define SIM_RULE_ENUM_MEMBER(name, kind, unit) SIM_RULE_##name,
    SIM_RULE_LIST(SIM_RULE_ENUM_MEMBER)
#undef SIM_RULE_ENUM_MEMBER
    SIM_RULE_COUNT
} SimRuleIndex;

/* How a value differs from the rule's classic default. The number means
 * something different per kind, and nothing at all for three of them, so a
 * caller reads it only for the kinds that carry one. */
typedef enum {
    SIM_RULE_CHANGE_UNCHANGED,  /* number unused */
    SIM_RULE_CHANGE_FASTER,     /* number = the multiple, >= 1 */
    SIM_RULE_CHANGE_SLOWER,     /* number = the multiple, >= 1 */
    SIM_RULE_CHANGE_MORE,       /* number = the multiple, >= 1 */
    SIM_RULE_CHANGE_FEWER,      /* number = the multiple, >= 1 */
    SIM_RULE_CHANGE_DELTA,      /* number = value - classic, signed */
    SIM_RULE_CHANGE_ON,         /* number unused */
    SIM_RULE_CHANGE_OFF,        /* number unused */
    SIM_RULE_CHANGE_RAW         /* number = the value itself */
} SimRuleChangeKind;

typedef struct {
    SimRuleChangeKind kind;
    double            number;
} SimRuleChange;

/* How many rules there are, for a caller walking them without seeing the
 * enum's last member as a loop bound. */
int simRulesRuleCount(void);

/* What a rule is called, and the rule a name spells. "" for an index that
 * names no rule; -1 for a name that spells none. */
const char *simRulesRuleName(int rule);
int         simRulesRuleIndex(const char *name);

/* A row's unit tag and the type its field holds. An index that names no rule
 * answers SIM_RULE_UNIT_COUNT and SIM_RULE_VALUE_INT — the neutral pair, so
 * a caller that has not bounded its index describes nothing rather than
 * reading off the end of the table. */
SimRuleUnit      simRulesRuleUnit(int rule);
SimRuleValueKind simRulesRuleValueKind(int rule);

/* The value the classic game plays this rule at, as a double whatever the
 * field's own type is. 0 for an index that names no rule. Read out of a
 * table simRulesClassic has just filled, so it follows those defaults
 * wherever they are written down and there is no second list of them. */
double simRulesClassicValue(int rule);

/* What a value does to a rule, against that rule's classic default: the same
 * question the editor's rules form, the lobby's popup and the viewer's rules
 * display all ask.
 *
 * The answer is a kind and a number rather than a string, and the tag and
 * the default are read here rather than passed in, so the caller needs
 * neither the rules table nor a language: a gui file turns the pair into
 * words with simRulesPhrase (src/gui/sim_rules_phrase.h), and this side of
 * the line stays free of the lang table.
 *
 * An index that names no rule answers UNCHANGED — there is no rule for a
 * value to have changed.
 *
 * simRulesDescribeValue is the same answer for a unit and a default handed
 * in, which is what lets the arms for a unit no rule carries yet be asked
 * directly. */
SimRuleChange simRulesDescribeValue(SimRuleUnit unit, double classic,
                                    double value);
SimRuleChange simRulesDescribeChange(int rule, double value);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SIM_RULES_NAMES_H */
