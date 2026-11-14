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
 *Filename:      brain_capacity.h
 *Author:        John Morrison
 *Purpose:
 *  How many bots of one brain this machine can run at
 *  full strength. The engine asks each brain once, when
 *  it is loaded: a Lua brain through the optional
 *  Brain.capacity(machine, measured), a C brain through
 *  a function it registers. A brain that gives no answer,
 *  or a wrong one, gets the default: logical cores - 1,
 *  at least 1. Every answer is clamped to 0..16 and kept
 *  by brain name and brain version.
 *
 *  The machine facts come from the host: the CPU speed
 *  test result (src/cpu_speed) when one is stored, and
 *  the cheap facts (core counts, worker threads, power)
 *  in any case. Nothing here runs the test.
 *
 *  The answer is advice. It does not change how many bots
 *  start; the lobby uses it to stop more bots being
 *  added on this machine. See tests/README.md, "Brain capacity".
 *********************************************************/

#ifndef BRAIN_CAPACITY_H
#define BRAIN_CAPACITY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BRAIN_CAPACITY_MIN 0
#define BRAIN_CAPACITY_MAX 16
#define BRAIN_CAPACITY_UNKNOWN (-1)
#define BRAIN_CAPACITY_CPU_NAME_LEN 128
#define BRAIN_CAPACITY_VERSION_LEN 32
#define BRAIN_CAPACITY_MEASURED_MAX 17 /* bot counts 0..16 */

/* The machine, as a brain sees it (the Lua "machine" table) */
typedef struct {
  char cpuName[BRAIN_CAPACITY_CPU_NAME_LEN]; /* "" when not known */
  int logicalCores;
  int physicalCores;
  double speed1t;  /* CPU speed test, 1 thread; 0 when not measured */
  double speedAll; /* CPU speed test, all cores; 0 when not measured */
  double scaling;     /* speedAll / speed1t; 0 when not measured */
  int workerThreads;  /* The bot worker pool's threads */
  bool onBattery;     /* False when not on battery or not known */
  char version[BRAIN_CAPACITY_VERSION_LEN]; /* WinBolo version */
} BrainMachineInfo;

/* One bot count's think times, measured on this machine (one row of the
   Lua "measured" table, keyed by botCount) */
typedef struct {
  int botCount;
  double thinkMsP50;
  double thinkMsP95;
  double belowTopPct; /* Thinks that ran below the brain's top level, % */
  double overrunPct;  /* Thinks that ran over their budget, % */
} BrainMeasuredRow;

/* What was measured. Not filled yet: the engine passes NULL (Lua nil). */
typedef struct {
  int count;
  BrainMeasuredRow rows[BRAIN_CAPACITY_MEASURED_MAX];
} BrainMeasured;

/* A C brain's capacity function: the answer, or a negative number for "use
   the default". measured may be NULL. */
typedef int (*BrainCapacityFn)(const BrainMachineInfo *machine,
                               const BrainMeasured *measured);

/* The machine facts the host gives. NULL clears them, so the next read
   fills the cheap facts again. version is copied in. */
void brainCapacitySetMachine(const BrainMachineInfo *machine);
/* The machine facts. When the host has given none, the cheap facts:
   SDL's logical core count, physical = logical, no speed test, and the
   power state. */
void brainCapacityGetMachine(BrainMachineInfo *out);

/* Gives the CPU speed test's result as the machine facts: the cheap facts
   (worker threads, power, version) are filled in as brainCapacityGetMachine
   fills them. cpuName may be NULL. When these facts differ from the ones
   given before (or none were), forgets the answers kept so far, as they
   were made from other facts. Giving the same result again keeps them. */
void brainCapacitySetMachineFromTest(const char *cpuName, int logicalCores,
                                     int physicalCores, double speed1t,
                                     double speedAll);

/* logical cores - 1, at least 1, at most BRAIN_CAPACITY_MAX */
int brainCapacityDefault(const BrainMachineInfo *machine);

/* A bot count given by hand (the dedicated server's -botcapacity N): a
   whole number, with nothing after it. 0 is allowed (no bots in this
   process). A number above BRAIN_CAPACITY_MAX gives BRAIN_CAPACITY_MAX, and
   *clamped (may be NULL) is set true. BRAIN_CAPACITY_UNKNOWN for NULL, "",
   a negative number or text that is not a number. */
int brainCapacityParseOverride(const char *text, bool *clamped);

/* A brain's raw answer made safe. ok false (an error, or not a number) or
   a value that is not finite gives the default; any other value is rounded
   down and clamped to BRAIN_CAPACITY_MIN..BRAIN_CAPACITY_MAX. */
int brainCapacityFromAnswer(bool ok, double value,
                            const BrainMachineInfo *machine);

/* The brain name answers are kept under, from the brain's path: the
   directory that holds its init.lua ("brains/GoalHunter/init.lua" and
   "brains/GoalHunter/opt/init.lua" both give "GoalHunter"), the same name
   the brain list shows. A bare file name gives the name without ".lua". */
void brainCapacityNameFromPath(const char *path, char *out, size_t outSize);

/* The answer kept for name + version, or BRAIN_CAPACITY_UNKNOWN. version
   may be NULL or "" for a brain with no version. */
int brainCapacityLookup(const char *name, const char *version);
/* The newest answer kept for name with any version, or
   BRAIN_CAPACITY_UNKNOWN. For a caller that knows the brain but not the
   version its brain.version field gives (the lobby's brain list shows file
   times, not brain.version). */
int brainCapacityLookupName(const char *name);
/* Keeps value (clamped) for name + version. Thread safe. */
void brainCapacityStore(const char *name, const char *version, int value);
/* Forgets every kept answer (tests, and a new speed test result). */
void brainCapacityClear(void);

/* A C brain's capacity function, by brain name. NULL removes it. */
void brainCapacityRegisterC(const char *name, BrainCapacityFn fn);
/* Asks the C brain registered as name, keeps the answer and returns it.
   BRAIN_CAPACITY_UNKNOWN when none is registered under name. */
int brainCapacityQueryC(const char *name, const char *version);

/* Asks the Lua brain in L's "brain" global how many bots of it this machine
   can run: Brain.capacity(machine, measured) in a protected call when the
   brain has one, else the C brain registered under its name, else the
   default. The answer is kept under the brain's name (from path, else
   givenName) and brain.version, so each is asked once; a later call reads
   the kept answer. measured is nil until the engine measures think times.
   An error, nil or an answer that is not a number gives the default.
   BRAIN_CAPACITY_UNKNOWN when "brain" is not a table. */
struct lua_State;
int brainCapacityResolveLua(struct lua_State *L, const char *path,
                            const char *givenName);

/* The most time a brain's capacity code may take: brain.version, an
   __index on "brain", and Brain.capacity together. */
#define BRAIN_CAPACITY_LUA_BUDGET_MS 2000u

/* brainCapacityResolveLua with its own limit: every Lua step runs in a
   protected call under a count hook that stops it once budgetMs pass (0:
   no time limit) or *abortFlag is not 0 (NULL: none). A brain that errors
   or runs too long gives the default. The hook and limit in force before
   the call come back after it. brainCapacityResolveLua is this with
   BRAIN_CAPACITY_LUA_BUDGET_MS and no abort flag. */
struct SDL_AtomicInt;
int brainCapacityResolveLuaEx(struct lua_State *L, const char *path,
                              const char *givenName, unsigned budgetMs,
                              struct SDL_AtomicInt *abortFlag);

/* Put L under the same limit (a count hook) until brainCapacityLimitClear:
   for a brain's chunk run before its capacity question. Note: the hook
   does not run inside LuaJIT compiled code, so turn the JIT off for such a
   VM first. */
void brainCapacityLimitSet(struct lua_State *L, unsigned budgetMs,
                           struct SDL_AtomicInt *abortFlag);
void brainCapacityLimitClear(struct lua_State *L);

/* A memory limit on a VM that only answers the capacity question. While
   it is armed, an allocation that would take the VM past limit bytes
   fails, so the call raises "not enough memory" (string.rep with a very
   large count fails at once instead of holding the thread). It is armed
   only inside the protected calls that run the brain's code
   (brainCapacityMemArm): an allocation of the host's own C code between
   them never fails, so no error is raised outside a protected call.
   Armed, the VM's allocator is wrapped; disarmed, the VM has its own
   allocator back (LuaJIT frees its memory pool at lua_close only when it
   sees its own allocator). m must outlive the VM. */
#define BRAIN_CAPACITY_MEM_LIMIT ((size_t)256 * 1024 * 1024)
typedef struct {
  void *(*allocf)(void *ud, void *ptr, size_t osize, size_t nsize);
  void  *allocd;
  size_t used;    /* bytes the VM holds now (from the VM's own count) */
  size_t limit;
  size_t peak;
} BrainCapacityMem;
/* Note m as L's limit (not armed yet). */
void brainCapacityMemLimitSet(struct lua_State *L, BrainCapacityMem *m,
                              size_t limit);
/* Arm or disarm L's limit; nothing when L has none. */
void brainCapacityMemArm(struct lua_State *L, bool on);

/* --- The lobby's Add Bot rule --- */

typedef enum {
  BRAIN_CAPACITY_ADD_OK = 0,
  BRAIN_CAPACITY_ADD_MEASURING, /* The CPU speed test is running */
  BRAIN_CAPACITY_ADD_FULL       /* This machine runs its capacity already */
} BrainCapacityAddState;

/* Whether the lobby may add one more bot. measuring: the CPU speed test is
   running on this machine. botsHere: bots that run on this machine now.
   capacity: the brain's answer, or BRAIN_CAPACITY_UNKNOWN for no cap.
   botsRunHere: false when the bots run on another machine (a remote server
   or Bot Host); this machine's numbers then do not apply. */
BrainCapacityAddState brainCapacityAddState(bool measuring, int botsHere,
                                            int capacity, bool botsRunHere);

#ifdef __cplusplus
}
#endif

#endif /* BRAIN_CAPACITY_H */
