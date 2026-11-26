/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* The free CPU of the whole machine (machine_cpu_free.h). */

#include "machine_cpu_free.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <TargetConditionals.h>
#  if !TARGET_OS_IPHONE
#    include <mach/mach.h>
#    include <mach/mach_host.h>
#    define MACHINE_CPU_FREE_MAC 1
#  endif
#endif

/* The first "cpu" line of /proc/stat: user nice system idle iowait irq
 * softirq steal (guest and guest_nice are already in user and nice, so
 * they are not added). Free is idle + iowait only. The total includes
 * steal, so steal counts as busy: on a virtual machine it is the time
 * this machine waited for a real core that the host gave to another
 * machine, and a bot cannot think in that time either. An older kernel
 * gives fewer values; a missing value (steal first) is 0. */
bool machineCpuParseProcStat(const char *line, uint64_t *idle,
                             uint64_t *total) {
    unsigned long long v[8];
    int n, j;
    if (line == NULL || idle == NULL || total == NULL) return false;
    memset(v, 0, sizeof(v));
    n = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0],
               &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    if (n < 4) return false;
    *idle = v[3] + v[4];   /* idle + iowait */
    *total = 0;
    for (j = 0; j < 8; j++) *total += v[j];   /* steal (v[7]) is busy */
    return true;
}

/* The machine's idle and total CPU time counters since it started. */
static bool machineCpuCounters(uint64_t *idle, uint64_t *total) {
#if defined(_WIN32)
    FILETIME fi, fk, fu;
    uint64_t i, k, u;
    if (!GetSystemTimes(&fi, &fk, &fu)) return false;
    i = ((uint64_t)fi.dwHighDateTime << 32) | fi.dwLowDateTime;
    k = ((uint64_t)fk.dwHighDateTime << 32) | fk.dwLowDateTime;
    u = ((uint64_t)fu.dwHighDateTime << 32) | fu.dwLowDateTime;
    /* The kernel time holds the idle time. */
    *idle = i;
    *total = k + u;
    return true;
#elif defined(__linux__) && !defined(__ANDROID__)
    FILE *f = fopen("/proc/stat", "r");
    char line[512];
    bool ok;
    if (f == NULL) return false;
    ok = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    return ok && machineCpuParseProcStat(line, idle, total);
#elif defined(MACHINE_CPU_FREE_MAC)
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        (host_info_t)&info, &count) != KERN_SUCCESS) {
        return false;
    }
    *idle = info.cpu_ticks[CPU_STATE_IDLE];
    *total = (uint64_t)info.cpu_ticks[CPU_STATE_USER] +
             info.cpu_ticks[CPU_STATE_SYSTEM] +
             info.cpu_ticks[CPU_STATE_NICE] +
             info.cpu_ticks[CPU_STATE_IDLE];
    return true;
#else
    (void)idle;
    (void)total;
    return false;
#endif
}

void machineCpuFreeInit(MachineCpuFree *m) {
    if (m != NULL) memset(m, 0, sizeof(*m));
}

uint8_t machineCpuFreePercent(uint64_t idle0, uint64_t total0,
                              uint64_t idle1, uint64_t total1) {
    uint64_t di, dt;
    double pct;
    if (total1 <= total0 || idle1 < idle0) return MACHINE_CPU_FREE_UNKNOWN;
    di = idle1 - idle0;
    dt = total1 - total0;
    if (di > dt) di = dt;
    pct = (double)di * 100.0 / (double)dt + 0.5;
    return (uint8_t)(pct >= 100.0 ? 100 : (int)pct);
}

uint8_t machineCpuFreeSample(MachineCpuFree *m) {
    uint64_t idle, total;
    uint8_t pct;
    if (m == NULL) return MACHINE_CPU_FREE_UNKNOWN;
    if (!machineCpuCounters(&idle, &total)) {
        m->have = false;
        return MACHINE_CPU_FREE_UNKNOWN;
    }
    pct = m->have ? machineCpuFreePercent(m->idle, m->total, idle, total)
                  : (uint8_t)MACHINE_CPU_FREE_UNKNOWN;
    m->have = true;
    m->idle = idle;
    m->total = total;
    return pct;
}
