/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A scripted round recorded to a .wbv, for the log viewer's scripts.json
 * cases.
 *
 * The recording needs the sim and the scenario host, and the cases that read
 * it back need the viewer's headers; the two worlds cannot share a
 * translation unit (see replay_harness.h). So the recording lives in
 * lv_scripts_fixture.c, on the sim side, and hands back only a path.
 *
 * The round is the committed map's own scenario followed by one mod from the
 * server's scenarios directory. The names and values below are what it
 * declares, so a case can check what the viewer read against them.
 */

#ifndef WINBOLO_TEST_LV_SCRIPTS_FIXTURE_H
#define WINBOLO_TEST_LV_SCRIPTS_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>

#define LVSF_MAP_LEAF       "wbtest_lv_scripts.map"
#define LVSF_MAP_SCRIPT     "wbtest_lv_scripts.scenario.lua"
#define LVSF_MOD_FILE       "lvmod.lua"
#define LVSF_SCENARIO_NAME  "Viewer Scenario"
#define LVSF_MOD_NAME       "Viewer Mod"
#define LVSF_RULE_NAME      "tank_death_ticks"
#define LVSF_RULE_VALUE     400
#define LVSF_REGION_NAME    "keep"
#define LVSF_REGION_X       10
#define LVSF_REGION_Y       12
#define LVSF_REGION_W       6
#define LVSF_REGION_H       4

/* The rule-change round: the same map scenario name and mod, but its rules
 * table sets tank_full_shells, and its on_tick raises pill_max_armour from the
 * classic 15 on the LVSF_RULE_SET_FRAME'th frame, which the round records as
 * one log_RuleSet a little way into the file. The round runs
 * LVSF_RULE_ROUND_TICKS sim ticks after the recording opens, so the change has
 * frames on both sides of it. */
#define LVSF_RULE_SHELLS       60
#define LVSF_PILL_CAP          30
#define LVSF_RULE_SET_FRAME    10
#define LVSF_RULE_ROUND_TICKS  40

/* The presentation round: the same map scenario name and mod, and an on_tick
 * that on the LVSF_PRES_FRAME'th frame draws a one-rectangle panel for
 * everyone, gives team LVSF_PRES_TEAM a score of LVSF_PRES_SCORE labelled
 * LVSF_PRES_LABEL and puts LVSF_PRES_LINE across the screen for five
 * seconds. The round runs LVSF_PRES_ROUND_TICKS sim ticks after the recording
 * opens. */
#define LVSF_PRES_FRAME        10
#define LVSF_PRES_TEAM         1
#define LVSF_PRES_SCORE        7
#define LVSF_PRES_LABEL        "pts"
#define LVSF_PRES_LINE         "Hold the keep"
#define LVSF_PRES_ROUND_TICKS  40

#define LVSF_STR_(x) #x
#define LVSF_STR(x)  LVSF_STR_(x)

/* Record the scripted round to a scratch .wbv named after tag and write its
 * path into path. The scenarios, the sim and the host are gone on return;
 * the .wbv is left for the caller to decode and remove. False on any
 * failure, with nothing left behind but possibly the .wbv. */
bool lvScriptsRecordScriptedRound(const char *tag, char *path, size_t pathLen);

/* The same for the rule-change round. */
bool lvScriptsRecordRuleRound(const char *tag, char *path, size_t pathLen);

/* The same for the presentation round. */
bool lvScriptsRecordPresentationRound(const char *tag, char *path,
                                      size_t pathLen);

#endif /* WINBOLO_TEST_LV_SCRIPTS_FIXTURE_H */
