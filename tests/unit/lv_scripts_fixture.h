/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
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

/* Record the scripted round to a scratch .wbv named after tag and write its
 * path into path. The scenarios, the sim and the host are gone on return;
 * the .wbv is left for the caller to decode and remove. False on any
 * failure, with nothing left behind but possibly the .wbv. */
bool lvScriptsRecordScriptedRound(const char *tag, char *path, size_t pathLen);

#endif /* WINBOLO_TEST_LV_SCRIPTS_FIXTURE_H */
