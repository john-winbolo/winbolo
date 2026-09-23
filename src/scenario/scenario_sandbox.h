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
   depth count of their own.

   stopped travels with the other two because it belongs to a call in the same
   way: it says that this call has already been stopped once, and an inner
   call must neither see the outer one's answer nor leave its own behind.

   printed and printSaid are the console lines this call has put out and
   whether it has already been told its allowance is spent. They travel for
   the reason instr does: an inner call that handed the outer one a fresh
   allowance would leave the per-call bound meaning nothing. */
typedef struct {
    bool     armed;
    bool     stopped;
    uint32_t instr;
    uint32_t printed;
    bool     printSaid;
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
 *  Being stopped is latched here as well, and only here and
 *  in the disarm: pcall, xpcall and coroutine.resume are all
 *  in the library a script is given, and the latch is what
 *  says a failure one of them caught was the budget's and is
 *  raised again rather than kept.
 *
 *  The budget is per call, so every call gets the whole of
 *  it however many the round has already made. The console
 *  lines print may put out are counted the same way, against
 *  SCN_PRINT_PER_CALL, and go back to zero here with it.
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
 *
 *  The stopped latch goes back with the count, so a call that
 *  was stopped leaves nothing behind for the next one to be
 *  refused by.
 *********************************************************/
void scnSandboxDisarmCall(lua_State *L, const ScnSandboxCall *saved);

/*********************************************************
 *NAME:          scnSandboxTickReset
 *PURPOSE:
 *  A new tick: the console lines print may put out across
 *  everything this tick calls go back to SCN_PRINT_PER_TICK.
 *
 *  A tick runs a script's hooks, the timers it set and the
 *  policies it answers, so the per-call bound alone would
 *  multiply by however many of those a script arranges. This
 *  is the bound that holds whatever the shape of them.
 *
 *  It also opens the tick's window: from here until
 *  scnSandboxTickClose every armed call's instructions are
 *  counted against SCN_BUDGET_TICK_INSTR as well as against
 *  the call's own budget, from zero and with the tick's latch
 *  clear. The call that takes the total past that is stopped
 *  with a Lua error, and the latch it sets is what
 *  scnSandboxTickSpent answers and what pcall, xpcall and
 *  coroutine.resume raise again for.
 *
 *  Called once per tick, before the tick runs anything, by
 *  whoever drives the scenario's tick. NULL is a round with
 *  no state and nothing to reset.
 *********************************************************/
void scnSandboxTickReset(lua_State *L);

/*********************************************************
 *NAME:          scnSandboxTickClose
 *PURPOSE:
 *  Closes the tick's window and clears its latch. Every call
 *  made from here until the next reset is bounded by its own
 *  call's budget alone and is never skipped for what the tick
 *  spent: on_end, policies the engine asks between ticks, and
 *  the lobby's asks from the GUI thread.
 *
 *  Called by whoever drives the tick, once the tick's own
 *  calls are over. NULL is nothing to close.
 *********************************************************/
void scnSandboxTickClose(lua_State *L);

/*********************************************************
 *NAME:          scnSandboxTickSpent
 *PURPOSE:
 *  True while the tick's window is open and its total has
 *  been spent, which is the host's answer to whether a call
 *  the tick still has to make should be made at all. False
 *  outside a tick, and for NULL.
 *********************************************************/
bool scnSandboxTickSpent(lua_State *L);

/*********************************************************
 *NAME:          scnSandboxCharge
 *PURPOSE:
 *  Charges n instructions to the call that is running, as
 *  the count hook charges a step: to the call's budget, and
 *  to the tick's while a tick is open.
 *
 *  C work the hook cannot see is charged here so the budgets
 *  see it. Where the charge takes the call or the tick past
 *  its budget this raises the budget's own error and sets
 *  the same latch the hook does, so pcall, xpcall and
 *  coroutine.resume raise it again rather than keeping it.
 *  A caller must be able to unwind from that like any other
 *  Lua error.
 *
 *  Does nothing while no call is armed, as the hook does.
 *********************************************************/
void scnSandboxCharge(lua_State *L, uint32_t n);

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

/*********************************************************
 *NAME:          scnSandboxPushBase
 *PURPOSE:
 *  Pushes a table holding type, select and rawget as
 *  scnSandboxOpenLibs installed them. The trigger router
 *  takes it as a chunk argument: it loads after the
 *  author's script has run, and a script that assigned
 *  one of those three names would otherwise take the
 *  router down with it. On a state this file did not
 *  open the table is empty and the router reads the
 *  globals.
 *********************************************************/
void scnSandboxPushBase(lua_State *L);

/*********************************************************
 *NAME:          scnSandboxOpenPrint
 *PURPOSE:
 *  Sets the global print to the sandbox's own, and does
 *  nothing else. For a state opened with the whole standard
 *  library rather than through scnSandboxOpenLibs.
 *
 *  Such a state has no per-state record beside it, so
 *  scnSandboxPrintTake lets every line through uncapped.
 *  The only thing this changes is where the output goes:
 *  the server console, where the operator and the desktop
 *  client look, rather than the host's stdout.
 *********************************************************/
void scnSandboxOpenPrint(lua_State *L);

#endif /* SCENARIO_SANDBOX_H */
