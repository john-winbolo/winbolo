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
 *Name:          Server Simulation Memory Figures
 *Filename:      server_sim_memory.c
 *Author:        John Morrison
 *Purpose:
 *  The T1 memory accessors on server_sim.h: this process's
 *  own memory, read from the platform, and the figures the
 *  dedicated server's memory report prints (each running
 *  bot's Lua heap, the world-list counts of the sim and of
 *  the bots' ClientSims), filled into a plain struct for a
 *  caller that holds only the opaque handle. The desktop
 *  client's System Info panel reads them; the dedicated
 *  server's report (server_memreport.c) reads the process
 *  figures through here too, so the platform code has one
 *  copy. The deep walk of a bot's Lua tables stays in the
 *  report: it pauses the game and allocates inside the
 *  bot's state, so it is a dedicated-server console command.
 *********************************************************/

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

#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "bot_mem_report.h"
#include "threads.h"

bool serverSimReadProcessMemory(ServerSimProcessMemory *out) {
  if (out == NULL) {
    return false;
  }
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
    /* MACH_TASK_BASIC_INFO gives resident and its peak. macOS has no
     * "private bytes"; the nearest is phys_footprint from TASK_VM_INFO,
     * the figure Activity Monitor shows as "Memory": dirty and compressed
     * pages the process owns, which takes in every Lua heap.
     *
     * Virtual stays n/a. virtual_size counts the shared library cache and
     * the address ranges the kernel reserves in every process, so it reads
     * about 400 GB for any process on Apple silicon (ps -o vsz says the
     * same for a shell), and task_info has no committed-address-space
     * figure like the Windows and Linux ones. A leak on a Mac shows in
     * private. */
    mach_task_basic_info_data_t info;
    task_vm_info_data_t vm;
    mach_msg_type_number_t count;
    bool ok = false;

    count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) == KERN_SUCCESS) {
      out->rss = info.resident_size;
      out->peakRss = info.resident_size_max;
      out->haveRss = out->havePeakRss = true;
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
  /* Emscripten and anything else: no process figures, every flag false. */
  return false;
#endif
}

static void memListsCopy(ServerSimListCounts *out, const BotMemListCounts *in) {
  out->shells          = in->shells;
  out->explosions      = in->explosions;
  out->tankExplosions  = in->tankExplosions;
  out->minesExplosions = in->minesExplosions;
  out->floodFill       = in->floodFill;
  out->building        = in->building;
  out->rubble          = in->rubble;
  out->swamp           = in->swamp;
  out->grass           = in->grass;
}

void serverSimGetMemoryInfo(ServerSim *sim, ServerSimMemoryInfo *out) {
  BotMemListCounts lists;
  int i;

  if (out == NULL) {
    return;
  }
  memset(out, 0, sizeof(*out));
  (void)serverSimReadProcessMemory(&out->process);
  if (sim == NULL) {
    return;
  }

  /* The bots' lua_States are touched only inside the tick, which holds the
   * mutex for its whole length; holding it here keeps the next tick, and a
   * lobby command that reaches a brain, off them while this reads. The
   * mutex counts its depth, so a caller already holding it is fine. */
  threadsWaitForMutex();
  for (i = 0; i < MAX_TANKS; i++) {
    size_t bytes = 0;
    out->botLuaValid[i] = botMemLuaHeapBytes(sim, (BYTE)i, &bytes);
    if (out->botLuaValid[i]) {
      out->botLuaBytes[i] = (uint64_t)bytes;
      out->luaTotalBytes += (uint64_t)bytes;
      out->botCount++;
    }
  }
  botMemCountLists(&sim->sim, &lists);
  memListsCopy(&out->serverLists, &lists);
  out->botListCount = botMemSumBotClientLists(sim, &lists);
  memListsCopy(&out->botLists, &lists);
  threadsReleaseMutex();
}
