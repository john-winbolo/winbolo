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

/* Lua garbage collection in the free time after a bot's think.
 *
 * With the stock collector, LuaJIT collects while the brain allocates, so
 * collection work lands inside the think and counts against its budget. A
 * cycle end costs about 3 ms, and most over-budget thinks had one in them.
 *
 * A brain that sets the global BRAIN_GC_MODE = "slack" (GoalHunter does, from
 * C.GC_MODE) gets this instead: the host stops the automatic collector for
 * that bot's lua_State, and after each think it runs small collector steps in
 * the time left before the bot's think deadline (less a margin). A tick with
 * no time left runs no collection; the next cheap tick goes on from where the
 * collector stopped. If the heap grows too far past its size after the last
 * full cycle, steps run even without free time, so memory cannot grow without
 * end. Any other value (or no global) leaves the stock collector alone: the
 * call only samples the heap for the stats line.
 *
 * The state for each bot lives in its lua_State's registry, so it starts
 * fresh with each new state and needs no field in any host struct. Only the
 * thread that runs the bot's thinks may call this. On a PUC-Lua build the
 * call does nothing. */

#ifndef BRAIN_GC_H
#define BRAIN_GC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct lua_State;

/* Time kept back from the deadline: collection stops this long before it. */
#define BRAIN_GC_MARGIN_MS        2.0
/* KB argument of each lua_gc(LUA_GCSTEP) call. About KB + 1 basic collector
 * steps; measured well under 0.1 ms on a 20 MB brain heap
 * (brain_gc_slack_step_time). */
#define BRAIN_GC_STEP_KB          8
/* A new cycle starts (in free time only) once the heap reaches this percent
 * of its size after the last full cycle. */
#define BRAIN_GC_START_PCT        125
/* Past base * FORCE_PCT / 100 + FORCE_FLOOR, steps run even with no free
 * time, at most BRAIN_GC_FORCE_MS per think. The same applies when no cycle
 * has finished for BRAIN_GC_FORCE_TICKS thinks and the heap is past the start
 * line. */
#define BRAIN_GC_FORCE_PCT        150
#define BRAIN_GC_FORCE_FLOOR      (4u * 1024u * 1024u)
#define BRAIN_GC_FORCE_MS         1.0
#define BRAIN_GC_FORCE_TICKS      1500u
/* Past base * FINISH_PCT / 100 + FINISH_FLOOR, the cycle is finished in this
 * call whatever it costs. The last line of defence for memory. */
#define BRAIN_GC_FINISH_PCT       200
#define BRAIN_GC_FINISH_FLOOR     (8u * 1024u * 1024u)
/* Under BRAIN_DEBUG_MODE, one stats line on stderr every this many thinks. */
#define BRAIN_GC_STATS_EVERY      3000u

typedef enum {
    BRAIN_GC_MODE_UNREAD = 0,  /* the brain's choice is not read yet */
    BRAIN_GC_MODE_AUTO,        /* stock collector; the call only samples */
    BRAIN_GC_MODE_SLACK        /* collector stopped; steps after the think */
} BrainGcMode;

/* What one brainGcAfterThink call did. */
typedef struct {
    BrainGcMode mode;
    double      ms;          /* time in collector steps this call */
    uint32_t    steps;       /* lua_gc(LUA_GCSTEP) calls */
    bool        cycleEnd;    /* a cycle finished in this call */
    bool        forced;      /* steps ran because the heap was past a limit */
    bool        finished;    /* the FINISH limit made this call end a cycle */
    size_t      heapBytes;   /* heap after this call */
    size_t      baseBytes;   /* heap after the last finished cycle */
} BrainGcResult;

/* Run after a think, outside the think's budget hook and outside
 * lastThinkMs. deadlineCounter is the bot's think deadline in
 * SDL_GetPerformanceCounter units; collection in free time ends
 * BRAIN_GC_MARGIN_MS before it. playerNum is only for the stats line.
 * out may be NULL. */
void brainGcAfterThink(struct lua_State *L, uint64_t deadlineCounter,
                       int playerNum, BrainGcResult *out);

#endif /* BRAIN_GC_H */
