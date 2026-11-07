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

/* A CPU speed test of about 2 s, and the CPU facts that go with it.
 *
 * The test runs CoreMark (src/third_party/coremark) in three steps:
 *   1. a short warm-up on one thread, which also finds how fast one
 *      iteration is;
 *   2. about 0.8 s on one thread;
 *   3. about 0.8 s on every logical core at once, each thread with its own
 *      CoreMark data.
 * Each step checks CoreMark's CRCs against the known values of the
 * performance run (seeds 0, 0, 0x66 and 2000 bytes of data).
 *
 * The scores are CoreMark iterations per second over a short run. They are
 * for comparing machines with each other; they are not certified CoreMark
 * results, which need a run of at least 10 s.
 *
 * Nothing here is sent anywhere. The caller decides what to do with the
 * result. */

#ifndef WINBOLO_CPU_SPEED_TEST_H
#define WINBOLO_CPU_SPEED_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <SDL3/SDL_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CPU_SPEED_NAME_LEN 128

/* How long each step runs. cpuSpeedTestDefaultParams gives the 2 s test. */
typedef struct {
  uint32_t warmIterations; /* Iterations in the warm-up step */
  double phaseSecs;        /* Aimed length of the 1-thread step */
  double allCoreFactor;    /* The all-core step aims at phaseSecs times this */
  uint32_t minIterations;  /* No step runs fewer iterations than this */
  int threads;             /* Threads in the all-core step; 0 = logical cores */
  /* Optional. When it is set and becomes nonzero, the test stops at the end
     of the batch of iterations it is in (a few milliseconds) and returns
     with cancelled set. With it set, each thread runs its iterations in
     batches so it can look at the flag. */
  SDL_AtomicInt *cancel;
  /* The all-core step's threads run at SDL_THREAD_PRIORITY_LOW. The caller
     sets its own thread's priority for the 1-thread steps, which run on the
     calling thread. */
  bool lowPriority;
} CpuSpeedParams;

typedef struct {
  /* The scores, in CoreMark iterations per second */
  double cm1;     /* On one thread */
  double cmAll;   /* On all threads at once, the threads added together */
  double scaling; /* cmAll / cm1 */

  /* The machine */
  int threads;       /* Threads the all-core step ran */
  int logicalCores;  /* SDL_GetNumLogicalCPUCores */
  int physicalCores; /* From the OS; the logical count when it cannot say */
  char cpuName[CPU_SPEED_NAME_LEN]; /* Empty when the OS does not say */

  /* Every CRC of every step matched the known values */
  bool crcOk;
  /* The cancel flag stopped the test; the scores are not to be used */
  bool cancelled;

  /* What each step did */
  uint32_t warmIterations;
  double warmSecs;
  uint32_t oneIterations;
  double oneSecs;
  uint32_t allIterationsPerThread;
  double allSecs;
  double totalSecs; /* Wall time of the whole test */
} CpuSpeedResult;

/* The 2 s test: 3000 warm-up iterations, 0.8 s on one thread, then 0.8 x 0.9
   s worth of iterations on each logical core, and at least 1000 iterations a
   step, no cancel flag and normal priority. */
void cpuSpeedTestDefaultParams(CpuSpeedParams *params);

/* Runs the test and fills result. False when it could not run (no memory,
   or a thread that would not start); result then holds what was found so
   far. A run that finished with a wrong CRC returns true with crcOk false.
   A cancelled run returns false with cancelled set. */
bool cpuSpeedTestRun(const CpuSpeedParams *params, CpuSpeedResult *result);

/* The CPU facts alone, with no timing. They take well under a millisecond. */
int cpuSpeedLogicalCores(void);
int cpuSpeedPhysicalCores(void);
void cpuSpeedCpuName(char *out, size_t outSize);

/* One line for a person to read, with no newline at the end. */
void cpuSpeedFormatLine(const CpuSpeedResult *result, char *out,
                        size_t outSize);

/* One line of JSON, with no newline at the end. The keys match the bench
   script's (cm_1t, cm_all, allcore_scaling and the rest). */
void cpuSpeedFormatJson(const CpuSpeedResult *result, char *out,
                        size_t outSize);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_CPU_SPEED_TEST_H */
