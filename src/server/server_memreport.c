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

/* The dedicated server's memory report — see server_memreport.h.
 *
 * One line per report, written where the server writes its other console
 * messages (stderr, or the -logfile file). Every figure carries its unit:
 *
 *   [mem 2026-10-06 15:40:00] up 1h00m00s | tick 180000 | rounds 1 |
 *   map Everard Island | process rss 345.2 MB (peak 360.1 MB),
 *   private 413.9 MB, virtual 2.10 TB | lua 8 bots 120.3 MB (+1.2 MB):
 *   p0 15123 KB (+12) p1 ... | private minus lua 293.6 MB (+0.4 MB) |
 *   lists server: shells 3 ... | lists bots (8): shells 21 ... |
 *   recording brain-debug off, game log off
 *
 * A process figure the platform could not read shows as "n/a", never 0.
 *
 * The timed report reads no more than a clock until it is due. When it is
 * due, the Lua figures are lua_gc counters (no walk) and the list figures
 * walk lists that hold tens of items, so a report costs well under a
 * millisecond. */

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  /* Version 2 maps GetProcessMemoryInfo onto kernel32, so no psapi.lib. */
  #ifndef PSAPI_VERSION
    #define PSAPI_VERSION 2
  #endif
  #include <psapi.h>
#elif defined(__APPLE__)
  #include <mach/mach.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "bot_mem_report.h"
#include "brain_record.h"
#include "log.h"
#include "threads.h"
#include "server_memreport.h"

/* servermessages.c has no header; servermain.c declares it the same way. */
void serverMessageConsoleMessage(ServerSim *sim, char *msg);

/* Room for 16 bots at ~22 characters each plus everything else. */
#define MEMREPORT_LINE 2048
#define MEMREPORT_DEEP_TOP 20

static Uint64 s_periodMs = 0;  /* off until -memreport turns it on */
static bool   s_deepEvery = false;
static Uint64 s_startMs = 0;
static Uint64 s_lastMs = 0;
static SDL_AtomicInt s_request;  /* 0 none, 1 report, 2 report + deep */

static uint32_t s_rounds = 0;
static bool     s_wasRunning = false;

static bool     s_havePrev = false;
static size_t   s_prevBotBytes[MAX_TANKS];
static bool     s_prevBotValid[MAX_TANKS];
static uint64_t s_prevLuaTotal = 0;
static uint64_t s_prevNonLua = 0;
static bool     s_prevNonLuaValid = false;

/* A year: far past any real period, and small enough to convert. */
#define MEMREPORT_MAX_MINUTES (365.0 * 24.0 * 60.0)

double serverMemReportPeriodFromArgs(bool given, const char *value, bool deep,
                                     char *warn, size_t warnSize) {
  double mins = 0.0;

  if (warn != NULL && warnSize > 0) {
    warn[0] = '\0';
  }
  if (given) {
    mins = (double)SERVER_MEMREPORT_DEFAULT_MINUTES;
    if (value != NULL) {
      char *end = NULL;
      double parsed = strtod(value, &end);
      if (end != value) {
        /* "5m" parses as 5; 0 or less turns the report off. */
        mins = parsed;
      } else if (value[0] != '-' && warn != NULL && warnSize > 0) {
        /* A following flag means a bare -memreport; anything else is a
         * mistyped period, which must not turn the report off. */
        snprintf(warn, warnSize,
                 "Warning: -memreport '%s' is not a number; using %d minutes",
                 value, SERVER_MEMREPORT_DEFAULT_MINUTES);
      }
    }
  }
  /* NaN fails every comparison: treat it as off, like 0. */
  if (!(mins > 0.0)) {
    mins = 0.0;
  } else if (mins > MEMREPORT_MAX_MINUTES) {
    mins = MEMREPORT_MAX_MINUTES;
  }
  if (deep) {
    if (!given) {
      mins = (double)SERVER_MEMREPORT_DEFAULT_MINUTES;
    } else if (mins == 0.0 && warn != NULL && warnSize > 0) {
      snprintf(warn, warnSize,
               "Warning: -memreportdeep has no effect; -memreport %s turns "
               "the timed memory report off",
               (value != NULL) ? value : "0");
    }
  }
  return mins;
}

void serverMemReportConfigure(double periodMinutes, bool deepEveryReport) {
  if (!(periodMinutes > 0.0)) {
    s_periodMs = 0;
  } else {
    if (periodMinutes > MEMREPORT_MAX_MINUTES) {
      periodMinutes = MEMREPORT_MAX_MINUTES;
    }
    s_periodMs = (Uint64)(periodMinutes * 60000.0);
    if (s_periodMs == 0) {
      s_periodMs = 1;
    }
  }
  s_deepEvery = deepEveryReport;
}

void serverMemReportRequest(bool deep) {
  /* Keep the larger request: a "mem" that arrives after a "memdeep" and
   * before the next poll must not drop the deep walk. */
  int want = deep ? 2 : 1;
  int cur;
  do {
    cur = SDL_GetAtomicInt(&s_request);
    if (cur >= want) {
      return;
    }
  } while (!SDL_CompareAndSwapAtomicInt(&s_request, cur, want));
}

bool serverMemReadProcess(ServerMemProcess *out) {
  memset(out, 0, sizeof(*out));
#if defined(_WIN32)
  {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    MEMORYSTATUSEX ms;
    bool ok = false;
    memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) {
      out->rss = pmc.WorkingSetSize;
      out->peakRss = pmc.PeakWorkingSetSize;
      out->priv = pmc.PrivateUsage;
      out->haveRss = out->havePeakRss = out->havePriv = true;
      ok = true;
    }
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
      out->virt = ms.ullTotalVirtual - ms.ullAvailVirtual;
      out->haveVirt = true;
      ok = true;
    }
    return ok;
  }
#elif defined(__linux__)
  {
    /* /proc/self/status lines are "VmRSS:     12345 kB". */
    FILE *fp = fopen("/proc/self/status", "r");
    char line[256];
    bool ok = false;
    if (fp == NULL) {
      return false;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
      unsigned long long kb;
      if (sscanf(line, "VmRSS: %llu", &kb) == 1) {
        out->rss = kb * 1024ull; out->haveRss = true; ok = true;
      } else if (sscanf(line, "VmHWM: %llu", &kb) == 1) {
        out->peakRss = kb * 1024ull; out->havePeakRss = true; ok = true;
      } else if (sscanf(line, "VmData: %llu", &kb) == 1) {
        out->priv = kb * 1024ull; out->havePriv = true; ok = true;
      } else if (sscanf(line, "VmSize: %llu", &kb) == 1) {
        out->virt = kb * 1024ull; out->haveVirt = true; ok = true;
      }
    }
    fclose(fp);
    return ok;
  }
#elif defined(__APPLE__)
  {
    /* MACH_TASK_BASIC_INFO gives resident (and its peak) and virtual.
     * macOS has no "private bytes"; the nearest is phys_footprint from
     * TASK_VM_INFO, the figure Activity Monitor shows as "Memory": dirty
     * and compressed pages the process owns, which takes in every Lua
     * heap. virtual_size takes in the shared system region, so it reads
     * far bigger than on the other platforms. */
    mach_task_basic_info_data_t info;
    task_vm_info_data_t vm;
    mach_msg_type_number_t count;
    bool ok = false;

    count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) == KERN_SUCCESS) {
      out->rss = info.resident_size;
      out->peakRss = info.resident_size_max;
      out->virt = info.virtual_size;
      out->haveRss = out->havePeakRss = out->haveVirt = true;
      ok = true;
    }
    /* phys_footprint is in revision 1 of the struct and later; an older
     * kernel fills fewer fields and says so in count. */
    memset(&vm, 0, sizeof(vm));
    count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  (task_info_t)&vm, &count) == KERN_SUCCESS &&
        count >= TASK_VM_INFO_REV1_COUNT) {
      out->priv = vm.phys_footprint;
      out->havePriv = true;
      ok = true;
    }
    return ok;
  }
#else
  return false;
#endif
}

/* Bounded append onto a line buffer. */
typedef struct {
  char  *buf;
  size_t cap;
  size_t len;
} MemLine;

static void memLineAdd(MemLine *l, const char *fmt, ...) {
  va_list ap;
  int n;
  if (l->len + 1 >= l->cap) {
    return;
  }
  va_start(ap, fmt);
  n = vsnprintf(l->buf + l->len, l->cap - l->len, fmt, ap);
  va_end(ap);
  if (n > 0) {
    l->len += (size_t)n;
    if (l->len >= l->cap) {
      l->len = l->cap - 1;
    }
  }
}

static double memMB(uint64_t bytes) {
  return (double)bytes / (1024.0 * 1024.0);
}

/* Signed difference in MB, for the "(+x MB)" growth figures. */
static double memDeltaMB(uint64_t now, uint64_t before) {
  return ((double)now - (double)before) / (1024.0 * 1024.0);
}

/* One process figure in MB, or "n/a" when it could not be read: a 0
 * there would read as a real figure. */
static void memLineMB(MemLine *l, bool have, uint64_t bytes) {
  if (have) {
    memLineAdd(l, "%.1f MB", memMB(bytes));
  } else {
    memLineAdd(l, "n/a");
  }
}

static void memLineProcess(MemLine *l, const ServerMemProcess *pm) {
  memLineAdd(l, "process rss ");
  memLineMB(l, pm->haveRss, pm->rss);
  memLineAdd(l, " (peak ");
  memLineMB(l, pm->havePeakRss, pm->peakRss);
  memLineAdd(l, "), private ");
  memLineMB(l, pm->havePriv, pm->priv);
  memLineAdd(l, ", virtual ");
  memLineMB(l, pm->haveVirt, pm->virt);
}

void serverMemFormatProcess(const ServerMemProcess *pm, char *buf,
                            size_t bufSize) {
  MemLine l = { buf, bufSize, 0 };
  if (buf == NULL || bufSize == 0) {
    return;
  }
  buf[0] = '\0';
  memLineProcess(&l, pm);
}

static void memLineLists(MemLine *l, const BotMemListCounts *c) {
  memLineAdd(l, "shells %u, explosions %u, tank explosions %u, "
                "mine explosions %u, flood fill %u, building %u, "
                "rubble %u, swamp %u, grass %u",
             c->shells, c->explosions, c->tankExplosions,
             c->minesExplosions, c->floodFill, c->building,
             c->rubble, c->swamp, c->grass);
}

static void memDeepEmit(void *ctx, const char *line) {
  serverMessageConsoleMessage((ServerSim *)ctx, (char *)line);
}

static void memReportWrite(ServerSim *sim, Uint64 nowMs, bool deep) {
  char buf[MEMREPORT_LINE];
  MemLine l = { buf, sizeof(buf), 0 };
  ServerMemProcess pm;
  BotMemListCounts lists;
  uint64_t luaTotal = 0;
  uint64_t nonLua = 0;
  int bots = 0;
  size_t botBytes[MAX_TANKS];
  bool botValid[MAX_TANKS];
  Uint64 up = (nowMs - s_startMs) / 1000u;
  time_t wall = time(NULL);
  struct tm tmv;
  char stamp[32];

#ifdef _WIN32
  localtime_s(&tmv, &wall);
#else
  localtime_r(&wall, &tmv);
#endif
  strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);

  (void)serverMemReadProcess(&pm);

  memLineAdd(&l, "[mem %s] up %uh%02um%02us | tick %u | rounds %u | map %s",
             stamp, (unsigned)(up / 3600u), (unsigned)((up / 60u) % 60u),
             (unsigned)(up % 60u), (unsigned)serverSimGetTick(sim),
             (unsigned)s_rounds, sim->mapName);
  memLineAdd(&l, " | ");
  memLineProcess(&l, &pm);

  for (int i = 0; i < MAX_TANKS; i++) {
    botValid[i] = botMemLuaHeapBytes(sim, (BYTE)i, &botBytes[i]);
    if (botValid[i]) {
      luaTotal += botBytes[i];
      bots++;
    }
  }
  memLineAdd(&l, " | lua %d bots %.1f MB", bots, memMB(luaTotal));
  if (s_havePrev) {
    memLineAdd(&l, " (%+.1f MB)", memDeltaMB(luaTotal, s_prevLuaTotal));
  }
  if (bots > 0) {
    memLineAdd(&l, ":");
  }
  for (int i = 0; i < MAX_TANKS; i++) {
    if (!botValid[i]) {
      continue;
    }
    memLineAdd(&l, " p%d %u KB", i, (unsigned)(botBytes[i] / 1024u));
    if (s_havePrev && s_prevBotValid[i]) {
      memLineAdd(&l, " (%+d)", (int)(((long long)botBytes[i] -
                                       (long long)s_prevBotBytes[i]) / 1024));
    }
  }

  /* Private bytes include every Lua heap (LuaJIT and PUC-Lua both take
   * their heap from the process), so what is left is the engine's own C
   * heap plus code and JIT machine code. On Linux "private" is VmData,
   * on macOS phys_footprint. */
  if (pm.priv > luaTotal) {
    nonLua = pm.priv - luaTotal;
  }
  memLineAdd(&l, " | private minus lua ");
  memLineMB(&l, pm.havePriv, nonLua);
  if (s_havePrev && pm.havePriv && s_prevNonLuaValid) {
    memLineAdd(&l, " (%+.1f MB)", memDeltaMB(nonLua, s_prevNonLua));
  }

  botMemCountLists(&sim->sim, &lists);
  memLineAdd(&l, " | lists server: ");
  memLineLists(&l, &lists);
  {
    int n = botMemSumBotClientLists(sim, &lists);
    memLineAdd(&l, " | lists bots (%d): ", n);
    memLineLists(&l, &lists);
  }
  memLineAdd(&l, " | recording brain-debug %s, game log %s",
             brainRecordIsEnabled() ? "on" : "off",
             logIsRecording() ? "on" : "off");

  serverMessageConsoleMessage(sim, buf);

  if (deep) {
    for (int i = 0; i < MAX_TANKS; i++) {
      if (botValid[i]) {
        (void)botMemLuaDeepWalk(sim, (BYTE)i, MEMREPORT_DEEP_TOP,
                                memDeepEmit, sim);
      }
    }
  }

  for (int i = 0; i < MAX_TANKS; i++) {
    s_prevBotBytes[i] = botValid[i] ? botBytes[i] : 0;
    s_prevBotValid[i] = botValid[i];
  }
  s_prevLuaTotal = luaTotal;
  s_prevNonLua = nonLua;
  s_prevNonLuaValid = pm.havePriv;
  s_havePrev = true;
}

void serverMemReportPoll(ServerSim *sim) {
  Uint64 nowMs;
  int req;
  bool due;
  bool running;

  if (sim == NULL) {
    return;
  }
  nowMs = SDL_GetTicks();
  if (s_startMs == 0) {
    s_startMs = nowMs;
    s_lastMs = nowMs;
  }
  /* Rounds played: count each entry into the running state. */
  running = (sim->state == serverStateRunning);
  if (running && !s_wasRunning) {
    s_rounds++;
  }
  s_wasRunning = running;

  /* Take and clear in one step, so a request the console makes now is
   * either in req or left for the next tick, never dropped. */
  req = SDL_SetAtomicInt(&s_request, 0);
  due = (s_periodMs > 0 && nowMs - s_lastMs >= s_periodMs);
  if (req == 0 && !due) {
    return;
  }
  if (due) {
    s_lastMs = nowMs;
  }

  /* The bots' lua_States are touched only inside the tick, which has
   * finished; the mutex keeps a console or lobby command that reaches a
   * brain off them while the report reads. */
  threadsWaitForMutex();
  memReportWrite(sim, nowMs, req == 2 || (due && s_deepEvery));
  threadsReleaseMutex();
}
