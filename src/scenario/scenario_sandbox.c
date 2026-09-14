/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Sandbox
 *Filename:      scenario_sandbox.c
 *Author:        John Morrison
 *Purpose:
 *  What a scenario script is given: base, coroutine,
 *  string, table, math, os and — where the VM has one —
 *  utf8, each opened by hand rather than the standard
 *  library being opened as a whole.
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
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "server_sim.h"            /* serverSimConsoleMessage */

#include "scenario_sandbox.h"

/* One console line a script's print can produce. A line past this is cut
   rather than dropped, so an operator still sees what a long one was
   about. */
#define SCN_PRINT_LEN 1024

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
