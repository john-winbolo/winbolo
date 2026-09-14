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
#include <stdint.h>

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
 *  It carries the count hook the call budget is kept by, set
 *  here and never cleared, so a state made this way runs
 *  interpreted on a LuaJIT host.
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

/* What a call into script code saves while it runs, so one made from inside
   another leaves the outer one's count as it found it. The caller keeps it
   on its own C stack, which is what makes the pair re-entrant without a
   depth count of their own. */
typedef struct {
    bool     armed;
    uint32_t instr;
} ScnSandboxCall;

/*********************************************************
 *NAME:          scnSandboxArmCall
 *PURPOSE:
 *  Start one call into script code: the instruction count
 *  goes back to zero and SCN_BUDGET_CALL_INSTR begins to
 *  apply. A call that passes it is stopped with a Lua error
 *  where the script stands, which the caller's lua_pcall
 *  catches.
 *
 *  The budget is per call, so every call gets the whole of
 *  it however many the round has already made.
 *
 *  What was running is written into saved, for the disarm to
 *  put back. Calls nest — a script's hook issues an op, the
 *  op asks a policy, and the policy is script code arriving
 *  on the same thread — and the inner one must not leave the
 *  outer uncounted.
 *********************************************************/
void scnSandboxArmCall(lua_State *L, ScnSandboxCall *saved);

/*********************************************************
 *NAME:          scnSandboxDisarmCall
 *PURPOSE:
 *  The call is over: whatever was running before it is put
 *  back. Where nothing was, that leaves the state uncounted,
 *  so what the host does next — reading the table the script
 *  declared, releasing what a round was holding — is its own.
 *  Where an outer call was, that one goes on counting from
 *  where it had reached, so nesting cannot be used to start
 *  a budget again.
 *
 *  Called on the path where the call raised as well as the
 *  one where it returned, with the saved value its own arm
 *  wrote.
 *********************************************************/
void scnSandboxDisarmCall(lua_State *L, const ScnSandboxCall *saved);

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
