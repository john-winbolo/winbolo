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

/* Lua garbage collection in the free time after a bot's think. See
 * brain_gc.h for the design. */

#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <lua.h>
#include <lauxlib.h>

#include "brain_gc.h"

#ifdef WINBOLO_LUAJIT

typedef struct {
    BrainGcMode mode;
    bool        debugStats;  /* BRAIN_DEBUG_MODE was on when the mode was read */
    bool        inCycle;     /* steps have run since the last cycle end */
    size_t      baseBytes;   /* heap after the last finished cycle */
    uint32_t    ticksSinceCycle;
    size_t      lastHeap;    /* auto mode: heap after the previous think */
    /* The stats window (BRAIN_GC_STATS_EVERY thinks). */
    uint32_t    winThinks, winGcTicks, winCycles, winForced, winFinished;
    uint32_t    winSteps;
    double      winMs, winMaxMs;
    size_t      winHeapMin, winHeapMax;
} BrainGcState;

/* The registry key: the address of this static. */
static const char kBrainGcStateKey = 0;

static size_t brainGcHeapBytes(lua_State *L) {
    return (size_t)lua_gc(L, LUA_GCCOUNT, 0) * 1024u +
           (size_t)lua_gc(L, LUA_GCCOUNTB, 0);
}

static BrainGcState *brainGcGetState(lua_State *L) {
    BrainGcState *s;

    lua_pushlightuserdata(L, (void *)&kBrainGcStateKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    s = (BrainGcState *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (s != NULL) {
        return s;
    }

    /* First call on this state: the brain has opened and thought once, so
     * its choice is set. Read it once. */
    s = (BrainGcState *)lua_newuserdata(L, sizeof(*s));
    memset(s, 0, sizeof(*s));
    lua_pushlightuserdata(L, (void *)&kBrainGcStateKey);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);

    lua_getglobal(L, "BRAIN_GC_MODE");
    {
        const char *m = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
        s->mode = (m != NULL && strcmp(m, "slack") == 0)
                      ? BRAIN_GC_MODE_SLACK : BRAIN_GC_MODE_AUTO;
    }
    lua_pop(L, 1);
    lua_getglobal(L, "BRAIN_DEBUG_MODE");
    s->debugStats = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    s->baseBytes = brainGcHeapBytes(L);
    s->lastHeap  = s->baseBytes;
    if (s->mode == BRAIN_GC_MODE_SLACK) {
        lua_gc(L, LUA_GCSTOP, 0);
        /* The heap size now is wherever the stock collector left it, not a
         * size after a full cycle. Run a cycle in the first free time so the
         * limits rest on a real figure. */
        s->inCycle = true;
    }
    return s;
}

typedef struct {
    Uint64   stopAt;         /* counter value to stop at (unless untilEnd) */
    bool     untilEnd;       /* run until the cycle ends, whatever the time */
    uint32_t steps;
    bool     cycleEnd;
} BrainGcStepRun;

/* Under lua_cpcall: a step can run a __gc finalizer, and an error raised by
 * one must not leave through a C frame with no protected call around it. */
static int brainGcStepLoop(lua_State *L) {
    BrainGcStepRun *r = (BrainGcStepRun *)lua_touserdata(L, 1);
    for (;;) {
        int end = lua_gc(L, LUA_GCSTEP, BRAIN_GC_STEP_KB);
        r->steps++;
        if (end) {
            r->cycleEnd = true;
            break;
        }
        if (!r->untilEnd && SDL_GetPerformanceCounter() >= r->stopAt) {
            break;
        }
    }
    return 0;
}

static void brainGcStats(BrainGcState *s, int playerNum, size_t heap,
                         double ms, bool ran, bool cycleEnd, bool forced,
                         bool finished, uint32_t steps) {
    if (s->winThinks == 0) {
        s->winHeapMin = heap;
        s->winHeapMax = heap;
    }
    s->winThinks++;
    if (heap < s->winHeapMin) s->winHeapMin = heap;
    if (heap > s->winHeapMax) s->winHeapMax = heap;
    if (ran) s->winGcTicks++;
    if (cycleEnd) s->winCycles++;
    if (forced) s->winForced++;
    if (finished) s->winFinished++;
    s->winSteps += steps;
    s->winMs += ms;
    if (ms > s->winMaxMs) s->winMaxMs = ms;

    if (s->winThinks < BRAIN_GC_STATS_EVERY) {
        return;
    }
    if (s->debugStats) {
        if (s->mode == BRAIN_GC_MODE_SLACK) {
            fprintf(stderr,
                    "brain gc: player %d mode=slack thinks=%u gc_ticks=%u "
                    "steps=%u cycles=%u forced=%u finished=%u "
                    "gc_ms_total=%.1f gc_ms_per_think=%.3f gc_ms_max=%.2f "
                    "heap_kb_min=%u heap_kb_max=%u base_kb=%u\n",
                    playerNum, s->winThinks, s->winGcTicks, s->winSteps,
                    s->winCycles, s->winForced, s->winFinished, s->winMs,
                    s->winMs / (double)s->winThinks, s->winMaxMs,
                    (unsigned)(s->winHeapMin / 1024u),
                    (unsigned)(s->winHeapMax / 1024u),
                    (unsigned)(s->baseBytes / 1024u));
        } else {
            /* The stock collector runs inside the think, out of sight; a
             * heap drop between two thinks is the sign a cycle ended. */
            fprintf(stderr,
                    "brain gc: player %d mode=auto thinks=%u heap_drops=%u "
                    "heap_kb_min=%u heap_kb_max=%u\n",
                    playerNum, s->winThinks, s->winCycles,
                    (unsigned)(s->winHeapMin / 1024u),
                    (unsigned)(s->winHeapMax / 1024u));
        }
    }
    s->winThinks = s->winGcTicks = s->winCycles = 0;
    s->winForced = s->winFinished = s->winSteps = 0;
    s->winMs = s->winMaxMs = 0.0;
}

void brainGcAfterThink(lua_State *L, uint64_t deadlineCounter,
                       int playerNum, BrainGcResult *out) {
    BrainGcState  *s;
    BrainGcStepRun run;
    size_t heap;
    bool   ran = false, forced = false, finished = false;
    double ms = 0.0;

    if (out != NULL) memset(out, 0, sizeof(*out));
    if (L == NULL) return;

    s    = brainGcGetState(L);
    heap = brainGcHeapBytes(L);

    if (s->mode != BRAIN_GC_MODE_SLACK) {
        bool drop = heap < s->lastHeap;
        s->lastHeap = heap;
        brainGcStats(s, playerNum, heap, 0.0, false, drop, false, false, 0);
        if (out != NULL) {
            out->mode      = s->mode;
            out->heapBytes = heap;
            out->baseBytes = s->baseBytes;
        }
        return;
    }

    s->ticksSinceCycle++;
    /* Something else ran a full collection (a memory report, the brain's
     * own collectgarbage("collect")): the heap is now below the figure the
     * limits rest on, so take the lower one. */
    if (!s->inCycle && heap < s->baseBytes) {
        s->baseBytes = heap;
    }

    memset(&run, 0, sizeof(run));
    {
        size_t base     = s->baseBytes;
        size_t startAt  = base / 100u * BRAIN_GC_START_PCT;
        size_t forceAt  = base / 100u * BRAIN_GC_FORCE_PCT + BRAIN_GC_FORCE_FLOOR;
        size_t finishAt = base / 100u * BRAIN_GC_FINISH_PCT + BRAIN_GC_FINISH_FLOOR;
        Uint64 freq     = SDL_GetPerformanceFrequency();
        Uint64 now      = SDL_GetPerformanceCounter();
        Uint64 margin   = (Uint64)(BRAIN_GC_MARGIN_MS * (double)freq / 1000.0);
        Uint64 slackEnd = deadlineCounter > margin ? deadlineCounter - margin : 0;
        bool   want     = s->inCycle || heap >= startAt;

        if (heap >= finishAt) {
            run.untilEnd = true;
            ran = finished = forced = true;
        } else if (want && (heap >= forceAt ||
                            s->ticksSinceCycle >= BRAIN_GC_FORCE_TICKS)) {
            Uint64 cap = now + (Uint64)(BRAIN_GC_FORCE_MS * (double)freq / 1000.0);
            run.stopAt = slackEnd > cap ? slackEnd : cap;
            ran = forced = true;
        } else if (want && now < slackEnd) {
            run.stopAt = slackEnd;
            ran = true;
        }

        if (ran) {
            int rc = lua_cpcall(L, brainGcStepLoop, &run);
            if (rc != 0) {
                /* A finalizer raised. The collector state is still sound;
                 * drop the message and go on next think. */
                lua_pop(L, 1);
            }
            ms = (double)(SDL_GetPerformanceCounter() - now) * 1000.0 /
                 (double)freq;
            if (run.cycleEnd) {
                s->inCycle = false;
                s->ticksSinceCycle = 0;
                s->baseBytes = brainGcHeapBytes(L);
            } else {
                s->inCycle = true;
            }
            heap = brainGcHeapBytes(L);
        }
    }

    /* Every step (and any collectgarbage call the brain made) sets the
     * collector's threshold again, which would let it run inside the next
     * think. Stop it on every call. */
    lua_gc(L, LUA_GCSTOP, 0);

    brainGcStats(s, playerNum, heap, ms, ran, run.cycleEnd, forced, finished,
                 run.steps);
    if (out != NULL) {
        out->mode      = s->mode;
        out->ms        = ms;
        out->steps     = run.steps;
        out->cycleEnd  = run.cycleEnd;
        out->forced    = forced;
        out->finished  = finished;
        out->heapBytes = heap;
        out->baseBytes = s->baseBytes;
    }
}

#else /* !WINBOLO_LUAJIT */

void brainGcAfterThink(struct lua_State *L, uint64_t deadlineCounter,
                       int playerNum, BrainGcResult *out) {
    (void)L;
    (void)deadlineCounter;
    (void)playerNum;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->mode = BRAIN_GC_MODE_AUTO;
    }
}

#endif /* WINBOLO_LUAJIT */
