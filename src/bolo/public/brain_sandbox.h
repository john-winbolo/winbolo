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
 * The brain Lua sandbox. luabrainshandler.c applies it to every brain VM a
 * production host (WinBolo, WinBoloDS) creates; the unit tests apply the
 * same code to a VM of their own and try each way out of it.
 */

#ifndef BRAIN_SANDBOX_H
#define BRAIN_SANDBOX_H

#include <stdbool.h>
#include <stddef.h>

#include <lua.h>

/* Longest brain path the sandbox handles. Matches LUA_BRAINS_PATH_MAX. */
#define BRAIN_SANDBOX_PATH_MAX 512

/* Canonicalize path and check it lands inside root (the root itself or
 * below it). Writes the resolved path into out on success. */
bool brainSandboxResolveInRoot(const char *root, const char *path,
                               char *out, size_t outlen);

/* Lock down the standard libraries and the path-taking C bindings of a
 * freshly opened brain VM. root is the brain's own directory, the only
 * place file access is allowed. Call after the C bindings are registered
 * and before any brain code runs. */
void brainSandboxApply(lua_State *L, const char *root);

/* Load a brain's init file as a chunk onto the stack, through SDL first
 * (Android assets) and then stdio. When sandboxed is true the chunk must
 * be source text: Lua does not check bytecode, and crafted bytecode gets
 * out of the VM. Returns LUA_OK, or a Lua error code with the message on
 * the stack. */
int brainSandboxLoadInitFile(lua_State *L, const char *path, bool sandboxed);

#endif /* BRAIN_SANDBOX_H */
