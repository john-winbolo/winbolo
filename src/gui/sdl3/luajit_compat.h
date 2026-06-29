/*
 * luajit_compat.h — Lua 5.4 C-API shims for building the WinBolo brain glue
 * against LuaJIT (a Lua 5.1 VM). Force-included into every Lua-touching TU when
 * the WINBOLO_LUAJIT CMake option is on (-FI luajit_compat.h).
 *
 * Only the 5.3/5.4 C-API surface the brain code actually uses is shimmed:
 *   - lua_getextraspace (5.4): LuaJIT has no per-state extra space. We hand back
 *     a stable per-lua_State void* slot, lazily created as a GC'd userdata box
 *     pinned in the registry. Created once per state (at brain setup, single
 *     threaded); later calls are a single registry rawget.
 *   - lua_isinteger (5.3): approximate as "a number with no fractional part".
 *   - LUA_EXTRASPACE: some code may reference the size; define it.
 *
 * This is part of the experimental lua-jit branch — see docs/luajit-port.md.
 */
#ifndef WINBOLO_LUAJIT_COMPAT_H
#define WINBOLO_LUAJIT_COMPAT_H

#ifdef WINBOLO_LUAJIT

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef LUA_EXTRASPACE
#define LUA_EXTRASPACE (sizeof(void *))
#endif

/* Fixed registry key (its address) under which each state stores its box. */
static char wbn_lj_extraspace_key__;

static inline void *lua_getextraspace(lua_State *L) {
  void **box;
  lua_pushlightuserdata(L, (void *)&wbn_lj_extraspace_key__);
  lua_rawget(L, LUA_REGISTRYINDEX);                 /* registry[key] */
  if (lua_islightuserdata(L, -1) || lua_isuserdata(L, -1)) {
    box = (void **)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return box;
  }
  lua_pop(L, 1);                                     /* nil */
  box = (void **)lua_newuserdata(L, sizeof(void *)); /* GC-managed box */
  *box = NULL;
  lua_pushlightuserdata(L, (void *)&wbn_lj_extraspace_key__);
  lua_pushvalue(L, -2);                              /* the userdata */
  lua_rawset(L, LUA_REGISTRYINDEX);                  /* registry[key] = box */
  lua_pop(L, 1);                                     /* leave nothing extra */
  return box;
}

/* 5.3 integer-subtype probe. LuaJIT 2.1 has a dual number model but no API
 * distinction; treat an integral-valued number as an integer. */
static inline int lua_isinteger(lua_State *L, int idx) {
  if (lua_type(L, idx) != LUA_TNUMBER) return 0;
  lua_Number n = lua_tonumber(L, idx);
  return n == (lua_Number)(long long)n;
}

/* 5.2 raw length — LuaJIT (without LUA52COMPAT) only has lua_objlen. */
static inline size_t lua_rawlen(lua_State *L, int idx) {
  return lua_objlen(L, idx);
}

/* 5.2 luaL_tolstring — convert any value to a string, push it, return it
 * (honouring __tostring). Mirrors PUC's semantics so callers' pop logic holds. */
static inline const char *luaL_tolstring(lua_State *L, int idx, size_t *len) {
  if (luaL_callmeta(L, idx, "__tostring")) {
    if (!lua_isstring(L, -1))
      luaL_error(L, "'__tostring' must return a string");
  } else {
    switch (lua_type(L, idx)) {
      case LUA_TNUMBER:
      case LUA_TSTRING:
        lua_pushvalue(L, idx);
        break;
      case LUA_TBOOLEAN:
        lua_pushstring(L, lua_toboolean(L, idx) ? "true" : "false");
        break;
      case LUA_TNIL:
        lua_pushliteral(L, "nil");
        break;
      default: {
        char b[64];
        snprintf(b, sizeof(b), "%s: %p", luaL_typename(L, idx),
                 lua_topointer(L, idx));
        lua_pushstring(L, b);
        break;
      }
    }
  }
  return lua_tolstring(L, -1, len);
}

/* NB: LuaJIT 2.1 already provides luaL_traceback — do NOT shim it (redefining
 * extern as static is a hard error here). */

#endif /* WINBOLO_LUAJIT */
#endif /* WINBOLO_LUAJIT_COMPAT_H */
