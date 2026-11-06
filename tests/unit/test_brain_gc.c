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

/* Lua collection after the think (src/bolo/brain_gc.c), on a bare lua_State
 * with a heap built to look like a brain's: a live part that stays and a
 * stream of garbage, as each think makes.
 *
 *   brain_gc_auto_leaves_collector       - no BRAIN_GC_MODE: no steps, the
 *                                          stock collector still runs
 *   brain_gc_slack_steps_in_free_time    - steps run in free time, the
 *                                          collector stays stopped, a cycle
 *                                          ends and the heap shrinks
 *   brain_gc_slack_no_free_time_no_steps - past the deadline and under the
 *                                          limits: no collection at all
 *   brain_gc_slack_heap_limits           - past the force limit steps run
 *                                          with no free time, bounded; past
 *                                          the finish limit the cycle ends
 *   brain_gc_slack_step_time             - one step on a 20 MB heap is well
 *                                          under 1 ms (prints the figures) */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "brain_gc.h"
#include "test_harness.h"

#ifdef WINBOLO_LUAJIT

static size_t bgHeap(lua_State *L) {
    return (size_t)lua_gc(L, LUA_GCCOUNT, 0) * 1024u +
           (size_t)lua_gc(L, LUA_GCCOUNTB, 0);
}

static uint64_t bgCounterIn(double ms) {
    return SDL_GetPerformanceCounter() +
           (uint64_t)(ms * (double)SDL_GetPerformanceFrequency() / 1000.0);
}

/* A state with `liveMb` of tables that stay reachable (global LIVE) and a
 * global function MAKE_GARBAGE(kb) that allocates about kb KB that nothing
 * keeps. mode NULL leaves BRAIN_GC_MODE unset. */
static lua_State *bgState(const char *mode, int liveMb) {
    char chunk[1024];
    lua_State *L = luaL_newstate();
    if (L == NULL) return NULL;
    luaL_openlibs(L);
    if (mode != NULL) {
        lua_pushstring(L, mode);
        lua_setglobal(L, "BRAIN_GC_MODE");
    }
    snprintf(chunk, sizeof(chunk),
        "LIVE = {}\n"
        "local n = %d * 1024 * 1024 / 64\n"
        "for i = 1, n do LIVE[i] = { i, i + 1, tostring(i) } end\n"
        "function MAKE_GARBAGE(kb)\n"
        "  local t\n"
        "  for i = 1, kb * 16 do t = { i, i } end\n"
        "  return t\n"
        "end\n", liveMb);
    if (luaL_dostring(L, chunk) != 0) {
        fprintf(stderr, "bgState: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return NULL;
    }
    return L;
}

static bool bgGarbage(lua_State *L, int kb) {
    lua_getglobal(L, "MAKE_GARBAGE");
    lua_pushinteger(L, kb);
    if (lua_pcall(L, 1, 0, 0) != 0) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

/* Call after "thinks" with a large deadline until a cycle ends. */
static bool bgFinishCycle(lua_State *L, BrainGcResult *r) {
    for (int i = 0; i < 1000; i++) {
        brainGcAfterThink(L, bgCounterIn(50.0), 0, r);
        if (r->cycleEnd) return true;
    }
    return false;
}

int run_brain_gc_auto_leaves_collector(void) {
    int rc = 0;
    BrainGcResult r;
    lua_State *L = bgState(NULL, 2);
    UT_ASSERT(L != NULL);

    UT_CHECK(bgGarbage(L, 512));
    brainGcAfterThink(L, bgCounterIn(50.0), 0, &r);
    UT_CHECK(r.mode == BRAIN_GC_MODE_AUTO);
    UT_CHECK(r.steps == 0);
    UT_CHECK(r.ms == 0.0);
    UT_CHECK_MSG(lua_gc(L, LUA_GCISRUNNING, 0) != 0,
                 "auto mode stopped the stock collector");
    /* "auto" spelled out is the same as no global. */
    lua_close(L);
    L = bgState("auto", 2);
    UT_CHECK(L != NULL);
    brainGcAfterThink(L, bgCounterIn(50.0), 0, &r);
    UT_CHECK(r.mode == BRAIN_GC_MODE_AUTO);
    UT_CHECK(lua_gc(L, LUA_GCISRUNNING, 0) != 0);
done:
    if (L != NULL) lua_close(L);
    return rc;
}

int run_brain_gc_slack_steps_in_free_time(void) {
    int rc = 0;
    BrainGcResult r;
    size_t before;
    lua_State *L = bgState("slack", 8);
    UT_ASSERT(L != NULL);

    /* The first call reads the mode, stops the collector and starts a
     * cycle; with 50 ms of free time it runs steps. */
    UT_CHECK(bgGarbage(L, 4096));
    brainGcAfterThink(L, bgCounterIn(50.0), 0, &r);
    UT_CHECK(r.mode == BRAIN_GC_MODE_SLACK);
    UT_CHECK(r.steps > 0);
    UT_CHECK(!r.forced);
    UT_CHECK_MSG(lua_gc(L, LUA_GCISRUNNING, 0) == 0,
                 "the collector runs again after the steps");
    UT_CHECK(bgFinishCycle(L, &r));
    UT_CHECK(lua_gc(L, LUA_GCISRUNNING, 0) == 0);

    /* Garbage made with the collector stopped stays until a cycle in free
     * time takes it. Allocating far less than the start line runs nothing. */
    before = bgHeap(L);
    UT_CHECK(bgGarbage(L, 64));
    brainGcAfterThink(L, bgCounterIn(50.0), 0, &r);
    UT_CHECK_MSG(r.steps == 0, "a cycle started under the start line "
                 "(heap %zu base %zu)", r.heapBytes, r.baseBytes);
    UT_CHECK(bgGarbage(L, (int)(r.baseBytes / 1024u / 2u)));
    UT_CHECK(bgHeap(L) > before);
    before = bgHeap(L);
    UT_CHECK(bgFinishCycle(L, &r));
    UT_CHECK_MSG(r.heapBytes < before, "heap %zu not below %zu after a cycle",
                 r.heapBytes, before);
    UT_CHECK(lua_gc(L, LUA_GCISRUNNING, 0) == 0);

    /* The brain calling collectgarbage("collect") sets the threshold again;
     * the next call stops the collector once more. */
    UT_CHECK(luaL_dostring(L, "collectgarbage('collect')") == 0);
    UT_CHECK(lua_gc(L, LUA_GCISRUNNING, 0) != 0);
    brainGcAfterThink(L, bgCounterIn(50.0), 0, &r);
    UT_CHECK(lua_gc(L, LUA_GCISRUNNING, 0) == 0);
done:
    lua_close(L);
    return rc;
}

int run_brain_gc_slack_no_free_time_no_steps(void) {
    int rc = 0;
    BrainGcResult r;
    lua_State *L = bgState("slack", 4);
    UT_ASSERT(L != NULL);

    brainGcAfterThink(L, bgCounterIn(50.0), 0, &r);
    UT_CHECK(bgFinishCycle(L, &r));

    /* A deadline 1 ms away is inside the 2 ms margin: no free time. A cycle
     * already under way waits too. */
    UT_CHECK(bgGarbage(L, (int)(r.baseBytes / 1024u / 2u)));
    for (int i = 0; i < 20; i++) {
        brainGcAfterThink(L, bgCounterIn(1.0), 0, &r);
        UT_CHECK_MSG(r.steps == 0 && r.ms == 0.0,
                     "think %d: %u steps with no free time", i, r.steps);
    }
    /* The next cheap tick does the work. */
    UT_CHECK(bgFinishCycle(L, &r));
done:
    lua_close(L);
    return rc;
}

int run_brain_gc_slack_heap_limits(void) {
    int rc = 0;
    BrainGcResult r;
    size_t base, forceAt, finishAt;
    bool sawForced = false;
    lua_State *L = bgState("slack", 8);
    UT_ASSERT(L != NULL);

    brainGcAfterThink(L, bgCounterIn(50.0), 0, &r);
    UT_CHECK(bgFinishCycle(L, &r));
    base     = r.baseBytes;
    forceAt  = base / 100u * BRAIN_GC_FORCE_PCT + BRAIN_GC_FORCE_FLOOR;
    finishAt = base / 100u * BRAIN_GC_FINISH_PCT + BRAIN_GC_FINISH_FLOOR;

    /* Past the force line with the deadline already gone: steps run, each
     * call bounded near BRAIN_GC_FORCE_MS. */
    UT_CHECK(bgGarbage(L, (int)((forceAt - bgHeap(L)) / 1024u) + 256));
    UT_CHECK(bgHeap(L) >= forceAt && bgHeap(L) < finishAt);
    r.cycleEnd = false;
    for (int i = 0; i < 2000 && !r.cycleEnd; i++) {
        brainGcAfterThink(L, 0, 0, &r);
        if (r.cycleEnd) break;
        UT_CHECK(r.forced && !r.finished);
        UT_CHECK(r.steps > 0);
        /* One step past the line at most; a loaded test machine may be
         * slower, so the bound is loose. */
        UT_CHECK_MSG(r.ms < BRAIN_GC_FORCE_MS + 5.0, "forced call took %.2f ms",
                     r.ms);
        sawForced = true;
    }
    UT_CHECK(sawForced);
    UT_CHECK(r.cycleEnd);
    UT_CHECK(lua_gc(L, LUA_GCISRUNNING, 0) == 0);

    /* Past the finish line: one call ends the cycle whatever it costs. */
    base     = r.baseBytes;
    finishAt = base / 100u * BRAIN_GC_FINISH_PCT + BRAIN_GC_FINISH_FLOOR;
    UT_CHECK(bgGarbage(L, (int)((finishAt - bgHeap(L)) / 1024u) + 256));
    brainGcAfterThink(L, 0, 0, &r);
    UT_CHECK(r.finished);
    UT_CHECK(r.cycleEnd);
    UT_CHECK(r.heapBytes < finishAt);
    UT_CHECK(lua_gc(L, LUA_GCISRUNNING, 0) == 0);
done:
    lua_close(L);
    return rc;
}

int run_brain_gc_slack_step_time(void) {
    int rc = 0;
    double times[4096];
    int n = 0;
    double freq = (double)SDL_GetPerformanceFrequency();
    lua_State *L = bgState("slack", 20);
    UT_ASSERT(L != NULL);
    lua_gc(L, LUA_GCSETSTEPMUL, 400);   /* GoalHunter's C.GC_STEPMUL */
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCSTOP, 0);
    UT_CHECK(bgGarbage(L, 8192));

    /* Time each lua_gc(LUA_GCSTEP, BRAIN_GC_STEP_KB) through a whole cycle. */
    for (;;) {
        uint64_t t0 = SDL_GetPerformanceCounter();
        int end = lua_gc(L, LUA_GCSTEP, BRAIN_GC_STEP_KB);
        uint64_t t1 = SDL_GetPerformanceCounter();
        if (n < (int)(sizeof(times) / sizeof(times[0]))) {
            times[n++] = (double)(t1 - t0) * 1000.0 / freq;
        }
        if (end) break;
    }
    lua_gc(L, LUA_GCSTOP, 0);
    {
        double sum = 0.0, mx = 0.0;
        int over = 0;
        for (int i = 0; i < n; i++) {
            sum += times[i];
            if (times[i] > mx) mx = times[i];
            if (times[i] > 0.25) over++;
        }
        fprintf(stderr, "brain_gc_slack_step_time: heap %zu KB, %d steps of "
                "%d KB, cycle %.2f ms, step mean %.4f ms max %.3f ms, "
                "%d over 0.25 ms\n", bgHeap(L) / 1024u, n, BRAIN_GC_STEP_KB,
                sum, sum / n, mx, over);
        UT_CHECK_MSG(sum / n < 0.25, "mean step %.3f ms", sum / n);
    }
done:
    lua_close(L);
    return rc;
}

#else /* PUC-Lua: the module does nothing, and says so. */

int run_brain_gc_auto_leaves_collector(void) {
    BrainGcResult r;
    lua_State *L = luaL_newstate();
    UT_ASSERT(L != NULL);
    brainGcAfterThink(L, 0, 0, &r);
    UT_ASSERT(r.mode == BRAIN_GC_MODE_AUTO && r.steps == 0);
    lua_close(L);
    return 0;
}
int run_brain_gc_slack_steps_in_free_time(void)    { return 0; }
int run_brain_gc_slack_no_free_time_no_steps(void) { return 0; }
int run_brain_gc_slack_heap_limits(void)           { return 0; }
int run_brain_gc_slack_step_time(void)             { return 0; }

#endif
