/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_scenario_fndesc.h
 * Purpose:
 *   The one line the editor shows beside each hook and
 *   policy a scenario author can write.
 *
 *   The catalogue in scenario_lua.h says what the functions
 *   are, what kind each is and what it is handed; it does
 *   not say what any of them is for, because the scenario
 *   library has no lang table to say it in. This is that
 *   half, and it lives here rather than beside the rules'
 *   equivalent in src/gui/sdl3 because reaching the two list
 *   macros means including scenario_lua.h, which names Lua
 *   types. mapeditor_scenario_check.c already includes it
 *   and builds under both the standalone editor and the
 *   embedded one, so this file asks nothing new of either.
 *
 *   docs/SCENARIO_API.md is where each function is set out
 *   at length. A line here is the short form for a list, not
 *   a replacement for that.
 *********************************************************/

#ifndef MAPEDITOR_SCENARIO_FNDESC_H
#define MAPEDITOR_SCENARIO_FNDESC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the function does, one sentence. "" for a row that is not in the
 * catalogue. row indexes scenarioLuaFunctions, whose order this table
 * follows because both come from the same two lists. */
const char *meScnFnDescription(size_t row);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_SCENARIO_FNDESC_H */
