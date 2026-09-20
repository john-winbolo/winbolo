/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * The range behind each rule:
 *
 *   sim_rules_range_every_rule    — every rule answers a range, its ends are
 *       the right way round, and an index that names no rule or a NULL out
 *       answers false and leaves the struct neutral.
 *   sim_rules_range_matches_check — the range is the bound the validator
 *       refuses on: every rule moved just past each end it states is refused
 *       as out of range, and the refusal names that rule.
 *   sim_rules_range_paired        — the rows a second rule caps answer that
 *       rule, and every other row answers none.
 *
 * Only the refusal side is asserted. A value sitting on a bound can still
 * break a pair, so a case asserting that lo and hi themselves pass would be
 * asserting something a range does not claim.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "sim_rules.h"
#include "sim_rules_names.h"
#include "test_harness.h"

/* Move one rule's field without naming it: every field is four bytes and a
 * rule's index is its field's own position, which is how simRulesClassicValue
 * already reads a value back out of a filled table. */
static void srrSetValue(SimRules *rules, int rule, double value) {
    int32_t *words = (int32_t *) (void *) rules;

    if (simRulesRuleValueKind(rule) == SIM_RULE_VALUE_FLOAT) {
        float rate = (float) value;
        memcpy(&words[rule], &rate, sizeof(rate));
    } else {
        int32_t whole = (int32_t) value;
        memcpy(&words[rule], &whole, sizeof(whole));
    }
}

/* ── 1. A range for every rule, and none for anything else ─────────────── */

int run_sim_rules_range_every_rule(void) {
    SimRuleRange range;
    int          i;

    for (i = 0; i < simRulesRuleCount(); i++) {
        const char *name = simRulesRuleName(i);

        UT_ASSERT_MSG(simRulesRuleRange(i, &range), "%s has no range", name);
        UT_ASSERT_MSG(!range.hasHi || range.lo <= range.hi,
                      "%s runs from %.4f up to %.4f", name, range.lo,
                      range.hi);
        UT_ASSERT_MSG(range.cappedBy == -1 ||
                          (range.cappedBy >= 0 &&
                           range.cappedBy < simRulesRuleCount()),
                      "%s is capped by rule %d, which names no rule", name,
                      range.cappedBy);
    }

    /* Either end of the index, each leaving the struct neutral rather than
       whatever the caller had in it. */
    range.lo       = 7.0;
    range.hi       = 9.0;
    range.hasHi    = true;
    range.cappedBy = 3;
    UT_ASSERT_MSG(!simRulesRuleRange(-1, &range), "index -1 answered a range");
    UT_ASSERT_MSG(range.lo == 0.0 && range.hi == 0.0 && !range.hasHi &&
                      range.cappedBy == -1,
                  "index -1 left %.4f..%.4f, hasHi %d, cappedBy %d", range.lo,
                  range.hi, (int) range.hasHi, range.cappedBy);

    range.lo       = 7.0;
    range.hi       = 9.0;
    range.hasHi    = true;
    range.cappedBy = 3;
    UT_ASSERT_MSG(!simRulesRuleRange(simRulesRuleCount(), &range),
                  "one past the last rule answered a range");
    UT_ASSERT_MSG(range.lo == 0.0 && range.hi == 0.0 && !range.hasHi &&
                      range.cappedBy == -1,
                  "one past the last rule left %.4f..%.4f, hasHi %d, "
                  "cappedBy %d",
                  range.lo, range.hi, (int) range.hasHi, range.cappedBy);

    /* And nowhere to put the answer is an answer of its own, not a crash. */
    UT_ASSERT_MSG(!simRulesRuleRange(SIM_RULE_tank_reload_ticks, NULL),
                  "a NULL out answered a range");

    return 0;
}

/* ── 2. The range is the bound the validator refuses on ────────────────── */

int run_sim_rules_range_matches_check(void) {
    SimRules     rules;
    SimRuleRange range;
    char         why[256];
    int          i;

    for (i = 0; i < simRulesRuleCount(); i++) {
        const char  *name = simRulesRuleName(i);
        const bool   rate = simRulesRuleValueKind(i) == SIM_RULE_VALUE_FLOAT;
        /* Far enough past the end to be outside it at the width the field
           holds: a whole number moves by one, a rate by half. */
        const double step = rate ? 0.5 : 1.0;

        UT_ASSERT_MSG(simRulesRuleRange(i, &range), "%s has no range", name);

        simRulesClassic(&rules);
        srrSetValue(&rules, i, range.lo - step);
        why[0] = '\0';
        UT_ASSERT_MSG(simRulesCheck(&rules, why, sizeof why) ==
                          SIM_RULES_FAULT_RANGE,
                      "%s below its floor of %.4f was not refused as out of "
                      "range: %s",
                      name, range.lo, why);
        UT_ASSERT_MSG(strstr(why, name) != NULL,
                      "%s below its floor gave \"%s\"", name, why);

        /* A row whose ceiling is INT32_MAX has no value above it to hand in,
           and a row with no fixed ceiling has no ceiling to test. */
        if (!range.hasHi || range.hi >= (double) INT32_MAX) {
            continue;
        }

        simRulesClassic(&rules);
        srrSetValue(&rules, i, range.hi + step);
        why[0] = '\0';
        UT_ASSERT_MSG(simRulesCheck(&rules, why, sizeof why) ==
                          SIM_RULES_FAULT_RANGE,
                      "%s above its ceiling of %.4f was not refused as out "
                      "of range: %s",
                      name, range.hi, why);
        UT_ASSERT_MSG(strstr(why, name) != NULL,
                      "%s above its ceiling gave \"%s\"", name, why);
    }

    return 0;
}

/* ── 3. The rows a second rule caps ────────────────────────────────────── */

int run_sim_rules_range_paired(void) {
    /* The pairs that hold one rule directly under another, as the validator
       tests them. A ceiling that is an expression of more than one rule names
       no single rule, so those rows are not here and answer none. */
    static const struct {
        int rule;
        int cap;
    } paired[] = {
        { SIM_RULE_pill_attack_min_ticks,    SIM_RULE_pill_attack_ticks },
        { SIM_RULE_base_capture_armour,      SIM_RULE_base_full_armour },
        { SIM_RULE_base_hit_armour,          SIM_RULE_base_capture_armour },
        { SIM_RULE_base_min_armour,          SIM_RULE_base_full_armour },
        { SIM_RULE_base_min_shells,          SIM_RULE_base_full_shells },
        { SIM_RULE_base_min_mines,           SIM_RULE_base_full_mines },
        { SIM_RULE_lgm_cost_road,            SIM_RULE_tank_full_trees },
        { SIM_RULE_lgm_cost_building,        SIM_RULE_tank_full_trees },
        { SIM_RULE_lgm_cost_repair_building, SIM_RULE_tank_full_trees },
        { SIM_RULE_lgm_cost_boat,            SIM_RULE_tank_full_trees },
        { SIM_RULE_lgm_cost_pill_new,        SIM_RULE_tank_full_trees },
        { SIM_RULE_lgm_cost_mine,            SIM_RULE_tank_full_mines },
        { SIM_RULE_pill_repair_amount,       SIM_RULE_pill_max_armour },
        { SIM_RULE_pill_shell_damage,        SIM_RULE_pill_max_armour },
        { SIM_RULE_tank_explosion_damage,    SIM_RULE_pill_max_armour },
        { SIM_RULE_sound_soft_range,         SIM_RULE_sound_none_range },
        { SIM_RULE_gunsight_min,             SIM_RULE_gunsight_max }
    };
    const size_t rows = sizeof(paired) / sizeof(paired[0]);
    SimRuleRange range;
    int          i;

    /* Both directions at once: a row in the list answers its rule, and a row
       that is not in it answers none. */
    for (i = 0; i < simRulesRuleCount(); i++) {
        const char *name = simRulesRuleName(i);
        int         want = -1;
        size_t      j;

        for (j = 0; j < rows; j++) {
            if (paired[j].rule == i) {
                want = paired[j].cap;
                break;
            }
        }

        UT_ASSERT_MSG(simRulesRuleRange(i, &range), "%s has no range", name);
        UT_ASSERT_MSG(range.cappedBy == want,
                      "%s is capped by \"%s\", expected \"%s\"", name,
                      simRulesRuleName(range.cappedBy),
                      simRulesRuleName(want));
    }

    /* A ceiling that is an expression of two rules names neither of them. */
    UT_ASSERT_MSG(simRulesRuleRange(SIM_RULE_shell_start_add, &range),
                  "shell_start_add has no range");
    UT_ASSERT_MSG(!range.hasHi && range.cappedBy == -1,
                  "shell_start_add answered hasHi %d and cappedBy \"%s\", and "
                  "what caps it is shell_life times gunsight_min halved",
                  (int) range.hasHi, simRulesRuleName(range.cappedBy));

    /* And a row carrying both: a fixed ceiling of its own and a rule above
       it, which is why hasHi and cappedBy are separate answers. */
    UT_ASSERT_MSG(simRulesRuleRange(SIM_RULE_gunsight_min, &range),
                  "gunsight_min has no range");
    UT_ASSERT_MSG(range.hasHi && range.hi == 255.0,
                  "gunsight_min answered hasHi %d and a ceiling of %.4f",
                  (int) range.hasHi, range.hi);
    UT_ASSERT_MSG(range.cappedBy == SIM_RULE_gunsight_max,
                  "gunsight_min is capped by \"%s\", expected gunsight_max",
                  simRulesRuleName(range.cappedBy));

    return 0;
}
