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

/*********************************************************
 *Name:          Brain Capacity
 *Filename:      brain_capacity.c
 *Author:        John Morrison
 *Purpose:
 *  The machine facts given to brains, the default and
 *  the clamp of each brain's capacity answer, and the
 *  answers kept by brain name and version. See
 *  brain_capacity.h.
 *********************************************************/

#include "brain_capacity.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

#include "bot_worker_pool.h"

#define BRAIN_CAPACITY_KEY_LEN 192
#define BRAIN_CAPACITY_CACHE_MAX 64
#define BRAIN_CAPACITY_C_MAX 16

typedef struct {
  char key[BRAIN_CAPACITY_KEY_LEN];
  int value;
} BrainCapacityEntry;

typedef struct {
  char name[BRAIN_CAPACITY_KEY_LEN];
  BrainCapacityFn fn;
} BrainCapacityCEntry;

/* Every field below is under s_lock. The lock is made on first use; brains
   load on the main thread and on the bot threads. */
static SDL_SpinLock s_lock = 0;
static bool s_haveMachine = false;
static BrainMachineInfo s_machine;
static BrainCapacityEntry s_cache[BRAIN_CAPACITY_CACHE_MAX];
static int s_cacheCount = 0;
static BrainCapacityCEntry s_cBrains[BRAIN_CAPACITY_C_MAX];
static int s_cBrainCount = 0;

static void capacityKey(char *out, size_t outSize, const char *name,
                        const char *version) {
  SDL_snprintf(out, outSize, "%s@%s", name != NULL ? name : "",
               version != NULL ? version : "");
}

static int clampCapacity(int value) {
  if (value < BRAIN_CAPACITY_MIN) {
    return BRAIN_CAPACITY_MIN;
  }
  if (value > BRAIN_CAPACITY_MAX) {
    return BRAIN_CAPACITY_MAX;
  }
  return value;
}

void brainCapacitySetMachine(const BrainMachineInfo *machine) {
  SDL_LockSpinlock(&s_lock);
  if (machine == NULL) {
    s_haveMachine = false;
    memset(&s_machine, 0, sizeof(s_machine));
  } else {
    s_machine = *machine;
    s_machine.cpuName[sizeof(s_machine.cpuName) - 1] = '\0';
    s_machine.version[sizeof(s_machine.version) - 1] = '\0';
    s_haveMachine = true;
  }
  SDL_UnlockSpinlock(&s_lock);
}

void brainCapacityGetMachine(BrainMachineInfo *out) {
  bool have;
  int seconds = -1;
  int percent = -1;

  SDL_LockSpinlock(&s_lock);
  have = s_haveMachine;
  if (have) {
    *out = s_machine;
  }
  SDL_UnlockSpinlock(&s_lock);
  if (have) {
    return;
  }

  /* The cheap facts. No speed test: its scores stay 0. */
  memset(out, 0, sizeof(*out));
  out->logicalCores = SDL_GetNumLogicalCPUCores();
  if (out->logicalCores < 1) {
    out->logicalCores = 1;
  }
  out->physicalCores = out->logicalCores;
  out->workerThreads = botWorkerPoolGetSize();
  out->onBattery = SDL_GetPowerInfo(&seconds, &percent) == SDL_POWERSTATE_ON_BATTERY;
#ifdef WINBOLO_VERSION
  SDL_strlcpy(out->version, WINBOLO_VERSION, sizeof(out->version));
#endif
}

void brainCapacitySetMachineFromTest(const char *cpuName, int logicalCores,
                                     int physicalCores, double coremark1t,
                                     double coremarkAll) {
  BrainMachineInfo m;

  brainCapacitySetMachine(NULL);
  brainCapacityGetMachine(&m);
  if (cpuName != NULL) {
    SDL_strlcpy(m.cpuName, cpuName, sizeof(m.cpuName));
  }
  if (logicalCores > 0) {
    m.logicalCores = logicalCores;
  }
  m.physicalCores = physicalCores > 0 ? physicalCores : m.logicalCores;
  if (m.physicalCores > m.logicalCores) {
    m.physicalCores = m.logicalCores;
  }
  m.coremark1t = coremark1t > 0.0 ? coremark1t : 0.0;
  m.coremarkAll = coremarkAll > 0.0 ? coremarkAll : 0.0;
  m.scaling = m.coremark1t > 0.0 ? m.coremarkAll / m.coremark1t : 0.0;
  brainCapacitySetMachine(&m);
  brainCapacityClear();
}

int brainCapacityDefault(const BrainMachineInfo *machine) {
  int logical = machine != NULL ? machine->logicalCores : 0;
  int value = logical - 1;

  if (value < 1) {
    value = 1;
  }
  return clampCapacity(value);
}

int brainCapacityFromAnswer(bool ok, double value,
                            const BrainMachineInfo *machine) {
  if (ok == false || isfinite(value) == 0) {
    return brainCapacityDefault(machine);
  }
  if (value <= (double)BRAIN_CAPACITY_MIN) {
    return BRAIN_CAPACITY_MIN;
  }
  if (value >= (double)BRAIN_CAPACITY_MAX) {
    return BRAIN_CAPACITY_MAX;
  }
  return clampCapacity((int)floor(value));
}

void brainCapacityNameFromPath(const char *path, char *out, size_t outSize) {
  const char *end;
  const char *start;
  size_t len;

  if (out == NULL || outSize == 0) {
    return;
  }
  out[0] = '\0';
  if (path == NULL || path[0] == '\0') {
    return;
  }
  end = path + strlen(path);
  /* A path to a directory: drop the trailing separators. */
  while (end > path && (end[-1] == '/' || end[-1] == '\\')) {
    end--;
  }
  start = end;
  while (start > path && start[-1] != '/' && start[-1] != '\\') {
    start--;
  }
  len = (size_t)(end - start);
  if (len > 4 && SDL_strncasecmp(end - 4, ".lua", 4) == 0) {
    if (start == path) {
      /* A bare file name: the name is the file's own. */
      end -= 4;
    } else {
      /* A file in the brain's directory: the name is the directory's. */
      end = start - 1;
      start = end;
      while (start > path && start[-1] != '/' && start[-1] != '\\') {
        start--;
      }
    }
  }
  /* The production copy sits in opt/ under the brain's own directory. */
  if ((size_t)(end - start) == 3 && SDL_strncasecmp(start, "opt", 3) == 0 &&
      start > path) {
    end = start - 1;
    start = end;
    while (start > path && start[-1] != '/' && start[-1] != '\\') {
      start--;
    }
  }
  len = (size_t)(end - start);
  if (len >= outSize) {
    len = outSize - 1;
  }
  memcpy(out, start, len);
  out[len] = '\0';
}

int brainCapacityLookup(const char *name, const char *version) {
  char key[BRAIN_CAPACITY_KEY_LEN];
  int value = BRAIN_CAPACITY_UNKNOWN;

  capacityKey(key, sizeof(key), name, version);
  SDL_LockSpinlock(&s_lock);
  for (int i = 0; i < s_cacheCount; i++) {
    if (strcmp(s_cache[i].key, key) == 0) {
      value = s_cache[i].value;
      break;
    }
  }
  SDL_UnlockSpinlock(&s_lock);
  return value;
}

void brainCapacityStore(const char *name, const char *version, int value) {
  char key[BRAIN_CAPACITY_KEY_LEN];
  int i;

  capacityKey(key, sizeof(key), name, version);
  value = clampCapacity(value);
  SDL_LockSpinlock(&s_lock);
  for (i = 0; i < s_cacheCount; i++) {
    if (strcmp(s_cache[i].key, key) == 0) {
      break;
    }
  }
  if (i == s_cacheCount) {
    if (s_cacheCount < BRAIN_CAPACITY_CACHE_MAX) {
      s_cacheCount++;
    } else {
      /* Full: the oldest answer makes room. */
      memmove(&s_cache[0], &s_cache[1],
              sizeof(s_cache[0]) * (BRAIN_CAPACITY_CACHE_MAX - 1));
      i = BRAIN_CAPACITY_CACHE_MAX - 1;
    }
    SDL_strlcpy(s_cache[i].key, key, sizeof(s_cache[i].key));
  }
  s_cache[i].value = value;
  SDL_UnlockSpinlock(&s_lock);
}

void brainCapacityClear(void) {
  SDL_LockSpinlock(&s_lock);
  s_cacheCount = 0;
  SDL_UnlockSpinlock(&s_lock);
}

void brainCapacityRegisterC(const char *name, BrainCapacityFn fn) {
  int i;

  if (name == NULL) {
    return;
  }
  SDL_LockSpinlock(&s_lock);
  for (i = 0; i < s_cBrainCount; i++) {
    if (strcmp(s_cBrains[i].name, name) == 0) {
      break;
    }
  }
  if (fn == NULL) {
    if (i < s_cBrainCount) {
      s_cBrains[i] = s_cBrains[s_cBrainCount - 1];
      s_cBrainCount--;
    }
  } else if (i < s_cBrainCount) {
    s_cBrains[i].fn = fn;
  } else if (s_cBrainCount < BRAIN_CAPACITY_C_MAX) {
    SDL_strlcpy(s_cBrains[s_cBrainCount].name, name,
                sizeof(s_cBrains[s_cBrainCount].name));
    s_cBrains[s_cBrainCount].fn = fn;
    s_cBrainCount++;
  }
  SDL_UnlockSpinlock(&s_lock);
}

int brainCapacityQueryC(const char *name, const char *version) {
  BrainCapacityFn fn = NULL;
  BrainMachineInfo machine;
  int raw;
  int value;

  if (name == NULL) {
    return BRAIN_CAPACITY_UNKNOWN;
  }
  SDL_LockSpinlock(&s_lock);
  for (int i = 0; i < s_cBrainCount; i++) {
    if (strcmp(s_cBrains[i].name, name) == 0) {
      fn = s_cBrains[i].fn;
      break;
    }
  }
  SDL_UnlockSpinlock(&s_lock);
  if (fn == NULL) {
    return BRAIN_CAPACITY_UNKNOWN;
  }

  /* The function runs outside the lock: it may read the machine facts. */
  brainCapacityGetMachine(&machine);
  raw = fn(&machine, NULL);
  value = brainCapacityFromAnswer(raw >= 0, (double)raw, &machine);
  brainCapacityStore(name, version, value);
  return value;
}

BrainCapacityAddState brainCapacityAddState(bool measuring, int botsHere,
                                            int capacity, bool botsRunHere) {
  if (botsRunHere == false) {
    /* The bots run on a remote server or Bot Host. Its own answer would
       be used here once that server sends it; this machine's speed and
       this machine's answer do not apply to it. */
    return BRAIN_CAPACITY_ADD_OK;
  }
  if (measuring) {
    return BRAIN_CAPACITY_ADD_MEASURING;
  }
  if (capacity >= 0 && botsHere >= capacity) {
    return BRAIN_CAPACITY_ADD_FULL;
  }
  return BRAIN_CAPACITY_ADD_OK;
}
