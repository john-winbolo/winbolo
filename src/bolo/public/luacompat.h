/*
 * luacompat.h — Compatibility shims for Lua 5.1 / LuaJIT
 *
 * Lua 5.3+ introduced lua_isinteger() and LUA_OK which don't exist in 5.1.
 * Include this header after the standard Lua headers to get portable macros.
 */

#ifndef LUACOMPAT_H
#define LUACOMPAT_H

#include <lua.h>

/* LUA_OK was added in Lua 5.2; in 5.1 lua_pcall returns 0 on success. */
#ifndef LUA_OK
#define LUA_OK 0
#endif

/* lua_isinteger() was added in Lua 5.3.  In 5.1/LuaJIT all numbers are
 * doubles, so we fall back to lua_isnumber(). */
#if LUA_VERSION_NUM < 503
#define lua_isinteger(L, idx) lua_isnumber((L), (idx))
#endif

/* lua_rawlen() was added in Lua 5.2; in 5.1 it's lua_objlen(). */
#if LUA_VERSION_NUM < 502
#define lua_rawlen(L, idx) lua_objlen((L), (idx))
#endif

#endif /* LUACOMPAT_H */
