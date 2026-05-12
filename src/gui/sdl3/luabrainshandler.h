/*
 * $Id$
 *
 * Copyright (c) 1998-2008 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */


/*********************************************************
*Name:          luaBrainsHandler
*Filename:      luabrainshandler.h
*Author:        John Morrison
*Creation Date: 09/03/26
*Last Modified: 09/03/26
*Purpose:
*  SDL3 cross-platform brain handler using embedded Lua 5.4.
*  Replaces win32/brainsHandler.c for the SDL3 build.
*  Brains are .lua files loaded from the Brains/ directory
*  rather than Win32 DLLs.
*
*  Brain scripts must return a table with at minimum a
*  think(info) function. open(info) and close(info) are
*  optional.
*
*  Requires Lua 5.4 headers at third_party/lua-5.4/src/.
*********************************************************/

#ifndef LUA_BRAINS_HANDLER_H
#define LUA_BRAINS_HANDLER_H

#include "global.h"
#include "brain.h"  /* For BrainInfo, aiType */
#include "brain_pathfinder.h"
#include "brain_worldsim.h"
#include "brain_overlay.h"

/* Forward declarations */
struct ClientSim;
struct lua_State;

#ifdef __cplusplus
extern "C" {
#endif

/* Brain type tag — used to dispatch between Lua and ONNX brains */
typedef enum { BRAIN_TYPE_LUA, BRAIN_TYPE_ONNX } BrainType;

/* Directory and file extension constants */
#define LUA_BRAINS_DIR        "Brains"
#define LUA_BRAINS_DEV_DIR    "../brains"  /* Source tree brains for dev */
#define LUA_BRAINS_EXT        ".lua"
#define LUA_BRAINS_EXT_LEN    4
#define LUA_BRAINS_MAX        64
#define LUA_BRAINS_NAME_MAX   256
#define LUA_BRAINS_PATH_MAX   512


/*********************************************************
*NAME:          luaBrainLoadBrains
*PURPOSE:
*  Scans the Brains/ directory for .lua files and
*  populates the internal brain list. Must be called
*  before any other luaBrain* function.
*  Returns whether at least the directory was accessible.
*
*ARGUMENTS: none
*********************************************************/
bool luaBrainLoadBrains(void);

/*********************************************************
*NAME:          luaBrainStart
*PURPOSE:
*  Loads and starts a brain from the given .lua file path.
*  Creates a fresh Lua state, registers all game constants,
*  executes the script, and calls brain.open(info).
*  Returns whether the brain started successfully.
*
*ARGUMENTS:
*  path - Full path to the .lua brain file
*  name - Display name of the brain (for logging)
*********************************************************/
bool luaBrainStart(const char *path, const char *name, struct ClientSim *cs);

/*********************************************************
*NAME:          luaBrainRun
*PURPOSE:
*  Executes the brain's think(info) callback for one game
*  tick. Acquires the client mutex, populates BrainInfo,
*  calls brain.think(), extracts outputs, then releases
*  the mutex. No-op if no brain is running.
*
*ARGUMENTS: none
*********************************************************/
void luaBrainRun(void);

/*********************************************************
*NAME:          luaBrainStop
*PURPOSE:
*  Calls brain.close(info) and destroys the Lua state.
*  Safe to call when no brain is running.
*
*ARGUMENTS: none
*********************************************************/
void luaBrainStop(void);

/*********************************************************
*NAME:          luaBrainShutdown
*PURPOSE:
*  Shuts down the brain subsystem on game exit.
*  Equivalent to brainsHandlerShutdown().
*
*ARGUMENTS: none
*********************************************************/
void luaBrainShutdown(void);

/*********************************************************
*NAME:          luaBrainIsRunning
*PURPOSE:
*  Returns whether a brain is currently active.
*
*ARGUMENTS: none
*********************************************************/
bool luaBrainIsRunning(void);

/*********************************************************
*NAME:          luaBrainGetNum
*PURPOSE:
*  Returns the number of .lua brains found in Brains/.
*
*ARGUMENTS: none
*********************************************************/
int luaBrainGetNum(void);

/*********************************************************
*NAME:          luaBrainGetName
*PURPOSE:
*  Returns the display name (filename without .lua) of
*  the brain at the given index. Returns NULL if out of
*  range.
*
*ARGUMENTS:
*  index - Zero-based index into the brain list
*********************************************************/
const char *luaBrainGetName(int index);

/*********************************************************
*NAME:          luaBrainGetPath
*PURPOSE:
*  Returns the full file path of the brain at the given
*  index. Returns NULL if out of range.
*
*ARGUMENTS:
*  index - Zero-based index into the brain list
*********************************************************/
const char *luaBrainGetPath(int index);

/*********************************************************
*NAME:          luaBrainGetRunningIndex
*PURPOSE:
*  Returns the index of the currently running brain, or
*  -1 if no brain is active.
*
*ARGUMENTS: none
*********************************************************/
int luaBrainGetRunningIndex(void);

/*********************************************************
*NAME:          luaBrainGetType
*PURPOSE:
*  Returns the BrainType (LUA or ONNX) of the brain at
*  the given index. Returns BRAIN_TYPE_LUA if out of range.
*
*ARGUMENTS:
*  index - Zero-based index into the brain list
*********************************************************/
BrainType luaBrainGetType(int index);


/* ------------------------------------------------------------------ */
/* ML brain singleton (ONNX)                                           */
/* ------------------------------------------------------------------ */

/*********************************************************
*NAME:          mlBrainStartSingleton
*PURPOSE:
*  Loads an ONNX model and starts it as the active brain.
*  Stops any currently running brain first.
*  Returns true on success.
*
*ARGUMENTS:
*  path - Path to the .onnx model file
*  name - Display name (for logging)
*  cs   - ClientSim to drive
*********************************************************/
bool mlBrainStartSingleton(const char *path, const char *name, struct ClientSim *cs);

/*********************************************************
*NAME:          mlBrainStopSingleton
*PURPOSE:
*  Stops the active ONNX brain and frees resources.
*  Safe to call when no ONNX brain is running.
*
*ARGUMENTS: none
*********************************************************/
void mlBrainStopSingleton(void);

/*********************************************************
*NAME:          mlBrainRunSingleton
*PURPOSE:
*  Runs one inference tick for the active ONNX brain.
*  No-op if no ONNX brain is running.
*  Returns true on success.
*
*ARGUMENTS:
*  cs - ClientSim to read/write
*********************************************************/
bool mlBrainRunSingleton(struct ClientSim *cs);

/*********************************************************
*NAME:          mlBrainSingletonIsRunning
*PURPOSE:
*  Returns whether an ONNX brain is currently active.
*
*ARGUMENTS: none
*********************************************************/
bool mlBrainSingletonIsRunning(void);

/* ------------------------------------------------------------------ */
/* Brain settings descriptor                                           */
/* ------------------------------------------------------------------ */

/* Setting value types supported by the UI */
typedef enum {
  LUA_BRAIN_SETTING_BOOL,   /* bool  — rendered as a checkbox      */
  LUA_BRAIN_SETTING_INT,    /* int   — rendered as a slider/input  */
  LUA_BRAIN_SETTING_FLOAT,  /* float — rendered as a slider/input  */
  LUA_BRAIN_SETTING_STRING  /* string — rendered as a text input   */
} LuaBrainSettingType;

/* A single brain setting descriptor */
typedef struct {
  char                id[64];    /* Opaque key passed to set_setting()    */
  char                label[128];/* Human-readable label shown in the UI  */
  LuaBrainSettingType type;

  /* Current value — only the field matching type is valid */
  union {
    bool   b;
    int    i;
    float  f;
    char   s[256];
  } value;

  /* Optional range hint for int/float (ignored when min == max == 0) */
  float range_min;
  float range_max;
} LuaBrainSetting;

/*********************************************************
*NAME:          luaBrainGetSettings
*PURPOSE:
*  Calls brain.settings() on the currently running brain
*  and returns a heap-allocated array of LuaBrainSetting
*  descriptors.  Returns NULL if the brain has no
*  settings() function or no brain is running.
*  The caller must free the array with luaBrainFreeSettings().
*
*ARGUMENTS:
*  out_count - receives the number of entries in the array
*********************************************************/
LuaBrainSetting *luaBrainGetSettings(int *out_count);

/*********************************************************
*NAME:          luaBrainSetSetting
*PURPOSE:
*  Calls brain.set_setting(id, value) on the currently
*  running brain. value is passed as the appropriate Lua
*  type for the setting's declared type.
*
*ARGUMENTS:
*  setting - The setting descriptor (id + type + new value)
*********************************************************/
void luaBrainSetSetting(const LuaBrainSetting *setting);

/*********************************************************
*NAME:          luaBrainFreeSettings
*PURPOSE:
*  Frees an array previously returned by luaBrainGetSettings().
*
*ARGUMENTS:
*  settings - Array to free (may be NULL)
*********************************************************/
void luaBrainFreeSettings(LuaBrainSetting *settings);

/* ------------------------------------------------------------------ */
/* Multi-instance brain API                                            */
/* ------------------------------------------------------------------ */

/* A standalone brain instance bound to a specific ClientSim.
 * Multiple instances can coexist (e.g. one per bot).
 * The singleton GUI brain uses this internally. */
typedef struct {
    struct lua_State *L;        /* Lua VM for this brain */
    struct ClientSim *cs;       /* ClientSim this brain drives */
    bool running;               /* Is the brain active? */
    bool isFirst;               /* First THINK flag */
    const BYTE *worldPtr;       /* get_terrain upvalue (updated per-tick) */
    BrainPathfinder *pathfinder; /* C pathfinder instance (cpf_* globals) */
    BrainWorldSim *worldsim;     /* C world simulator instance (wsim_* globals) */
    /* Per-brain overlay command buffer + the pointer-to-pointer the
     * Lua overlay_* closures hold as upvalue. brainCoreRegisterOverlay
     * captures &overlayPtr so we could swap buffers per tick (we
     * don't, but the indirection is what the API expects). */
    OverlayCmdBuffer  overlay;
    OverlayCmdBuffer *overlayPtr;
    BrainInfo bInfo;            /* Per-instance BrainInfo */
    /* True iff the most recent luaBrainInstanceTick aborted via the
     * tick-budget count hook (Lua error suffix
     * "tick_budget_exceeded"). The producer reads this after the
     * worker returns to decide between the survive-with-wasKilled
     * path and the real-error remove-the-bot path. Reset to false at
     * the top of each tick. */
    bool wasKilled;
} LuaBrainInstance;

/*********************************************************
*NAME:          luaBrainInstanceCreate
*PURPOSE:
*  Creates a Lua VM, loads the brain script, registers
*  constants and get_terrain, and calls brain.open().
*  The instance is bound to the given ClientSim.
*  Caller must hold any necessary locks.
*  Returns true on success.
*********************************************************/
bool luaBrainInstanceCreate(LuaBrainInstance *inst, const char *path,
                            const char *name, struct ClientSim *cs,
                            aiType aiMode, bool debug_mode);

/*********************************************************
*NAME:          luaBrainsSetRunScript
*PURPOSE:
*  Sets a Lua script path for --run-script mode.
*  When non-empty, RUN_SCRIPT_PATH is injected as a Lua
*  global so Brain.open can dofile() the script and exit.
*  Call before any brain instance is created.
*
*  Threading: set once at startup, before any brain instance
*  is created. The path is captured into the per-brain
*  RUN_SCRIPT_PATH Lua global at luaBrainInstanceCreate()
*  time; modification after the first instance exists has no
*  effect on already-created brains.
*********************************************************/
void luaBrainsSetRunScript(const char *path);

/*********************************************************
*NAME:          luaBrainsSetProfile
*PURPOSE:
*  Two-flag profiling toggle:
*    profile     — drives BRAIN_PROFILE Lua global. When true,
*                  the brain emits opt() phase markers and
*                  populates opt.last_sections so the BrainTest
*                  Y panel time bar has data.
*    profile_log — drives BRAIN_PROFILE_LOG. When true, optimize.lua
*                  flushes per-tick blocks to optimize.log and
*                  performance.ticks.log. Implies profile.
*
*  Threading: set once at startup, before any brain instance
*  is created. Captured into the per-brain Lua globals at
*  luaBrainInstanceCreate() time; later modification has no
*  effect on already-created brains.
*********************************************************/
void luaBrainsSetProfile(int profile, int profile_log);

/*********************************************************
*NAME:          luaBrainsSetLogJson
*PURPOSE:
*  Drives the BRAIN_LOG_JSON Lua global. When true, the
*  brain opens its JSONL behavior trace files (brain_p<N>.jsonl,
*  goal_player<N>.log) and emits per-tick decision events.
*  Independent of profile/profile-log: behavior trace is about
*  decisions, not performance.
*********************************************************/
void luaBrainsSetLogJson(int enable);

/*********************************************************
*NAME:          luaBrainInstanceTick
*PURPOSE:
*  Runs one brain think cycle: populates BrainInfo from
*  the bound ClientSim, calls brain.think(), extracts
*  outputs back into the ClientSim. Returns false on
*  Lua error (caller should destroy the instance).
*********************************************************/
bool luaBrainInstanceTick(LuaBrainInstance *inst);

/*********************************************************
*NAME:          luaBrainInstanceDestroy
*PURPOSE:
*  Calls brain.close() and destroys the Lua VM.
*  Safe to call on an already-destroyed or zero-init'd
*  instance.
*********************************************************/
void luaBrainInstanceDestroy(LuaBrainInstance *inst);

/*********************************************************
*NAME:          luaBrainSetTickInputs
*PURPOSE:
*  Writes the per-tick host-provided inputs onto the brain's
*  Lua table:
*    brain.lastThinkMs - wall-clock cost of the previous tick
*    brain.targetMs    - per-bot budget for the current tick
*    brain.wasKilled   - set when the previous tick was forced
*                        to abort (always false today; see plan
*                        for the kill-on-overrun follow-up)
*  Brains may read these to scale their work voluntarily; they
*  are not required to do so.
*
*  Producer-thread only — pushes / pops on the brain's own
*  lua_State while the bot's worker thread is idle, so no
*  cross-thread Lua access happens. Stack-balanced (pops every
*  value it pushes).
*********************************************************/
void luaBrainSetTickInputs(LuaBrainInstance *inst,
                           double lastThinkMs,
                           double targetMs,
                           bool   wasKilled);

/*********************************************************
*NAME:          luaBrainInstanceSetDebugMode
*PURPOSE:
*  Updates the BRAIN_DEBUG_MODE Lua global on this instance's
*  VM. Lets BrainTest toggle between "viz-supporting work
*  active" and "production-mode skip" at runtime, so the user
*  can feel the perf cost of debug-only allocations without
*  reloading the brain.
*
*  Note: the brain source itself is whichever was loaded at
*  construction (brains/<bot>/init.lua for debug=true,
*  brains/<bot>/opt/init.lua for debug=false). This setter
*  only flips the runtime gate — `if BRAIN_DEBUG_MODE then`
*  blocks in the un-stripped source will start/stop executing.
*  In stripped opt/ source those blocks were removed by
*  lua_strip and the toggle has no effect.
*********************************************************/
void luaBrainInstanceSetDebugMode(LuaBrainInstance *inst, bool enabled);

/*********************************************************
*NAME:          luaBrainInstanceGetSettings
*PURPOSE:
*  Calls brain.settings() on this instance's Lua VM.
*  Returns a heap-allocated array (free with
*  luaBrainFreeSettings). NULL if no settings function.
*********************************************************/
LuaBrainSetting *luaBrainInstanceGetSettings(LuaBrainInstance *inst,
                                             int *out_count);

/*********************************************************
*NAME:          luaBrainInstanceSetSetting
*PURPOSE:
*  Calls brain.set_setting(id, value) on this instance.
*********************************************************/
void luaBrainInstanceSetSetting(LuaBrainInstance *inst,
                                const LuaBrainSetting *setting);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LUA_BRAINS_HANDLER_H */
