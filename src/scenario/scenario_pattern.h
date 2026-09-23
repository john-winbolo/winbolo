/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Pattern
 *Filename:      scenario_pattern.h
 *Author:        John Morrison
 *Purpose:
 *  string.find, string.match, string.gmatch and string.gsub
 *  as a scenario script is given them: Lua 5.4.7's pattern
 *  matcher, run on either VM, with its steps charged to the
 *  scenario's instruction budgets.
 *
 *  The sandbox installs these four in place of the VM's own
 *  and nothing else calls them. A bot brain's state keeps
 *  its VM's matcher.
 *
 *  This is the sandbox's header and not a frontend's. It
 *  names Lua types, so whatever includes it links
 *  lua_static; scenario_host.h, which is what a frontend
 *  includes, names none.
 *********************************************************/

#ifndef SCENARIO_PATTERN_H
#define SCENARIO_PATTERN_H

#include <lua.h>

/*********************************************************
 *NAME:          scnPatternFind
 *PURPOSE:
 *  string.find(s, pattern [, init [, plain]]) with 5.4's
 *  rules: the start and end of the first match and then its
 *  captures, or nil. A plain search, and a pattern with no
 *  special characters, compare bytes and run no pattern.
 *
 *  Its steps are charged to the running call; a search that
 *  runs past the call's budget or the tick's raises that
 *  budget's error where it stands.
 *********************************************************/
int scnPatternFind(lua_State *L);

/*********************************************************
 *NAME:          scnPatternMatch
 *PURPOSE:
 *  string.match(s, pattern [, init]) with 5.4's rules: the
 *  captures of the first match, the whole match where the
 *  pattern has none, or nil. Charged as find is.
 *********************************************************/
int scnPatternMatch(lua_State *L);

/*********************************************************
 *NAME:          scnPatternGmatch
 *PURPOSE:
 *  string.gmatch(s, pattern [, init]) with 5.4's rules,
 *  init among them: an iterator over the matches from init
 *  on, where an empty match right after the previous one is
 *  skipped. Each call of the iterator is charged for the
 *  steps it takes, against the call it is made from.
 *********************************************************/
int scnPatternGmatch(lua_State *L);

/*********************************************************
 *NAME:          scnPatternGsub
 *PURPOSE:
 *  string.gsub(s, pattern, repl [, n]) with 5.4's rules: the
 *  string with up to n matches replaced, and how many were.
 *  repl is a string whose % escapes are %0 to %9 and %%,
 *  any other raising an error, or a table or function
 *  asked with the first capture or all of them. Charged as
 *  find is; a replacement function's own Lua is counted by
 *  the hook as any other.
 *********************************************************/
int scnPatternGsub(lua_State *L);

#endif /* SCENARIO_PATTERN_H */
