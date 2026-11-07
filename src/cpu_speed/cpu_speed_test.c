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

/* The CPU speed test and the CPU facts that go with it - see
 * cpu_speed_test.h.
 *
 * The CoreMark set-up below follows core_main.c's main step by step (the
 * data split, the three inits, the seed CRC and the known CRCs of the 2K
 * performance run), so a score here is the score CoreMark's own program
 * gives for the same iteration count. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "cpu_speed_test.h"
#include "coremark.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <dirent.h>
#endif

#if (defined(__x86_64__) || defined(__i386__)) && !defined(_MSC_VER)
#include <cpuid.h>
#define CPU_SPEED_HAVE_CPUID 1
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#define CPU_SPEED_HAVE_CPUID 1
#endif

/* In core_util.c; core_main.c declares it for itself, coremark.h does not. */
ee_s32 get_seed_32(int i);

/* The seed CRC and the list, matrix and state CRCs that CoreMark's
   core_main.c knows for seeds 0, 0, 0x66 and 666 bytes per algorithm (its
   known_id 3, the 2K performance run). */
#define CM_SEED_CRC   0xe9f5
#define CM_LIST_CRC   0xe714
#define CM_MATRIX_CRC 0x1fd7
#define CM_STATE_CRC  0x8e3a

/* The most threads the all-core step runs. */
#define CPU_SPEED_MAX_THREADS 1024

/* One CoreMark context: its results and the memory its data lives in. Each
   thread of the all-core step has its own. */
typedef struct {
  core_results res;
  void *mem;
  ee_u32 iterations;     /* The step's count for this context */
  SDL_AtomicInt *cancel; /* The caller's cancel flag, or NULL */
  bool lowPriority;      /* Run this thread at SDL_THREAD_PRIORITY_LOW */
  bool batchCrcBad;      /* A batch before the last gave a wrong CRC */
} CmContext;

/* Iterations per batch when the test can be cancelled: a few milliseconds
   on any machine the game runs on. */
#define CM_CANCEL_BATCH 200

void cpuSpeedTestDefaultParams(CpuSpeedParams *params) {
  params->warmIterations = 3000;
  params->phaseSecs = 0.8;
  params->allCoreFactor = 0.9;
  params->minIterations = 1000;
  params->threads = 0;
  params->cancel = NULL;
  params->lowPriority = false;
}

/* Sets one context up for a step of iterations, as core_main.c's main does
   for each of its contexts. The data is built again for every step, as the
   bench script's separate CoreMark runs each built it again. */
static void cmContextSetup(CmContext *ctx, ee_u32 iterations) {
  core_results *res = &ctx->res;
  ee_u32 size;

  ctx->batchCrcBad = false;
  res->seed1 = (ee_s16)get_seed_32(1);
  res->seed2 = (ee_s16)get_seed_32(2);
  res->seed3 = (ee_s16)get_seed_32(3);
  res->iterations = iterations;
  ctx->iterations = iterations;
  res->execs = ALL_ALGORITHMS_MASK;
  res->err = 0;

  /* The 2000 bytes split three ways, one part per algorithm */
  size = TOTAL_DATA_SIZE / NUM_ALGORITHMS;
  res->size = size;
  res->memblock[0] = ctx->mem;
  res->memblock[1] = (char *)ctx->mem;
  res->memblock[2] = (char *)ctx->mem + size;
  res->memblock[3] = (char *)ctx->mem + size * 2;

  res->list = core_list_init(size, res->memblock[1], res->seed1);
  core_init_matrix(size, res->memblock[2],
                   (ee_s32)res->seed1 | (((ee_s32)res->seed2) << 16),
                   &res->mat);
  core_init_state(size, res->seed1, res->memblock[3]);
}

/* True when the context's last iterate call gave the known CRCs. */
static bool cmIterateCrcOk(const CmContext *ctx) {
  const core_results *res = &ctx->res;
  ee_u16 seedcrc = 0;

  seedcrc = crc16(res->seed1, seedcrc);
  seedcrc = crc16(res->seed2, seedcrc);
  seedcrc = crc16(res->seed3, seedcrc);
  seedcrc = crc16((ee_s16)res->size, seedcrc);
  return seedcrc == CM_SEED_CRC && res->crclist == CM_LIST_CRC &&
         res->crcmatrix == CM_MATRIX_CRC && res->crcstate == CM_STATE_CRC;
}

/* True when every batch of the context's run gave the known CRCs. */
static bool cmContextCrcOk(const CmContext *ctx) {
  return ctx->batchCrcBad == false && cmIterateCrcOk(ctx);
}

/* Runs a context's iterations. With no cancel flag it is one call of
   CoreMark's iterate, as CoreMark's own program makes. With one, the
   iterations run in batches and the flag is read between them. The data is
   built again before each batch after the first (CoreMark's list keeps
   results from earlier iterations), so every batch gives the known CRCs, and
   each batch's are checked. */
static void cmContextRun(CmContext *ctx) {
  ee_u32 total = ctx->iterations;
  ee_u32 left = total;
  bool first = true;

  if (ctx->cancel == NULL) {
    iterate(&ctx->res);
    return;
  }
  while (left > 0 && SDL_GetAtomicInt(ctx->cancel) == 0) {
    ee_u32 batch = left < CM_CANCEL_BATCH ? left : CM_CANCEL_BATCH;
    if (first == false) {
      bool bad = ctx->batchCrcBad || cmIterateCrcOk(ctx) == false;
      cmContextSetup(ctx, total);
      ctx->batchCrcBad = bad;
    }
    ctx->res.iterations = batch;
    iterate(&ctx->res);
    left -= batch;
    first = false;
  }
  ctx->res.iterations = total;
}

static int SDLCALL cmThreadMain(void *data) {
  CmContext *ctx = (CmContext *)data;

  if (ctx->lowPriority) {
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
  }
  cmContextRun(ctx);
  return 0;
}

/* Runs one step: count contexts, each of iterations, at once. The time runs
   from before the first thread starts until the last one has finished, as
   in core_main.c. One context runs on the calling thread. False when a
   thread would not start. */
static bool cmRunStep(CmContext *ctxs, int count, ee_u32 iterations,
                      double *secs, bool *crcOk) {
  SDL_Thread **threads = NULL;
  Uint64 start;
  Uint64 stop;
  bool started = true;
  int i;

  for (i = 0; i < count; i++) {
    cmContextSetup(&ctxs[i], iterations);
  }

  if (count > 1) {
    threads = (SDL_Thread **)calloc((size_t)count, sizeof(*threads));
    if (threads == NULL) {
      return false;
    }
  }

  start = SDL_GetTicksNS();
  if (count == 1) {
    cmContextRun(&ctxs[0]);
  } else {
    for (i = 0; i < count; i++) {
      threads[i] = SDL_CreateThread(cmThreadMain, "cpu_speed_test", &ctxs[i]);
      if (threads[i] == NULL) {
        started = false;
      }
    }
    for (i = 0; i < count; i++) {
      if (threads[i] != NULL) {
        SDL_WaitThread(threads[i], NULL);
      }
    }
  }
  stop = SDL_GetTicksNS();
  free(threads);

  *secs = (double)(stop - start) / 1e9;
  if (*secs <= 0.0) {
    *secs = 1e-9;
  }
  for (i = 0; i < count; i++) {
    if (cmContextCrcOk(&ctxs[i]) == false) {
      *crcOk = false;
    }
  }
  return started;
}

/* An iteration count for a step that aims at secs at a rate of perSec,
   kept between minIterations and what an ee_u32 holds. */
static ee_u32 cmIterationsFor(double perSec, double secs,
                              uint32_t minIterations) {
  double want = perSec * secs;

  if (want < (double)minIterations) {
    return minIterations;
  }
  if (want > 4.0e9) {
    return (ee_u32)4000000000u;
  }
  return (ee_u32)want;
}

/* False, with result->cancelled set, when the caller's flag is up. */
static bool cmNotCancelled(const CpuSpeedParams *params,
                           CpuSpeedResult *result) {
  if (params->cancel != NULL && SDL_GetAtomicInt(params->cancel) != 0) {
    result->cancelled = true;
    return false;
  }
  return true;
}

bool cpuSpeedTestRun(const CpuSpeedParams *params, CpuSpeedResult *result) {
  CpuSpeedParams defaults;
  CmContext *ctxs;
  Uint64 testStart;
  bool ok = true;
  int threads;
  int i;

  if (params == NULL) {
    cpuSpeedTestDefaultParams(&defaults);
    params = &defaults;
  }
  testStart = SDL_GetTicksNS();

  memset(result, 0, sizeof(*result));
  result->logicalCores = cpuSpeedLogicalCores();
  result->physicalCores = cpuSpeedPhysicalCores();
  cpuSpeedCpuName(result->cpuName, sizeof(result->cpuName));
  result->crcOk = true;

  threads = params->threads > 0 ? params->threads : result->logicalCores;
  if (threads < 1) {
    threads = 1;
  }
  if (threads > CPU_SPEED_MAX_THREADS) {
    threads = CPU_SPEED_MAX_THREADS;
  }
  result->threads = threads;

  /* CoreMark checks its own types once, as main does. */
  if (check_data_types() != 0) {
    result->crcOk = false;
  }

  ctxs = (CmContext *)calloc((size_t)threads, sizeof(*ctxs));
  if (ctxs == NULL) {
    return false;
  }
  for (i = 0; i < threads; i++) {
    ctxs[i].cancel = params->cancel;
    ctxs[i].lowPriority = params->lowPriority;
    ctxs[i].mem = portable_malloc(TOTAL_DATA_SIZE);
    if (ctxs[i].mem == NULL) {
      ok = false;
    }
  }

  if (ok) {
    /* 1. Warm-up, which also gives the rate for the next step's count */
    result->warmIterations = params->warmIterations;
    ok = cmRunStep(ctxs, 1, result->warmIterations, &result->warmSecs,
                   &result->crcOk);
    ok = ok && cmNotCancelled(params, result);
  }
  if (ok) {
    /* 2. One thread */
    result->oneIterations =
        cmIterationsFor((double)result->warmIterations / result->warmSecs,
                        params->phaseSecs, params->minIterations);
    ok = cmRunStep(ctxs, 1, result->oneIterations, &result->oneSecs,
                   &result->crcOk);
    result->cm1 = (double)result->oneIterations / result->oneSecs;
    ok = ok && cmNotCancelled(params, result);
  }
  if (ok) {
    /* 3. Every thread at once. Each one aims a little under the 1-thread
       step's time, as a core shared with a second thread runs slower. */
    result->allIterationsPerThread =
        cmIterationsFor(result->cm1, params->phaseSecs * params->allCoreFactor,
                        params->minIterations);
    ok = cmRunStep(ctxs, threads, result->allIterationsPerThread,
                   &result->allSecs, &result->crcOk);
    result->cmAll = (double)threads *
                    (double)result->allIterationsPerThread / result->allSecs;
    result->scaling = result->cm1 > 0.0 ? result->cmAll / result->cm1 : 0.0;
    ok = ok && cmNotCancelled(params, result);
  }

  for (i = 0; i < threads; i++) {
    portable_free(ctxs[i].mem);
  }
  free(ctxs);

  result->totalSecs = (double)(SDL_GetTicksNS() - testStart) / 1e9;
  return ok;
}

int cpuSpeedLogicalCores(void) {
  int n = SDL_GetNumLogicalCPUCores();
  return n > 0 ? n : 1;
}

#if defined(__linux__) && !defined(_WIN32)
/* Reads the first number in a small file, or -1. */
static long cpuSpeedReadLong(const char *path) {
  FILE *f = fopen(path, "r");
  long value = -1;

  if (f != NULL) {
    if (fscanf(f, "%ld", &value) != 1) {
      value = -1;
    }
    fclose(f);
  }
  return value;
}

/* Adds (package, core) to the list when it is not in it yet. */
static void cpuSpeedAddPair(long *pairs, int *count, int max, long package,
                            long core) {
  int i;

  for (i = 0; i < *count; i++) {
    if (pairs[i * 2] == package && pairs[i * 2 + 1] == core) {
      return;
    }
  }
  if (*count < max) {
    pairs[*count * 2] = package;
    pairs[*count * 2 + 1] = core;
    (*count)++;
  }
}

/* Physical cores from /sys: one per different (package, core id) among the
   CPUs that have a topology directory. 0 when /sys does not say. */
static int cpuSpeedLinuxSysCores(void) {
  enum { MAX_PAIRS = 4096 };
  static long pairs[MAX_PAIRS * 2];
  DIR *dir = opendir("/sys/devices/system/cpu");
  struct dirent *entry;
  int count = 0;

  if (dir == NULL) {
    return 0;
  }
  while ((entry = readdir(dir)) != NULL) {
    char path[300];
    const char *p = entry->d_name;
    long core;
    long package;

    if (strncmp(p, "cpu", 3) != 0 || p[3] < '0' || p[3] > '9') {
      continue;
    }
    for (p += 3; *p >= '0' && *p <= '9'; p++) {
    }
    if (*p != '\0') {
      continue;
    }
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/%s/topology/core_id",
             entry->d_name);
    core = cpuSpeedReadLong(path);
    if (core < 0) {
      continue;
    }
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/%s/topology/physical_package_id",
             entry->d_name);
    package = cpuSpeedReadLong(path);
    cpuSpeedAddPair(pairs, &count, MAX_PAIRS, package, core);
  }
  closedir(dir);
  return count;
}

/* Physical cores from /proc/cpuinfo's "physical id" and "core id" lines.
   0 when it has none. */
static int cpuSpeedLinuxCpuinfoCores(void) {
  enum { MAX_PAIRS = 4096 };
  static long pairs[MAX_PAIRS * 2];
  FILE *f = fopen("/proc/cpuinfo", "r");
  char line[512];
  long package = 0;
  long core = -1;
  int count = 0;

  if (f == NULL) {
    return 0;
  }
  while (fgets(line, sizeof(line), f) != NULL) {
    const char *colon = strchr(line, ':');

    if (strncmp(line, "physical id", 11) == 0 && colon != NULL) {
      package = strtol(colon + 1, NULL, 10);
    } else if (strncmp(line, "core id", 7) == 0 && colon != NULL) {
      core = strtol(colon + 1, NULL, 10);
    } else if (line[0] == '\n' && core >= 0) {
      cpuSpeedAddPair(pairs, &count, MAX_PAIRS, package, core);
      package = 0;
      core = -1;
    }
  }
  if (core >= 0) {
    cpuSpeedAddPair(pairs, &count, MAX_PAIRS, package, core);
  }
  fclose(f);
  return count;
}
#endif

int cpuSpeedPhysicalCores(void) {
  int logical = cpuSpeedLogicalCores();
  int physical = 0;

#ifdef _WIN32
  {
    DWORD len = 0;
    BYTE *buf;

    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    if (len > 0 && (buf = (BYTE *)malloc(len)) != NULL) {
      if (GetLogicalProcessorInformationEx(
              RelationProcessorCore,
              (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        DWORD offset = 0;
        while (offset < len) {
          PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info =
              (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + offset);
          if (info->Size == 0) {
            break;
          }
          if (info->Relationship == RelationProcessorCore) {
            physical++;
          }
          offset += info->Size;
        }
      }
      free(buf);
    }
  }
#elif defined(__APPLE__)
  {
    int value = 0;
    size_t len = sizeof(value);
    if (sysctlbyname("hw.physicalcpu", &value, &len, NULL, 0) == 0) {
      physical = value;
    }
  }
#elif defined(__linux__)
  physical = cpuSpeedLinuxSysCores();
  if (physical <= 0) {
    physical = cpuSpeedLinuxCpuinfoCores();
  }
#endif

  /* A count the OS gives for more cores than SDL sees (CPUs taken offline
     or kept from this process) is cut to what this process can run on. */
  if (physical <= 0 || physical > logical) {
    physical = logical;
  }
  return physical;
}

/* Copies src to out with the spaces at each end taken off and each run of
   spaces inside made one space. */
static void cpuSpeedTidyName(const char *src, char *out, size_t outSize) {
  size_t n = 0;
  bool space = false;

  if (outSize == 0) {
    return;
  }
  while (*src == ' ' || *src == '\t') {
    src++;
  }
  for (; *src != '\0' && *src != '\n' && *src != '\r'; src++) {
    if (*src == ' ' || *src == '\t') {
      space = true;
      continue;
    }
    if (space && n > 0 && n + 1 < outSize) {
      out[n++] = ' ';
    }
    space = false;
    if (n + 1 < outSize) {
      out[n++] = *src;
    }
  }
  out[n] = '\0';
}

#ifdef CPU_SPEED_HAVE_CPUID
/* The brand string the CPU itself gives, or false when it has none. */
static bool cpuSpeedCpuidName(char *out, size_t outSize) {
  unsigned int regs[12];
  char brand[49];
  unsigned int leaf;

#ifdef _MSC_VER
  int r[4];
  __cpuid(r, (int)0x80000000);
  if ((unsigned int)r[0] < 0x80000004u) {
    return false;
  }
  for (leaf = 0; leaf < 3; leaf++) {
    __cpuid(r, (int)(0x80000002u + leaf));
    memcpy(&regs[leaf * 4], r, sizeof(r));
  }
#else
  unsigned int a, b, c, d;
  if (__get_cpuid(0x80000000u, &a, &b, &c, &d) == 0 || a < 0x80000004u) {
    return false;
  }
  for (leaf = 0; leaf < 3; leaf++) {
    __get_cpuid(0x80000002u + leaf, &a, &b, &c, &d);
    regs[leaf * 4] = a;
    regs[leaf * 4 + 1] = b;
    regs[leaf * 4 + 2] = c;
    regs[leaf * 4 + 3] = d;
  }
#endif
  memcpy(brand, regs, 48);
  brand[48] = '\0';
  cpuSpeedTidyName(brand, out, outSize);
  return out[0] != '\0';
}
#endif

void cpuSpeedCpuName(char *out, size_t outSize) {
  if (out == NULL || outSize == 0) {
    return;
  }
  out[0] = '\0';

#ifdef _WIN32
  {
    char value[256];
    DWORD size = sizeof(value);
    if (RegGetValueA(HKEY_LOCAL_MACHINE,
                     "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     "ProcessorNameString", RRF_RT_REG_SZ, NULL, value,
                     &size) == ERROR_SUCCESS) {
      value[sizeof(value) - 1] = '\0';
      cpuSpeedTidyName(value, out, outSize);
    }
  }
#elif defined(__APPLE__)
  {
    char value[256];
    size_t size = sizeof(value);
    if (sysctlbyname("machdep.cpu.brand_string", value, &size, NULL, 0) == 0) {
      value[sizeof(value) - 1] = '\0';
      cpuSpeedTidyName(value, out, outSize);
    }
  }
#elif defined(__linux__)
  {
    /* x86 has "model name"; many ARM kernels give only "Hardware" or
       "Model". The first of them found is used, in that order of trust. */
    static const char *const keys[] = { "model name", "Model", "Hardware",
                                        "cpu model" };
    FILE *f = fopen("/proc/cpuinfo", "r");
    char found[4][256];
    char line[512];
    size_t k;

    memset(found, 0, sizeof(found));
    if (f != NULL) {
      while (fgets(line, sizeof(line), f) != NULL) {
        const char *colon = strchr(line, ':');
        if (colon == NULL) {
          continue;
        }
        for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
          size_t keyLen = strlen(keys[k]);
          if (found[k][0] == '\0' && strncmp(line, keys[k], keyLen) == 0 &&
              (line[keyLen] == ' ' || line[keyLen] == '\t' ||
               line[keyLen] == ':')) {
            cpuSpeedTidyName(colon + 1, found[k], sizeof(found[k]));
          }
        }
      }
      fclose(f);
    }
    for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
      if (found[k][0] != '\0') {
        snprintf(out, outSize, "%s", found[k]);
        break;
      }
    }
  }
#endif

#ifdef CPU_SPEED_HAVE_CPUID
  if (out[0] == '\0') {
    cpuSpeedCpuidName(out, outSize);
  }
#endif
}

void cpuSpeedFormatLine(const CpuSpeedResult *result, char *out,
                        size_t outSize) {
  snprintf(out, outSize,
           "CPU speed test: %s; %d physical / %d logical cores; "
           "1 thread %.0f it/s; %d threads %.0f it/s (%.2f x); CRC %s; "
           "%.2f s",
           result->cpuName[0] != '\0' ? result->cpuName : "unknown CPU",
           result->physicalCores, result->logicalCores, result->cm1,
           result->threads, result->cmAll, result->scaling,
           result->crcOk ? "ok" : "WRONG", result->totalSecs);
}

/* Writes s as the body of a JSON string: quotes and backslashes escaped,
   control characters left out. */
static void cpuSpeedJsonEscape(const char *s, char *out, size_t outSize) {
  size_t n = 0;

  if (outSize == 0) {
    return;
  }
  for (; *s != '\0' && n + 2 < outSize; s++) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x20) {
      continue;
    }
    if (c == '"' || c == '\\') {
      out[n++] = '\\';
    }
    out[n++] = (char)c;
  }
  out[n] = '\0';
}

void cpuSpeedFormatJson(const CpuSpeedResult *result, char *out,
                        size_t outSize) {
  char name[CPU_SPEED_NAME_LEN * 2];

  cpuSpeedJsonEscape(result->cpuName, name, sizeof(name));
  snprintf(out, outSize,
           "{\"cpu_name\": \"%s\", \"logical_cores\": %d, "
           "\"physical_cores\": %d, \"threads\": %d, \"cm_1t\": %.1f, "
           "\"cm_all\": %.1f, \"per_thread_all\": %.1f, "
           "\"allcore_scaling\": %.3f, \"crc_ok\": %s, \"warm_it\": %u, "
           "\"warm_s\": %.3f, \"it1\": %u, \"t1_s\": %.3f, \"itn\": %u, "
           "\"tn_s\": %.3f, \"total_wall_s\": %.3f}",
           name, result->logicalCores, result->physicalCores, result->threads,
           result->cm1, result->cmAll,
           result->threads > 0 ? result->cmAll / result->threads : 0.0,
           result->scaling, result->crcOk ? "true" : "false",
           (unsigned)result->warmIterations, result->warmSecs,
           (unsigned)result->oneIterations, result->oneSecs,
           (unsigned)result->allIterationsPerThread, result->allSecs,
           result->totalSecs);
}
