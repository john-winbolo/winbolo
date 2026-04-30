/*
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
 *********************************************************/
bool brainCoreCallThink(lua_State *L, BrainInfo *info);

/*********************************************************
 *NAME:          brainCoreCallMethod
 *PURPOSE:
 *  Calls brain.open(info) or brain.close(info).
 *  Missing methods are silently ignored.
 *  Returns false on Lua error.
 *********************************************************/
bool brainCoreCallMethod(lua_State *L, BrainInfo *info, const char *method);

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

#endif /* BRAINCORE_H */
