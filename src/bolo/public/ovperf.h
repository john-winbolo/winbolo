/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 *Name:          Overview Perf
 *Filename:      ovperf.h
 *Purpose:
 *  Draw-cost counters for the map overview. Four
 *  measurements, each kept as a mean and a maximum over a
 *  window of rendered frames and appended to ovperf.log in
 *  the working directory:
 *    A - the overview's terrain loop: time, squares
 *        visited, squares drawn, render calls issued;
 *    B - the client mutex: time waited for and time held
 *        round the main frame, and round the pop-out
 *        render;
 *    C - the stamp pass: overviewMapUpdate's time and the
 *        squares it rewrote;
 *    D - the hosted server's tick: time between entries
 *        and time waited for the threads mutex.
 *
 *  Built only when WB_OVPERF is 1. Every call site sits in
 *  its own #if WB_OVPERF block, so at 0 the switch leaves
 *  nothing behind in any translation unit.
 *
 *  Public leaf: the sim (C) and the frontend (A, B, D)
 *  both include it, and it includes nothing of either.
 *********************************************************/

#ifndef OVPERF_H
#define OVPERF_H

/* Set to 1 to build the map-overview draw-cost counters. */
#define WB_OVPERF 0

#if WB_OVPERF

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SDL_GetPerformanceCounter. Read the timestamps first, then hand them to
 * the accumulator, so its lock never sits inside a measured interval. */
uint64_t ovPerfNow(void);

/* A - terrain loop. One call per overviewViewDrawTerrain, bracketing the
 * double loop only. */
void ovPerfTerrain(uint64_t t0, uint64_t t1, int squaresVisited,
                   int squaresDrawn, int renderCalls);

/* B - lock hold. waitStart before the wait, acquired when it returns,
 * released after the release. */
void ovPerfFrameLock(uint64_t waitStart, uint64_t acquired, uint64_t released);
void ovPerfPopoutLock(uint64_t waitStart, uint64_t acquired, uint64_t released);

/* C - stamp pass. ovPerfStampSquares is bumped once per square by the
 * stamp loop and folded in by ovPerfStamp, which zeroes it again. It takes
 * no lock: the stamp loop and ovPerfStamp both run on the thread that
 * ticks the client, and nothing else touches it. */
extern unsigned long ovPerfStampSquares;
void ovPerfStamp(uint64_t t0, uint64_t t1);

/* D - hosted server tick. entry from the top of the callback, the other
 * two either side of threadsWaitForMutex. The delta between consecutive
 * entries is worked out here from the previous call's entry. */
void ovPerfServerTick(uint64_t entry, uint64_t mutexWaitStart,
                      uint64_t mutexAcquired);

/* Count a rendered frame. Every OVPERF_DUMP_FRAMES of them, write the
 * accumulated line and reset. Call once per frame, outside any lock. */
void ovPerfFrameEnd(void);

#ifdef __cplusplus
}
#endif

#endif /* WB_OVPERF */

#endif /* OVPERF_H */
