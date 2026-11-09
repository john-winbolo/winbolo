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

/*
 * The brain Lua sandbox, moved here from luabrainshandler.c so the unit
 * tests can apply the same code to a VM of their own.
 */

#ifdef _WIN32
#  include <windows.h>
#endif

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "brain_sandbox.h"

/* True if canonPath is canonRoot itself or a path beneath it. The
 * separator check stops "/a/brainX" from matching root "/a/brain". */
static bool brain_under_root(const char *canonRoot, const char *canonPath) {
  size_t rl = SDL_strlen(canonRoot);
  if (rl == 0) return false;
#ifdef _WIN32
  if (SDL_strncasecmp(canonPath, canonRoot, rl) != 0) return false;
#else
  if (SDL_strncmp(canonPath, canonRoot, rl) != 0) return false;
#endif
  char sep = canonPath[rl];
  return sep == '\0' || sep == '/' || sep == '\\';
}

/* Canonicalize `path` (resolving .., symlinks, and relative-to-cwd) and
 * verify it lands inside `root`. Writes the resolved absolute path into
 * `out` and returns true on success. For a not-yet-existing file (write
 * mode) the parent directory is canonicalized instead, so new files under
 * the root are allowed while escapes are still rejected. */
bool brainSandboxResolveInRoot(const char *root, const char *path,
                               char *out, size_t outlen) {
  if (!path || !path[0]) return false;
#ifdef _WIN32
  char  rootCanon[BRAIN_SANDBOX_PATH_MAX];
  char  canon[BRAIN_SANDBOX_PATH_MAX * 2];
  DWORD rn = GetFullPathNameA(root, sizeof(rootCanon), rootCanon, NULL);
  DWORD cn = GetFullPathNameA(path, sizeof(canon), canon, NULL);
  if (rn == 0 || rn >= sizeof(rootCanon)) return false;
  if (cn == 0 || cn >= sizeof(canon)) return false;
  if (!brain_under_root(rootCanon, canon)) return false;
  SDL_strlcpy(out, canon, outlen);
  return true;
#else
  char *rootCanon = realpath(root, NULL);
  if (!rootCanon) return false;

  bool  ok    = false;
  char *canon = realpath(path, NULL);     /* full resolve if it exists */
  if (canon) {
    ok = brain_under_root(rootCanon, canon);
    if (ok) SDL_strlcpy(out, canon, outlen);
    free(canon);
  } else {
    /* New file: canonicalize the parent dir and re-append the basename. */
    char  tmp[BRAIN_SANDBOX_PATH_MAX * 2];
    char *parent;
    const char *base;
    char *slash;
    SDL_strlcpy(tmp, path, sizeof(tmp));
    slash = SDL_strrchr(tmp, '/');
    if (slash) { *slash = '\0'; base = slash + 1; parent = realpath(tmp[0] ? tmp : ".", NULL); }
    else       { base = tmp;    parent = realpath(".", NULL); }
    if (parent) {
      char full[BRAIN_SANDBOX_PATH_MAX * 2];
      SDL_snprintf(full, sizeof(full), "%s/%s", parent, base);
      ok = brain_under_root(rootCanon, full);
      if (ok) SDL_strlcpy(out, full, outlen);
      free(parent);
    }
  }
  free(rootCanon);
  return ok;
#endif
}

/* Jailed io.open(filename [, mode]): path-check, then delegate to the real
 * io.open (upvalue 1) with the resolved path. upvalue 2 is the brain root. */
static int l_jailed_io_open(lua_State *L) {
  const char *fname = luaL_checkstring(L, 1);
  const char *mode  = luaL_optstring(L, 2, "r");
  const char *root  = lua_tostring(L, lua_upvalueindex(2));
  char resolved[BRAIN_SANDBOX_PATH_MAX * 2];
  int  base;

  if (!brainSandboxResolveInRoot(root, fname, resolved, sizeof(resolved))) {
    lua_pushnil(L);
    lua_pushfstring(L, "io.open: '%s' is outside the brain directory", fname);
    return 2;
  }
  base = lua_gettop(L);
  lua_pushvalue(L, lua_upvalueindex(1)); /* the real io.open */
  lua_pushstring(L, resolved);
  lua_pushstring(L, mode);
  lua_call(L, 2, LUA_MULTRET);
  return lua_gettop(L) - base;
}

/* Jailed loadfile(filename): path-check, then load as TEXT only (rejects
 * bytecode) with the normal sandboxed _ENV. upvalue 1 is the brain root. */
static int l_jailed_loadfile(lua_State *L) {
  const char *fname = luaL_checkstring(L, 1);
  const char *root  = lua_tostring(L, lua_upvalueindex(1));
  char   resolved[BRAIN_SANDBOX_PATH_MAX * 2];
  size_t len = 0;
  char  *src;

  if (!brainSandboxResolveInRoot(root, fname, resolved, sizeof(resolved))) {
    lua_pushnil(L);
    lua_pushfstring(L, "loadfile: '%s' is outside the brain directory", fname);
    return 2;
  }
  src = (char *)SDL_LoadFile(resolved, &len);
  if (!src) {
    lua_pushnil(L);
    lua_pushfstring(L, "loadfile: cannot open '%s'", fname);
    return 2;
  }
  if (luaL_loadbufferx(L, src, len, resolved, "t") != LUA_OK) {
    SDL_free(src);
    lua_pushnil(L);
    lua_insert(L, -2); /* nil, errmsg */
    return 2;
  }
  SDL_free(src);
  return 1; /* the loaded chunk */
}

/* Jailed os.remove(filename): path-check, then remove. upvalue 1 = root. */
static int l_jailed_os_remove(lua_State *L) {
  const char *fname = luaL_checkstring(L, 1);
  const char *root  = lua_tostring(L, lua_upvalueindex(1));
  char resolved[BRAIN_SANDBOX_PATH_MAX * 2];

  if (!brainSandboxResolveInRoot(root, fname, resolved, sizeof(resolved))) {
    lua_pushnil(L);
    lua_pushfstring(L, "os.remove: '%s' is outside the brain directory", fname);
    return 2;
  }
  if (remove(resolved) != 0) {
    lua_pushnil(L);
    lua_pushfstring(L, "os.remove: cannot remove '%s'", fname);
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

/* Generic jail for a C binding whose first argument is a filesystem path
 * (e.g. gh_opt_log.open / gh_opt_log.append / gh_shield.load). Validates the
 * path against the brain root, then forwards the call to the original function
 * (upvalue 1) with the sanitized path substituted for arg 1 and the remaining
 * arguments unchanged. upvalue 2 is the brain root. */
static int l_jailed_path_arg1(lua_State *L) {
  const char *p    = lua_tostring(L, 1);
  const char *root = lua_tostring(L, lua_upvalueindex(2));
  char resolved[BRAIN_SANDBOX_PATH_MAX * 2];
  int  n, i;

  if (p == NULL || !brainSandboxResolveInRoot(root, p, resolved, sizeof(resolved))) {
    lua_pushnil(L);
    lua_pushfstring(L, "blocked: path '%s' is outside the brain directory",
                    p ? p : "(nil)");
    return 2;
  }
  n = lua_gettop(L);
  lua_pushvalue(L, lua_upvalueindex(1));        /* original function */
  lua_pushstring(L, resolved);                  /* sanitized arg 1 */
  for (i = 2; i <= n; i++) lua_pushvalue(L, i); /* original args 2..n */
  lua_call(L, n, LUA_MULTRET);
  return lua_gettop(L) - n;
}

/* Replace tbl[method] with l_jailed_path_arg1 wrapping the original, if the
 * global `tbl` exists and `method` is a function. No-op otherwise (brains that
 * don't register the binding are unaffected). */
static void brain_jail_table_method(lua_State *L, const char *tbl,
                                     const char *method, const char *root) {
  lua_getglobal(L, tbl);
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, method);
    if (lua_isfunction(L, -1)) {
      lua_pushstring(L, root);
      lua_pushcclosure(L, l_jailed_path_arg1, 2); /* upvalues: original, root */
      lua_setfield(L, -2, method);
    } else {
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
}

/* LuaJIT's luaL_openlibs puts these loaders in package.preload, where
 * require() finds them through the preload searcher:
 *   ffi           - calls any C function and reads or writes any address.
 *   string.buffer - buf:ref() and buf:reserve() give back raw pointers as
 *                   FFI cdata, which reach any address without ffi.
 *   jit.util      - gives out machine-code and IR call addresses.
 *   jit.profile   - runs brain callbacks from the profiler's timer.
 * table.new and table.clear stay: they only make or empty a table.
 * Clears each name from the table on top of the stack (no-op if it is not
 * a table). PUC-Lua has none of these, so this does nothing there. */
static void brain_drop_preloads(lua_State *L) {
  static const char *kill_preload[] = { "ffi", "string.buffer", "jit.util",
                                        "jit.profile", NULL };
  int i;
  if (!lua_istable(L, -1)) return;
  for (i = 0; kill_preload[i]; i++) {
    lua_pushnil(L);
    lua_setfield(L, -2, kill_preload[i]);
  }
}

/* Jail for a C binding whose optional first argument is a file path to
 * write (cpf_astar_log_enable). No argument or nil passes straight
 * through (it closes the log); a path must resolve inside the brain root.
 * upvalue 1 is the original function, upvalue 2 the root. */
static int l_jailed_optpath_arg1(lua_State *L) {
  const char *root = lua_tostring(L, lua_upvalueindex(2));
  const char *p;
  char resolved[BRAIN_SANDBOX_PATH_MAX * 2];
  int  base = lua_gettop(L);

  if (lua_isnoneornil(L, 1)) {
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L) - base;
  }
  p = (lua_type(L, 1) == LUA_TSTRING) ? lua_tostring(L, 1) : NULL;
  if (p == NULL || !brainSandboxResolveInRoot(root, p, resolved,
                                              sizeof(resolved))) {
    lua_pushnil(L);
    lua_pushfstring(L, "blocked: path '%s' is outside the brain directory",
                    p ? p : "(not a string)");
    return 2;
  }
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_pushstring(L, resolved);
  lua_call(L, 1, LUA_MULTRET);
  return lua_gettop(L) - base;
}

/* Replace the global function `name` with l_jailed_optpath_arg1 wrapping
 * it. No-op if the global is not a function. */
static void brain_jail_global_optpath(lua_State *L, const char *name,
                                      const char *root) {
  lua_getglobal(L, name);
  if (lua_isfunction(L, -1)) {
    lua_pushstring(L, root);
    lua_pushcclosure(L, l_jailed_optpath_arg1, 2);
    lua_setglobal(L, name);
  } else {
    lua_pop(L, 1);
  }
}

/* Lock down the standard libraries of a freshly-opened brain VM. `root`
 * is the brain's own directory: the only place file access is permitted. */
void brainSandboxApply(lua_State *L, const char *root) {
  static const char *kill_io[]    = { "popen", "lines", "input", "output",
                                      "tmpfile", NULL };
  static const char *kill_os[]    = { "execute", "exit", "rename", "getenv",
                                      "tmpname", "setlocale", NULL };
  static const char *kill_debug[] = { "sethook", "gethook", "getupvalue",
                                      "setupvalue", "upvalueid", "upvaluejoin",
                                      "getlocal", "setlocal", "getregistry",
                                      "setmetatable", "debug", NULL };
  int i;

  /* io: jail io.open, drop the arbitrary-path / shell members. */
  lua_getglobal(L, "io");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "open");           /* real io.open */
    lua_pushstring(L, root);
    lua_pushcclosure(L, l_jailed_io_open, 2);
    lua_setfield(L, -2, "open");
    for (i = 0; kill_io[i]; i++) {
      lua_pushnil(L);
      lua_setfield(L, -2, kill_io[i]);
    }
  }
  lua_pop(L, 1);

  /* base loaders: jail loadfile (text-only), remove load/dofile. */
  lua_pushstring(L, root);
  lua_pushcclosure(L, l_jailed_loadfile, 1);
  lua_setglobal(L, "loadfile");
  lua_pushnil(L); lua_setglobal(L, "load");
  lua_pushnil(L); lua_setglobal(L, "dofile");
  /* loadstring is LuaJIT's load: it takes bytecode as readily as source.
   * string.dump is how a brain would make that bytecode. */
  lua_pushnil(L); lua_setglobal(L, "loadstring");
  lua_getglobal(L, "string");
  if (lua_istable(L, -1)) {
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
  }
  lua_pop(L, 1);

  /* os: jail os.remove, drop process/env mutators; keep time/clock/date. */
  lua_getglobal(L, "os");
  if (lua_istable(L, -1)) {
    lua_pushstring(L, root);
    lua_pushcclosure(L, l_jailed_os_remove, 1);
    lua_setfield(L, -2, "remove");
    for (i = 0; kill_os[i]; i++) {
      lua_pushnil(L);
      lua_setfield(L, -2, kill_os[i]);
    }
  }
  lua_pop(L, 1);

  /* debug: keep only traceback + getinfo; drop the introspection that
   * could reach the originals we just replaced or hook the VM. */
  lua_getglobal(L, "debug");
  if (lua_istable(L, -1)) {
    for (i = 0; kill_debug[i]; i++) {
      lua_pushnil(L);
      lua_setfield(L, -2, kill_debug[i]);
    }
  }
  lua_pop(L, 1);

  /* package: no native code. Drop loadlib + cpath and the two C loader
   * searchers (slots 3 and 4 of the freshly-opened searchers table). The
   * Lua + SDL source searchers remain, so require() of brain modules works. */
  lua_getglobal(L, "package");
  if (lua_istable(L, -1)) {
    lua_pushnil(L); lua_setfield(L, -2, "loadlib");
    lua_pushnil(L); lua_setfield(L, -2, "cpath");
    lua_getfield(L, -1, "preload");
    brain_drop_preloads(L);
    lua_pop(L, 1);
    lua_getfield(L, -1, "loaded");
    brain_drop_preloads(L);
    lua_pop(L, 1);
    lua_getfield(L, -1, "searchers");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_getfield(L, -1, "loaders"); } /* LuaJIT/5.1 name */
    if (lua_istable(L, -1)) {
      lua_pushnil(L); lua_rawseti(L, -2, 4);
      lua_pushnil(L); lua_rawseti(L, -2, 3);
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  /* C brain bindings that fopen() a brain-supplied path are a parallel file
   * API; jail them to the brain directory too. These globals exist only for
   * brains that register them (e.g. GoalHunter); the wrapper is a no-op
   * otherwise. */
  brain_jail_table_method(L, "gh_opt_log", "open",   root);
  brain_jail_table_method(L, "gh_opt_log", "append", root);
  brain_jail_table_method(L, "gh_shield",  "load",   root);
  brain_jail_global_optpath(L, "cpf_astar_log_enable", root);

  /* The registry's own _PRELOAD is the table package.preload points at.
   * Clear it by that name too, so no copy of the loaders is left even if
   * the package table was not where it should be. */
  lua_getfield(L, LUA_REGISTRYINDEX, "_PRELOAD");
  brain_drop_preloads(L);
  lua_pop(L, 1);

  /* jit.attach hooks the trace compiler's events with brain callbacks
   * that receive raw trace and machine-code data. jit.on / off / flush /
   * status / opt stay: GoalHunter calls jit.off, and the host calls
   * jit.opt.start after this. */
  lua_getglobal(L, "jit");
  if (lua_istable(L, -1)) {
    lua_pushnil(L);
    lua_setfield(L, -2, "attach");
  }
  lua_pop(L, 1);
}

int brainSandboxLoadInitFile(lua_State *L, const char *path, bool sandboxed) {
  const char *mode = sandboxed ? "t" : NULL;
  size_t len = 0;
  /* SDL first: it reads Android assets too. Then plain stdio. Both take
   * the same mode, so neither way in accepts bytecode when sandboxed. */
  char *src = (char *)SDL_LoadFile(path, &len);
  if (src != NULL) {
    int err = luaL_loadbufferx(L, src, len, path, mode);
    SDL_free(src);
    return err;
  }
  return luaL_loadfilex(L, path, mode);
}
