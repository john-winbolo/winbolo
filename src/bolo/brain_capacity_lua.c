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

int brainCapacityResolveLua(struct lua_State *L, const char *path,
                            const char *givenName) {
  char version[BRAIN_CAPACITY_VERSION_LEN] = "";
  char name[BRAIN_CAPACITY_VERSION_LEN * 2];
  BrainMachineInfo machine;
  int top = lua_gettop(L);
  int value;
  bool ok = false;
  double raw = 0.0;

  /* Answers are kept under the brain's directory name, so every host and
     every way of adding a bot asks for the same key. */
  brainCapacityNameFromPath(path, name, sizeof(name));
  if (name[0] == '\0' && givenName != NULL) {
    SDL_strlcpy(name, givenName, sizeof(name));
  }

  lua_getglobal(L, "brain");
  if (!lua_istable(L, -1)) {
    lua_settop(L, top);
    return BRAIN_CAPACITY_UNKNOWN;
  }
  lua_getfield(L, -1, "version");
  if (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER) {
    SDL_strlcpy(version, lua_tostring(L, -1), sizeof(version));
  }
  lua_pop(L, 1);

  value = brainCapacityLookup(name, version);
  if (value != BRAIN_CAPACITY_UNKNOWN) {
    lua_settop(L, top);
    return value;
  }

  brainCapacityGetMachine(&machine);
  lua_getfield(L, -1, "capacity");
  if (lua_isfunction(L, -1)) {
    push_capacity_machine(L, &machine);
    lua_pushnil(L); /* measured: not measured yet */
    if (lua_pcall(L, 2, 1, 0) == LUA_OK) {
      if (lua_type(L, -1) == LUA_TNUMBER) {
        ok = true;
        raw = (double)lua_tonumber(L, -1);
      } else if (!lua_isnil(L, -1)) {
        WB_LOG_WARN(WB_LOG_CAT_LUA,
                    "brain '%s': capacity() returned a %s, not a number; "
                    "using the default", name ? name : "?",
                    luaL_typename(L, -1));
      }
    } else {
      const char *e = lua_tostring(L, -1);
      WB_LOG_WARN(WB_LOG_CAT_LUA,
                  "brain '%s': capacity() failed: %s; using the default",
                  name ? name : "?", e ? e : "(no message)");
    }
  } else {
    lua_settop(L, top);
    value = brainCapacityQueryC(name, version);
    if (value != BRAIN_CAPACITY_UNKNOWN) {
      return value;
    }
  }
  lua_settop(L, top);

  value = brainCapacityFromAnswer(ok, raw, &machine);
  brainCapacityStore(name, version, value);
  WB_LOG_INFO(WB_LOG_CAT_LUA, "brain '%s'%s%s: capacity %d bots on this machine",
              name ? name : "?", version[0] ? " version " : "", version, value);
  return value;
}

