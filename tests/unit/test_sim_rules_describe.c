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
 */

/*
 * The rule list as a frontend reads it: the names, the classic values behind
 * them, and what a value does to a rule.
 *
 *   sim_rules_describe_names  — every name resolves back to its own index,
 *       a name that spells no rule answers -1, an index that names none
 *       answers "" and a classic value of 0, and the value behind an index
 *       is the field the classic table holds, checked on an integer row and
 *       on a rate.
 *   sim_rules_describe_reload — the reload rule, which is the one a mod
 *       moves first: a third of the classic interval reads as a multiple
 *       with a decimal, twice it reads as a whole multiple the other way,
 *       and the classic value itself reads as unchanged.
 *   sim_rules_describe_ratios — a count doubled and halved, a small move
 *       answered as a difference rather than as a ratio, and the two sides
 *       of the band a ratio snaps to a whole number inside.
 *   sim_rules_describe_units  — the arms the tags choose: a flag, a
 *       percentage, a weight whose raw number is the answer, and every one
 *       of the rules at its own default reading as unchanged.
 *
 * The values a case asserts against are read from simRulesClassic rather
 * than written down here, so a default that moves moves the case with it;
 * the reload case is the one exception, and it says why.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "sim_rules.h"
#include "sim_rules_names.h"
#include "sim_rules_phrase.h" /* simRulesPhrase — src/gui/ is on this target's path */
#include "test_harness.h"

/* Doubles that came out of the same arithmetic compare exactly here — a
 * ratio of a whole number of ticks is exact in binary — but a case reads
 * better saying what it means by "the same number". */
static bool srdSame(double a, double b) {
    return fabs(a - b) < 1e-9;
}

static const char *srdKindName(SimRuleChangeKind kind) {
    switch (kind) {
        case SIM_RULE_CHANGE_UNCHANGED: return "UNCHANGED";
        case SIM_RULE_CHANGE_FASTER:    return "FASTER";
        case SIM_RULE_CHANGE_SLOWER:    return "SLOWER";
        case SIM_RULE_CHANGE_MORE:      return "MORE";
        case SIM_RULE_CHANGE_FEWER:     return "FEWER";
        case SIM_RULE_CHANGE_DELTA:     return "DELTA";
        case SIM_RULE_CHANGE_ON:        return "ON";
        case SIM_RULE_CHANGE_OFF:       return "OFF";
        case SIM_RULE_CHANGE_RAW:       return "RAW";
        default:                        return "?";
    }
}

#define SRD_EXPECT(change, wantKind, wantNumber, what)                       \
    do {                                                                     \
        SimRuleChange c_ = (change);                                         \
        UT_ASSERT_MSG(c_.kind == (wantKind),                                 \
                      "%s answered %s, expected %s", (what),                 \
                      srdKindName(c_.kind), srdKindName(wantKind));          \
        UT_ASSERT_MSG(srdSame(c_.number, (wantNumber)),                      \
                      "%s carried %.4f, expected %.4f", (what),              \
                      c_.number, (double)(wantNumber));                      \
    } while (0)

/* ── 1. The names and the values behind them ───────────────────────────── */

int run_sim_rules_describe_names(void) {
    SimRules classic;
    int      i;

    simRulesClassic(&classic);

    UT_ASSERT_MSG(simRulesRuleCount() == (int)SIM_RULE_COUNT,
                  "the count answered %d and the enum holds %d",
                  simRulesRuleCount(), (int)SIM_RULE_COUNT);

    for (i = 0; i < simRulesRuleCount(); i++) {
        const char *name = simRulesRuleName(i);

        UT_ASSERT_MSG(name != NULL && name[0] != '\0',
                      "rule %d has no name", i);
        UT_ASSERT_MSG(simRulesRuleIndex(name) == i,
                      "%s resolved to %d, not its own index %d", name,
                      simRulesRuleIndex(name), i);
    }

    /* A name nobody spells, and the two ends of the index. */
    UT_ASSERT_MSG(simRulesRuleIndex("tank_reload_tick") == -1,
                  "a misspelled rule name resolved to a rule");
    UT_ASSERT_MSG(simRulesRuleIndex("") == -1, "the empty name resolved");
    UT_ASSERT_MSG(simRulesRuleIndex(NULL) == -1, "a NULL name resolved");
    UT_ASSERT_MSG(simRulesRuleName(-1)[0] == '\0',
                  "index -1 answered a name");
    UT_ASSERT_MSG(simRulesRuleName((int)SIM_RULE_COUNT)[0] == '\0',
                  "one past the last rule answered a name");
    UT_ASSERT_MSG(srdSame(simRulesClassicValue(-1), 0.0),
                  "index -1 answered a classic value");
    UT_ASSERT_MSG(srdSame(simRulesClassicValue((int)SIM_RULE_COUNT), 0.0),
                  "one past the last rule answered a classic value");

    /* The value behind an index is the field, on both of the two widths. */
    UT_ASSERT_MSG(srdSame(simRulesClassicValue(SIM_RULE_tank_reload_ticks),
                          (double)classic.tank_reload_ticks),
                  "tank_reload_ticks reads %.4f and the table holds %ld",
                  simRulesClassicValue(SIM_RULE_tank_reload_ticks),
                  (long)classic.tank_reload_ticks);
    UT_ASSERT_MSG(srdSame(simRulesClassicValue(SIM_RULE_tank_full_shells),
                          (double)classic.tank_full_shells),
                  "tank_full_shells reads %.4f and the table holds %ld",
                  simRulesClassicValue(SIM_RULE_tank_full_shells),
                  (long)classic.tank_full_shells);
    UT_ASSERT_MSG(srdSame(simRulesClassicValue(SIM_RULE_base_refuel_shells_ticks),
                          (double)classic.base_refuel_shells_ticks),
                  "base_refuel_shells_ticks reads %.4f and the table holds "
                  "%.4f — a rate read as an integer",
                  simRulesClassicValue(SIM_RULE_base_refuel_shells_ticks),
                  (double)classic.base_refuel_shells_ticks);
    UT_ASSERT_MSG(srdSame(simRulesClassicValue(SIM_RULE_turn_road),
                          (double)classic.turn_road),
                  "turn_road reads %.4f and the table holds %.4f",
                  simRulesClassicValue(SIM_RULE_turn_road),
                  (double)classic.turn_road);

    /* The last row, which is where a list one short of the struct shows. */
    UT_ASSERT(SIM_RULE_tank_collision_mac == SIM_RULE_COUNT - 1);
    UT_ASSERT_MSG(srdSame(simRulesClassicValue((int)SIM_RULE_COUNT - 1),
                          (double)classic.tank_collision_mac),
                  "the last rule reads %.4f and tank_collision_mac "
                  "holds %ld",
                  simRulesClassicValue((int)SIM_RULE_COUNT - 1),
                  (long)classic.tank_collision_mac);

    /* The tags and widths the list carries, on one row of each width. */
    UT_ASSERT_MSG(simRulesRuleValueKind(SIM_RULE_tank_reload_ticks) ==
                      SIM_RULE_VALUE_INT,
                  "tank_reload_ticks is not an integer row");
    UT_ASSERT_MSG(simRulesRuleValueKind(SIM_RULE_turn_road) ==
                      SIM_RULE_VALUE_FLOAT,
                  "turn_road is not a rate row");
    UT_ASSERT_MSG(simRulesRuleUnit(SIM_RULE_tank_reload_ticks) ==
                      SIM_RULE_UNIT_TICKS_LOWER_IS_FASTER,
                  "tank_reload_ticks is not tagged as an interval");
    UT_ASSERT_MSG(simRulesRuleUnit(SIM_RULE_tree_weight_road) ==
                      SIM_RULE_UNIT_CONSTANT_BY_DESIGN,
                  "tree_weight_road is not tagged constant by design");

    return 0;
}

/* ── 2. The reload rule ────────────────────────────────────────────────── */

int run_sim_rules_describe_reload(void) {
    const double classic = simRulesClassicValue(SIM_RULE_tank_reload_ticks);

    /* The one case that states its number: the classic interval is 13
       ticks, and 13 over 5 is what makes the answer a decimal rather than
       a whole multiple. A default that moves fails here on purpose. */
    UT_ASSERT_MSG(srdSame(classic, 13.0),
                  "tank_reload_ticks is %.4f, and this case is written "
                  "against the 13 ticks the classic game reloads in",
                  classic);

    SRD_EXPECT(simRulesDescribeChange(SIM_RULE_tank_reload_ticks, 5.0),
               SIM_RULE_CHANGE_FASTER, 2.6, "reload at 5");
    SRD_EXPECT(simRulesDescribeChange(SIM_RULE_tank_reload_ticks, 26.0),
               SIM_RULE_CHANGE_SLOWER, 2.0, "reload at 26");
    SRD_EXPECT(simRulesDescribeChange(SIM_RULE_tank_reload_ticks, classic),
               SIM_RULE_CHANGE_UNCHANGED, 0.0, "reload at its own default");

    /* An index that names no rule has no change to describe. */
    SRD_EXPECT(simRulesDescribeChange(-1, 5.0),
               SIM_RULE_CHANGE_UNCHANGED, 0.0, "a value on index -1");
    SRD_EXPECT(simRulesDescribeChange((int)SIM_RULE_COUNT, 5.0),
               SIM_RULE_CHANGE_UNCHANGED, 0.0,
               "a value one past the last rule");

    return 0;
}

/* ── 3. Ratios, differences and the snap band ──────────────────────────── */

int run_sim_rules_describe_ratios(void) {
    const int    rule    = SIM_RULE_tank_full_shells;
    const double classic = simRulesClassicValue(rule);

    UT_ASSERT_MSG(classic > 0.0, "tank_full_shells has no classic stock");
    UT_ASSERT_MSG(simRulesRuleUnit(rule) == SIM_RULE_UNIT_COUNT,
                  "tank_full_shells is not tagged as a count");

    SRD_EXPECT(simRulesDescribeChange(rule, classic * 2.0),
               SIM_RULE_CHANGE_MORE, 2.0, "a count doubled");
    SRD_EXPECT(simRulesDescribeChange(rule, classic / 2.0),
               SIM_RULE_CHANGE_FEWER, 2.0, "a count halved");

    /* Well under half again either way: the difference is the answer. */
    SRD_EXPECT(simRulesDescribeChange(rule, classic + 4.0),
               SIM_RULE_CHANGE_DELTA, 4.0, "a count four higher");
    SRD_EXPECT(simRulesDescribeChange(rule, classic - 4.0),
               SIM_RULE_CHANGE_DELTA, -4.0, "a count four lower");

    /* The two sides of the five percent band around a whole multiple. 2.9
       is inside the band around three and snaps to it; 2.8 is outside and
       keeps its own decimal. */
    SRD_EXPECT(simRulesDescribeChange(rule, classic * 2.9),
               SIM_RULE_CHANGE_MORE, 3.0, "a ratio just inside the band");
    SRD_EXPECT(simRulesDescribeChange(rule, classic * 2.8),
               SIM_RULE_CHANGE_MORE, 2.8, "a ratio just outside the band");

    /* The same band the other way round, where the classic value is the
       larger of the two. */
    SRD_EXPECT(simRulesDescribeChange(rule, classic / 2.9),
               SIM_RULE_CHANGE_FEWER, 3.0,
               "a ratio just inside the band downwards");

    /* A speed row reads the other direction from an interval: more is
       faster, where fewer ticks is faster. */
    SRD_EXPECT(simRulesDescribeChange(SIM_RULE_speed_road,
                   simRulesClassicValue(SIM_RULE_speed_road) * 2.0),
               SIM_RULE_CHANGE_FASTER, 2.0, "the road speed doubled");
    SRD_EXPECT(simRulesDescribeChange(SIM_RULE_speed_road,
                   simRulesClassicValue(SIM_RULE_speed_road) / 2.0),
               SIM_RULE_CHANGE_SLOWER, 2.0, "the road speed halved");

    return 0;
}

/* ── 4. What each tag answers ──────────────────────────────────────────── */

int run_sim_rules_describe_units(void) {
    int i;

    /* The flag arm, asked directly and then through the one rule that
       carries the tag. A percentage names no rule today, so it is asked of
       the arm directly — the first rule that wants the tag finds the arm
       already answering. */
    SRD_EXPECT(simRulesDescribeValue(SIM_RULE_UNIT_FLAG, 0.0, 1.0),
               SIM_RULE_CHANGE_ON, 0.0, "a flag turned on");
    SRD_EXPECT(simRulesDescribeValue(SIM_RULE_UNIT_FLAG, 0.0, 0.0),
               SIM_RULE_CHANGE_UNCHANGED, 0.0, "a flag left off");
    SRD_EXPECT(simRulesDescribeValue(SIM_RULE_UNIT_FLAG, 1.0, 0.0),
               SIM_RULE_CHANGE_OFF, 0.0, "a flag turned off");
    UT_ASSERT_MSG(simRulesRuleUnit(SIM_RULE_pill_shell_cap) ==
                      SIM_RULE_UNIT_FLAG,
                  "pill_shell_cap is not tagged as a flag");
    SRD_EXPECT(simRulesDescribeChange(SIM_RULE_pill_shell_cap, 1.0),
               SIM_RULE_CHANGE_ON, 0.0, "the pill shell cap turned on");

    SRD_EXPECT(simRulesDescribeValue(SIM_RULE_UNIT_PERCENT, 50.0, 100.0),
               SIM_RULE_CHANGE_MORE, 2.0, "a percentage doubled");
    SRD_EXPECT(simRulesDescribeValue(SIM_RULE_UNIT_PERCENT, 50.0, 25.0),
               SIM_RULE_CHANGE_FEWER, 2.0, "a percentage halved");

    /* A weight in the tree draw: the number itself is the answer, and it
       stays the answer where a ratio could not be taken at all. */
    UT_ASSERT_MSG(srdSame(simRulesClassicValue(SIM_RULE_tree_weight_road),
                          -100.0),
                  "tree_weight_road is %.4f, not the -100 this case reads",
                  simRulesClassicValue(SIM_RULE_tree_weight_road));
    SRD_EXPECT(simRulesDescribeChange(SIM_RULE_tree_weight_road, -200.0),
               SIM_RULE_CHANGE_RAW, -200.0, "a negative weight doubled");
    UT_ASSERT_MSG(srdSame(simRulesClassicValue(SIM_RULE_tree_weight_deep_sea),
                          0.0),
                  "tree_weight_deep_sea is %.4f, not the 0 this case reads",
                  simRulesClassicValue(SIM_RULE_tree_weight_deep_sea));
    SRD_EXPECT(simRulesDescribeChange(SIM_RULE_tree_weight_deep_sea, 5.0),
               SIM_RULE_CHANGE_RAW, 5.0, "a weight raised off zero");

    /* A default of zero on a row that would otherwise take a ratio has
       none to take, so the difference is the answer. */
    SRD_EXPECT(simRulesDescribeValue(SIM_RULE_UNIT_COUNT, 0.0, 8.0),
               SIM_RULE_CHANGE_DELTA, 8.0, "a count raised off zero");
    SRD_EXPECT(simRulesDescribeValue(SIM_RULE_UNIT_COUNT, 8.0, 0.0),
               SIM_RULE_CHANGE_DELTA, -8.0, "a count dropped to zero");
    SRD_EXPECT(simRulesDescribeValue(SIM_RULE_UNIT_SPEED_HIGHER_IS_FASTER,
                                     -4.0, -8.0),
               SIM_RULE_CHANGE_DELTA, -4.0, "a negative rate lowered");

    /* Every rule at its own default says so, which is the case that fails
       when a row's value kind reads the wrong half of the table. */
    for (i = 0; i < simRulesRuleCount(); i++) {
        SimRuleChange c = simRulesDescribeChange(i, simRulesClassicValue(i));

        UT_ASSERT_MSG(c.kind == SIM_RULE_CHANGE_UNCHANGED,
                      "%s at its own default answered %s",
                      simRulesRuleName(i), srdKindName(c.kind));
    }

    return 0;
}

/* ── 5. The words a change is drawn as ─────────────────────────────────── */

/* The lang table is not linked here: test_stubs.c answers "?" for every id,
 * so a phrase that went to the table reads "?" and the cases below check the
 * arms that write the number themselves — the difference and the raw value —
 * and what an index that names no rule leaves behind. */
int run_sim_rules_phrase(void) {
    const double shells = simRulesClassicValue(SIM_RULE_tank_full_shells);
    const double river  = simRulesClassicValue(SIM_RULE_turn_river);
    char         out[64];

    UT_ASSERT_MSG(srdSame(river, 0.25),
                  "turn_river is %.4f, not the 0.25 these cases read", river);

    /* A count's difference: one place, the ".0" gone, the plus kept. */
    simRulesPhrase(SIM_RULE_tank_full_shells, shells + 4.0, out, sizeof(out));
    UT_ASSERT_MSG(strcmp(out, "+4") == 0,
                  "a count four higher reads '%s', expected '+4'", out);
    simRulesPhrase(SIM_RULE_tank_full_shells, shells - 4.0, out, sizeof(out));
    UT_ASSERT_MSG(strcmp(out, "-4") == 0,
                  "a count four lower reads '%s', expected '-4'", out);

    /* A rate moves by hundredths, so its difference keeps two places: the
       "+0" one place gives is not an answer. Trailing zeros still go, and
       the point with them. */
    simRulesPhrase(SIM_RULE_turn_river, 0.27, out, sizeof(out));
    UT_ASSERT_MSG(strcmp(out, "+0.02") == 0,
                  "a turn rate raised by 0.02 reads '%s', expected '+0.02'",
                  out);
    simRulesPhrase(SIM_RULE_turn_river, 0.2, out, sizeof(out));
    UT_ASSERT_MSG(strcmp(out, "-0.05") == 0,
                  "a turn rate lowered by 0.05 reads '%s', expected '-0.05'",
                  out);
    simRulesPhrase(SIM_RULE_turn_river, 0.35, out, sizeof(out));
    UT_ASSERT_MSG(strcmp(out, "+0.1") == 0,
                  "a turn rate raised by 0.1 reads '%s', expected '+0.1'",
                  out);

    /* A rate typed past the field's precision is the field's own value. */
    SRD_EXPECT(simRulesDescribeChange(SIM_RULE_turn_river, 0.25 + 1e-9),
               SIM_RULE_CHANGE_UNCHANGED, 0.0,
               "a turn rate a billionth off its default");

    /* A weight in the tree draw is its raw number, no sign added. */
    simRulesPhrase(SIM_RULE_tree_weight_road, -200.0, out, sizeof(out));
    UT_ASSERT_MSG(strcmp(out, "-200") == 0,
                  "a weight of -200 reads '%s', expected '-200'", out);

    /* The arms that go to the lang table come back as the stub's
       placeholder, which is the proof they went there rather than writing a
       number of their own. */
    simRulesPhrase(SIM_RULE_tank_full_shells, shells * 2.0, out, sizeof(out));
    UT_ASSERT_MSG(strcmp(out, "?") == 0,
                  "a count doubled reads '%s', expected the lang stub's '?'",
                  out);
    simRulesPhrase(SIM_RULE_tank_full_shells, shells, out, sizeof(out));
    UT_ASSERT_MSG(strcmp(out, "?") == 0,
                  "a count at its default reads '%s', expected the lang "
                  "stub's '?'", out);

    /* No rule, nothing beside it; and no room, nothing touched. */
    strcpy(out, "stale");
    simRulesPhrase(-1, 5.0, out, sizeof(out));
    UT_ASSERT_MSG(out[0] == '\0', "index -1 wrote '%s'", out);
    strcpy(out, "stale");
    simRulesPhrase((int)SIM_RULE_COUNT, 5.0, out, sizeof(out));
    UT_ASSERT_MSG(out[0] == '\0', "one past the last rule wrote '%s'", out);
    strcpy(out, "kept");
    simRulesPhrase(SIM_RULE_tank_full_shells, shells + 4.0, out, 0);
    UT_ASSERT_MSG(strcmp(out, "kept") == 0,
                  "a zero-length buffer was written: '%s'", out);

    return 0;
}
