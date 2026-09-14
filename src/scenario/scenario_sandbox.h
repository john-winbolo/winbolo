/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Sandbox
 *Filename:      scenario_sandbox.h
 *Author:        John Morrison
 *Purpose:
 *  The library a scenario state is opened with: the few
 *  standard libraries a script is given, the names taken
 *  back out of them, and print routed to the server
 *  console.
 *
 *  A scenario script travels inside a map file, so the
 *  state it runs in has no way to reach the filesystem, the
 *  process, native code or the VM's own internals: io,
 *  package, debug, ffi, jit and bit are never opened, and
 *  the loaders that would bring code in from elsewhere are
 *  removed from what is.
 *
 *  This is the library's header and the tests', not a
 *  frontend's. It names Lua types, so whatever includes it
 *  links lua_static; scenario_host.h, which is what a
 *  frontend includes, names none.
 *********************************************************/

#ifndef SCENARIO_SANDBOX_H
#define SCENARIO_SANDBOX_H

#include <lua.h>

/*********************************************************
 *NAME:          scnSandboxOpenLibs
 *PURPOSE:
 *  Opens the whitelisted standard libraries on a bare state
 *  and strips what the whitelist does not name. Every VM the
 *  host boots is opened through this rather than through the
 *  standard library as a whole.
 *
 *  math.randomseed survives this call so the host can seed
 *  the state; scnSandboxSealRandom removes it afterwards.
 *********************************************************/
void scnSandboxOpenLibs(lua_State *L);

/*********************************************************
 *NAME:          scnSandboxSealRandom
 *PURPOSE:
 *  Removes math.randomseed, once the host has drawn the
 *  round's seed. math.random still answers; what the script
 *  cannot do is reseed the sequence the host set.
 *********************************************************/
void scnSandboxSealRandom(lua_State *L);

#endif /* SCENARIO_SANDBOX_H */
