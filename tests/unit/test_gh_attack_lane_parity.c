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
 * GoalHunter clear-aim test, C and Lua side by side, with the minimum clear
 * lane (knob BLOCKER_MIN_LANE_BRAD).
 *
 * The goal pick runs the C sweep (brains/GoalHunter/c/gh_attack.c,
 * eval_parity -> clear_aim_from_world) and plan_position runs the Lua one
 * (attack.lua spot_clear_aim -> clear_aim_from_world -> M.aim_lane_wide).
 * Both must keep and drop the same aims, or the two picks disagree.
 *
 * A real Lua state (the VM the unit exe links), the real GoalHunter modules
 * from the source tree (and again from opt/), the real cpf_* bindings over a
 * real BrainPathfinder, and the real gh_attack module.  The map is grass with
 * a few forest squares and one wall; the target pill has live pills of both
 * owners around it, so many standoff spots see the pill down a narrow lane.
 * For every spot on three rings around the pill, and each lane width 0,
 * 0.125 and 0.5 brad, gh_attack.clear_aim must give the same aim index and
 * tree count as attack.lua's _clear_aim_from_world.  The case also asks that
 * some spots change their answer between 0 and 0.125 (narrow lanes are
 * really there), so a match is not only the old test matching itself.
 */

#include <stdio.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "global.h"
#include "brain_pathfinder.h"
#include "braincore.h"
#include "gh_attack.h"
#include "test_harness.h"

#ifndef WB_REPO_ROOT_DIR
#define WB_REPO_ROOT_DIR "."
#endif

/* Terrain codes, as constants.lua has them. */
#define GLP_T_BUILDING 0
#define GLP_T_FOREST   5
#define GLP_T_GRASS    7

#define GLP_PMX 128
#define GLP_PMY 128

static BYTE        s_glpMap[256 * 256];
static const BYTE *s_glpMapPtr = s_glpMap;

static const char *s_glpLua =
"local C   = require('constants')\n"
"local A   = require('attack')\n"
"local PMX, PMY = ...\n"
"local world = { pills = {}, pill_at = {}, base_at = {} }\n"
"local function add_pill(id, mx, my, owner, hp)\n"
"  local p = { id = id, mx = mx, my = my, owner = owner, health = hp or 15 }\n"
"  world.pills[id] = p\n"
"  local k = my * 256 + mx\n"
"  world.pill_at[k] = world.pill_at[k] or {}\n"
"  local l = world.pill_at[k]; l[#l + 1] = { id = id, pill = p }\n"
"end\n"
"add_pill(1, PMX, PMY, 'hostile')\n"
"add_pill(2, PMX - 3, PMY,     'friendly')\n"
"add_pill(3, PMX,     PMY - 3, 'hostile')\n"
"add_pill(4, PMX + 2, PMY + 2, 'friendly')\n"
"add_pill(5, PMX + 3, PMY - 1, 'neutral')\n"
"add_pill(6, PMX - 2, PMY + 3, 'hostile')\n"
"add_pill(7, PMX - 4, PMY - 4, 'friendly', 0)  -- dead: no blocker\n"
"gh_attack.sync_pill_at(world, PMX, PMY)\n"
"local R0 = C.ATTACK_PILL_STANDOFF\n"
"local n, bad, first = 0, 0, nil\n"
"local changed125, changed05 = 0, 0\n"
"for _, R in ipairs({ R0 - 1.0, R0, R0 + 0.5 }) do\n"
"  for deg = 0, 359 do\n"
"    local rad = deg * math.pi / 180\n"
"    local cx = PMX + 0.5 + math.sin(rad) * R\n"
"    local cy = PMY + 0.5 - math.cos(rad) * R\n"
"    local ox = math.floor(cx * 256 + 0.5)\n"
"    local oy = math.floor(cy * 256 + 0.5)\n"
"    local ans = {}\n"
"    for li, ml in ipairs({ 0, 0.125, 0.5 }) do\n"
"      n = n + 1\n"
"      local li_idx, _, _, li_trees = A._clear_aim_from_world(ox, oy, PMX, PMY, world, nil, ml)\n"
"      local c_idx, c_trees = gh_attack.clear_aim(ox, oy, PMX, PMY, ml)\n"
"      if li_idx ~= c_idx or (li_idx and li_trees ~= c_trees) then\n"
"        bad = bad + 1\n"
"        first = first or string.format('R=%.2f deg=%d lane=%.3f LUA=%s/%s C=%s/%s',\n"
"          R, deg, ml, tostring(li_idx), tostring(li_trees), tostring(c_idx), tostring(c_trees))\n"
"      end\n"
"      ans[li] = li_idx or 0\n"
"    end\n"
"    if ans[1] ~= ans[2] then changed125 = changed125 + 1 end\n"
"    if ans[2] ~= ans[3] then changed05 = changed05 + 1 end\n"
"  end\n"
"end\n"
"return n, bad, first or '', changed125, changed05\n";

static int glp_run(const char *brainDir) {
    memset(s_glpMap, GLP_T_GRASS, sizeof(s_glpMap));
    /* A few forest squares and one wall near the pill: trees on some lines,
     * a wall that blocks others. */
    s_glpMap[(GLP_PMY + 4) * 256 + (GLP_PMX + 1)] = GLP_T_FOREST;
    s_glpMap[(GLP_PMY + 1) * 256 + (GLP_PMX - 4)] = GLP_T_FOREST;
    s_glpMap[(GLP_PMY - 4) * 256 + (GLP_PMX + 3)] = GLP_T_FOREST;
    s_glpMap[(GLP_PMY + 4) * 256 + (GLP_PMX - 4)] = GLP_T_BUILDING;

    BrainPathfinder *pf = brainPathfinderCreate();
    UT_ASSERT(pf != NULL);
    brainPathfinderSetMap(pf, s_glpMap);

    lua_State *L = luaL_newstate();
    UT_ASSERT(L != NULL);
    *(void **)lua_getextraspace(L) = NULL;
    luaL_openlibs(L);
    brainCoreRegisterConstants(L);
    lua_pushboolean(L, 0); lua_setglobal(L, "BRAIN_DEBUG_MODE");
    lua_pushboolean(L, 0); lua_setglobal(L, "_PRINT2_ENABLED");
    lua_pushstring(L, brainDir); lua_setglobal(L, "BRAIN_DIR");
    brainCoreRegisterGetTerrain(L, &s_glpMapPtr);
    brainCoreRegisterPathfinder(L, &pf);
    naAttackRegister(L);

    lua_getglobal(L, "package");
    lua_pushfstring(L, "%s/?.lua;%s/?/init.lua", brainDir, brainDir);
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);

    /* The same two-argument math.atan init.lua installs on LuaJIT. */
    (void)luaL_dostring(L,
        "if math.atan2 then local a1, a2 = math.atan, math.atan2\n"
        "  math.atan = function(y, x) if x == nil then return a1(y) end return a2(y, x) end\n"
        "end");

    int rc = 0;
    if (luaL_loadbuffer(L, s_glpLua, strlen(s_glpLua), "=gh_attack_lane_parity") != 0) {
        fprintf(stderr, "FAIL load: %s\n", lua_tostring(L, -1));
        rc = 1;
    } else {
        lua_pushinteger(L, GLP_PMX);
        lua_pushinteger(L, GLP_PMY);
        if (lua_pcall(L, 2, 5, 0) != 0) {
            fprintf(stderr, "FAIL run (%s): %s\n", brainDir, lua_tostring(L, -1));
            rc = 1;
        } else {
            int n          = (int)lua_tointeger(L, -5);
            int bad        = (int)lua_tointeger(L, -4);
            const char *f  = lua_tostring(L, -3);
            int changed125 = (int)lua_tointeger(L, -2);
            int changed05  = (int)lua_tointeger(L, -1);
            printf("gh_attack_lane_parity (%s): %d checks, %d mismatches, "
                   "%d spots change at 0.125, %d more at 0.5\n",
                   brainDir, n, bad, changed125, changed05);
            if (bad > 0) {
                fprintf(stderr, "FAIL first mismatch: %s\n", f ? f : "");
                rc = 1;
            }
            if (n == 0 || changed125 == 0) {
                fprintf(stderr, "FAIL no spot with a narrow lane: the case "
                                "does not test the lane rule\n");
                rc = 1;
            }
        }
    }
    lua_close(L);
    brainPathfinderDestroy(pf);
    return rc;
}

int run_gh_attack_lane_parity(void) {
    int rc = glp_run(WB_REPO_ROOT_DIR "/brains/GoalHunter");
    rc |= glp_run(WB_REPO_ROOT_DIR "/brains/GoalHunter/opt");
    return rc;
}
