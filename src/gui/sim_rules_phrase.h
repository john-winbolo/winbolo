/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Simulation Rule Phrase
 *Filename:      sim_rules_phrase.h
 *Author:        John Morrison
 *Purpose:
 *  What a rule's value does to it, in words: "5x faster",
 *  "half as many", "off", "unchanged". The one place a
 *  SimRuleChange (public/sim_rules_names.h) becomes a
 *  phrase, so the lobby's popup, the viewer's rules display
 *  and the editor's rules form all say the same thing about
 *  the same value rather than each wording it their own way.
 *
 *  And what the rule itself is: the line of prose saying
 *  what it governs, and the range it accepts in words. Both
 *  screens ask here, so a rule reads the same wherever it
 *  is shown.
 *
 *  Here rather than in bolo_static because the wording is
 *  localised and the lang table is a gui thing;
 *  simRulesDescribeChange below it answers in a kind and a
 *  number, and simRulesRuleRange answers in bounds, which is
 *  what keeps the sim free of the lang table.
 *********************************************************/

#ifndef SIM_RULES_PHRASE_H
#define SIM_RULES_PHRASE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes the phrase for value on rule into out, always NUL-terminated.
 * An index that names no rule, or no room to write into, leaves out empty.
 * A value equal to the rule's classic default reads "unchanged" rather than
 * as nothing, so a caller drawing a column has something to draw. */
void simRulesPhrase(int rule, double value, char *out, size_t outLen);

/* What the rule does, one sentence. "" for an index that names no rule. */
const char *simRulesRuleDescription(int rule);

/* The rule's range in words: "0 to 255", "0 and up",
 * "1 and up, at most pill_attack_ticks". Always NUL-terminated; empty for
 * an index that names no rule. A capping rule's name is written as it is
 * spelled, never translated. */
void simRulesRangePhrase(int rule, char *out, size_t outLen);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SIM_RULES_PHRASE_H */
