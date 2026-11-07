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

/* The stored CPU speed test result, and the test run in the background.
 *
 * One result is kept per machine and WinBolo version, in a small JSON file
 * with the other per-user state (SDL_GetPrefPath, cpu_speed.json). The
 * environment variable WINBOLO_CPU_SPEED_FILE, or cpuSpeedStoreSetPath, puts
 * it somewhere else; the tests use a scratch file. A stored result for
 * another version, another CPU or another core count is not used, so the
 * test runs again after an update or on a new machine. Nothing in the file
 * is sent anywhere. */

#ifndef WINBOLO_CPU_SPEED_STORE_H
#define WINBOLO_CPU_SPEED_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "cpu_speed_test.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CPU_SPEED_VERSION_LEN 32
#define CPU_SPEED_CONTEXT_LEN 32

/* Where the test ran, as stored in "context" */
#define CPU_SPEED_CONTEXT_CPUTEST "cputest" /* -cputest, a quiet machine */
#define CPU_SPEED_CONTEXT_LOBBY   "lobby"   /* the client, in a game lobby */
#define CPU_SPEED_CONTEXT_SERVER  "server"  /* the dedicated server, in its lobby */

typedef struct {
  CpuSpeedResult result;
  char version[CPU_SPEED_VERSION_LEN];
  char context[CPU_SPEED_CONTEXT_LEN];
} CpuSpeedStored;

/* The file the result is kept in. NULL or "" goes back to the default
   (WINBOLO_CPU_SPEED_FILE when set, else the per-user directory). */
void cpuSpeedStoreSetPath(const char *path);
/* The path in use, or false when there is none (no per-user directory). */
bool cpuSpeedStorePath(char *out, size_t outSize);

/* The stored result for this version on this machine. False when there is
   none, or the one stored is for another version, CPU or core count. */
bool cpuSpeedStoreLoad(const char *version, CpuSpeedStored *out);

/* Stores result as the one for version. Only a finished run with every CRC
   right is stored. */
bool cpuSpeedStoreSave(const char *version, const char *context,
                       const CpuSpeedResult *result);

/* What the background run tells its caller: one line per call, with no
   newline. Called on the test's own thread. */
typedef void (*CpuSpeedLogFn)(const char *line);

/* Starts the test on a thread of its own at low priority, when no result is
   stored for version and no run is going. Returns at once; true when a run
   was started. A run that finishes stores its result with context; a
   cancelled run stores nothing. log may be NULL. */
bool cpuSpeedBackgroundStart(const char *version, const char *context,
                             CpuSpeedLogFn log);
/* Asks a run that is going to stop. Does not wait for it. */
void cpuSpeedBackgroundCancel(void);
/* A run is going. */
bool cpuSpeedBackgroundRunning(void);
/* Waits up to timeoutMs for a run to end, and says whether it has. For
   tests and for shutting down. */
bool cpuSpeedBackgroundWait(int timeoutMs);
/* Changes the steps a background run makes (the tests use a short run).
   NULL goes back to the 2 s test. Only read when a run starts. */
void cpuSpeedBackgroundSetParams(const CpuSpeedParams *params);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_CPU_SPEED_STORE_H */
