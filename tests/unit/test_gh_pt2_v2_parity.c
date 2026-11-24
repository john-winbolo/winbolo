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
 * test_gh_pt2_v2_parity.c -- pill take v2 in C
 * (brains/GoalHunter/c/gh_pt2_v2.c) against the Lua copy
 * (pilltake_v2.lua M.evaluate_lua).
 *
 * The test is brains/GoalHunter/test_pt2_v2_c_parity.lua: random small
 * worlds with random v2 knobs plus the cases recorded in a real game
 * (tests/unit/data/pt2_cases.lua). Each case runs both copies, in one call
 * and split over ticks (PT2_V2_SPLIT); the result, the order of the calls
 * they make and the terrain reads must be the same. It runs on
 * brains/GoalHunter and on brains/GoalHunter/opt.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "gh_pt2_old.h"
#include "gh_pt2_v2.h"
#include "test_harness.h"

#ifndef WB_REPO_ROOT_DIR
#define WB_REPO_ROOT_DIR "."
#endif

static int pt2v_run(const char *brainDir) {
    lua_State *L = luaL_newstate();
    UT_ASSERT(L != NULL);
    luaL_openlibs(L);
    lua_pushboolean(L, 0); lua_setglobal(L, "BRAIN_DEBUG_MODE");
    lua_pushboolean(L, 0); lua_setglobal(L, "_PRINT2_ENABLED");
    lua_pushstring(L, brainDir); lua_setglobal(L, "BRAIN_DIR");
    lua_pushstring(L, WB_REPO_ROOT_DIR "/tests/unit/data/pt2_cases.lua");
    lua_setglobal(L, "PT2_CASES_FILE");
    naPt2OldRegister(L);
    naPt2V2Register(L);

    lua_getglobal(L, "package");
    lua_pushfstring(L, "%s/?.lua;%s/?/init.lua", brainDir, brainDir);
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);

    char path[1024];
    snprintf(path, sizeof(path), "%s/test_pt2_v2_c_parity.lua", brainDir);
    int rc = 0;
    if (luaL_loadfile(L, path) != 0) {
        fprintf(stderr, "FAIL load: %s\n", lua_tostring(L, -1));
        rc = 1;
    } else if (lua_pcall(L, 0, 4, 0) != 0) {
        fprintf(stderr, "FAIL run (%s): %s\n", brainDir, lua_tostring(L, -1));
        rc = 1;
    } else {
        int n = (int)lua_tointeger(L, -4);
        int bad = (int)lua_tointeger(L, -3);
        const char *first = lua_tostring(L, -2);
        const char *cov = lua_tostring(L, -1);
        printf("gh_pt2_v2_parity (%s): %d cases, %d mismatches; %s\n", brainDir, n, bad, cov ? cov : "");
        if (bad > 0) {
            fprintf(stderr, "FAIL first mismatch: %s\n", first ? first : "");
            rc = 1;
        }
        if (n == 0) {
            fprintf(stderr, "FAIL no case ran\n");
            rc = 1;
        }
    }
    lua_close(L);
    return rc;
}

int run_gh_pt2_v2_parity(void) {
    int rc = pt2v_run(WB_REPO_ROOT_DIR "/brains/GoalHunter");
    rc |= pt2v_run(WB_REPO_ROOT_DIR "/brains/GoalHunter/opt");
    return rc;
}
