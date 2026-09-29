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
 * What a rule is, in words:
 *
 *   sim_rules_desc_table        — every rule has a description id of its own,
 *       inside the block those ids were given, no id used twice, and the
 *       accessor answers something for every rule and nothing for an index
 *       that names none.
 *   sim_rules_desc_range_phrase — the range phrase writes something for every
 *       rule, always NUL-terminates, writes nothing for an index that names
 *       no rule, and stays inside the length it is given.
 *
 * Neither case reads the English. test_stubs.c answers "?" for every lang id
 * in this binary, so the prose itself lives in src/gui/sdl3/lang.c and is
 * checked by eye; what a case here can hold is the wiring — that a rule has
 * an id, that the id is its own, and that both accessors answer safely
 * whatever index they are handed.
 *
 * The id list below is the same expansion of SIM_RULE_LIST the accessor's own
 * table is built from, so a rule added without a STR_RULE_DESC_ row of its
 * own stops this file compiling as well as that one.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "lang.h"
#include "sim_rules.h"
#include "sim_rules_names.h"
#include "sim_rules_phrase.h"
#include "test_harness.h"

#define SRD_ID_ROW(name, kind, unit) STR_RULE_DESC_##name,
static const langid srdDescIds[] = {
    SIM_RULE_LIST(SRD_ID_ROW)
};
#undef SRD_ID_ROW

/* The stretches of ids the descriptions were given, either end of each
 * included. They are four rather than one because the first eleven rules'
 * numbers were taken by the fog style strings and moved to the end of the
 * file, because the rules the table gained later were numbered on from
 * there, because the pillmassage pair start again past the map editor's
 * scenario strings, and because the pill shell cap pair took the next free
 * numbers after everything else. See the block comment in lang.h. */
#define SRD_FIRST_ID  2345u
#define SRD_LAST_ID   2425u
#define SRD_FIRST_ID2 2486u
#define SRD_LAST_ID2  2549u
#define SRD_FIRST_ID3 2575u
#define SRD_LAST_ID3  2576u
#define SRD_FIRST_ID4 2698u
#define SRD_LAST_ID4  2699u

/* ── 1. A description id per rule ──────────────────────────────────────── */

int run_sim_rules_desc_table(void) {
    const int count = (int)(sizeof(srdDescIds) / sizeof(srdDescIds[0]));
    int       i;
    int       j;

    UT_ASSERT_MSG(count == simRulesRuleCount(),
                  "there are %d description ids and %d rules", count,
                  simRulesRuleCount());

    for (i = 0; i < count; i++) {
        const char *name = simRulesRuleName(i);
        const char *desc = simRulesRuleDescription(i);

        UT_ASSERT_MSG((srdDescIds[i] >= SRD_FIRST_ID &&
                       srdDescIds[i] <= SRD_LAST_ID) ||
                          (srdDescIds[i] >= SRD_FIRST_ID2 &&
                           srdDescIds[i] <= SRD_LAST_ID2) ||
                          (srdDescIds[i] >= SRD_FIRST_ID3 &&
                           srdDescIds[i] <= SRD_LAST_ID3) ||
                          (srdDescIds[i] >= SRD_FIRST_ID4 &&
                           srdDescIds[i] <= SRD_LAST_ID4),
                      "%s has id %u, outside the %u..%u, %u..%u, %u..%u and "
                      "%u..%u the descriptions were given",
                      name, srdDescIds[i], SRD_FIRST_ID, SRD_LAST_ID,
                      SRD_FIRST_ID2, SRD_LAST_ID2, SRD_FIRST_ID3,
                      SRD_LAST_ID3, SRD_FIRST_ID4, SRD_LAST_ID4);
        UT_ASSERT_MSG(desc != NULL, "%s answered a NULL description", name);
        UT_ASSERT_MSG(desc[0] != '\0', "%s answered an empty description",
                      name);

        for (j = 0; j < i; j++) {
            UT_ASSERT_MSG(srdDescIds[i] != srdDescIds[j],
                          "%s and %s share id %u", simRulesRuleName(j), name,
                          srdDescIds[i]);
        }
    }

    /* An index that names no rule has nothing to describe. */
    UT_ASSERT_MSG(simRulesRuleDescription(-1)[0] == '\0',
                  "index -1 answered a description");
    UT_ASSERT_MSG(simRulesRuleDescription(simRulesRuleCount())[0] == '\0',
                  "one past the last rule answered a description");

    return 0;
}

/* ── 2. The range in words ─────────────────────────────────────────────── */

int run_sim_rules_desc_range_phrase(void) {
    char out[128];
    int  i;

    for (i = 0; i < simRulesRuleCount(); i++) {
        const char *name = simRulesRuleName(i);

        memset(out, 'x', sizeof(out));
        simRulesRangePhrase(i, out, sizeof(out));
        UT_ASSERT_MSG(memchr(out, '\0', sizeof(out)) != NULL,
                      "%s left its range phrase unterminated", name);
        UT_ASSERT_MSG(out[0] != '\0', "%s has no range phrase", name);
    }

    /* No rule, nothing beside it. */
    strcpy(out, "stale");
    simRulesRangePhrase(-1, out, sizeof(out));
    UT_ASSERT_MSG(out[0] == '\0', "index -1 wrote '%s'", out);
    strcpy(out, "stale");
    simRulesRangePhrase(simRulesRuleCount(), out, sizeof(out));
    UT_ASSERT_MSG(out[0] == '\0', "one past the last rule wrote '%s'", out);

    /* A buffer with no room is left as it was, and a short one is terminated
       inside its own length with nothing written past it. */
    strcpy(out, "kept");
    simRulesRangePhrase(SIM_RULE_gunsight_min, out, 0);
    UT_ASSERT_MSG(strcmp(out, "kept") == 0,
                  "a zero-length buffer was written: '%s'", out);

    memset(out, 'x', sizeof(out));
    simRulesRangePhrase(SIM_RULE_gunsight_min, out, 4);
    UT_ASSERT_MSG(memchr(out, '\0', 4) != NULL,
                  "a four-byte buffer came back unterminated");
    for (i = 4; i < (int)sizeof(out); i++) {
        UT_ASSERT_MSG(out[i] == 'x',
                      "the range phrase wrote past the length it was given, "
                      "at byte %d",
                      i);
    }

    return 0;
}
