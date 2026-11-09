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

/* ================================================================== */
/* Brain Lua sandbox                                                   */
/*                                                                     */
/* Moved here from luabrainshandler.c so the unit tests can apply the  */
/* same code to a VM of their own.                                     */
/*                                                                     */
/* Unless the host opted out (--allow-unsafe-brains, BrainTest),       */
/* brainSandboxApply() locks the freshly opened standard libraries     */
/* down so an untrusted brain .lua cannot run programs, load native    */
/* code, escape the VM, or touch the filesystem outside its own        */
/* directory:                                                          */
/*   - io.open / loadfile / os.remove are replaced with wrappers that  */
/*     confine every path to the brain directory (read AND write).     */
/*   - io.popen / io.lines / io.input / io.output / io.tmpfile,        */
/*     os.execute / os.exit / os.rename / os.getenv / os.tmpname /      */
/*     os.setlocale, load / loadstring / dofile / string.dump,          */
/*     getfenv / setfenv / newproxy, package.loadlib, package.searchpath */
/*     and the native package loaders, every preloaded library (LuaJIT's */
/*     ffi and jit.util among them), cpf_astar_log_enable, and all of    */
/*     the debug library but traceback and getinfo are removed outright. */
/*   - brainSandboxLoadInitFile loads the init file as source text     */
/*     only, never bytecode.                                           */
/* The shipped GoalHunter brain needs no changes: its loadfile() of    */
/* los_stamp_cache.lua and its io.open() cache paths all resolve under  */
/* the brain directory, and everything it actually removes is dev-only */
/* tooling that only runs under BrainTest (which sets s_allow_unsafe).  */
/* ================================================================== */

#ifdef _WIN32
#  include <windows.h>
#endif

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "brain_sandbox.h"

/* Read a whole file through SDL_IOFromFile (Android assets too) into a
 * buffer the caller SDL_free()s. NULL on failure or for an empty file. */
static char *sandbox_load_file(const char *path, size_t *outLen) {
  SDL_IOStream *io = SDL_IOFromFile(path, "rb");
  if (!io) return NULL;
  Sint64 size = SDL_GetIOSize(io);
  if (size <= 0) { SDL_CloseIO(io); return NULL; }
  char *buf = (char *)SDL_malloc((size_t)size);
  if (!buf) { SDL_CloseIO(io); return NULL; }
  size_t nread = SDL_ReadIO(io, buf, (size_t)size);
  SDL_CloseIO(io);
  if ((Sint64)nread != size) { SDL_free(buf); return NULL; }
  *outLen = (size_t)size;
  return buf;
}

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
  src = sandbox_load_file(resolved, &len);
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
  luaL_checkstack(L, n + 2, "too many arguments");
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

/* debug.getinfo([thread,] f [, what]) with the 'f' option taken out. Function
 * objects are the one thing here a brain could use: a level below its think is
 * host C code, which was never written to be called from Lua.
 *
 * The real getinfo runs one frame deeper than the caller thinks, so a level on
 * this thread moves up by one to name the frame the caller meant. A negative
 * level names no frame, so it goes through as it is. */
static int l_getinfo_no_func(lua_State *L) {
  int arg = lua_isthread(L, 1) ? 1 : 0;
  const char *what = luaL_optstring(L, arg + 2, "Slnu");
  char kept[16];
  size_t n = 0;

  luaL_checkany(L, arg + 1);

  for (; *what != '\0' && n + 1 < sizeof(kept); what++) {
    if (*what != 'f') kept[n++] = *what;
  }
  kept[n] = '\0';
  lua_pushvalue(L, lua_upvalueindex(1));
  if (arg) lua_pushvalue(L, 1);
  if (lua_type(L, arg + 1) == LUA_TNUMBER && lua_tointeger(L, arg + 1) >= 0 &&
      (arg == 0 || lua_tothread(L, 1) == L)) {
    lua_pushinteger(L, lua_tointeger(L, arg + 1) + 1);
  } else {
    lua_pushvalue(L, arg + 1);
  }
  lua_pushstring(L, kept);
  lua_call(L, arg + 2, 1);
  return 1;
}

/* Clear every field of the table on the stack top but those named in keep
 * (NULL-terminated, or NULL for none). Setting a field that exists to nil is
 * allowed during a traversal. */
static void brain_clear_table_except(lua_State *L, const char *const *keep) {
  lua_pushnil(L);
  while (lua_next(L, -2) != 0) {
    bool kept = false;
    lua_pop(L, 1); /* the value; the key stays for the next lua_next */
    if (keep != NULL && lua_type(L, -1) == LUA_TSTRING) {
      const char *key = lua_tostring(L, -1);
      int i;
      for (i = 0; keep[i] != NULL; i++) {
        if (SDL_strcmp(key, keep[i]) == 0) kept = true;
      }
    }
    if (!kept) {
      lua_pushvalue(L, -1);
      lua_pushnil(L);
      lua_rawset(L, -4);
    }
  }
}

/* Lock down the standard libraries of a freshly-opened brain VM. `root`
 * is the brain's own directory: the only place file access is permitted. */
void brainSandboxApply(lua_State *L, const char *root) {
  static const char *kill_io[]    = { "popen", "lines", "input", "output",
                                      "tmpfile", NULL };
  static const char *kill_os[]    = { "execute", "exit", "rename", "getenv",
                                      "tmpname", "setlocale", NULL };
  /* load and loadstring take bytecode, which Lua does not verify. LuaJIT's
   * loadstring is a separate global from load. The others reach a function's
   * environment or make an object with a finalizer, and no brain needs them. */
  static const char *kill_base[]  = { "load", "loadstring", "dofile",
                                      "getfenv", "setfenv", "newproxy", NULL };
  static const char *const keep_debug[] = { "traceback", "getinfo", NULL };
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
  for (i = 0; kill_base[i]; i++) {
    lua_pushnil(L);
    lua_setglobal(L, kill_base[i]);
  }

  /* string.dump: the other half of building bytecode. */
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
    brain_clear_table_except(L, keep_debug);
    lua_getfield(L, -1, "getinfo");
    lua_pushcclosure(L, l_getinfo_no_func, 1);
    lua_setfield(L, -2, "getinfo");
  }
  lua_pop(L, 1);

  /* package: no native code. Drop loadlib + cpath and the two C loader
   * searchers (slots 3 and 4 of the freshly-opened searchers table). The
   * Lua + SDL source searchers remain, so require() of brain modules works. */
  lua_getglobal(L, "package");
  if (lua_istable(L, -1)) {
    lua_pushnil(L); lua_setfield(L, -2, "loadlib");
    lua_pushnil(L); lua_setfield(L, -2, "cpath");
    lua_pushnil(L); lua_setfield(L, -2, "searchpath");
    /* The preload searcher stays in the list, so what it can find has to go:
     * luaL_openlibs on LuaJIT leaves ffi there, which calls native code, and
     * jit.util and jit.profile. No brain preloads anything of its own. */
    lua_getfield(L, -1, "preload");
    if (lua_istable(L, -1)) {
      brain_clear_table_except(L, NULL);
    }
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

  /* A second way to fopen a brain-supplied path, for debug sessions, whose
   * folder is outside the brain directory anyway. Its one log file is shared
   * by every bot in the process, so closing it here could also pull it out
   * from under another bot's thread. cpathfinder.lua checks it exists. */
  lua_pushnil(L);
  lua_setglobal(L, "cpf_astar_log_enable");
}

int brainSandboxLoadInitFile(lua_State *L, const char *path, bool sandboxed) {
  const char *mode = sandboxed ? "t" : NULL;
  size_t len = 0;
  char *src = sandbox_load_file(path, &len);
  if (src != NULL) {
    int err = luaL_loadbufferx(L, src, len, path, mode);
    SDL_free(src);
    return err;
  }
  return luaL_loadfilex(L, path, mode);
}
