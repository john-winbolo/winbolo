/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Sandbox
 *Filename:      scenario_sandbox.c
 *Author:        John Morrison
 *Purpose:
 *  The state a scenario script runs in, and what it is
 *  given: base, coroutine, string, table, math, os and —
 *  where the VM has one — utf8, each opened by hand rather
 *  than the standard library being opened as a whole.
 *
 *  The state allocates through a counter of its own, so a
 *  script that eats memory is refused at SCN_VM_MEMORY_MAX
 *  and raises there instead of taking the server's process
 *  with it, and it carries a count hook, so a script that
 *  loops without end is stopped at SCN_BUDGET_CALL_INSTR
 *  instructions and raises there too, as does the call that
 *  takes all of one tick's calls together past
 *  SCN_BUDGET_TICK_INSTR. All of them land in the
 *  lua_pcall the host makes every call through — and the
 *  two budgets' errors land there whatever the script does
 *  with them, because pcall, xpcall and coroutine.resume are
 *  given to it wrapped in a closure that raises them again.
 *
 *  The count cannot see inside a C function, so a string.rep
 *  that builds a string of a gigabyte is one instruction to
 *  it, and only the memory cap would stand in the way. So
 *  the functions that build a string as long as the script
 *  asks — string.rep, string.format and table.concat — and
 *  the four that run a pattern over one — find, match,
 *  gmatch and gsub — are wrapped too, and refuse a string
 *  over SCN_STRING_MAX with an ordinary error the script is
 *  free to catch.
 *
 *  A short subject is no bound on a pattern, which can
 *  backtrack for as long as it likes over one. So the four
 *  pattern functions are not the VM's own: they run the
 *  matcher in scenario_pattern.c, which charges its steps
 *  to the budgets through scnSandboxCharge and is stopped
 *  as a loop in Lua would be.
 *
 *  Opening them one at a time is what takes io, package,
 *  debug and, under LuaJIT, ffi, jit and bit away: none of
 *  them is ever created, so there is nothing to strip. What
 *  is stripped is the rest — every global the openers left
 *  that the whitelist does not name, the loaders among them,
 *  the dump that turns a function into bytecode, and
 *  everything in os but the clock.
 *
 *  print is replaced rather than removed, because a script
 *  reporting what it did is worth having and stock print
 *  writes to the host's stdout, which a dedicated server's
 *  operator is not necessarily reading. Sending it to the
 *  console is also what makes it worth counting: a console
 *  line reaches the operator's message log, which is opened
 *  and closed for each line written to it, so the lines one
 *  call and one tick may print are bounded here too. And a
 *  print is one console line: control characters in what a
 *  script prints, newlines among them, reach the console as
 *  spaces.
 *
 *  os.date is wrapped for a different reason: the format
 *  reaches the host's own strftime, and the C libraries this
 *  server is built against do not agree on what a valid
 *  conversion character is — one of them ends the process
 *  over an invalid one rather than complaining. So the
 *  format is read here first.
 *
 *  This file compiles under the scenario_host profile: it
 *  sees src/bolo/public/ and src/bolo/scenario_api/, and
 *  nothing under src/bolo/internal/.
 *********************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "server_sim.h"            /* serverSimConsoleMessage */

#include "scenario_host.h"         /* SCN_VM_MEMORY_MAX */
#include "scenario_pattern.h"      /* scnPatternFind and the other three */
#include "scenario_sandbox.h"

/* One console line a script's print can produce. A line past this is cut
   rather than dropped, so an operator still sees what a long one was
   about. */
#define SCN_PRINT_LEN 1024

/* The longest format os.date will take. A date written out in full needs a
   fraction of this, and a long one is worth refusing on its own account:
   LuaJIT sizes its buffer at thirty bytes for every % in the format and grows
   it four times over if strftime writes nothing, so a format that is mostly
   percent signs asks for an allocation far larger than it looks. The memory
   cap catches that; saying no to it here is cheaper and reads better. */
#define SCN_DATE_FMT_LEN 256

/* ── What a state is counted with ─────────────────────────────────── */

/* What one state is counted by: the memory it holds, the instructions the
   call now running has spent, and the console lines print has put out — for
   the call now running and for the tick it is part of. It outlives the call
   that made the state, so it is on the heap rather than beside the caller.
   used and cap sit idle on a build whose Lua would not take the allocator;
   everything else is kept whatever the allocator turned out to be.

   stopped is the hook's latch: it is raised there and put back by the arm and
   the disarm, so nothing a script can reach clears it.

   The two said flags are what keeps the notice a drop produces to one line a
   window: the count alone would say it again on every line after the first,
   which is the flood the bound is there to stop.

   tickInstr, tickCharged, tickOpen and tickStopped are the tick's and not a
   call's. tickInstr is what the tick's total is checked against: what every
   call the tick has made spent between them, until a trip puts it back to a
   grace short of the total. tickCharged is what those calls really spent, and
   a trip does not put it back; it is what scnSandboxTickInstr answers. tickOpen
   says whether a tick is running at all, and tickStopped is the latch that says
   the tick's total is spent. The arm and the disarm leave all four alone, since
   a nested call's instructions are the tick's as much as the outer one's; only
   the tick reset and the tick close put them back. Outside a tick the window is
   closed and nothing but the per-call budget applies. */
typedef struct {
    size_t   used;
    size_t   cap;
    bool     armed;
    bool     stopped;
    uint32_t instr;
    uint32_t tickInstr;
    uint32_t tickCharged;
    bool     tickOpen;
    bool     tickStopped;
    uint32_t printCall;
    uint32_t printTick;
    bool     printCallSaid;
    bool     printTickSaid;
} ScnSandboxState;

/* Where they are kept, so the hook and the close can find them again.
   lua_getallocf is not the way: on the uncounted build below it answers
   Lua's own ud, which is neither ours to free nor ours to count in. A script
   cannot reach the registry — debug is not among the libraries opened. */
#define SCN_SANDBOX_STATE_KEY "winbolo.scenario.state"
/* The base functions the trigger router calls, kept as the opener installed
 * them. A script is free to reassign the globals of the same names, and the
 * router loads after the script has run. */
#define SCN_SANDBOX_BASE_KEY  "winbolo.scenario.base"

static ScnSandboxState *scnSandboxStateOf(lua_State *L) {
    ScnSandboxState *s;

    lua_getfield(L, LUA_REGISTRYINDEX, SCN_SANDBOX_STATE_KEY);
    s = (ScnSandboxState *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return s;
}

/* True once a state has been made the ordinary way because this build's Lua
   would not take an allocator. Written once, on the first boot, from the
   thread that boots it. */
static bool scnSandboxUncounted = false;

/* The standard lua_Alloc, over realloc and free as Lua's own default
 * allocator is, with the running total in front of it.
 *
 * osize is a size only when there is a block to have had one: with ptr NULL
 * Lua passes the kind of object it is about to allocate there instead, and
 * counting that number as bytes would make the total nonsense. So the old
 * size is zero whenever ptr is NULL, and the comparison is made from that.
 *
 * Only growth is refused, and only growth past the cap. A shrink and a free
 * always go through — a state at the cap has to be able to collect its way
 * back under it, and a collection is made of frees. Refusing is returning
 * NULL, which Lua turns into the out-of-memory error the caller's lua_pcall
 * catches. */
static void *scnSandboxAlloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    ScnSandboxState *m   = (ScnSandboxState *)ud;
    size_t           old = (ptr == NULL) ? 0 : osize;
    void            *out;

    if (nsize == 0) {
        free(ptr);
        m->used -= old;
        return NULL;
    }
    /* Written as room remaining rather than as used + growth, so nothing
       here can overflow: used never passes cap, because this is the only
       thing that raises it. */
    if (nsize > old && (nsize - old) > (m->cap - m->used)) {
        return NULL;
    }
    out = realloc(ptr, nsize);
    if (out == NULL) {
        return NULL;
    }
    m->used = m->used - old + nsize;
    return out;
}

/* ── The instructions one call may spend ──────────────────────────── */

/* The tick's own total, asked on every step the call's budget has not already
 * stopped. It raises in whichever call is running when the tick's calls
 * between them pass SCN_BUDGET_TICK_INSTR — which is the call that went over,
 * not necessarily the one that spent the most.
 *
 * Latched and put back to a grace short of the total for the reasons the
 * per-call latch is below: pcall, xpcall and coroutine.resume read the latch
 * and raise the error again, and the grace lets an unwinding metamethod finish
 * while a script that caught the error and carried on is stopped again soon
 * after. The host reads the latch too, through scnSandboxTickSpent, and skips
 * every call the tick had still to make.
 *
 * A trip reached inside a nested call — a hook's op asking a policy — counts
 * two errors: one for the policy that was stopped, and one for the hook that
 * issued the op when its own next step finds the latch spent past the grace.
 * That is what the per-call latch does with a caught error too. */
static void scnSandboxTickCheck(lua_State *L, ScnSandboxState *s) {
    if (!s->tickOpen || s->tickInstr <= (uint32_t)SCN_BUDGET_TICK_INSTR) {
        return;
    }
    s->tickStopped = true;
    s->tickInstr   = (uint32_t)SCN_BUDGET_TICK_INSTR -
                     (uint32_t)SCN_BUDGET_GRACE_INSTR;
    luaL_error(L, "this tick's script calls together ran past the %d "
                  "instructions one tick may spend between them; this call "
                  "was stopped and the rest of this tick's calls are skipped",
               (int)SCN_BUDGET_TICK_INSTR);
}

/* n instructions spent by the call that is running, counted against its
 * budget and the tick's, raising where either is passed. The hook spends
 * through this, and so does scnSandboxCharge for the C work the hook cannot
 * see, so the two are stopped and latched by the one path. Only reached while
 * a call is armed. */
static void scnSandboxSpend(lua_State *L, ScnSandboxState *s, uint32_t n) {
    s->instr += n;
    if (s->tickOpen) {
        s->tickInstr   += n;
        s->tickCharged += n;
    }
    if (s->instr <= (uint32_t)SCN_BUDGET_CALL_INSTR) {
        scnSandboxTickCheck(L, s);
        return;
    }
    /* A step that finds both spent is the call's error, and the tick's latch
       is set with it, so the rest of the tick is skipped rather than its next
       call being stopped on its first step for a second error. */
    if (s->tickOpen && s->tickInstr > (uint32_t)SCN_BUDGET_TICK_INSTR) {
        s->tickStopped = true;
        s->tickInstr   = (uint32_t)SCN_BUDGET_TICK_INSTR -
                         (uint32_t)SCN_BUDGET_GRACE_INSTR;
    }
    /* Latched rather than disarmed. Leaving the count off for the rest of the
       call would hand a script that caught this error every instruction it
       liked afterwards, and pcall, xpcall and coroutine.resume are all in the
       library it is given — so the flag stays on and the latch below is what
       those three read to know a failure they caught is not theirs to keep.
       Only the arm and the disarm put it back, neither of which script code
       can reach.

       The count goes back to a grace short of the budget rather than to zero,
       so the error has room to unwind through whatever the script had
       standing: a metamethod running Lua on the way out would otherwise be
       stopped here again, mid-unwind. Once that room is spent the hook raises
       again, which is what bounds a script that caught the error and carried
       on regardless. */
    s->stopped = true;
    s->instr   = (uint32_t)SCN_BUDGET_CALL_INSTR -
                 (uint32_t)SCN_BUDGET_GRACE_INSTR;
    luaL_error(L, "this call ran past the instruction budget of %d and was "
                  "stopped; work that long belongs across several on_tick "
                  "calls rather than inside one of them",
               (int)SCN_BUDGET_CALL_INSTR);
}

/* Called every SCN_BUDGET_STEP_INSTR instructions the state executes, and
 * counting only while a call into script code is running.
 *
 * The flag is what makes that true. Every piece of script code this host
 * runs is inside one of the calls that arm below, but the host also reads
 * the script's own table from C between them; a count left running could
 * then raise with no lua_pcall between it and the state, which is the one
 * error the host answers by ending the process. Armed-only puts that out of
 * reach.
 *
 * The hook cannot see inside C, so a call that spends its time in one C
 * function is not what this bounds — it bounds a script looping in Lua,
 * which is what a runaway scenario looks like. The C functions that can run
 * long for what a script hands them charge their own work through
 * scnSandboxCharge. */
static void scnSandboxCountHook(lua_State *L, lua_Debug *ar) {
    ScnSandboxState *s = scnSandboxStateOf(L);

    (void)ar;
    if (s == NULL || !s->armed) {
        return;
    }
    scnSandboxSpend(L, s, (uint32_t)SCN_BUDGET_STEP_INSTR);
}

/* C work the hook cannot see, charged here so the budgets see it: the pattern
 * matcher counts its own steps and hands them over through this. It raises the
 * budget's own error and sets the same latch the hook does, so pcall, xpcall
 * and coroutine.resume raise it again rather than keeping it. Outside an armed
 * call it does nothing, as the hook does. */
void scnSandboxCharge(lua_State *L, uint32_t n) {
    ScnSandboxState *s = scnSandboxStateOf(L);

    if (s == NULL || !s->armed || n == 0) {
        return;
    }
    scnSandboxSpend(L, s, n);
}

/* Saved and put back rather than set and cleared, because these nest: a
 * script's hook issues an op, the op asks a policy, and the policy is script
 * code arriving on the same thread — the VM lock lets it through for exactly
 * that reason. A disarm that only cleared the flag would leave the hook that
 * is still running uncounted for the rest of its life, which is a script one
 * nested call away from looping inside a tick for as long as it likes.
 *
 * Putting the outer call's own count back, rather than carrying the inner
 * one's forward, is what the budget says it is: a million for each call.
 * The outer one goes on climbing from where it had reached, so a script
 * cannot start its budget again by nesting.
 *
 * The latch travels with the count for the same reason. An inner call begins
 * with it clear, whatever the outer one has already run into, and leaves the
 * outer one's answer behind it — a policy that was stopped does not make the
 * hook that issued the op look stopped to the pcall it is standing in.
 *
 * The lines a call has printed travel with them, and for the same reason the
 * instructions do: a per-call allowance an inner call could hand back fresh to
 * the outer one would be no allowance at all. What a nested call prints is
 * still counted against the tick, which is the bound that holds however the
 * calls are arranged. */
void scnSandboxArmCall(lua_State *L, ScnSandboxCall *saved) {
    ScnSandboxState *s = scnSandboxStateOf(L);

    if (saved != NULL) {
        saved->armed     = (s != NULL) ? s->armed : false;
        saved->stopped   = (s != NULL) ? s->stopped : false;
        saved->instr     = (s != NULL) ? s->instr : 0;
        saved->printed   = (s != NULL) ? s->printCall : 0;
        saved->printSaid = (s != NULL) ? s->printCallSaid : false;
    }
    if (s != NULL) {
        s->instr         = 0;
        s->armed         = true;
        s->stopped       = false;
        s->printCall     = 0;
        s->printCallSaid = false;
    }
}

void scnSandboxDisarmCall(lua_State *L, const ScnSandboxCall *saved) {
    ScnSandboxState *s = scnSandboxStateOf(L);

    if (s == NULL) {
        return;
    }
    if (saved != NULL) {
        s->armed         = saved->armed;
        s->stopped       = saved->stopped;
        s->instr         = saved->instr;
        s->printCall     = saved->printed;
        s->printCallSaid = saved->printSaid;
    } else {
        s->armed         = false;
        s->stopped       = false;
        s->printCall     = 0;
        s->printCallSaid = false;
    }
}

/* The tick's own allowance, put back by whoever runs the tick.
 *
 * A tick is the window because the sim has one and a wall clock would not
 * behave the same way in a test as on a live server: the tests drive the sim
 * as fast as the CPU allows, so a window of a second would hold many more
 * ticks there than the fifty a server runs and cut a fixture printing once a
 * tick for a reason that has nothing to do with what it is testing.
 *
 * Nothing here is per call, so this leaves the call counters alone: a call
 * that is running while this is reached — there is none, since the tick resets
 * before it runs anything — keeps whatever it had spent.
 *
 * It also opens the window the tick's instruction total is counted in, from
 * zero and with the latch clear. The window stays open until the tick close,
 * so every call the tick makes before then is counted against the one total
 * whichever path it arrives by. */
void scnSandboxTickReset(lua_State *L) {
    ScnSandboxState *s;

    if (L == NULL) {
        return;
    }
    s = scnSandboxStateOf(L);
    if (s == NULL) {
        return;
    }
    s->printTick     = 0;
    s->printTickSaid = false;
    s->tickInstr     = 0;
    s->tickCharged   = 0;
    s->tickStopped   = false;
    s->tickOpen      = true;
}

/* The window shut and the latch cleared, so what the host calls from here to
 * the next reset — on_end, a policy the engine asks between ticks, the lobby's
 * asks from the GUI thread — is bounded by its own call's budget alone and is
 * never skipped for a total an earlier call spent. */
void scnSandboxTickClose(lua_State *L) {
    ScnSandboxState *s;

    if (L == NULL) {
        return;
    }
    s = scnSandboxStateOf(L);
    if (s == NULL) {
        return;
    }
    s->tickOpen    = false;
    s->tickStopped = false;
}

bool scnSandboxTickSpent(lua_State *L) {
    ScnSandboxState *s;

    if (L == NULL) {
        return false;
    }
    s = scnSandboxStateOf(L);
    return s != NULL && s->tickOpen && s->tickStopped;
}

uint32_t scnSandboxTickInstr(lua_State *L) {
    ScnSandboxState *s;

    if (L == NULL) {
        return 0;
    }
    s = scnSandboxStateOf(L);
    return s != NULL ? s->tickCharged : 0;
}

/* ── Making and closing one ───────────────────────────────────────── */

lua_State *scnSandboxNewState(void) {
    ScnSandboxState *s = (ScnSandboxState *)malloc(sizeof(*s));
    lua_State       *L;

    if (s == NULL) {
        return NULL;
    }
    s->used          = 0;
    s->cap           = (size_t)SCN_VM_MEMORY_MAX;
    s->armed         = false;
    s->stopped       = false;
    s->instr         = 0;
    s->tickInstr     = 0;
    s->tickCharged   = 0;
    s->tickOpen      = false;
    s->tickStopped   = false;
    s->printCall     = 0;
    s->printTick     = 0;
    s->printCallSaid = false;
    s->printTickSaid = false;

    L = lua_newstate(scnSandboxAlloc, s);
    if (L == NULL) {
        /* The build's own answer, and the only one there is: LuaJIT refuses
           a custom allocator on a 64-bit target that is not GC64, and says
           so on stderr as it does. There is no flag a consumer can read to
           ask first, so the state is made the ordinary way instead and the
           operator is told once that this one is not bounded.

           The struct stays either way. Only the memory half of it depends on
           the allocator; the instruction count is the hook's and is kept on
           every build. */
        L = luaL_newstate();
        if (L == NULL) {
            free(s);
            return NULL;
        }
        if (!scnSandboxUncounted) {
            scnSandboxUncounted = true;
            serverSimConsoleMessage(
                "scenario: this build's Lua takes no allocator, so a "
                "scenario script's memory is not capped");
        }
    }

    /* Put beside the state at once, so every path that closes one can find
       what to free however early it gives up, and so the hook below has
       somewhere to count before anything runs. */
    lua_pushlightuserdata(L, s);
    lua_setfield(L, LUA_REGISTRYINDEX, SCN_SANDBOX_STATE_KEY);

    /* Set once, here, and never cleared. The state runs interpreted on
       LuaJIT because jit is never opened, and the budget depends on that:
       compiled code would not run this hook. Taking the hook off between
       calls would buy nothing, since whether the count applies is the flag
       the calls arm, not the presence of the hook. */
    lua_sethook(L, scnSandboxCountHook, LUA_MASKCOUNT,
                (int)SCN_BUDGET_STEP_INSTR);
    return L;
}

void scnSandboxCloseState(lua_State *L) {
    ScnSandboxState *s;

    if (L == NULL) {
        return;
    }
    /* Read before the close, because the registry goes with the state, and
       freed after it, because closing hands every block the state holds back
       through the allocator and the allocator reads this. */
    lua_getfield(L, LUA_REGISTRYINDEX, SCN_SANDBOX_STATE_KEY);
    s = (ScnSandboxState *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    lua_close(L);
    free(s);
}

bool scnSandboxMemoryCapped(void) {
    return !scnSandboxUncounted;
}

/* ── Opening a library ────────────────────────────────────────────── */

/* One standard library, opened by hand.
 *
 * luaL_requiref is the 5.4 way and LuaJIT has no such function, so this is
 * written to the older shape that both accept: push the opener, push the
 * name it is being opened under, call it. 5.1 and LuaJIT hand the name to
 * the opener as its one argument and the opener sets the global itself;
 * 5.4's openers build the table and leave the global to the caller, which is
 * what the set below is for. Setting a global 5.1 has already set writes the
 * same table over itself.
 *
 * An opener that answers with something other than a table has set whatever
 * it means to set and left nothing to name — nothing here does, and the
 * branch is what keeps a future one from writing a non-table global. */
static void scnSandboxOpenOne(lua_State *L, const char *name,
                              lua_CFunction opener) {
    lua_pushcfunction(L, opener);
    lua_pushstring(L, name);
    lua_call(L, 1, 1);
    if (lua_istable(L, -1)) {
        lua_setglobal(L, name);
    } else {
        lua_pop(L, 1);
    }
}

/* ── Taking a name back ───────────────────────────────────────────── */

/* One field of a library table. The tables were built moments ago by their
   own openers and carry no metatable, so the write is the plain one. */
static void scnSandboxClearField(lua_State *L, const char *table,
                                 const char *key) {
    lua_getglobal(L, table);
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, key);
    }
    lua_pop(L, 1);
}

/* Whether the key at idx is one of the four os keeps. */
static bool scnSandboxOsKeeps(lua_State *L, int idx) {
    static const char *const kKeep[] = { "time", "date", "clock",
                                         "difftime" };
    const char *key;
    size_t      i;

    /* Asked by type first: lua_tostring on a number key would convert it in
       place, which is the one thing a traversal cannot survive. */
    if (lua_type(L, idx) != LUA_TSTRING) {
        return false;
    }
    key = lua_tostring(L, idx);
    for (i = 0; i < sizeof(kKeep) / sizeof(kKeep[0]); i++) {
        if (strcmp(key, kKeep[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* os cut to the clock: time, date, clock and difftime stay and every other
 * field goes, execute, exit, getenv, remove, rename, setlocale and tmpname
 * among them.
 *
 * Walked with a keep list rather than written as a kill list, so a field a
 * later Lua adds to os is gone by default rather than the day somebody
 * remembers to name it. Setting an existing field to nil in the middle of a
 * traversal is allowed; adding one is not, and this adds none. */
static void scnSandboxTrimOs(lua_State *L) {
    lua_getglobal(L, LUA_OSLIBNAME);
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            lua_pop(L, 1);              /* the value; the key stays */
            if (!scnSandboxOsKeeps(L, -1)) {
                lua_pushvalue(L, -1);   /* the key again, to write through */
                lua_pushnil(L);
                lua_rawset(L, -4);
            }
        }
    }
    lua_pop(L, 1);
}

/* Whether the key at idx is one of the globals a script is given.
 *
 * Naming one this VM does not have costs nothing, because this list only
 * decides what is deleted and never creates anything: unpack is 5.1's and
 * rawlen is 5.2's, so each VM keeps the one it has, and utf8 is only opened
 * where there is an opener for it.
 *
 * collectgarbage, print, pcall and xpcall are here because what a script
 * ends up calling under those names is written over further down, by the
 * replacements and the wrappers. Take them away here and there is nothing
 * left for those to close over. */
static bool scnSandboxGlobalKeeps(lua_State *L, int idx) {
    static const char *const kKeep[] = {
        /* The libraries opened above, and the string naming the VM. */
        "_G", "coroutine", "string", "table", "math", "os", "utf8",
        "_VERSION",
        /* The base functions a script is meant to have. */
        "assert", "collectgarbage", "error", "getmetatable", "ipairs",
        "next", "pairs", "pcall", "print", "rawequal", "rawget", "rawlen",
        "rawset", "select", "setmetatable", "tonumber", "tostring", "type",
        "unpack", "xpcall"
    };
    const char *key;
    size_t      i;

    /* By type first, for the reason os is: a number key handed to
       lua_tostring is converted where it sits, and the traversal is over. */
    if (lua_type(L, idx) != LUA_TSTRING) {
        return false;
    }
    key = lua_tostring(L, idx);
    for (i = 0; i < sizeof(kKeep) / sizeof(kKeep[0]); i++) {
        if (strcmp(key, kKeep[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* The globals cut to that list. The loaders go with everything else it does
 * not name: load, loadstring, dofile, loadfile, require, module and newproxy
 * each bring code in from outside the chunk the host read, which is the one
 * thing a state running a map file's script must not do. Which of them this
 * VM had in the first place stopped mattering with the shape — the walk takes
 * away what is there.
 *
 * Named the other way round, as the loaders to take away, this only ever
 * removed what somebody had thought of, and three names nobody had decided on
 * were still here: getfenv, setfenv and gcinfo. None of them is a way out
 * today, with every loader gone, but setfenv(0, {}) replaces the environment
 * the host looks its own hooks up in, and that is not a thing to be left
 * behind by omission. A name a later Lua adds is now gone by default instead.
 *
 * This runs once every library is open, so the walk covers the library tables
 * as well as base — which is why those are on the list themselves. The
 * globals are reached through _G, which both VMs set from the base opener.
 * Setting an existing global to nil in the middle of a traversal is allowed;
 * adding one is not, and this adds none. */
static void scnSandboxTrimGlobals(lua_State *L) {
    lua_getglobal(L, "_G");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            lua_pop(L, 1);              /* the value; the key stays */
            if (!scnSandboxGlobalKeeps(L, -1)) {
                lua_pushvalue(L, -1);   /* the key again, to write through */
                lua_pushnil(L);
                lua_rawset(L, -4);
            }
        }
    }
    lua_pop(L, 1);
}

/* ── The calls that are replaced rather than removed ──────────────── */

/* collectgarbage without the "stop" option. A script that could stop the
 * collector could grow the state without bound while every allocation it
 * made stayed legitimate. Every other option — a step, a count, a full
 * collection — is forwarded to the original, which is upvalue 1, and answers
 * exactly as it always did. */
static int scnSandboxCollectGarbage(lua_State *L) {
    int n = lua_gettop(L);

    if (lua_type(L, 1) == LUA_TSTRING &&
        strcmp(lua_tostring(L, 1), "stop") == 0) {
        return luaL_error(L, "collectgarbage(\"stop\") is not available to a "
                             "scenario script");
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, n, LUA_MULTRET);
    return lua_gettop(L);
}

/* Whether this line may go out, and the one notice a window that has run out
 * produces.
 *
 * Both windows are asked, the call's first: a line the call has no room for
 * never reaches the console, so it costs the tick nothing either and only the
 * counter that refused it is the one an operator is told about. Neither count
 * passes its bound, so neither can wrap however long a script prints for.
 *
 * The notice is written from here rather than counted as a line of its own.
 * It is C, so nothing a script does can make it recurse, and the said flag
 * beside each count is what keeps it to one line: an operator is told once
 * where the output went and then left in silence until the window comes
 * round again.
 *
 * A state with nothing to count by — there is none the host boots, but the
 * registry lookup can answer nothing — prints as it always did rather than
 * going quiet. */
static bool scnSandboxPrintTake(ScnSandboxState *s) {
    char said[SCN_PRINT_LEN];

    if (s == NULL) {
        return true;
    }
    if (s->printCall >= (uint32_t)SCN_PRINT_PER_CALL) {
        if (!s->printCallSaid) {
            s->printCallSaid = true;
            snprintf(said, sizeof(said),
                     "scenario: a script has printed the %d lines one call "
                     "may print, and the rest of this call's output is "
                     "dropped", (int)SCN_PRINT_PER_CALL);
            serverSimConsoleMessage(said);
        }
        return false;
    }
    if (s->printTick >= (uint32_t)SCN_PRINT_PER_TICK) {
        if (!s->printTickSaid) {
            s->printTickSaid = true;
            snprintf(said, sizeof(said),
                     "scenario: a script has printed the %d lines one tick "
                     "may print, and the rest of this tick's output is "
                     "dropped", (int)SCN_PRINT_PER_TICK);
            serverSimConsoleMessage(said);
        }
        return false;
    }
    s->printCall++;
    s->printTick++;
    return true;
}

/* print, to the server console instead of to stdout. The arguments are
 * concatenated tab-separated and converted through __tostring where a value
 * carries one, which is what stock print does.
 *
 * The allowance is asked for before the line is built, so a script past its
 * bound is not paying for the concatenation of output nobody will see — and
 * so a __tostring metamethod, which is script code, is not run for it
 * either.
 *
 * One print is one console line. A script must not be able to write a line
 * that reads as the server's own, into the console and the operator's
 * message log, nor send control codes to the operator's terminal, so every
 * byte below 0x20 but the tab, and 0x7f, goes out as a space. The tab stays
 * because it is what separates the arguments; bytes from 0x80 up stay so
 * UTF-8 text is untouched. An unsafe state's print is this one too, and
 * keeps to the same rule. */
static int scnSandboxPrint(lua_State *L) {
    char   line[SCN_PRINT_LEN];
    size_t used = 0;
    int    n    = lua_gettop(L);
    int    i;

    if (!scnSandboxPrintTake(scnSandboxStateOf(L))) {
        return 0;
    }
    for (i = 1; i <= n; i++) {
        const char *s;
        size_t      len  = 0;
        size_t      room;

        if (i > 1 && used < sizeof(line) - 1) {
            line[used++] = '\t';
        }
        s = luaL_tolstring(L, i, &len);
        if (s != NULL) {
            room = sizeof(line) - 1 - used;
            if (len > room) {
                len = room;
            }
            memcpy(line + used, s, len);
            used += len;
        }
        lua_pop(L, 1);                  /* what luaL_tolstring pushed */
    }
    line[used] = '\0';
    for (i = 0; i < (int)used; i++) {
        unsigned char c = (unsigned char)line[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f) {
            line[i] = ' ';
        }
    }
    serverSimConsoleMessage(line);
    return 0;
}

/* The conversion characters a format may carry. This is the set the MSVC CRT
 * documents as valid, which is the narrowest of the libraries this server is
 * built against and so the one every host is held to: a script written against
 * a Linux server should run on a Windows one without the author finding out
 * the hard way which of the two is fussier.
 *
 * POSIX's E and O modifiers and MSVC's # flag are left out on purpose. A
 * scenario has no use for either, and %Ec is exactly the kind of thing one C
 * library takes and another does not. */
static const char kScnDateConv[] = "aAbBcCdDeFgGhHIjmMnprRStTuUVwWxXyYzZ%";

static bool scnSandboxDateConv(char c) {
    return c != '\0' && strchr(kScnDateConv, c) != NULL;
}

/* The specifier that was refused, written out for the author to read back.
   A format is a string like any other and may hold any byte at all, so one
   that is not printable is named by its number rather than sent to a console
   line as itself. out holds six bytes at least, which is the longest of the
   two shapes below with its terminator. */
static void scnSandboxDateShow(char c, char *out) {
    static const char kHex[] = "0123456789abcdef";
    unsigned char     u      = (unsigned char)c;

    out[0] = '%';
    if (u >= 0x20 && u < 0x7f) {
        out[1] = (char)u;
        out[2] = '\0';
        return;
    }
    out[1] = '\\';
    out[2] = 'x';
    out[3] = kHex[u >> 4];
    out[4] = kHex[u & 0x0f];
    out[5] = '\0';
}

/* os.date, over a format the host's C library is known to take.
 *
 * LuaJIT hands the format to strftime as it stands, checking none of it —
 * PUC-Lua reads it first and refuses what it does not know, LuaJIT does not.
 * On glibc that costs nothing, because an unknown conversion is copied through
 * as the two characters it was written as. The MSVC CRT answers an invalid one
 * by calling the invalid-parameter handler, which by default ends the process:
 * a line beside a map could stop a Windows dedicated server. So the format is
 * read here, and what reaches strftime is a format both libraries know.
 *
 * What goes straight through: no argument or a nil one, which LuaJIT defaults
 * to "%c", and "*t" or "!*t", which ask for a table rather than a string and
 * never reach strftime at all. Everything else is a format, optionally led by
 * the ! that selects UTC. The original is upvalue 1, as collectgarbage's is. */
static int scnSandboxDate(lua_State *L) {
    const char *fmt;
    size_t      len = 0;
    size_t      i;
    int         n;
    char        shown[8];

    if (!lua_isnoneornil(L, 1)) {
        fmt = luaL_checklstring(L, 1, &len);
        if (len > (size_t)SCN_DATE_FMT_LEN) {
            return luaL_error(L, "os.date was given a format of %d bytes, and "
                                 "%d is as long as one may be",
                              (int)len, (int)SCN_DATE_FMT_LEN);
        }
        if (len > 0 && *fmt == '!') {
            fmt++;                      /* UTC; the format is what follows */
            len--;
        }
        /* Asked by length rather than by strcmp, because a Lua string may
           carry a NUL of its own and the bytes past one still matter here. */
        if (!(len == 2 && fmt[0] == '*' && fmt[1] == 't')) {
            for (i = 0; i < len; i++) {
                if (fmt[i] != '%') {
                    continue;
                }
                i++;
                if (i >= len) {
                    return luaL_error(L, "os.date was given a format ending in "
                                         "a %% with no conversion character "
                                         "after it");
                }
                if (!scnSandboxDateConv(fmt[i])) {
                    scnSandboxDateShow(fmt[i], shown);
                    return luaL_error(L, "os.date was given %s, which is not "
                                         "one of the conversion characters a "
                                         "scenario may use: hosts do not agree "
                                         "on what their C library takes, and "
                                         "one of them ends the process over a "
                                         "format it does not know",
                                      shown);
                }
            }
        }
    }
    n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, n, LUA_MULTRET);
    return lua_gettop(L);
}

/* ── The strings a C function may build ───────────────────────────── */

/* Each of the seven below is a closure over the function it runs, which is
 * upvalue 1 — the original, as collectgarbage's is, for rep, format and
 * concat, and the port for the four pattern functions — and over the name a
 * script knows it by, which is upvalue 2 and is what a refusal is worded
 * with. */
static int scnSandboxStringForward(lua_State *L) {
    int n = lua_gettop(L);

    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, n, LUA_MULTRET);
    return lua_gettop(L);
}

/* An ordinary error, and not the budget's: the latch is left alone, so a
   script that catches this keeps what it caught and may try something
   shorter. The length is written through snprintf because the two VMs'
   lua_pushfstring do not agree on how to print a number that is not an
   int, and one refused here can be past what an int holds. */
static int scnSandboxStringRefuse(lua_State *L, const char *what,
                                  double len) {
    char said[192];

    snprintf(said, sizeof(said),
             "%s %s %.15g bytes, and %u is as long as one may be",
             lua_tostring(L, lua_upvalueindex(2)), what, len,
             (unsigned)SCN_STRING_MAX);
    return luaL_error(L, "%s", said);
}

/* The same for string.rep's count, where the length would say nothing:
   copies of an empty string come to no bytes however many there are. */
static int scnSandboxStringRefuseCount(lua_State *L, double count) {
    char said[192];

    snprintf(said, sizeof(said),
             "%s was asked for %.15g copies, and %u is as many as one may "
             "make", lua_tostring(L, lua_upvalueindex(2)), count,
             (unsigned)SCN_STRING_MAX);
    return luaL_error(L, "%s", said);
}

/* The length the string or number at idx has as a string. A number is
   converted on a copy, so the argument the original reads is the one the
   script passed. False for anything else, which is left to the original to
   refuse in its own words. */
static bool scnSandboxStringLen(lua_State *L, int idx, size_t *len) {
    int t = lua_type(L, idx);

    if (t != LUA_TSTRING && t != LUA_TNUMBER) {
        return false;
    }
    lua_pushvalue(L, idx);
    lua_tolstring(L, -1, len);
    lua_pop(L, 1);
    return true;
}

/* string.rep, refused before anything is built. The result is n copies with
 * n - 1 separators between them, and 5.4 and LuaJIT 2.1 both take the
 * separator.
 *
 * The count is rounded up rather than down, so a fractional one is judged by
 * the longest thing a VM could make of it: LuaJIT truncates and 5.4 refuses
 * one outright, and either way nothing longer than what is checked here is
 * built. A count of zero or less builds "", and a NaN is the original's to
 * answer, so both go straight through.
 *
 * An infinite count is refused by the count and not by the length, because a
 * length of inf bytes says nothing a count of inf copies does not say better.
 *
 * A count over SCN_STRING_MAX is refused whatever the strings are, empty ones
 * included: 5.4 runs its copy loop once per copy even when there is nothing
 * to copy, and the hook cannot see that loop either. Where the strings have a
 * byte in them the refusal gives the length they would have come to; where
 * they are empty it gives the count, since the length would be nothing.
 *
 * Nothing here can overflow. A count past SCN_STRING_MAX + 1 is refused
 * before it is converted. Below that the count fits in seventeen bits, and
 * each length is checked against the cap before it is multiplied, so the
 * product is well inside 64 bits. */
static int scnSandboxStringRep(lua_State *L) {
    size_t     len    = 0;
    size_t     seplen = 0;
    lua_Number want;
    uint64_t   n;
    uint64_t   total;

    if (!scnSandboxStringLen(L, 1, &len) || !lua_isnumber(L, 2) ||
        (!lua_isnoneornil(L, 3) && !scnSandboxStringLen(L, 3, &seplen))) {
        return scnSandboxStringForward(L);
    }
    want = lua_tonumber(L, 2);
    if (!(want >= 1.0)) {
        return scnSandboxStringForward(L);
    }
    if (want > (lua_Number)SCN_STRING_MAX + 1.0) {
        if (!isinf(want) && (len > 0 || seplen > 0)) {
            return scnSandboxStringRefuse(
                L, "would build a string of",
                (double)want * (double)len +
                    ((double)want - 1.0) * (double)seplen);
        }
        return scnSandboxStringRefuseCount(L, (double)want);
    }
    n = (uint64_t)want;
    if ((lua_Number)n < want) {
        n++;
    }
    if (n > 1 && seplen > (size_t)SCN_STRING_MAX) {
        return scnSandboxStringRefuse(L, "would build a string of more than",
                                      (double)seplen);
    }
    if (len > (size_t)SCN_STRING_MAX) {
        return scnSandboxStringRefuse(L, "would build a string of more than",
                                      (double)len);
    }
    total = n * (uint64_t)len + (n - 1) * (uint64_t)seplen;
    if (total > (uint64_t)SCN_STRING_MAX) {
        return scnSandboxStringRefuse(L, "would build a string of",
                                      (double)total);
    }
    if (n > (uint64_t)SCN_STRING_MAX) {
        return scnSandboxStringRefuseCount(L, (double)n);
    }
    return scnSandboxStringForward(L);
}

/* table.concat, refused before anything is built. The walk reads the same
 * range the original will: i from the third argument or 1, j from the fourth
 * or the table's length. LuaJIT reads both as 32-bit integers and takes the
 * length raw; 5.4 reads them as lua_Integer and takes the length through
 * luaL_len, which honours __len.
 *
 * Each element is read the way that VM's original reads it: raw under
 * LuaJIT, and through lua_geti under 5.4, which honours __index — a table
 * whose elements come from __index would otherwise pass the walk empty and be
 * built in full by the original. An __index function is script code, so it
 * runs under the count like any other, and an error it raises goes to the
 * caller as it would from the original.
 *
 * The walk stops as soon as the running total is over the cap, so a table of
 * any size costs no more than the cap's worth of elements to judge. Each
 * addition is made against the room remaining, so the total never passes the
 * cap and nothing can overflow. An element that is neither a string nor a
 * number ends the walk and is left to the original, which refuses it in its
 * own words.
 *
 * Empty strings do not add to the total, so the cap alone does not end the
 * walk over a long table of them. The walk is charged one instruction per
 * element, so a long table costs the budget what it reads. */
static int scnSandboxTableConcat(lua_State *L) {
    size_t   seplen = 0;
    size_t   len;
    uint64_t total  = 0;
    bool     ok;
#ifdef WINBOLO_LUAJIT
    int         i;
    int         j;
    int         k;
#else
    lua_Integer i;
    lua_Integer j;
    lua_Integer k;
#endif

    if (!lua_istable(L, 1) ||
        (!lua_isnoneornil(L, 2) && !scnSandboxStringLen(L, 2, &seplen))) {
        return scnSandboxStringForward(L);
    }
#ifdef WINBOLO_LUAJIT
    i = luaL_optint(L, 3, 1);
    j = lua_isnoneornil(L, 4) ? (int)lua_objlen(L, 1) : luaL_checkint(L, 4);
#else
    i = luaL_optinteger(L, 3, 1);
    j = lua_isnoneornil(L, 4) ? luaL_len(L, 1) : luaL_checkinteger(L, 4);
#endif
    if (i <= j) {
        /* Stepped by hand rather than with k <= j, which never ends when j is
           the largest integer there is. */
        for (k = i;; k++) {
            scnSandboxCharge(L, 1);
#ifdef WINBOLO_LUAJIT
            lua_rawgeti(L, 1, k);
#else
            lua_geti(L, 1, k);
#endif
            ok = scnSandboxStringLen(L, -1, &len);
            lua_pop(L, 1);
            if (!ok) {
                return scnSandboxStringForward(L);
            }
            if (k != i) {
                if (seplen > (size_t)SCN_STRING_MAX - total) {
                    return scnSandboxStringRefuse(
                        L, "would build a string of at least",
                        (double)total + (double)seplen);
                }
                total += seplen;
            }
            if (len > (size_t)SCN_STRING_MAX - total) {
                return scnSandboxStringRefuse(
                    L, "would build a string of at least",
                    (double)total + (double)len);
            }
            total += len;
            if (k == j) {
                break;
            }
        }
    }
    return scnSandboxStringForward(L);
}

/* string.format, refused up front where the format or a string argument is
 * already over the cap, and otherwise once the original has answered. Its
 * result is bounded by what it was given — a width or a precision stops at
 * two digits — so this is the one of the three that may build first and look
 * after. The string it built is dropped with the error and collected like
 * any other. */
static int scnSandboxStringFormat(lua_State *L) {
    int    n   = lua_gettop(L);
    size_t len = 0;
    int    a;

    for (a = 1; a <= n; a++) {
        if (lua_type(L, a) == LUA_TSTRING) {
            lua_tolstring(L, a, &len);
            if (len > (size_t)SCN_STRING_MAX) {
                return scnSandboxStringRefuse(
                    L, a == 1 ? "was given a format of"
                              : "was given a string of",
                    (double)len);
            }
        }
    }
    scnSandboxStringForward(L);
    if (lua_type(L, 1) == LUA_TSTRING) {
        lua_tolstring(L, 1, &len);
        if (len > (size_t)SCN_STRING_MAX) {
            return scnSandboxStringRefuse(L, "built a string of",
                                          (double)len);
        }
    }
    return lua_gettop(L);
}

/* find, match, gmatch and gsub, refused before the pattern runs. Only a
 * string subject is measured: a number's text is a couple of dozen bytes at
 * most, and anything else is the matcher's to refuse.
 *
 * These four are not closures over the originals. The VMs' own matchers count
 * no steps, so a pattern that backtracks without end would hold the tick inside
 * one of them however short the subject; the port in scenario_pattern.c counts
 * its steps against the budgets instead. Upvalue 1 is the port's function,
 * called here directly rather than through lua_call, so an argument error it
 * raises names the function the script called. Upvalue 2 is the name, as for
 * the three above. */
static int scnSandboxStringSubject(lua_State *L) {
    size_t len = 0;

    if (lua_type(L, 1) == LUA_TSTRING) {
        lua_tolstring(L, 1, &len);
        if (len > (size_t)SCN_STRING_MAX) {
            return scnSandboxStringRefuse(L, "was given a subject of",
                                          (double)len);
        }
    }
    return lua_tocfunction(L, lua_upvalueindex(1))(L);
}

/* One field of a library table replaced by one of the closures above, as
   scnSandboxGuardField does for the catchers, with the name a refusal reads
   beside the original. */
static void scnSandboxStringField(lua_State *L, const char *table,
                                  const char *key, lua_CFunction wrap) {
    lua_getglobal(L, table);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, key);
    if (lua_isfunction(L, -1)) {
        lua_pushfstring(L, "%s.%s", table, key);
        lua_pushcclosure(L, wrap, 2);
        lua_setfield(L, -2, key);
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

/* One of the four pattern functions put in place of the original, which is
   dropped rather than closed over: nothing a script can reach refers to it
   afterwards. The string metatable's __index is this same table, so a method
   call such as s:find(p) reaches the port as well. */
static void scnSandboxPatternField(lua_State *L, const char *key,
                                   lua_CFunction port) {
    lua_getglobal(L, LUA_STRLIBNAME);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pushcfunction(L, port);
    lua_pushfstring(L, "%s.%s", LUA_STRLIBNAME, key);
    lua_pushcclosure(L, scnSandboxStringSubject, 2);
    lua_setfield(L, -2, key);
    lua_pop(L, 1);
}

/* ── The one error a script may not keep ──────────────────────────── */

/* pcall, xpcall and coroutine.resume, each over the original.
 *
 * These three are how script code is handed a failure rather than having it
 * raised through it, and the error the budget raises is the one failure a
 * call may not keep: a script that caught it would go on spending the
 * instructions it has already been stopped for, and the hook alone cannot
 * prevent that — a loop around a pcall would be cut off and catch it again
 * for as long as it liked.
 *
 * So each asks the latches once the call it protected is over — the call's
 * and the tick's — and raises again where that call failed with either set. The inner error having been an
 * ordinary one changes nothing: once the hook has latched, this call's budget
 * is spent whatever the protected code failed at.
 *
 * coroutine.wrap needs none of this. The function it answers with raises into
 * its caller rather than answering with a failure, so the error arrives at
 * whichever of these three caught it — or at the host's own lua_pcall, where
 * none did.
 *
 * All three answer the same shape, how it went and then what came of it, so
 * one body serves them all. The original is upvalue 1, as collectgarbage's
 * is. */
static int scnSandboxGuardCall(lua_State *L) {
    ScnSandboxState *s = scnSandboxStateOf(L);
    int              n = lua_gettop(L);

    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, n, LUA_MULTRET);
    if (s != NULL && s->stopped && lua_gettop(L) >= 1 &&
        !lua_toboolean(L, 1)) {
        return luaL_error(L, "this call ran past the instruction budget of %d "
                             "and was stopped; catching that error does not "
                             "give the call its instructions back, so it is "
                             "raised again",
                          (int)SCN_BUDGET_CALL_INSTR);
    }
    /* And the tick's latch, for the same reason: a caught error does not
       give the tick its instructions back either. */
    if (s != NULL && s->tickStopped && lua_gettop(L) >= 1 &&
        !lua_toboolean(L, 1)) {
        return luaL_error(L, "this tick's script calls together ran past the "
                             "%d instructions one tick may spend between "
                             "them; catching that error does not give the "
                             "tick its instructions back, so it is raised "
                             "again",
                          (int)SCN_BUDGET_TICK_INSTR);
    }
    return lua_gettop(L);
}

/* One global, and one field of a library table, replaced by a closure over
   what was there. A name that is not there is left alone rather than made:
   these run after the libraries are open, so what is missing is missing
   because this VM never had it. */
static void scnSandboxGuardGlobal(lua_State *L, const char *name) {
    lua_getglobal(L, name);
    if (lua_isfunction(L, -1)) {
        lua_pushcclosure(L, scnSandboxGuardCall, 1);
        lua_setglobal(L, name);
    } else {
        lua_pop(L, 1);
    }
}

static void scnSandboxGuardField(lua_State *L, const char *table,
                                 const char *key) {
    lua_getglobal(L, table);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, key);
    if (lua_isfunction(L, -1)) {
        lua_pushcclosure(L, scnSandboxGuardCall, 1);
        lua_setfield(L, -2, key);
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

/* ── The whitelist ────────────────────────────────────────────────── */

void scnSandboxOpenLibs(lua_State *L) {
    /* base first: under 5.1 and LuaJIT it brings coroutine with it, which is
       why the coroutine opener below is 5.4's alone. */
    scnSandboxOpenOne(L, "_G", luaopen_base);
#ifndef WINBOLO_LUAJIT
    scnSandboxOpenOne(L, LUA_COLIBNAME, luaopen_coroutine);
#endif
    scnSandboxOpenOne(L, LUA_STRLIBNAME, luaopen_string);
    scnSandboxOpenOne(L, LUA_TABLIBNAME, luaopen_table);
    scnSandboxOpenOne(L, LUA_MATHLIBNAME, luaopen_math);
    scnSandboxOpenOne(L, LUA_OSLIBNAME, luaopen_os);
    /* utf8 is PUC-Lua's and LuaJIT has none, so a script that wants it has to
       ask whether it is there. The asymmetry is in docs/SCENARIO_API.md. */
#if !defined(WINBOLO_LUAJIT) && defined(LUA_UTF8LIBNAME)
    scnSandboxOpenOne(L, LUA_UTF8LIBNAME, luaopen_utf8);
#endif

    scnSandboxTrimGlobals(L);

    /* The copies the router reads its base functions from. Taken now, off
       the whitelist just applied and before any chunk has run, so what the
       router calls under these names is what the opener installed whatever
       the script later assigns to the globals. */
    {
        static const char *const kBase[] = { "type", "select", "rawget" };
        size_t i;

        lua_createtable(L, 0, (int)(sizeof(kBase) / sizeof(kBase[0])));
        for (i = 0; i < sizeof(kBase) / sizeof(kBase[0]); i++) {
            lua_getglobal(L, kBase[i]);
            lua_setfield(L, -2, kBase[i]);
        }
        lua_setfield(L, LUA_REGISTRYINDEX, SCN_SANDBOX_BASE_KEY);
    }

    /* The other half of the loaders: dump turns a function back into
       bytecode, which the chunk loader refuses to take. */
    scnSandboxClearField(L, LUA_STRLIBNAME, "dump");

    scnSandboxTrimOs(L);

    lua_getglobal(L, "collectgarbage");
    if (lua_isfunction(L, -1)) {
        lua_pushcclosure(L, scnSandboxCollectGarbage, 1);
        lua_setglobal(L, "collectgarbage");
    } else {
        lua_pop(L, 1);
    }

    lua_pushcfunction(L, scnSandboxPrint);
    lua_setglobal(L, "print");

    /* After the trims above, so this closes over the os.date that survived
       them rather than over one that is about to be deleted. */
    lua_getglobal(L, LUA_OSLIBNAME);
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "date");
        if (lua_isfunction(L, -1)) {
            lua_pushcclosure(L, scnSandboxDate, 1);
            lua_setfield(L, -2, "date");
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    /* string is also the __index of the string metatable, so replacing the
       fields here covers a method call such as ("x"):rep(n) as well. */
    scnSandboxStringField(L, LUA_STRLIBNAME, "rep", scnSandboxStringRep);
    scnSandboxStringField(L, LUA_STRLIBNAME, "format",
                          scnSandboxStringFormat);
    scnSandboxStringField(L, LUA_TABLIBNAME, "concat",
                          scnSandboxTableConcat);
    scnSandboxPatternField(L, "find", scnPatternFind);
    scnSandboxPatternField(L, "match", scnPatternMatch);
    scnSandboxPatternField(L, "gmatch", scnPatternGmatch);
    scnSandboxPatternField(L, "gsub", scnPatternGsub);

    /* Last, so each of these closes over the function its own opener
       installed rather than over something replaced afterwards. coroutine is
       base's under LuaJIT and its own library under 5.4, and either way it is
       a global table by the time this reads it. */
    scnSandboxGuardGlobal(L, "pcall");
    scnSandboxGuardGlobal(L, "xpcall");
    scnSandboxGuardField(L, LUA_COLIBNAME, "resume");
}

void scnSandboxSealRandom(lua_State *L) {
    scnSandboxClearField(L, LUA_MATHLIBNAME, "randomseed");
}

void scnSandboxPushBase(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, SCN_SANDBOX_BASE_KEY);
    if (!lua_istable(L, -1)) {
        /* A state opened some other way: the router falls back to the
           globals, which is what it did before the copies were kept. */
        lua_pop(L, 1);
        lua_newtable(L);
    }
}

void scnSandboxOpenPrint(lua_State *L) {
    lua_pushcfunction(L, scnSandboxPrint);
    lua_setglobal(L, "print");
}
