/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* The free (idle) CPU of the whole machine, all cores, as a percent.
 *
 * A Bot Host measures it for its own machine and sends only the percent
 * (CTRL_BOT_HOST_CPU_FREE); the server measures its own the same way. No
 * core count or CPU name leaves the machine.
 *
 *   Windows: GetSystemTimes.
 *   Linux:   the first "cpu" line of /proc/stat (idle + iowait as idle;
 *            steal counts as busy).
 *   macOS:   host_statistics HOST_CPU_LOAD_INFO.
 *   Others (Android, iOS) and any failure: MACHINE_CPU_FREE_UNKNOWN. */

#ifndef WINBOLO_MACHINE_CPU_FREE_H
#define WINBOLO_MACHINE_CPU_FREE_H

#include <stdbool.h>
#include <stdint.h>

#define MACHINE_CPU_FREE_UNKNOWN 0xFFu

typedef struct {
    bool     have;      /* a first reading of the counters was taken */
    uint64_t idle;
    uint64_t total;
} MachineCpuFree;

void machineCpuFreeInit(MachineCpuFree *m);
/* The free percent (0..100) since the last call, or
 * MACHINE_CPU_FREE_UNKNOWN on the first call and on any failure. */
uint8_t machineCpuFreeSample(MachineCpuFree *m);
/* The idle and total counters from the first "cpu" line of /proc/stat
 * (machineCpuParseProcStat; for the unit tests too). False when the line
 * is not a "cpu" line or has fewer than the four first values. */
bool machineCpuParseProcStat(const char *line, uint64_t *idle,
                             uint64_t *total);
/* The percent from two counter readings (for the unit tests too). */
uint8_t machineCpuFreePercent(uint64_t idle0, uint64_t total0,
                              uint64_t idle1, uint64_t total1);

/* The machine's idle and total CPU counters now. False when this program
 * cannot read them (a sandbox). */
bool machineCpuFreeCounters(uint64_t *idle, uint64_t *total);
/* True when this program can read the counters itself. */
bool machineCpuFreeCanRead(void);
/* For a program that cannot read the counters (the Bot Host child in its
 * sandbox): machineCpuFreeSample asks fn when its own read fails. The
 * game sends the child its own readings (bothost_child.h). NULL: none. */
typedef bool (*MachineCpuCounterSource)(uint64_t *idle, uint64_t *total);
void machineCpuFreeSetFallback(MachineCpuCounterSource fn);

#endif /* WINBOLO_MACHINE_CPU_FREE_H */
