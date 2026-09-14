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
 *  with it.
 *
 *  Opening them one at a time is what takes io, package,
 *  debug and, under LuaJIT, ffi, jit and bit away: none of
 *  them is ever created, so there is nothing to strip. What
 *  is stripped is the rest — the loaders base carries, the
 *  dump that turns a function into bytecode, and everything
 *  in os but the clock.
 *
 *  print is replaced rather than removed, because a script
 *  reporting what it did is worth having and stock print
 *  writes to the host's stdout, which a dedicated server's
 *  operator is not necessarily reading.
 *
 *  This file compiles under the scenario_host profile: it
 *  sees src/bolo/public/ and src/bolo/scenario_api/, and
 *  nothing under src/bolo/internal/.
 *********************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "server_sim.h"            /* serverSimConsoleMessage */

#include "scenario_host.h"         /* SCN_VM_MEMORY_MAX */
#include "scenario_sandbox.h"

/* One console line a script's print can produce. A line past this is cut
   rather than dropped, so an operator still sees what a long one was
   about. */
#define SCN_PRINT_LEN 1024

/* ── The memory one state may hold ────────────────────────────────── */

/* What a state's allocations are counted in. It outlives the call that made
   the state, so it is on the heap rather than beside the caller. */
typedef struct {
    size_t used;
    size_t cap;
} ScnSandboxMem;

/* Where the count is kept so the close can find it again. lua_getallocf is
   not the way: on the uncounted build below it answers Lua's own ud, which
   is not ours to free. A script cannot reach the registry — debug is not
   among the libraries opened. */
#define SCN_SANDBOX_MEM_KEY "winbolo.scenario.mem"

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
    ScnSandboxMem *m   = (ScnSandboxMem *)ud;
    size_t         old = (ptr == NULL) ? 0 : osize;
    void          *out;

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

lua_State *scnSandboxNewState(void) {
    ScnSandboxMem *m = (ScnSandboxMem *)malloc(sizeof(*m));
    lua_State     *L;

    if (m == NULL) {
        return NULL;
    }
    m->used = 0;
    m->cap  = (size_t)SCN_VM_MEMORY_MAX;

    L = lua_newstate(scnSandboxAlloc, m);
    if (L == NULL) {
        /* The build's own answer, and the only one there is: LuaJIT refuses
           a custom allocator on a 64-bit target that is not GC64, and says
           so on stderr as it does. There is no flag a consumer can read to
           ask first, so the state is made the ordinary way instead and the
           operator is told once that this one is not bounded. */
        free(m);
        L = luaL_newstate();
        if (L != NULL && !scnSandboxUncounted) {
            scnSandboxUncounted = true;
            serverSimConsoleMessage(
                "scenario: this build's Lua takes no allocator, so a "
                "scenario script's memory is not capped");
        }
        return L;
    }

    /* Put beside the state at once, so every path that closes one can find
       what to free however early it gives up. */
    lua_pushlightuserdata(L, m);
    lua_setfield(L, LUA_REGISTRYINDEX, SCN_SANDBOX_MEM_KEY);
    return L;
}

void scnSandboxCloseState(lua_State *L) {
    ScnSandboxMem *m;

    if (L == NULL) {
        return;
    }
    /* Read before the close, because the registry goes with the state, and
       freed after it, because closing hands every block the state holds back
       through the allocator and the allocator reads this. NULL on the
       uncounted path, where free has nothing to do. */
    lua_getfield(L, LUA_REGISTRYINDEX, SCN_SANDBOX_MEM_KEY);
    m = (ScnSandboxMem *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    lua_close(L);
    free(m);
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

static void scnSandboxClearGlobal(lua_State *L, const char *name) {
    lua_pushnil(L);
    lua_setglobal(L, name);
}

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

/* ── The two calls that are replaced rather than removed ──────────── */

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

/* print, to the server console instead of to stdout. The arguments are
 * concatenated tab-separated and converted through __tostring where a value
 * carries one, which is what stock print does. */
static int scnSandboxPrint(lua_State *L) {
    char   line[SCN_PRINT_LEN];
    size_t used = 0;
    int    n    = lua_gettop(L);
    int    i;

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
    serverSimConsoleMessage(line);
    return 0;
}

/* ── The whitelist ────────────────────────────────────────────────── */

void scnSandboxOpenLibs(lua_State *L) {
    /* The loaders. Each of them brings code in from outside the chunk the
       host read, which is the one thing a state running a map file's script
       must not do. loadstring, module and newproxy are 5.1's and are not
       there to take on 5.4; require and module come with package, which is
       never opened, and the writes below are what says so whichever VM this
       is built against. */
    static const char *const kStrip[] = {
        "load", "loadstring", "dofile", "loadfile", "require",
        "module", "newproxy"
    };
    size_t i;

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

    for (i = 0; i < sizeof(kStrip) / sizeof(kStrip[0]); i++) {
        scnSandboxClearGlobal(L, kStrip[i]);
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
}

void scnSandboxSealRandom(lua_State *L) {
    scnSandboxClearField(L, LUA_MATHLIBNAME, "randomseed");
}
