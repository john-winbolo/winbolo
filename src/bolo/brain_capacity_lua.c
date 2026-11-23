/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 *Name:          Brain Capacity (Lua)
 *Filename:      brain_capacity_lua.c
 *Author:        John Morrison
 *Purpose:
 *  Asks a Lua brain for its capacity answer through the
 *  optional Brain.capacity(machine, measured). See
 *  brain_capacity.h.
 *********************************************************/

#include "brain_capacity.h"

#include <SDL3/SDL.h>
#include <stdbool.h>

#include <lua.h>
#include <lauxlib.h>

#include "../common/wb_log.h"

/* Pushes the "machine" table Brain.capacity reads. */
static void push_capacity_machine(lua_State *L, const BrainMachineInfo *m) {
  lua_createtable(L, 0, 9);
  lua_pushstring(L, m->cpuName);
  lua_setfield(L, -2, "cpu_name");
  lua_pushinteger(L, (lua_Integer)m->logicalCores);
  lua_setfield(L, -2, "logical_cores");
  lua_pushinteger(L, (lua_Integer)m->physicalCores);
  lua_setfield(L, -2, "physical_cores");
  lua_pushnumber(L, m->speed1t);
  lua_setfield(L, -2, "speed_1t");
  lua_pushnumber(L, m->speedAll);
  lua_setfield(L, -2, "speed_all");
  lua_pushnumber(L, m->scaling);
  lua_setfield(L, -2, "scaling");
  lua_pushinteger(L, (lua_Integer)m->workerThreads);
  lua_setfield(L, -2, "worker_threads");
  lua_pushboolean(L, m->onBattery);
  lua_setfield(L, -2, "on_battery");
  lua_pushstring(L, m->version);
  lua_setfield(L, -2, "version");
}

/* ---- The limit on a brain's capacity code ------------------------------- */

/* The registry key of the CapLimit in force on a VM. */
static const char capLimitKey = 0;

typedef struct {
  Uint64         deadline;   /* SDL_GetTicks() past which it stops; 0 none */
  SDL_AtomicInt *abortFlag;  /* not 0: stop now; NULL none */
} CapLimit;

/* Lua instructions between two looks at the clock. */
#define CAP_HOOK_COUNT 1000

static void cap_hook(lua_State *L, lua_Debug *ar) {
  const CapLimit *lim;
  (void)ar;
  lua_pushlightuserdata(L, (void *)&capLimitKey);
  lua_rawget(L, LUA_REGISTRYINDEX);
  lim = (const CapLimit *)lua_touserdata(L, -1);
  lua_pop(L, 1);
  if (lim == NULL) return;
  if (lim->abortFlag != NULL && SDL_GetAtomicInt(lim->abortFlag) != 0) {
    luaL_error(L, "stopped: the host is stopping");
  }
  if (lim->deadline != 0 && SDL_GetTicks() > lim->deadline) {
    luaL_error(L, "stopped: the time limit for this brain code ran out");
  }
}

void brainCapacityLimitSet(struct lua_State *L, unsigned budgetMs,
                           struct SDL_AtomicInt *abortFlag) {
  CapLimit *lim;
  lua_pushlightuserdata(L, (void *)&capLimitKey);
  lim = (CapLimit *)lua_newuserdata(L, sizeof(*lim));
  lim->deadline = budgetMs > 0 ? SDL_GetTicks() + budgetMs : 0;
  lim->abortFlag = abortFlag;
  lua_rawset(L, LUA_REGISTRYINDEX);
  lua_sethook(L, cap_hook, LUA_MASKCOUNT, CAP_HOOK_COUNT);
}

void brainCapacityLimitClear(struct lua_State *L) {
  lua_sethook(L, NULL, 0, 0);
  lua_pushlightuserdata(L, (void *)&capLimitKey);
  lua_pushnil(L);
  lua_rawset(L, LUA_REGISTRYINDEX);
}

/* ---- The memory limit on a capacity-only VM ----------------------------- */

/* The registry key of the BrainCapacityMem of a VM. */
static const char capMemKey = 0;

static void *cap_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  BrainCapacityMem *m = (BrainCapacityMem *)ud;
  /* A new block: osize is not a size (Lua 5.4 passes the type there). */
  size_t old = ptr != NULL ? osize : 0;
  void *p;
  if (nsize > old &&
      (nsize - old > m->limit || m->used > m->limit - (nsize - old))) {
    return NULL;
  }
  p = m->allocf(m->allocd, ptr, osize, nsize);
  if (nsize != 0 && p == NULL) return NULL;
  m->used = m->used > old ? m->used - old : 0;
  m->used += nsize;
  if (m->used > m->peak) m->peak = m->used;
  return p;
}

void brainCapacityMemLimitSet(struct lua_State *L, BrainCapacityMem *m,
                              size_t limit) {
  m->allocf = NULL;
  m->allocd = NULL;
  m->used = 0;
  m->peak = 0;
  m->limit = limit;
  lua_pushlightuserdata(L, (void *)&capMemKey);
  lua_pushlightuserdata(L, m);
  lua_rawset(L, LUA_REGISTRYINDEX);
}

void brainCapacityMemArm(struct lua_State *L, bool on) {
  BrainCapacityMem *m;
  void *ud = NULL;
  lua_Alloc f;
  /* No allocation here: a light userdata key and a raw read. */
  lua_pushlightuserdata(L, (void *)&capMemKey);
  lua_rawget(L, LUA_REGISTRYINDEX);
  m = (BrainCapacityMem *)lua_touserdata(L, -1);
  lua_pop(L, 1);
  if (m == NULL) return;
  f = lua_getallocf(L, &ud);
  if (on) {
    if (f == cap_alloc) return;
    m->allocf = f;
    m->allocd = ud;
    m->used = (size_t)lua_gc(L, LUA_GCCOUNT, 0) * 1024u +
              (size_t)lua_gc(L, LUA_GCCOUNTB, 0);
    if (m->used > m->peak) m->peak = m->used;
    lua_setallocf(L, cap_alloc, m);
  } else if (f == cap_alloc && ud == m) {
    lua_setallocf(L, m->allocf, m->allocd);
  }
}

/* ---- The protected steps -------------------------------------------------- */

/* Pushes brain.version (a string, or nil) and true when "brain" is a
 * table, else nil and false. Runs in a protected call: an __index that
 * throws or loops cannot crash or freeze the caller. */
static int cap_read_version(lua_State *L) {
  lua_settop(L, 0);
  lua_getglobal(L, "brain");
  if (!lua_istable(L, -1)) {
    lua_pushnil(L);
    lua_pushboolean(L, 0);
    return 2;
  }
  lua_getfield(L, 1, "version");
  if (lua_type(L, -1) == LUA_TNUMBER) {
    lua_tostring(L, -1);   /* the number as text, in place */
  } else if (lua_type(L, -1) != LUA_TSTRING) {
    lua_pop(L, 1);
    lua_pushnil(L);
  }
  lua_pushboolean(L, 1);
  return 2;
}

/* Upvalue 1: the BrainMachineInfo (light userdata). Pushes false when the
 * brain has no capacity function, else true and its answer. */
static int cap_call(lua_State *L) {
  const BrainMachineInfo *m =
      (const BrainMachineInfo *)lua_touserdata(L, lua_upvalueindex(1));
  lua_settop(L, 0);
  lua_getglobal(L, "brain");
  if (!lua_istable(L, -1)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  lua_getfield(L, 1, "capacity");
  if (!lua_isfunction(L, -1)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  push_capacity_machine(L, m);
  lua_pushnil(L); /* measured: not measured yet */
  lua_call(L, 2, 1);
  lua_pushboolean(L, 1);
  lua_insert(L, -2);
  return 2;
}

int brainCapacityResolveLua(struct lua_State *L, const char *path,
                            const char *givenName) {
  return brainCapacityResolveLuaEx(L, path, givenName,
                                   BRAIN_CAPACITY_LUA_BUDGET_MS, NULL);
}

int brainCapacityResolveLuaEx(struct lua_State *L, const char *path,
                              const char *givenName, unsigned budgetMs,
                              struct SDL_AtomicInt *abortFlag) {
  char version[BRAIN_CAPACITY_VERSION_LEN] = "";
  char name[BRAIN_CAPACITY_VERSION_LEN * 2];
  BrainMachineInfo machine;
  int top = lua_gettop(L);
  int value, pcallErr;
  bool ok = false, haveFn = false, isTable = false, limited;
  double raw = 0.0;
  lua_Hook oldHook = lua_gethook(L);
  int oldMask = lua_gethookmask(L);
  int oldCount = lua_gethookcount(L);

  /* Answers are kept under the brain's directory name, so every host and
     every way of adding a bot asks for the same key. */
  brainCapacityNameFromPath(path, name, sizeof(name));
  if (name[0] == '\0' && givenName != NULL) {
    SDL_strlcpy(name, givenName, sizeof(name));
  }

  /* The brain's code below runs under the limit; the limit in force
     before (the caller's, or none) comes back after. */
  limited = budgetMs > 0 || abortFlag != NULL;
  if (limited) {
    lua_pushlightuserdata(L, (void *)&capLimitKey);
    lua_rawget(L, LUA_REGISTRYINDEX);   /* the old limit, kept on the stack */
    brainCapacityLimitSet(L, budgetMs, abortFlag);
  }

  lua_pushcfunction(L, cap_read_version);
  brainCapacityMemArm(L, true);
  pcallErr = lua_pcall(L, 0, 2, 0);
  brainCapacityMemArm(L, false);
  if (pcallErr != LUA_OK) {
    const char *e = lua_tostring(L, -1);
    WB_LOG_WARN(WB_LOG_CAT_LUA,
                "brain '%s': reading brain.version failed: %s",
                name, e ? e : "(no message)");
    value = BRAIN_CAPACITY_UNKNOWN;
    goto done;
  }
  isTable = lua_toboolean(L, -1) != 0;
  if (lua_type(L, -2) == LUA_TSTRING) {
    SDL_strlcpy(version, lua_tostring(L, -2), sizeof(version));
  }
  lua_pop(L, 2);
  if (!isTable) {
    value = BRAIN_CAPACITY_UNKNOWN;
    goto done;
  }

  value = brainCapacityLookup(name, version);
  if (value != BRAIN_CAPACITY_UNKNOWN) goto done;

  brainCapacityGetMachine(&machine);
  lua_pushlightuserdata(L, &machine);
  lua_pushcclosure(L, cap_call, 1);
  brainCapacityMemArm(L, true);
  pcallErr = lua_pcall(L, 0, 2, 0);
  brainCapacityMemArm(L, false);
  if (pcallErr == LUA_OK) {
    haveFn = lua_toboolean(L, -2) != 0;
    if (haveFn) {
      if (lua_type(L, -1) == LUA_TNUMBER) {
        ok = true;
        raw = (double)lua_tonumber(L, -1);
      } else if (!lua_isnil(L, -1)) {
        WB_LOG_WARN(WB_LOG_CAT_LUA,
                    "brain '%s': capacity() returned a %s, not a number; "
                    "using the default", name, luaL_typename(L, -1));
      }
    }
  } else {
    const char *e = lua_tostring(L, -1);
    haveFn = true;   /* it failed: the default, as before */
    WB_LOG_WARN(WB_LOG_CAT_LUA,
                "brain '%s': capacity() failed: %s; using the default",
                name, e ? e : "(no message)");
  }
  if (!haveFn) {
    value = brainCapacityQueryC(name, version);
    if (value != BRAIN_CAPACITY_UNKNOWN) goto done;
  }

  value = brainCapacityFromAnswer(ok, raw, &machine);
  brainCapacityStore(name, version, value);
  WB_LOG_INFO(WB_LOG_CAT_LUA, "brain '%s'%s%s: capacity %d bots on this machine",
              name, version[0] ? " version " : "", version, value);

done:
  if (limited) {
    /* The old limit is at top + 1: put it back under its key. */
    lua_settop(L, top + 1);
    lua_pushlightuserdata(L, (void *)&capLimitKey);
    lua_insert(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_sethook(L, oldHook, oldMask, oldCount);
  }
  lua_settop(L, top);
  return value;
}
