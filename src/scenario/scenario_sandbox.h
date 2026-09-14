/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Sandbox
 *Filename:      scenario_sandbox.h
 *Author:        John Morrison
 *Purpose:
 *  What a scenario state is made of: the state itself, whose
 *  memory is counted against a cap, the few standard
 *  libraries a script is given, the names taken back out of
 *  them, and print routed to the server console.
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

#include <stdbool.h>

#include <lua.h>

/*********************************************************
 *NAME:          scnSandboxNewState
 *PURPOSE:
 *  A bare Lua state whose allocations are counted against
 *  SCN_VM_MEMORY_MAX: the allocation that would take it past
 *  the cap is refused, which Lua raises as an out-of-memory
 *  error the caller's lua_pcall catches. NULL when there is
 *  no memory for a state at all.
 *
 *  No libraries and no game table — whoever boots the state
 *  puts those on.
 *********************************************************/
lua_State *scnSandboxNewState(void);

/*********************************************************
 *NAME:          scnSandboxCloseState
 *PURPOSE:
 *  Closes a state scnSandboxNewState made and frees what it
 *  was counted with. NULL is nothing to close.
 *
 *  Every scenario state closes through this: the count lives
 *  beside the state on the heap, so a plain lua_close would
 *  leak it.
 *********************************************************/
void scnSandboxCloseState(lua_State *L);

/*********************************************************
 *NAME:          scnSandboxMemoryCapped
 *PURPOSE:
 *  Whether the states this process boots are counted. False
 *  on a build whose Lua refuses a custom allocator, where a
 *  state is made the ordinary way and nothing bounds it.
 *
 *  The answer is settled by the first state the process
 *  boots, which is where the refusal would show; before that
 *  it answers as the build intends.
 *********************************************************/
bool scnSandboxMemoryCapped(void);

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
