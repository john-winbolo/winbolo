/*
 * The CPU speed test, its stored result, and the brain capacity answer.
 *
 * run_cpu_speed_test_short            - a short run of the CoreMark test: every
 *                                       CRC right, both scores above 0, and
 *                                       physical cores <= logical cores
 * run_cpu_speed_test_cancel           - a cancel flag that is already set
 *                                       stops the test, with nothing to use
 * run_cpu_speed_store_roundtrip       - a result saved to a scratch file loads
 *                                       back for the same version only; a
 *                                       cancelled or bad-CRC result is never
 *                                       saved
 * run_cpu_speed_background            - the background run stores its result
 *                                       with its context, and does not run
 *                                       again once one is stored
 * run_cpu_speed_background_cancel     - a background run cancelled at once
 *                                       stores nothing
 * run_brain_capacity_default_clamp    - the default (logical - 1, at least 1)
 *                                       and the clamp of a raw answer to 0..16
 * run_brain_capacity_cache            - answers are kept by name + version
 * run_brain_capacity_c_brain          - a registered C brain's answer, and its
 *                                       "use the default"
 * run_brain_capacity_name_from_path   - the key name from a brain's path
 * run_brain_capacity_lua_fallback     - Brain.capacity through a real Lua
 *                                       state: a number, an error, a string,
 *                                       nil, no function, and the kept answer
 * run_brain_capacity_add_state        - the lobby's Add Bot rule: measuring,
 *                                       full, room left, and bots that run on
 *                                       another machine
 */

#include <math.h>
#include <string.h>
#include <SDL3/SDL.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "test_harness.h"
#include "cpu_speed_test.h"
#include "cpu_speed_store.h"
#include "brain_capacity.h"

/* A run of a few tens of milliseconds: enough iterations for the CRCs,
   nowhere near the 2 s test. */
static void shortParams(CpuSpeedParams *p) {
  cpuSpeedTestDefaultParams(p);
  p->warmIterations = 20;
  p->phaseSecs = 0.02;
  p->minIterations = 10;
  p->threads = 2;
}

int run_cpu_speed_test_short(void) {
  CpuSpeedParams p;
  CpuSpeedResult r;

  shortParams(&p);
  UT_ASSERT(cpuSpeedTestRun(&p, &r));
  UT_ASSERT(r.crcOk);
  UT_ASSERT(r.cancelled == false);
  UT_ASSERT_MSG(r.cm1 > 0.0, "cm1 %f", r.cm1);
  UT_ASSERT_MSG(r.cmAll > 0.0, "cmAll %f", r.cmAll);
  UT_ASSERT(r.threads == 2);
  UT_ASSERT(r.logicalCores >= 1);
  UT_ASSERT(r.physicalCores >= 1);
  UT_ASSERT_MSG(r.physicalCores <= r.logicalCores, "%d physical, %d logical",
                r.physicalCores, r.logicalCores);
  UT_ASSERT(r.logicalCores == cpuSpeedLogicalCores());
  UT_ASSERT(cpuSpeedPhysicalCores() <= cpuSpeedLogicalCores());
  UT_ASSERT(r.oneIterations >= p.minIterations);
  UT_ASSERT(r.allIterationsPerThread >= p.minIterations);
  UT_ASSERT(r.totalSecs > 0.0);
  {
    char line[1024];
    cpuSpeedFormatJson(&r, line, sizeof(line));
    UT_ASSERT(strstr(line, "\"crc_ok\":true") != NULL);
    UT_ASSERT(strstr(line, "\"cm_1t\":") != NULL);
  }
  return 0;
}

int run_cpu_speed_test_cancel(void) {
  CpuSpeedParams p;
  CpuSpeedResult r;
  SDL_AtomicInt cancel;

  shortParams(&p);
  SDL_SetAtomicInt(&cancel, 1);
  p.cancel = &cancel;
  UT_ASSERT(cpuSpeedTestRun(&p, &r) == false);
  UT_ASSERT(r.cancelled);
  return 0;
}

static bool useScratchStore(const char *leaf) {
  char path[1024];

  if (!utScratchPath(path, sizeof(path), leaf)) {
    return false;
  }
  cpuSpeedStoreSetPath(path);
  return true;
}

int run_cpu_speed_store_roundtrip(void) {
  CpuSpeedResult r;
  CpuSpeedStored st;

  UT_ASSERT(useScratchStore("cpu_speed.json"));
  UT_ASSERT(cpuSpeedStoreLoad("9.9.9", &st) == false); /* none yet */

  memset(&r, 0, sizeof(r));
  cpuSpeedCpuName(r.cpuName, sizeof(r.cpuName));
  r.logicalCores = cpuSpeedLogicalCores();
  r.physicalCores = cpuSpeedPhysicalCores();
  r.threads = r.logicalCores;
  r.cm1 = 25000.0;
  r.cmAll = 67000.0;
  r.scaling = r.cmAll / r.cm1;
  r.crcOk = true;
  r.totalSecs = 2.0;

  /* A cancelled run and a bad CRC are never stored. */
  r.cancelled = true;
  UT_ASSERT(cpuSpeedStoreSave("9.9.9", CPU_SPEED_CONTEXT_LOBBY, &r) == false);
  r.cancelled = false;
  r.crcOk = false;
  UT_ASSERT(cpuSpeedStoreSave("9.9.9", CPU_SPEED_CONTEXT_LOBBY, &r) == false);
  r.crcOk = true;
  UT_ASSERT(cpuSpeedStoreLoad("9.9.9", &st) == false);

  UT_ASSERT(cpuSpeedStoreSave("9.9.9", CPU_SPEED_CONTEXT_LOBBY, &r));
  UT_ASSERT(cpuSpeedStoreLoad("9.9.9", &st));
  UT_ASSERT(strcmp(st.version, "9.9.9") == 0);
  UT_ASSERT(strcmp(st.context, CPU_SPEED_CONTEXT_LOBBY) == 0);
  UT_ASSERT(st.result.cm1 == 25000.0);
  UT_ASSERT(st.result.cmAll == 67000.0);
  UT_ASSERT(st.result.logicalCores == r.logicalCores);
  UT_ASSERT(st.result.physicalCores == r.physicalCores);
  UT_ASSERT(st.result.crcOk);

  /* Another version runs the test again. */
  UT_ASSERT(cpuSpeedStoreLoad("9.9.10", &st) == false);

  cpuSpeedStoreSetPath(NULL);
  return 0;
}

int run_cpu_speed_background(void) {
  CpuSpeedParams p;
  CpuSpeedStored st;

  UT_ASSERT(useScratchStore("cpu_speed_bg.json"));
  shortParams(&p);
  cpuSpeedBackgroundSetParams(&p);

  UT_ASSERT(cpuSpeedBackgroundStart("9.9.9", CPU_SPEED_CONTEXT_LOBBY, NULL));
  UT_ASSERT(cpuSpeedBackgroundWait(30000));
  UT_ASSERT(cpuSpeedBackgroundRunning() == false);
  UT_ASSERT(cpuSpeedStoreLoad("9.9.9", &st));
  UT_ASSERT(strcmp(st.context, CPU_SPEED_CONTEXT_LOBBY) == 0);
  UT_ASSERT(st.result.cm1 > 0.0 && st.result.cmAll > 0.0);

  /* Stored for this version: no second run. */
  UT_ASSERT(cpuSpeedBackgroundStart("9.9.9", CPU_SPEED_CONTEXT_LOBBY, NULL) ==
            false);
  UT_ASSERT(cpuSpeedBackgroundRunning() == false);

  cpuSpeedBackgroundSetParams(NULL);
  cpuSpeedStoreSetPath(NULL);
  return 0;
}

int run_cpu_speed_background_cancel(void) {
  CpuSpeedParams p;
  CpuSpeedStored st;

  UT_ASSERT(useScratchStore("cpu_speed_cancel.json"));
  /* Long enough that the cancel lands before the end. */
  cpuSpeedTestDefaultParams(&p);
  p.phaseSecs = 5.0;
  p.threads = 2;
  cpuSpeedBackgroundSetParams(&p);

  UT_ASSERT(cpuSpeedBackgroundStart("9.9.9", CPU_SPEED_CONTEXT_LOBBY, NULL));
  cpuSpeedBackgroundCancel();
  UT_ASSERT(cpuSpeedBackgroundWait(30000));
  UT_ASSERT(cpuSpeedStoreLoad("9.9.9", &st) == false);

  cpuSpeedBackgroundSetParams(NULL);
  cpuSpeedStoreSetPath(NULL);
  return 0;
}

static BrainMachineInfo machineWith(int logical) {
  BrainMachineInfo m;
  memset(&m, 0, sizeof(m));
  m.logicalCores = logical;
  m.physicalCores = logical;
  return m;
}

int run_brain_capacity_default_clamp(void) {
  BrainMachineInfo m12 = machineWith(12);
  BrainMachineInfo m4 = machineWith(4);
  BrainMachineInfo m1 = machineWith(1);
  BrainMachineInfo m0 = machineWith(0);
  BrainMachineInfo m64 = machineWith(64);

  UT_ASSERT(brainCapacityDefault(&m12) == 11);
  UT_ASSERT(brainCapacityDefault(&m4) == 3);
  UT_ASSERT(brainCapacityDefault(&m1) == 1);
  UT_ASSERT(brainCapacityDefault(&m0) == 1);
  UT_ASSERT(brainCapacityDefault(&m64) == BRAIN_CAPACITY_MAX);
  UT_ASSERT(brainCapacityDefault(NULL) == 1);

  /* No answer, or not a finite number: the default. */
  UT_ASSERT(brainCapacityFromAnswer(false, 5.0, &m12) == 11);
  UT_ASSERT(brainCapacityFromAnswer(true, NAN, &m12) == 11);
  UT_ASSERT(brainCapacityFromAnswer(true, INFINITY, &m4) == 3);
  /* A number: rounded down and clamped to 0..16. */
  UT_ASSERT(brainCapacityFromAnswer(true, 5.0, &m12) == 5);
  UT_ASSERT(brainCapacityFromAnswer(true, 3.9, &m12) == 3);
  UT_ASSERT(brainCapacityFromAnswer(true, 0.0, &m12) == 0);
  UT_ASSERT(brainCapacityFromAnswer(true, -3.0, &m12) == 0);
  UT_ASSERT(brainCapacityFromAnswer(true, 16.0, &m12) == 16);
  UT_ASSERT(brainCapacityFromAnswer(true, 99.0, &m12) == 16);
  UT_ASSERT(brainCapacityFromAnswer(true, 1e30, &m12) == 16);
  return 0;
}

int run_brain_capacity_cache(void) {
  brainCapacityClear();
  UT_ASSERT(brainCapacityLookup("UtBrain", "") == BRAIN_CAPACITY_UNKNOWN);
  brainCapacityStore("UtBrain", "", 4);
  brainCapacityStore("UtBrain", "2", 7);
  brainCapacityStore("UtOther", NULL, 40); /* clamped */
  UT_ASSERT(brainCapacityLookup("UtBrain", "") == 4);
  UT_ASSERT(brainCapacityLookup("UtBrain", NULL) == 4);
  UT_ASSERT(brainCapacityLookup("UtBrain", "2") == 7);
  UT_ASSERT(brainCapacityLookup("UtBrain", "3") == BRAIN_CAPACITY_UNKNOWN);
  UT_ASSERT(brainCapacityLookup("UtOther", "") == 16);
  brainCapacityStore("UtBrain", "", 2);
  UT_ASSERT(brainCapacityLookup("UtBrain", "") == 2);
  brainCapacityClear();
  UT_ASSERT(brainCapacityLookup("UtBrain", "2") == BRAIN_CAPACITY_UNKNOWN);
  return 0;
}

static int cBrainFive(const BrainMachineInfo *m, const BrainMeasured *meas) {
  (void)m;
  return meas == NULL ? 5 : 0;
}
static int cBrainDefault(const BrainMachineInfo *m, const BrainMeasured *meas) {
  (void)m;
  (void)meas;
  return -1;
}
static int cBrainBig(const BrainMachineInfo *m, const BrainMeasured *meas) {
  (void)m;
  (void)meas;
  return 40;
}

int run_brain_capacity_c_brain(void) {
  BrainMachineInfo m;

  brainCapacityClear();
  brainCapacitySetMachine(NULL);
  brainCapacityGetMachine(&m);
  UT_ASSERT(m.logicalCores >= 1);
  UT_ASSERT(m.coremark1t == 0.0); /* no speed test given */

  UT_ASSERT(brainCapacityQueryC("UtCBrain", "") == BRAIN_CAPACITY_UNKNOWN);
  brainCapacityRegisterC("UtCBrain", cBrainFive);
  UT_ASSERT(brainCapacityQueryC("UtCBrain", "") == 5);
  UT_ASSERT(brainCapacityLookup("UtCBrain", "") == 5);
  brainCapacityRegisterC("UtCBrain", cBrainDefault);
  UT_ASSERT(brainCapacityQueryC("UtCBrain", "1") == brainCapacityDefault(&m));
  brainCapacityRegisterC("UtCBrain", cBrainBig);
  UT_ASSERT(brainCapacityQueryC("UtCBrain", "2") == 16);
  brainCapacityRegisterC("UtCBrain", NULL);
  UT_ASSERT(brainCapacityQueryC("UtCBrain", "3") == BRAIN_CAPACITY_UNKNOWN);

  /* The speed test's facts reach the brain, and forget the kept answers. */
  brainCapacitySetMachineFromTest("Ut CPU", 8, 4, 25000.0, 90000.0);
  brainCapacityGetMachine(&m);
  UT_ASSERT(strcmp(m.cpuName, "Ut CPU") == 0);
  UT_ASSERT(m.logicalCores == 8 && m.physicalCores == 4);
  UT_ASSERT(m.coremark1t == 25000.0 && m.coremarkAll == 90000.0);
  UT_ASSERT(fabs(m.scaling - 3.6) < 1e-9);
  UT_ASSERT(brainCapacityLookup("UtCBrain", "") == BRAIN_CAPACITY_UNKNOWN);

  brainCapacitySetMachine(NULL);
  brainCapacityClear();
  return 0;
}

int run_brain_capacity_name_from_path(void) {
  char n[64];

  brainCapacityNameFromPath("brains/GoalHunter/init.lua", n, sizeof(n));
  UT_ASSERT(strcmp(n, "GoalHunter") == 0);
  brainCapacityNameFromPath("C:\\x\\Brains\\GoalHunter\\init.lua", n, sizeof(n));
  UT_ASSERT(strcmp(n, "GoalHunter") == 0);
  brainCapacityNameFromPath("brains/GoalHunter/opt/init.lua", n, sizeof(n));
  UT_ASSERT(strcmp(n, "GoalHunter") == 0);
  brainCapacityNameFromPath("brains/GoalHunter/", n, sizeof(n));
  UT_ASSERT(strcmp(n, "GoalHunter") == 0);
  brainCapacityNameFromPath("idle.lua", n, sizeof(n));
  UT_ASSERT(strcmp(n, "idle") == 0);
  brainCapacityNameFromPath("", n, sizeof(n));
  UT_ASSERT(n[0] == '\0');
  brainCapacityNameFromPath(NULL, n, sizeof(n));
  UT_ASSERT(n[0] == '\0');
  return 0;
}

/* Runs src as the brain script, keeps the table it returns as "brain", and
   asks it for its answer under name. */
static int luaCapacity(const char *src, const char *name) {
  lua_State *L = luaL_newstate();
  int value = -100;

  if (L == NULL) {
    return value;
  }
  luaL_openlibs(L);
  if (luaL_dostring(L, src) == 0) {
    lua_setglobal(L, "brain");
    value = brainCapacityResolveLua(L, NULL, name);
    if (lua_gettop(L) != 0) {
      value = -200; /* the stack was not left as it was */
    }
  }
  lua_close(L);
  return value;
}

int run_brain_capacity_lua_fallback(void) {
  BrainMachineInfo m;
  int def;

  brainCapacityClear();
  brainCapacitySetMachineFromTest("Ut CPU", 6, 6, 30000.0, 150000.0);
  brainCapacityGetMachine(&m);
  def = brainCapacityDefault(&m); /* 5 */
  UT_ASSERT(def == 5);

  /* A number, made from the machine table it was handed. */
  UT_ASSERT(luaCapacity("return { capacity = function(m, meas) "
                        "assert(meas == nil); assert(m.cpu_name == 'Ut CPU'); "
                        "assert(m.scaling == 5); assert(m.on_battery ~= nil); "
                        "return m.physical_cores * m.coremark_1t / 25000 end }",
                        "UtLuaNum") == 7);
  /* An error, a string, nil and no function give the default. */
  UT_ASSERT(luaCapacity("return { capacity = function() error('boom') end }",
                        "UtLuaErr") == def);
  UT_ASSERT(luaCapacity("return { capacity = function() return 'six' end }",
                        "UtLuaStr") == def);
  UT_ASSERT(luaCapacity("return { capacity = function() return nil end }",
                        "UtLuaNil") == def);
  UT_ASSERT(luaCapacity("return { think = function() end }", "UtLuaNone") ==
            def);
  /* Clamped. */
  UT_ASSERT(luaCapacity("return { capacity = function() return 99 end }",
                        "UtLuaBig") == 16);
  /* Not a table: no answer. */
  UT_ASSERT(luaCapacity("return 5", "UtLuaBad") == BRAIN_CAPACITY_UNKNOWN);

  /* Asked once per name + version: the kept answer wins over a new one. */
  UT_ASSERT(luaCapacity("return { capacity = function() return 2 end }",
                        "UtLuaNum") == 7);
  /* A new version is asked again. */
  UT_ASSERT(luaCapacity("return { version = 'b', capacity = function() "
                        "return 2 end }", "UtLuaNum") == 2);

  brainCapacitySetMachine(NULL);
  brainCapacityClear();
  return 0;
}

int run_brain_capacity_add_state(void) {
  /* Bots that run here. */
  UT_ASSERT(brainCapacityAddState(true, 0, 5, true) ==
            BRAIN_CAPACITY_ADD_MEASURING);
  UT_ASSERT(brainCapacityAddState(false, 0, 5, true) == BRAIN_CAPACITY_ADD_OK);
  UT_ASSERT(brainCapacityAddState(false, 4, 5, true) == BRAIN_CAPACITY_ADD_OK);
  UT_ASSERT(brainCapacityAddState(false, 5, 5, true) ==
            BRAIN_CAPACITY_ADD_FULL);
  UT_ASSERT(brainCapacityAddState(false, 9, 5, true) ==
            BRAIN_CAPACITY_ADD_FULL);
  UT_ASSERT(brainCapacityAddState(false, 0, 0, true) ==
            BRAIN_CAPACITY_ADD_FULL);
  /* No answer: no cap. */
  UT_ASSERT(brainCapacityAddState(false, 15, BRAIN_CAPACITY_UNKNOWN, true) ==
            BRAIN_CAPACITY_ADD_OK);
  /* A bot removed: room again. */
  UT_ASSERT(brainCapacityAddState(false, 4, 5, true) == BRAIN_CAPACITY_ADD_OK);
  /* Bots on another machine: this machine's numbers do not apply. */
  UT_ASSERT(brainCapacityAddState(true, 9, 5, false) == BRAIN_CAPACITY_ADD_OK);
  UT_ASSERT(brainCapacityAddState(false, 9, 5, false) ==
            BRAIN_CAPACITY_ADD_OK);
  return 0;
}
