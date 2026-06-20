/*
 * Copyright (c) 1998-2026 John Morrison.
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
 *Name:          Brain Core
 *Filename:      braincore.h
 *Author:        John Morrison
 *Purpose:
 *  Shared Lua brain helpers used by both the client-side
 *  luabrainshandler.c and server-side server_brains.c.
 *  Contains constant registration, BrainInfo marshaling,
 *  and brain method invocation — all independent of
 *  client globals.
 *********************************************************/

#ifndef BRAINCORE_H
#define BRAINCORE_H

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "global.h"
#include "brain.h"
#include "brain_pathfinder.h"
#include "brain_worldsim.h"
#include "brain_overlay.h"

/*********************************************************
 *NAME:          brainCoreRegisterConstants
 *PURPOSE:
 *  Registers all KEY_*, BUILDMODE_*, TERRAIN_*, and
 *  OBJECT_* constants as Lua globals. Does NOT register
 *  get_terrain() — use brainCoreRegisterGetTerrain for
 *  that, since each caller needs its own world pointer.
 *********************************************************/
void brainCoreRegisterConstants(lua_State *L);

/*********************************************************
 *NAME:          brainCoreRegisterGetTerrain
 *PURPOSE:
 *  Registers the get_terrain(x,y) C closure with a
 *  pointer-to-pointer as upvalue. The caller sets
 *  *worldPtr before each brain tick so the closure reads
 *  the correct map data.
 *
 *ARGUMENTS:
 *  L        - Lua state
 *  worldPtr - Pointer to the BYTE* that points at the
 *             256x256 map. Stored as light userdata
 *             upvalue; must remain valid for the lifetime
 *             of the Lua state.
 *********************************************************/
void brainCoreRegisterGetTerrain(lua_State *L, const BYTE **worldPtr);

/*********************************************************
 *NAME:          brainCoreGetWorldPtrPtr
 *PURPOSE:
 *  Retrieve the worldPtr-pointer stashed by brainCoreRegisterGetTerrain.
 *  Lets bot-specific C modules read raw terrain without going through
 *  the Lua get_terrain closure (avoiding ~50 ns overhead per tile in
 *  hot loops like terrain-factor / pill-stamp builds).
 *
 *  Returns NULL if brainCoreRegisterGetTerrain was never called on
 *  this lua_State. Otherwise the returned pointer-to-pointer dereferences
 *  to the host's currently-active 256×256 BYTE map.
 *********************************************************/
const BYTE **brainCoreGetWorldPtrPtr(lua_State *L);

/*********************************************************
 *NAME:          brainCorePushInfo
 *PURPOSE:
 *  Marshals a BrainInfo struct into a Lua table and pushes
 *  it onto the stack.
 *********************************************************/
void brainCorePushInfo(lua_State *L, const BrainInfo *info);

/*********************************************************
 *NAME:          brainCoreExtractOutput
 *PURPOSE:
 *  Reads the table returned by brain.think() and writes
 *  outputs back into the BrainInfo struct. The return
 *  table must be on top of the stack; does not pop it.
 *********************************************************/
void brainCoreExtractOutput(lua_State *L, BrainInfo *info);

/*********************************************************
 *NAME:          brainCoreCallThink
 *PURPOSE:
 *  Calls brain.think(info) and processes the return table.
 *  Returns false on error.
 *
 *  When out_killed is non-NULL, sets *out_killed = true iff
 *  the pcall failed with the budget-hook sentinel
 *  "tick_budget_exceeded" (Lua prepends <chunk>:<line>:  to
 *  luaL_error messages, so we suffix-match). This lets the
 *  caller distinguish a recoverable budget abort from a real
 *  Lua bug; when out_killed is NULL the caller doesn't care
 *  and gets the legacy false-on-any-error contract.
 *********************************************************/
bool brainCoreCallThink(lua_State *L, BrainInfo *info, bool *out_killed);

/*********************************************************
 *NAME:          brainCoreCallMethod
 *PURPOSE:
 *  Calls brain.open(info) or brain.close(info).
 *  Missing methods are silently ignored.
 *  Returns false on Lua error.
 *********************************************************/
bool brainCoreCallMethod(lua_State *L, BrainInfo *info, const char *method);

/*********************************************************
 *NAME:          brc_write_crash_log
 *PURPOSE:
 *  Writes a unique-per-crash brain_crash_<UTC>_pid<PID>_bot<N>.log
 *  inside DEBUG_SESSION_DIR (read from the Lua global) or CWD when
 *  no session dir is set. Internally called from brainCoreCallThink /
 *  brainCoreCallMethod on Lua pcall failure. Exposed (non-static) so
 *  tests/unit/test_brain_crash_log.c can verify the file format
 *  without standing up a full BrainInfo + brain.think pcall.
 *
 *ARGUMENTS:
 *  L                 - Lua state (used to read state.bot_index,
 *                      state.tick, _G.DEBUG_SESSION_DIR — all
 *                      best-effort, missing/wrong-type fields are
 *                      tolerated)
 *  method            - "think" / "init" / etc., used in the banner
 *                      and filename
 *  err_or_traceback  - The pcall payload (typically the value
 *                      returned by the debug.traceback message
 *                      handler: "<err>\nstack traceback:\n...")
 *********************************************************/
void brc_write_crash_log(lua_State *L,
                         const char *method,
                         const char *err_or_traceback);

/*********************************************************
 *NAME:          brainCoreRegisterPathfinder
 *PURPOSE:
 *  Registers cpf_* Lua globals backed by a per-brain
 *  BrainPathfinder instance. Uses a pointer-to-pointer
 *  upvalue (same pattern as brainCoreRegisterGetTerrain).
 *
 *ARGUMENTS:
 *  L     - Lua state
 *  pfPtr - Pointer to the BrainPathfinder* that the
 *          closures will dereference. Must remain valid
 *          for the lifetime of the Lua state.
 *********************************************************/
void brainCoreRegisterPathfinder(lua_State *L, BrainPathfinder **pfPtr);

/*********************************************************
 *NAME:          brainCoreRegisterWorldSim
 *PURPOSE:
 *  Registers wsim_* Lua globals backed by a per-brain
 *  BrainWorldSim instance. Uses a pointer-to-pointer
 *  upvalue (same pattern as brainCoreRegisterPathfinder).
 *
 *ARGUMENTS:
 *  L      - Lua state
 *  wsPtr  - Pointer to the BrainWorldSim* that the
 *           closures will dereference. Must remain valid
 *           for the lifetime of the Lua state.
 *********************************************************/
void brainCoreRegisterWorldSim(lua_State *L, BrainWorldSim **wsPtr);

/*********************************************************
 *NAME:          brainCoreRegisterOverlay
 *PURPOSE:
 *  Registers overlay_* Lua globals for debug drawing.
 *  Uses a pointer-to-pointer upvalue so the buffer can
 *  be swapped per brain instance.
 *********************************************************/
void brainCoreRegisterOverlay(lua_State *L, OverlayCmdBuffer **bufPtr);

/*********************************************************
 *NAME:          brainCoreRegisterPrintCapture
 *PURPOSE:
 *  Overrides Lua's print() to call a capture callback
 *  in addition to writing to stderr. The callback receives
 *  the concatenated print output as a single string.
 *
 *ARGUMENTS:
 *  L   - Lua state
 *  cb  - Callback function (tick, text, userdata)
 *  ud  - Opaque userdata passed to callback
 *  tickPtr - Pointer to current tick counter (read each call)
 *********************************************************/
typedef void (*BrainPrintCaptureFunc)(uint32_t tick, const char *text, void *userdata);

/* Register a print capture override on a specific Lua state */
void brainCoreRegisterPrintCapture(lua_State *L, BrainPrintCaptureFunc cb,
                                    void *ud, const uint32_t *tickPtr);

/* Set a global print capture that all new Lua brain instances will use.
 * Call before creating any brain instances. */
void brainCoreSetGlobalPrintCapture(BrainPrintCaptureFunc cb, void *ud,
                                     const uint32_t *tickPtr);

/*********************************************************
 *NAME:          Worker-pool threading rule (host callbacks)
 *PURPOSE:
 *  The host-callback setters below must be invoked before
 *  the first parallel brain tick. The bot worker pool runs
 *  the first per-bot tick serially so registration bindings
 *  populate host-side registries in a known order. After
 *  init, callbacks must not be modified while workers run.
 *********************************************************/

/*********************************************************
 *NAME:          braintest_viz_register hook
 *PURPOSE:
 *  Lua binding `braintest_viz_register(id, label, short,
 *  long, default_on)` lets brains contribute rows to
 *  BrainTest's V dialog without BrainTest knowing about
 *  any specific brain at compile time.
 *
 *  brainCoreRegisterVizRegister(L) installs the binding on
 *  a brain Lua state. The binding routes to a static
 *  callback set by the host via brainCoreSetVizRegisterCallback.
 *  Hosts that don't care (WinBolo client, headless server)
 *  leave the callback NULL and the binding silently no-ops.
 *
 *  Returns the registered viz_idx (or -1 on failure) so the
 *  brain can stamp it on subsequent overlay commands.
 *********************************************************/
typedef int (*BrainVizRegisterFunc)(const char *id,
                                     const char *label,
                                     const char *short_desc,
                                     const char *long_desc,
                                     int default_on);

void brainCoreSetVizRegisterCallback(BrainVizRegisterFunc cb);
void brainCoreRegisterVizRegister(lua_State *L);

/*********************************************************
 *NAME:          braintest_panel_register hook
 *PURPOSE:
 *  Lua binding `braintest_panel_register(name, lua_expr)`
 *  lets brains advertise text panels for BrainTest's
 *  Q-toggled side window. The host opens a tab per panel
 *  and polls `lua_expr` periodically; whatever string the
 *  expression returns is rendered as the tab body.
 *
 *  Same shape as the viz register callback: hosts wire up
 *  via brainCoreSetPanelRegisterCallback; non-host runtimes
 *  (game client, headless server) leave it NULL and the
 *  binding silently no-ops.
 *********************************************************/
typedef int (*BrainPanelRegisterFunc)(const char *name,
                                       const char *type,
                                       const char *lua_expr,
                                       const char *shortcut);

void brainCoreSetPanelRegisterCallback(BrainPanelRegisterFunc cb);
void brainCoreRegisterPanelRegister(lua_State *L);

/*********************************************************
 *NAME:          braintest_shotsim_poi_register hook
 *
 *  Lua binding `braintest_shotsim_poi_register(name, lua_expr)`
 *  for surfacing brain-defined points of interest as
 *  one-click endpoint sources in BrainTest's shot-sim
 *  panel. The lua_expr returns either two integers (wx, wy)
 *  when the POI is currently available, or nil. Callback is
 *  set by the host via brainCoreSetShotSimPoiRegisterCallback;
 *  non-host runtimes leave it NULL and the binding silently
 *  no-ops.
 *********************************************************/
typedef int (*BrainShotSimPoiRegisterFunc)(const char *name,
                                            const char *lua_expr);

void brainCoreSetShotSimPoiRegisterCallback(BrainShotSimPoiRegisterFunc cb);
void brainCoreRegisterShotSimPoiRegister(lua_State *L);

/*********************************************************
 *NAME:          viz_detail registry hook
 *
 *  Lua bindings overlay_detail / overlay_detail_text /
 *  overlay_detail_clear let brains register clickable map
 *  primitives with rich text bodies for an interactive
 *  inspection dialog (BrainTest 'D' key). Host (BrainTest)
 *  installs the callbacks; non-host runtimes leave them
 *  NULL and the bindings silently no-op.
 *********************************************************/
typedef int  (*BrainVizDetailRegisterFunc)(const char *id, const char *kind,
                                            float x1, float y1, float x2, float y2,
                                            const char *label);
typedef int  (*BrainVizDetailAppendBodyFunc)(const char *id, const char *line);
typedef void (*BrainVizDetailClearFunc)(void);

void brainCoreSetVizDetailRegisterCallback(BrainVizDetailRegisterFunc cb);
void brainCoreSetVizDetailAppendBodyCallback(BrainVizDetailAppendBodyFunc cb);
void brainCoreSetVizDetailClearCallback(BrainVizDetailClearFunc cb);

/* bt_yield() — host event-pump hook.
 * When set, the Lua global bt_yield() calls this so long-running Brain.open()
 * operations (LOS computation, stamp caches) can keep the window responsive.
 * Non-BrainTest hosts leave this NULL; bt_yield() becomes a no-op.
 *
 * Threading: set once at startup, before any brain instance is created. The
 * callback pointer is read unsynchronized from worker threads during
 * brain.think() / brain.open() (Lua brain code can invoke bt_yield()), so
 * runtime modification after the first brain instance exists is undefined. */
void brainCoreSetYieldCallback(void (*cb)(void));
void brainCoreRegisterVizDetail(lua_State *L);

/* NOTE: pill_contrib bindings used to live here. They were specific to
 * GoalHunter's BrainTest overlay (shift-2 cycle-through-pills), so they
 * moved to brains/GoalHunter/c/gh_overlay_pillcontrib.h to keep this
 * header generic. Hosts that want the overlay should also
 *   #include "gh_overlay_pillcontrib.h"
 * and call naPillContribRegister(L) after brainCore* registrations. */

#endif /* BRAINCORE_H */
