/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Simulation Rule Phrase
 *Filename:      sim_rules_phrase.c
 *Author:        John Morrison
 *Purpose:
 *  Renders what simRulesDescribeChange answers as the words
 *  a player reads beside a rule's value.
 *
 *  The phrases are lang table strings, so a translation
 *  carries them; the number is formatted here and handed in
 *  as {string1} rather than {number}, because a multiple can
 *  be 2.6 and {number} renders an integer. SDL_snprintf is
 *  used rather than snprintf so the decimal point is a point
 *  whatever locale the player runs in — the lang file's own
 *  wording is what changes per language, not the number's
 *  punctuation.
 *********************************************************/

#include <SDL3/SDL.h>

#include "platform_types.h"  /* BOLO_STATIC_ASSERT */
#include "sim_rules_names.h"
#include "../lang.h"
#include "../sim_rules_phrase.h"

/* A number to at most `decimals` places, the trailing zeros and then the
 * point trimmed, so a whole multiple reads "5x faster" rather than "5.0x
 * faster" and a rate's difference reads "+0.02" rather than "+0.0". sign
 * true puts a "+" on a positive number, which is what a difference wants
 * and a multiple does not. The minus is the ASCII one: it sits beside
 * digits the same font draws, and a lang file is free to write the Unicode
 * minus in its own wording if it wants one. */
static void simRulesPhraseNumber(double number, bool sign, int decimals,
                                 char *buf, size_t bufLen) {
    size_t len;

    SDL_snprintf(buf, bufLen, (sign && number > 0.0) ? "+%.*f" : "%.*f",
                 decimals, number);
    if (SDL_strchr(buf, '.') == NULL) {
        return;
    }
    len = SDL_strlen(buf);
    while (len > 0 && buf[len - 1] == '0') {
        buf[--len] = '\0';
    }
    if (len > 0 && buf[len - 1] == '.') {
        buf[--len] = '\0';
    }
}

void simRulesPhrase(int rule, double value, char *out, size_t outLen) {
    SimRuleChange change;
    MessageArgs   args;
    langid        id;

    if (out == NULL || outLen == 0) {
        return;
    }
    out[0] = '\0';
    if (rule < 0 || rule >= simRulesRuleCount()) {
        /* No rule, so nothing to say beside it. */
        return;
    }

    change = simRulesDescribeChange(rule, value);
    SDL_memset(&args, 0, sizeof(args));

    switch (change.kind) {
        case SIM_RULE_CHANGE_UNCHANGED:
            SDL_strlcpy(out, langGetText(STR_RULE_UNCHANGED), outLen);
            return;
        case SIM_RULE_CHANGE_ON:
            SDL_strlcpy(out, langGetText(STR_RULE_ON), outLen);
            return;
        case SIM_RULE_CHANGE_OFF:
            SDL_strlcpy(out, langGetText(STR_RULE_OFF), outLen);
            return;
        case SIM_RULE_CHANGE_DELTA:
            /* The number is the whole answer, so there is nothing to
               translate: "+20", "-3". A rate row moves by hundredths, so it
               gets two places where a count gets one. */
            simRulesPhraseNumber(
                change.number, true,
                (simRulesRuleValueKind(rule) == SIM_RULE_VALUE_FLOAT) ? 2 : 1,
                out, outLen);
            return;
        case SIM_RULE_CHANGE_RAW:
            simRulesPhraseNumber(change.number, false, 1, out, outLen);
            return;
        case SIM_RULE_CHANGE_FASTER:
            id = STR_RULE_FASTER;
            break;
        case SIM_RULE_CHANGE_SLOWER:
            id = STR_RULE_SLOWER;
            break;
        case SIM_RULE_CHANGE_MORE:
            /* Exactly double reads as a phrase rather than as a multiple:
               "twice as many" is what a player says, and "half as many" is
               the same answer the other way. */
            id = (change.number == 2.0) ? STR_RULE_TWICE : STR_RULE_MORE;
            break;
        case SIM_RULE_CHANGE_FEWER:
            id = (change.number == 2.0) ? STR_RULE_HALF : STR_RULE_FEWER;
            break;
        default:
            return;
    }

    simRulesPhraseNumber(change.number, false, 1, args.string1,
                         sizeof(args.string1));
    SDL_strlcpy(out, langGetTextFmt(id, &args), outLen);
}

/* ---- What the rule is, and what it takes ---------------------------------
 *
 * One description id per rule, read off SIM_RULE_LIST itself, so the table is
 * that list and a rule added without a STR_RULE_DESC_ row of its own does not
 * compile. */

#define SIM_RULE_DESC_ROW(name, kind, unit) STR_RULE_DESC_##name,
static const langid simRuleDescIds[] = {
    SIM_RULE_LIST(SIM_RULE_DESC_ROW)
};
#undef SIM_RULE_DESC_ROW

BOLO_STATIC_ASSERT(sizeof(simRuleDescIds) / sizeof(simRuleDescIds[0]) ==
                       (size_t)SIM_RULE_COUNT,
                   sim_rule_descriptions_cover_every_rule);

const char *simRulesRuleDescription(int rule) {
    if (rule < 0 || rule >= simRulesRuleCount()) {
        return "";
    }
    return langGetText(simRuleDescIds[rule]);
}

void simRulesRangePhrase(int rule, char *out, size_t outLen) {
    SimRuleRange range;
    MessageArgs  args;
    char         ends[LANG_MSGARG_STRING_LEN];

    if (out == NULL || outLen == 0) {
        return;
    }
    out[0] = '\0';
    if (!simRulesRuleRange(rule, &range)) {
        /* No rule, so no range to state. */
        return;
    }

    /* Two places on the ends, trimmed: a rate's floor is 0.01 and a count's
       is 0, and the same call writes both. */
    SDL_memset(&args, 0, sizeof(args));
    simRulesPhraseNumber(range.lo, false, 2, args.string1,
                         sizeof(args.string1));
    if (range.hasHi) {
        simRulesPhraseNumber(range.hi, false, 2, args.string2,
                             sizeof(args.string2));
        SDL_strlcpy(ends, langGetTextFmt(STR_RULE_RANGE_BETWEEN, &args),
                    sizeof(ends));
    } else {
        SDL_strlcpy(ends, langGetTextFmt(STR_RULE_RANGE_FROM, &args),
                    sizeof(ends));
    }

    if (range.cappedBy < 0) {
        SDL_strlcpy(out, ends, outLen);
        return;
    }

    /* A rule another rule caps carries both halves: its own ends, and the
       rule above it. That rule is named rather than described — the name is
       what a manifest and a script spell, and no language changes it. */
    SDL_memset(&args, 0, sizeof(args));
    SDL_strlcpy(args.string1, ends, sizeof(args.string1));
    SDL_strlcpy(args.string2, simRulesRuleName(range.cappedBy),
                sizeof(args.string2));
    SDL_strlcpy(out, langGetTextFmt(STR_RULE_RANGE_CAPPED, &args), outLen);
}
