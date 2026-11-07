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

/* The stored CPU speed test result and the background run - see
 * cpu_speed_store.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>

#include "cJSON.h"
#include "cpu_speed_store.h"

#define CPU_SPEED_FILE_NAME "cpu_speed.json"
#define CPU_SPEED_FILE_MAX  (64 * 1024)

static char s_storePath[1024];

void cpuSpeedStoreSetPath(const char *path) {
  if (path == NULL) {
    s_storePath[0] = '\0';
  } else {
    SDL_strlcpy(s_storePath, path, sizeof(s_storePath));
  }
}

bool cpuSpeedStorePath(char *out, size_t outSize) {
  const char *env;
  char *pref;

  if (s_storePath[0] != '\0') {
    SDL_strlcpy(out, s_storePath, outSize);
    return true;
  }
  env = SDL_getenv("WINBOLO_CPU_SPEED_FILE");
  if (env != NULL && env[0] != '\0') {
    SDL_strlcpy(out, env, outSize);
    return true;
  }
  /* SDL_GetPrefPath ends with a separator; the caller frees it. */
  pref = SDL_GetPrefPath("WinBolo", "WinBolo");
  if (pref == NULL) {
    return false;
  }
  SDL_snprintf(out, outSize, "%s%s", pref, CPU_SPEED_FILE_NAME);
  SDL_free(pref);
  return true;
}

static double jsonNumber(const cJSON *obj, const char *key) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsNumber(item) ? item->valuedouble : 0.0;
}

static void jsonString(const cJSON *obj, const char *key, char *out,
                       size_t outSize) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsString(item) && item->valuestring != NULL) {
    SDL_strlcpy(out, item->valuestring, outSize);
  } else if (outSize > 0) {
    out[0] = '\0';
  }
}

bool cpuSpeedStoreLoad(const char *version, CpuSpeedStored *out) {
  char path[1024];
  char name[CPU_SPEED_NAME_LEN];
  size_t len = 0;
  char *text;
  cJSON *root;
  const cJSON *crc;
  bool ok;

  memset(out, 0, sizeof(*out));
  if (version == NULL || cpuSpeedStorePath(path, sizeof(path)) == false) {
    return false;
  }
  text = (char *)SDL_LoadFile(path, &len);
  if (text == NULL) {
    return false;
  }
  if (len > CPU_SPEED_FILE_MAX) {
    SDL_free(text);
    return false;
  }
  root = cJSON_ParseWithLength(text, len);
  SDL_free(text);
  if (root == NULL) {
    return false;
  }

  jsonString(root, "version", out->version, sizeof(out->version));
  jsonString(root, "context", out->context, sizeof(out->context));
  jsonString(root, "cpu_name", out->result.cpuName,
             sizeof(out->result.cpuName));
  out->result.logicalCores = (int)jsonNumber(root, "logical_cores");
  out->result.physicalCores = (int)jsonNumber(root, "physical_cores");
  out->result.threads = (int)jsonNumber(root, "threads");
  out->result.speed1t = jsonNumber(root, "speed_1t");
  out->result.speedAll = jsonNumber(root, "speed_all");
  out->result.scaling = jsonNumber(root, "allcore_scaling");
  out->result.totalSecs = jsonNumber(root, "total_wall_s");
  crc = cJSON_GetObjectItemCaseSensitive(root, "crc_ok");
  out->result.crcOk = cJSON_IsTrue(crc);
  cJSON_Delete(root);

  /* Only this version on this machine. The name and the logical core count
     are read again, as they are cheap; a moved install or a new CPU then
     runs the test again. */
  cpuSpeedCpuName(name, sizeof(name));
  ok = strcmp(out->version, version) == 0 &&
       strcmp(out->result.cpuName, name) == 0 &&
       out->result.logicalCores == cpuSpeedLogicalCores() &&
       out->result.crcOk && out->result.speed1t > 0.0 && out->result.speedAll > 0.0;
  if (ok == false) {
    memset(out, 0, sizeof(*out));
  }
  return ok;
}

bool cpuSpeedStoreSave(const char *version, const char *context,
                       const CpuSpeedResult *result) {
  char path[1024];
  char tmpPath[1040];
  cJSON *root;
  char *text;
  FILE *f;
  bool ok = false;

  if (version == NULL || result == NULL || result->crcOk == false ||
      result->cancelled || result->speed1t <= 0.0 || result->speedAll <= 0.0) {
    return false;
  }
  if (cpuSpeedStorePath(path, sizeof(path)) == false) {
    return false;
  }

  root = cJSON_CreateObject();
  if (root == NULL) {
    return false;
  }
  cJSON_AddStringToObject(root, "version", version);
  cJSON_AddStringToObject(root, "context", context != NULL ? context : "");
  cJSON_AddNumberToObject(root, "saved_unix", (double)time(NULL));
  cJSON_AddStringToObject(root, "cpu_name", result->cpuName);
  cJSON_AddNumberToObject(root, "logical_cores", result->logicalCores);
  cJSON_AddNumberToObject(root, "physical_cores", result->physicalCores);
  cJSON_AddNumberToObject(root, "threads", result->threads);
  cJSON_AddNumberToObject(root, "speed_1t", result->speed1t);
  cJSON_AddNumberToObject(root, "speed_all", result->speedAll);
  cJSON_AddNumberToObject(root, "allcore_scaling", result->scaling);
  cJSON_AddBoolToObject(root, "crc_ok", result->crcOk);
  cJSON_AddNumberToObject(root, "total_wall_s", result->totalSecs);
  text = cJSON_Print(root);
  cJSON_Delete(root);
  if (text == NULL) {
    return false;
  }

  /* Written beside the file and moved over it, so a reader never sees half
     a file. */
  SDL_snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
  f = fopen(tmpPath, "wb");
  if (f != NULL) {
    size_t n = strlen(text);
    ok = fwrite(text, 1, n, f) == n;
    if (fclose(f) != 0) {
      ok = false;
    }
    if (ok) {
      ok = SDL_RenamePath(tmpPath, path);
    }
    if (ok == false) {
      SDL_RemovePath(tmpPath);
    }
  }
  cJSON_free(text);
  return ok;
}

/* The background run. One at a time: s_running is 1 from the start until
   the thread is about to end. */
static SDL_AtomicInt s_running;
static SDL_AtomicInt s_cancel;
static CpuSpeedParams s_bgParams;
static bool s_bgParamsSet = false;

typedef struct {
  char version[CPU_SPEED_VERSION_LEN];
  char context[CPU_SPEED_CONTEXT_LEN];
  CpuSpeedLogFn log;
  CpuSpeedParams params;
} CpuSpeedJob;

void cpuSpeedBackgroundSetParams(const CpuSpeedParams *params) {
  if (params == NULL) {
    s_bgParamsSet = false;
  } else {
    s_bgParams = *params;
    s_bgParamsSet = true;
  }
}

static int SDLCALL cpuSpeedBackgroundMain(void *data) {
  CpuSpeedJob *job = (CpuSpeedJob *)data;
  CpuSpeedResult result;
  char line[1024];
  bool ran;

  SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
  if (job->log != NULL) {
    SDL_snprintf(line, sizeof(line),
                 "CPU speed test: starting (%s, about 2 s, low priority)",
                 job->context);
    job->log(line);
  }

  ran = cpuSpeedTestRun(&job->params, &result);
  if (ran && result.crcOk && SDL_GetAtomicInt(&s_cancel) == 0) {
    bool saved = cpuSpeedStoreSave(job->version, job->context, &result);
    if (job->log != NULL) {
      cpuSpeedFormatLine(&result, line, sizeof(line));
      job->log(line);
      if (saved == false) {
        job->log("CPU speed test: the result could not be stored");
      }
    }
  } else if (job->log != NULL) {
    if (result.cancelled || SDL_GetAtomicInt(&s_cancel) != 0) {
      job->log("CPU speed test: stopped before the end; nothing stored, it "
               "runs again next time");
    } else {
      cpuSpeedFormatLine(&result, line, sizeof(line));
      job->log(line);
      job->log("CPU speed test: the run failed; nothing stored");
    }
  }

  free(job);
  SDL_SetAtomicInt(&s_running, 0);
  return 0;
}

bool cpuSpeedBackgroundStart(const char *version, const char *context,
                             CpuSpeedLogFn log) {
  CpuSpeedStored stored;
  CpuSpeedJob *job;
  SDL_Thread *thread;

  if (version == NULL) {
    return false;
  }
  if (SDL_CompareAndSwapAtomicInt(&s_running, 0, 1) == false) {
    return false;
  }
  if (cpuSpeedStoreLoad(version, &stored)) {
    SDL_SetAtomicInt(&s_running, 0);
    return false;
  }
  job = (CpuSpeedJob *)calloc(1, sizeof(*job));
  if (job == NULL) {
    SDL_SetAtomicInt(&s_running, 0);
    return false;
  }
  SDL_strlcpy(job->version, version, sizeof(job->version));
  SDL_strlcpy(job->context, context != NULL ? context : "", sizeof(job->context));
  job->log = log;
  if (s_bgParamsSet) {
    job->params = s_bgParams;
  } else {
    cpuSpeedTestDefaultParams(&job->params);
  }
  job->params.cancel = &s_cancel;
  job->params.lowPriority = true;
  SDL_SetAtomicInt(&s_cancel, 0);

  thread = SDL_CreateThread(cpuSpeedBackgroundMain, "cpu_speed_bg", job);
  if (thread == NULL) {
    free(job);
    SDL_SetAtomicInt(&s_running, 0);
    return false;
  }
  SDL_DetachThread(thread);
  return true;
}

void cpuSpeedBackgroundCancel(void) {
  if (SDL_GetAtomicInt(&s_running) != 0) {
    SDL_SetAtomicInt(&s_cancel, 1);
  }
}

bool cpuSpeedBackgroundRunning(void) {
  return SDL_GetAtomicInt(&s_running) != 0;
}

bool cpuSpeedBackgroundWait(int timeoutMs) {
  Uint64 end = SDL_GetTicks() + (Uint64)(timeoutMs > 0 ? timeoutMs : 0);

  while (SDL_GetAtomicInt(&s_running) != 0) {
    if (SDL_GetTicks() >= end) {
      return false;
    }
    SDL_Delay(5);
  }
  return true;
}
