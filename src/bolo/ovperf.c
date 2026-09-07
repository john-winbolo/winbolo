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
 *Filename:      ovperf.c
 *Purpose:
 *  Draw-cost counters for the map overview - see ovperf.h.
 *  Every sample lands in an accumulator holding a running
 *  sum, a count and a maximum. Every OVPERF_DUMP_FRAMES
 *  rendered frames the lot goes out as one line of
 *  ovperf.log and the accumulators start again.
 *
 *  Samples come from more than one thread: the hosted
 *  server's tick from the SDL timer thread, the frame and
 *  pop-out locks and the dump from the main thread. One
 *  mutex of this module's own guards the accumulators. It
 *  is a leaf - nothing is called while it is held - so it
 *  can be taken under the client mutex or the threads mutex
 *  with no ordering to keep. Callers take their timestamps
 *  before calling in, so the mutex is never inside a
 *  measured interval. The one unlocked value is the
 *  per-square stamp counter, bumped and read on a single
 *  thread.
 *********************************************************/

/* Kept outside the switch so the translation unit is never empty: MSVC
 * warns on one at /W4, and the build treats warnings as errors. */
#include <stdint.h>

#include "ovperf.h"

#if WB_OVPERF

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#define OVPERF_DUMP_FRAMES 120
#define OVPERF_LOG_NAME "ovperf.log"

/* Running sum, count and maximum of one quantity over a dump window. */
typedef struct OvStat {
  double sum;
  double max;
  unsigned n;
} OvStat;

/* Everything one dump line reports, reset together. */
typedef struct OvWindow {
  OvStat aMs, aVisited, aDrawn, aCalls;
  OvStat bFrameWait, bFrameHold, bPopWait, bPopHold;
  OvStat cMs, cSquares;
  OvStat dDelta, dMutexWait;
  unsigned frames;
  uint64_t start; /* performance counter when the window opened */
} OvWindow;

static SDL_Mutex *ovMutex = NULL; /* guards everything below except the log */
static OvWindow win;
static uint64_t dPrevEntry = 0; /* last hostedServerTimerCb entry seen */
static bool dHavePrev = false;  /* dPrevEntry holds a real timestamp */

/* Main thread only: written by the dump, never under ovMutex. */
static FILE *logFile = NULL;
static bool logTried = false;

unsigned long ovPerfStampSquares = 0;

/* Made on first use from whichever thread gets there first; a loser of the
 * race throws its own away and takes the winner's. */
static SDL_Mutex *ovPerfMutex(void) {
  SDL_Mutex *m = (SDL_Mutex *)SDL_GetAtomicPointer((void **)&ovMutex);
  if (m == NULL) {
    SDL_Mutex *fresh = SDL_CreateMutex();
    if (SDL_CompareAndSwapAtomicPointer((void **)&ovMutex, NULL, fresh)) {
      m = fresh;
    } else {
      SDL_DestroyMutex(fresh);
      m = (SDL_Mutex *)SDL_GetAtomicPointer((void **)&ovMutex);
    }
  }
  return m;
}

static double ovMs(uint64_t t0, uint64_t t1) {
  return (double)(t1 - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

static void ovStatAdd(OvStat *s, double v) {
  s->sum += v;
  if (s->n == 0 || v > s->max) {
    s->max = v;
  }
  s->n++;
}

/* key=mean/max; 0/0 when the window had no samples. */
static void ovStatPrint(FILE *f, const char *key, const OvStat *s,
                        int decimals) {
  double mean = s->n > 0 ? s->sum / (double)s->n : 0.0;
  fprintf(f, " %s=%.*f/%.*f", key, decimals, mean, decimals, s->max);
}

uint64_t ovPerfNow(void) {
  return SDL_GetPerformanceCounter();
}

void ovPerfTerrain(uint64_t t0, uint64_t t1, int squaresVisited,
                   int squaresDrawn, int renderCalls) {
  double ms = ovMs(t0, t1);
  SDL_Mutex *m = ovPerfMutex();

  SDL_LockMutex(m);
  ovStatAdd(&win.aMs, ms);
  ovStatAdd(&win.aVisited, (double)squaresVisited);
  ovStatAdd(&win.aDrawn, (double)squaresDrawn);
  ovStatAdd(&win.aCalls, (double)renderCalls);
  SDL_UnlockMutex(m);
}

void ovPerfFrameLock(uint64_t waitStart, uint64_t acquired,
                     uint64_t released) {
  double wait = ovMs(waitStart, acquired);
  double hold = ovMs(acquired, released);
  SDL_Mutex *m = ovPerfMutex();

  SDL_LockMutex(m);
  ovStatAdd(&win.bFrameWait, wait);
  ovStatAdd(&win.bFrameHold, hold);
  SDL_UnlockMutex(m);
}

void ovPerfPopoutLock(uint64_t waitStart, uint64_t acquired,
                      uint64_t released) {
  double wait = ovMs(waitStart, acquired);
  double hold = ovMs(acquired, released);
  SDL_Mutex *m = ovPerfMutex();

  SDL_LockMutex(m);
  ovStatAdd(&win.bPopWait, wait);
  ovStatAdd(&win.bPopHold, hold);
  SDL_UnlockMutex(m);
}

void ovPerfStamp(uint64_t t0, uint64_t t1) {
  double ms = ovMs(t0, t1);
  unsigned long squares = ovPerfStampSquares;
  SDL_Mutex *m = ovPerfMutex();

  ovPerfStampSquares = 0;
  SDL_LockMutex(m);
  ovStatAdd(&win.cMs, ms);
  ovStatAdd(&win.cSquares, (double)squares);
  SDL_UnlockMutex(m);
}

void ovPerfServerTick(uint64_t entry, uint64_t mutexWaitStart,
                      uint64_t mutexAcquired) {
  double wait = ovMs(mutexWaitStart, mutexAcquired);
  SDL_Mutex *m = ovPerfMutex();

  SDL_LockMutex(m);
  /* The first call has nothing to measure a delta from. */
  if (dHavePrev) {
    ovStatAdd(&win.dDelta, ovMs(dPrevEntry, entry));
  }
  dPrevEntry = entry;
  dHavePrev = true;
  ovStatAdd(&win.dMutexWait, wait);
  SDL_UnlockMutex(m);
}

/* Opens the log on the first dump and writes the column key once. A failed
 * open is not retried. */
static bool ovLogOpen(void) {
  char *cwd;

  if (logFile != NULL) {
    return true;
  }
  if (logTried) {
    return false;
  }
  logTried = true;
  logFile = fopen(OVPERF_LOG_NAME, "a");
  if (logFile == NULL) {
    return false;
  }
  fprintf(logFile,
          "# OVPERF window=%d rendered frames; x/y = mean/max over the "
          "window, ms unless named otherwise; *_n = samples in the window. "
          "a_* terrain loop per overview render: ms, squares visited, "
          "squares drawn, SDL_RenderTexture calls. b_frame_* client mutex "
          "wait/hold round the main frame; b_pop_* the same round the "
          "pop-out render. c_* overviewMapUpdate ms and squares stamped "
          "per call. d_* ms between hosted server tick entries and ms "
          "waiting for the threads mutex.\n",
          OVPERF_DUMP_FRAMES);
  fflush(logFile);
  cwd = SDL_GetCurrentDirectory();
  SDL_Log("ovperf: writing %s%s", cwd != NULL ? cwd : "", OVPERF_LOG_NAME);
  SDL_free(cwd);
  return true;
}

static void ovDump(const OvWindow *w, uint64_t now) {
  double wallS;

  if (!ovLogOpen()) {
    return;
  }
  wallS = ovMs(w->start, now) / 1000.0;
  fprintf(logFile, "OVPERF frames=%u wall_s=%.2f fps=%.1f", w->frames, wallS,
          wallS > 0.0 ? (double)w->frames / wallS : 0.0);
  ovStatPrint(logFile, "a_ms", &w->aMs, 3);
  ovStatPrint(logFile, "a_visited", &w->aVisited, 0);
  ovStatPrint(logFile, "a_drawn", &w->aDrawn, 0);
  ovStatPrint(logFile, "a_calls", &w->aCalls, 0);
  fprintf(logFile, " a_n=%u", w->aMs.n);
  ovStatPrint(logFile, "b_frame_wait", &w->bFrameWait, 3);
  ovStatPrint(logFile, "b_frame_hold", &w->bFrameHold, 3);
  fprintf(logFile, " b_frame_n=%u", w->bFrameHold.n);
  ovStatPrint(logFile, "b_pop_wait", &w->bPopWait, 3);
  ovStatPrint(logFile, "b_pop_hold", &w->bPopHold, 3);
  fprintf(logFile, " b_pop_n=%u", w->bPopHold.n);
  ovStatPrint(logFile, "c_ms", &w->cMs, 3);
  ovStatPrint(logFile, "c_squares", &w->cSquares, 0);
  fprintf(logFile, " c_n=%u", w->cMs.n);
  ovStatPrint(logFile, "d_delta", &w->dDelta, 3);
  ovStatPrint(logFile, "d_wait", &w->dMutexWait, 3);
  fprintf(logFile, " d_n=%u\n", w->dMutexWait.n);
  fflush(logFile);
}

void ovPerfFrameEnd(void) {
  OvWindow done;
  uint64_t now = SDL_GetPerformanceCounter();
  SDL_Mutex *m = ovPerfMutex();

  /* Copy out and reset under the lock; the file write happens outside it
   * so the timer thread never queues behind an fprintf. */
  SDL_LockMutex(m);
  if (win.start == 0) {
    win.start = now;
  }
  win.frames++;
  if (win.frames < OVPERF_DUMP_FRAMES) {
    SDL_UnlockMutex(m);
    return;
  }
  done = win;
  memset(&win, 0, sizeof(win));
  win.start = now;
  SDL_UnlockMutex(m);

  ovDump(&done, now);
}

#endif /* WB_OVPERF */
