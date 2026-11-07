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

/* Gives the brain capacity code (brain_capacity.h) the CPU speed test
 * result stored for this version, so Brain.capacity sees coremark_1t and
 * coremark_all. Header only: the cpu_speed_test library does not link the
 * engine, and the engine (built for Android, the web and iOS as well) does
 * not link the speed test. Each program that has both calls this. */

#ifndef WINBOLO_CPU_SPEED_CAPACITY_H
#define WINBOLO_CPU_SPEED_CAPACITY_H

#include <stdbool.h>

#include "brain_capacity.h"
#include "cpu_speed_store.h"

/* Reads the result stored for version and gives it to the brain capacity
 * code. False, and nothing changes, when none is stored. Giving a result
 * forgets the answers kept so far, so call it before brains load, or after
 * a new result is stored. */
static inline bool cpuSpeedApplyStoredToBrains(const char *version) {
  CpuSpeedStored stored;
  if (!cpuSpeedStoreLoad(version, &stored)) {
    return false;
  }
  brainCapacitySetMachineFromTest(stored.result.cpuName,
                                  stored.result.logicalCores,
                                  stored.result.physicalCores,
                                  stored.result.cm1, stored.result.cmAll);
  return true;
}

#endif /* WINBOLO_CPU_SPEED_CAPACITY_H */
