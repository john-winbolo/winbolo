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

/* The dedicated server's memory report: one line, every few minutes, that
 * says how much memory the process holds and where it sits (each bot's Lua
 * heap, the world's object lists), so a server that runs for days can show
 * which part grows. The operator console's "mem" asks for one now, and
 * "memdeep" adds a walk of each bot's Lua tables. */

#ifndef WINBOLO_SERVER_MEMREPORT_H
#define WINBOLO_SERVER_MEMREPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ServerSim;

/* Default period of the timed report, in minutes. */
#define SERVER_MEMREPORT_DEFAULT_MINUTES 10

/* Set the period of the timed report (minutes; 0 turns the timed report
 * off — "mem" on the console still works) and whether every timed report
 * also does the deep walk. Call once at startup. */
void serverMemReportConfigure(double periodMinutes, bool deepEveryReport);

/* Ask for a report at the next tick. Safe from any thread: the console
 * thread calls it, and the tick thread does the work. */
void serverMemReportRequest(bool deep);

/* Called by the tick driver after each serverInstanceTick, with the tick
 * lock held and the server mutex NOT held. Writes a report when one is due
 * or was asked for; otherwise costs one clock read. */
void serverMemReportPoll(struct ServerSim *sim);

/* The process's own memory, in bytes. Fields the platform cannot give are
 * 0. Returns false when nothing could be read. */
typedef struct {
  uint64_t rss;      /* resident set / working set */
  uint64_t peakRss;  /* high-water mark of rss */
  uint64_t priv;     /* private bytes (Windows) / VmData (Linux) */
  uint64_t virt;     /* virtual address space in use */
} ServerMemProcess;

bool serverMemReadProcess(ServerMemProcess *out);

#endif /* WINBOLO_SERVER_MEMREPORT_H */
