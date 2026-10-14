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
 * BENCH: THE BLOCKER STEP ON THE DECOY SQUARE (test_bench_decoy_step.c).
 *
 * With DECOY_GETAWAY_BLOCKER_STEP_FIRST on, decoy_getaway's M.update runs
 * the blocker count every think while a bot holds a decoy square
 * (brains/GoalHunter/decoy_getaway.lua, the "THE BLOCKER STEP" block in
 * M.update): M.closest_pill, then M.blockers (one cpf.simulate_shot from
 * the pill to the park square, walked in Lua), then the hit ledger
 * (M.led_entry, M.hear, M.wall_shots) and the ga.blk table.
 *
 * This case times exactly that work in isolation: a real Lua state (the
 * VM the unit exe links: LuaJIT in the normal build), the real GoalHunter
 * modules loaded from the source tree, the real cpf_* bindings over a real
 * BrainPathfinder, and a hand-made world of 16 pills with one wall on the
 * closest enemy pill's shell line.
 *
 * It is a BENCH, not a check: it does nothing unless WB_BENCH=1 is set, so
 * the default run passes it at once.  Run it by name:
 *     set WB_BENCH=1
 *     WinBoloUnitTests.exe --test bench_decoy_step_first
 * WB_BENCH_N sets the calls per repeat (default 100000).
 *
 * "cadence N" is the same block as it is now, with THE BLOCKER CHECK CACHE
 * (DECOY_GETAWAY_BLOCKER_CHECK_EVERY = N): each call is one think one tick
 * after the last, player 1, nothing on the line changes, so with N = 4 the
 * closest pill and the trace run on one think in four.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "global.h"
#include "brain_pathfinder.h"
#include "braincore.h"
#include "test_harness.h"

#ifndef WB_REPO_ROOT_DIR
#define WB_REPO_ROOT_DIR "."
#endif

#define BDS_REPEATS 7

/* Terrain codes, as constants.lua has them. */
#define BDS_T_BUILDING 0
#define BDS_T_GRASS    7

/* The decoy square and the closest enemy pill (about 7.2 squares away, in
 * range 8), as the scan would find them on an open field. */
#define BDS_TANK_X 106
#define BDS_TANK_Y 104
#define BDS_PILL_X 100
#define BDS_PILL_Y 100

static BYTE        s_map[256 * 256];
static const BYTE *s_mapPtr = s_map;

static int l_bench_now(lua_State *L) {
    lua_pushnumber(L, (double)SDL_GetPerformanceCounter() /
                      (double)SDL_GetPerformanceFrequency());
    return 1;
}

/* The Lua side.  setup() builds the world and returns the tiles of the
 * pill's shell line; run(name, n) times n calls of one part and returns
 * the seconds.  The body of "full" is the step-first block of M.update,
 * copied line for line (the print2 inside the hear branch never runs: no
 * events). */
static const char *s_benchLua =
"local C   = require('constants')\n"
"local U   = require('util')\n"
"local cpf = require('cpathfinder')\n"
"local DG  = require('decoy_getaway')\n"
"local B = {}\n"
"local world, ga, h, info, now\n"
"local TX, TY, PX, PY = ...\n"
"local function add_pill(id, mx, my, owner)\n"
"  local p = { id = id, mx = mx, my = my, owner = owner, health = 15 }\n"
"  world.pills[id] = p\n"
"  local k = my * 256 + mx\n"
"  world.pill_at[k] = world.pill_at[k] or {}\n"
"  local l = world.pill_at[k]; l[#l + 1] = { pill = p }\n"
"end\n"
"function B.setup()\n"
"  world = { pills = {}, pill_at = {} }\n"
"  -- 16 pills: 5 hostile in range of the decoy square (the closest is\n"
"  -- id 1), 3 of ours in range, 8 far off (hostile and neutral).\n"
"  add_pill(1,  PX, PY, 'hostile')\n"
"  add_pill(2,  TX + 7, TY - 3, 'hostile')\n"
"  add_pill(3,  TX - 2, TY + 7, 'hostile')\n"
"  add_pill(4,  TX + 6, TY + 5, 'hostile')\n"
"  add_pill(5,  TX - 7, TY + 3, 'hostile')\n"
"  add_pill(6,  TX + 3, TY - 7, 'friendly')\n"
"  add_pill(7,  TX - 3, TY - 6, 'friendly')\n"
"  add_pill(8,  TX + 7, TY + 1, 'friendly')\n"
"  for i = 0, 7 do\n"
"    add_pill(9 + i, 40 + i * 20, 200 - i * 3, (i % 2 == 0) and 'hostile' or 'neutral')\n"
"  end\n"
"  h = { mx = TX, my = TY, oid = 1 }\n"
"  ga = { alive = {}, led = {}, park_mx = TX, park_my = TY }\n"
"  info = { events = {}, tankx = U.m2w(TX), tanky = U.m2w(TY) }\n"
"  now = 1000\n"
"  return cpf.simulate_shot(U.m2w(PX), U.m2w(PY), U.m2w(TX), U.m2w(TY), cpf.SHOT_PILL, 0)\n"
"end\n"
"local function full()\n"
"  ga.led = ga.led or {}\n"
"  local kx, ky = ga.park_mx or h.mx, ga.park_my or h.my\n"
"  local pid, p = DG.closest_pill(world, kx, ky, ga.alive, now)\n"
"  local n, list, why\n"
"  if p then\n"
"    n, list, why = DG.blockers(world, p, kx, ky, nil)\n"
"    if why == 'last' and list[1].kind ~= 'pill' then DG.led_entry(ga.led, list[1]) end\n"
"  end\n"
"  if DG.hear(ga.led, info.events, now) > 0 then error('no events expected') end\n"
"  if p then\n"
"    if why == 'last' and list[1].kind ~= 'pill' then n = DG.wall_shots(ga.led, list[1]) end\n"
"    ga.blk = { id = pid, mx = p.mx, my = p.my, shots = n, list = list,\n"
"               why = why, tx = kx, ty = ky,\n"
"               twx = info.tankx, twy = info.tanky, off = false }\n"
"  end\n"
"end\n"
"local function cadence(every)\n"
"  return function()\n"
"    now = now + 1\n"
"    ga.led = ga.led or {}\n"
"    local kx, ky = ga.park_mx or h.mx, ga.park_my or h.my\n"
"    local chk = DG.blk_recheck(world, ga, ga.bc, kx, ky, now, every, 1)\n"
"    if chk then\n"
"      ga.bc = DG.blk_check(world, ga, kx, ky, now)\n"
"      ga.bc.chk = chk\n"
"    end\n"
"    local bc = ga.bc\n"
"    local pid, n, list, why = bc.pid, bc.n, bc.list, bc.why\n"
"    local p = pid and world.pills[pid]\n"
"    if p then\n"
"      if why == 'last' and list[1].kind ~= 'pill' then DG.led_entry(ga.led, list[1]) end\n"
"    end\n"
"    if DG.hear(ga.led, info.events, now) > 0 then error('no events expected') end\n"
"    if p then\n"
"      if why == 'last' and list[1].kind ~= 'pill' then n = DG.wall_shots(ga.led, list[1]) end\n"
"      ga.blk = { id = pid, mx = p.mx, my = p.my, shots = n, list = list,\n"
"                 why = why, tx = kx, ty = ky,\n"
"                 twx = info.tankx, twy = info.tanky, off = false,\n"
"                 age = now - bc.t }\n"
"    end\n"
"  end\n"
"end\n"
"local parts = {\n"
"  full = full,\n"
"  cadence1 = cadence(1),\n"
"  cadence4 = cadence(4),\n"
"  closest = function() DG.closest_pill(world, TX, TY, ga.alive, now) end,\n"
"  blockers = function() DG.blockers(world, world.pills[1], TX, TY, nil) end,\n"
"  trace = function()\n"
"    cpf.simulate_shot(U.m2w(PX), U.m2w(PY), U.m2w(TX), U.m2w(TY), cpf.SHOT_PILL, 0)\n"
"  end,\n"
"}\n"
"function B.check()\n"
"  full()\n"
"  local b = ga.blk\n"
"  return b and b.id, b and b.why, b and b.shots, b and #b.list\n"
"end\n"
"function B.run(name, n)\n"
"  local f = parts[name]\n"
"  for i = 1, 1000 do f() end\n"
"  collectgarbage('collect')\n"
"  local t0 = bench_now()\n"
"  for i = 1, n do f() end\n"
"  return bench_now() - t0\n"
"end\n"
"return B\n";

static int cmpDouble(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void report(const char *name, double *us, int n) {
    double mean = 0.0;
    for (int i = 0; i < n; i++) mean += us[i];
    mean /= n;
    qsort(us, (size_t)n, sizeof(double), cmpDouble);
    printf("  %-34s mean %7.3f  median %7.3f  min %7.3f  us/call\n",
           name, mean, us[n / 2], us[0]);
}

int run_bench_decoy_step_first(void) {
    const char *on = getenv("WB_BENCH");
    if (on == NULL || on[0] != '1') {
        printf("bench_decoy_step_first: skipped (set WB_BENCH=1 to run)\n");
        return 0;
    }
    const char *ns = getenv("WB_BENCH_N");
    int n = (ns && atoi(ns) > 0) ? atoi(ns) : 100000;

    memset(s_map, BDS_T_GRASS, sizeof(s_map));

    BrainPathfinder *pf = brainPathfinderCreate();
    UT_ASSERT(pf != NULL);
    brainPathfinderSetMap(pf, s_map);

    lua_State *L = luaL_newstate();
    UT_ASSERT(L != NULL);
    *(void **)lua_getextraspace(L) = NULL;
    luaL_openlibs(L);
    brainCoreRegisterConstants(L);
    lua_pushboolean(L, 0); lua_setglobal(L, "BRAIN_DEBUG_MODE");
    lua_pushboolean(L, 0); lua_setglobal(L, "_PRINT2_ENABLED");
    brainCoreRegisterGetTerrain(L, &s_mapPtr);
    brainCoreRegisterPathfinder(L, &pf);
    lua_pushcfunction(L, l_bench_now); lua_setglobal(L, "bench_now");

    /* package.path: the GoalHunter source modules. */
    lua_getglobal(L, "package");
    lua_pushstring(L, WB_REPO_ROOT_DIR "/brains/GoalHunter/?.lua;"
                      WB_REPO_ROOT_DIR "/brains/GoalHunter/?/init.lua");
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);

    if (luaL_loadbuffer(L, s_benchLua, strlen(s_benchLua), "=bench_decoy_step") != 0) {
        fprintf(stderr, "FAIL load: %s\n", lua_tostring(L, -1));
        return 1;
    }
    lua_pushinteger(L, BDS_TANK_X); lua_pushinteger(L, BDS_TANK_Y);
    lua_pushinteger(L, BDS_PILL_X); lua_pushinteger(L, BDS_PILL_Y);
    if (lua_pcall(L, 4, 1, 0) != 0) {
        fprintf(stderr, "FAIL init: %s\n", lua_tostring(L, -1));
        return 1;
    }
    int bench = lua_gettop(L);

    /* setup: the shell line on the open field.  Put ONE wall on it, in the
     * middle, so the line has a last blocker (the step-first trigger case)
     * and the walk still runs to the tank's square. */
    lua_getfield(L, bench, "setup");
    if (lua_pcall(L, 0, 1, 0) != 0) {
        fprintf(stderr, "FAIL setup: %s\n", lua_tostring(L, -1));
        return 1;
    }
    int ntiles = (int)lua_objlen(L, -1);
    UT_ASSERT(ntiles >= 3);
    lua_rawgeti(L, -1, ntiles / 2 + 1);
    lua_getfield(L, -1, "mx"); int wx = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, -1, "my"); int wy = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_pop(L, 2);
    s_map[wy * 256 + wx] = BDS_T_BUILDING;
    brainPathfinderSetMap(pf, s_map);

    lua_getfield(L, bench, "check");
    if (lua_pcall(L, 0, 4, 0) != 0) {
        fprintf(stderr, "FAIL check: %s\n", lua_tostring(L, -1));
        return 1;
    }
    int pid = (int)lua_tointeger(L, -4);
    const char *why = lua_tostring(L, -3);
    int shots = (int)lua_tointeger(L, -2);
    int nblk = (int)lua_tointeger(L, -1);
    printf("bench_decoy_step_first: tank (%d,%d), pill 1 at (%d,%d), "
           "trace %d tiles, wall at (%d,%d)\n",
           BDS_TANK_X, BDS_TANK_Y, BDS_PILL_X, BDS_PILL_Y, ntiles, wx, wy);
    printf("  check: closest pill %d, why=%s shots=%d blockers=%d\n",
           pid, why ? why : "nil", shots, nblk);
    UT_ASSERT(pid == 1);
    UT_ASSERT(why != NULL && strcmp(why, "last") == 0);
    UT_ASSERT(nblk == 1);
    lua_pop(L, 4);
    (void)luaL_dostring(L, "return jit and jit.version or _VERSION");
    printf("  VM: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);

    printf("  %d calls x %d repeats\n", n, BDS_REPEATS);
    static const char *const parts[] = { "full", "cadence1", "cadence4",
                                         "closest", "blockers", "trace" };
    static const char *const labels[] = {
        "full step-first block (per think)",
        "cadence 1 (every think)",
        "cadence 4 (average per think)",
        "  M.closest_pill",
        "  M.blockers (trace + Lua walk)",
        "  cpf.simulate_shot only (C+tbl)",
    };
    /* Pass 0 as the game runs it (JIT on).  Pass 1 with the JIT off: the
     * interpreter, an upper bound for code that is cold in a real think. */
    for (int pass = 0; pass < 2; pass++) {
    if (pass == 1) {
        (void)luaL_dostring(L, "if jit then jit.off(); jit.flush() end");
        printf("  JIT off (interpreter):\n");
    }
    for (int p = 0; p < 6; p++) {
        double us[BDS_REPEATS];
        for (int r = 0; r < BDS_REPEATS; r++) {
            lua_getfield(L, bench, "run");
            lua_pushstring(L, parts[p]);
            lua_pushinteger(L, n);
            if (lua_pcall(L, 2, 1, 0) != 0) {
                fprintf(stderr, "FAIL run %s: %s\n", parts[p], lua_tostring(L, -1));
                return 1;
            }
            us[r] = lua_tonumber(L, -1) * 1e6 / n;
            lua_pop(L, 1);
        }
        report(labels[p], us, BDS_REPEATS);
    }
    }

    /* The C walk alone: brainPathfinderSimulateShot, no Lua. */
    {
        double us[BDS_REPEATS];
        BrainShotTile tiles[64];
        WORLD ox = (WORLD)((BDS_PILL_X << 8) | 0x80), oy = (WORLD)((BDS_PILL_Y << 8) | 0x80);
        WORLD tx = (WORLD)((BDS_TANK_X << 8) | 0x80), ty = (WORLD)((BDS_TANK_Y << 8) | 0x80);
        volatile int sink = 0;
        for (int r = 0; r < BDS_REPEATS; r++) {
            Uint64 t0 = SDL_GetPerformanceCounter();
            for (int i = 0; i < n; i++) {
                sink += brainPathfinderSimulateShot(pf, ox, oy, tx, ty,
                                                    BRAIN_SHOT_SHOOTER_PILL, 0,
                                                    tiles, 64);
            }
            Uint64 t1 = SDL_GetPerformanceCounter();
            us[r] = (double)(t1 - t0) * 1e6 /
                    (double)SDL_GetPerformanceFrequency() / n;
        }
        report("  brainPathfinderSimulateShot (C)", us, BDS_REPEATS);
        (void)sink;
    }

    lua_close(L);
    brainPathfinderDestroy(pf);
    return 0;
}
