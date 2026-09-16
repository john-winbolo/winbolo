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

#include "sim_rules_names.h"
#include "../lang.h"
#include "../sim_rules_phrase.h"

/* A number with one decimal, the trailing ".0" trimmed, so a whole multiple
 * reads "5x faster" rather than "5.0x faster". sign true puts a "+" on a
 * positive number, which is what a difference wants and a multiple does
 * not. The minus is the ASCII one: it sits beside digits the same font
 * draws, and a lang file is free to write the Unicode minus in its own
 * wording if it wants one. */
static void simRulesPhraseNumber(double number, bool sign, char *buf,
                                 size_t bufLen) {
    size_t len;

    SDL_snprintf(buf, bufLen, (sign && number > 0.0) ? "+%.1f" : "%.1f",
                 number);
    len = SDL_strlen(buf);
    if (len > 2 && buf[len - 2] == '.' && buf[len - 1] == '0') {
        buf[len - 2] = '\0';
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
               translate: "+20", "-3". */
            simRulesPhraseNumber(change.number, true, out, outLen);
            return;
        case SIM_RULE_CHANGE_RAW:
            simRulesPhraseNumber(change.number, false, out, outLen);
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

    simRulesPhraseNumber(change.number, false, args.string1,
                         sizeof(args.string1));
    SDL_strlcpy(out, langGetTextFmt(id, &args), outLen);
}
