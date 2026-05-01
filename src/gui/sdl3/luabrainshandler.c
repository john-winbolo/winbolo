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
*Filename:      luabrainshandler.c
*Author:        John Morrison
*Creation Date: 09/03/26
*Last Modified: 09/03/26
*Purpose:
*  SDL3 cross-platform brain handler using embedded Lua 5.4.
*  Replaces win32/brainsHandler.c for the SDL3 build.
*
*  Each brain is a .lua file in the Brains/ directory that
*  returns a table with the following interface:
*
*    brain.open(info)    -- optional, called on start
*    brain.think(info)   -- required, called each tick
*    brain.close(info)   -- optional, called on stop
*
*  brain.think() must return a table:
*    {
*      holdkeys   = integer,  -- KEY_* bitmask to hold
*      tapkeys    = integer,  -- KEY_* bitmask to tap once
*      build      = nil or {x, y, action},
*      wantallies = integer,  -- PlayerBitMap
*      messagedest = integer, -- PlayerBitMap (0 = debug log)
*      sendmessage = string or nil,
*    }
*
*  All KEY_*, BUILDMODE_*, TERRAIN_*, and OBJECT_* constants
*  are pre-registered as Lua globals before the script runs.
*  get_terrain(x, y) is available as a C closure to read the
*  256x256 world map without copying it each tick.
*
*  Requires Lua 5.4. Headers expected at:
*    third_party/lua-5.4/src/lua.h
*    third_party/lua-5.4/src/lualib.h
*    third_party/lua-5.4/src/lauxlib.h
*********************************************************/

/* Must come before any Windows headers to prevent winsock.h / ws2def.h conflicts */
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <windows.h>
#endif

#include <SDL3/SDL.h>
#include <string.h>
#include <stdlib.h>

#include <stdio.h>   /* fopen, fprintf — for brain_error.log writes */

#ifndef _WIN32
#  include <dirent.h>
#  include <sys/stat.h>
#endif

/* Lua 5.4 — path resolved via CMake include_directories */
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "../../common/wb_log.h"
#include "../../bolo/global.h"
#include "../../bolo/brain.h"
#include "../../bolo/screen.h"
#include "../../bolo/client_sim.h"
#include "../../bolo/util.h"
#include "../../bolo/braincore.h"
#include "../clientmutex.h"
#include "../gamefront.h"
#include "luabrainshandler.h"


/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

static LuaBrainInstance singletonInst;           /* The GUI client's brain  */
static int        brainsNum          = 0;        /* Discovered brain count  */
static int        brainsRunningIdx   = -1;       /* Index of active brain   */
static bool       brainsProcExecuting = false;   /* Re-entrancy guard       */

static char      brainsNames[LUA_BRAINS_MAX][LUA_BRAINS_NAME_MAX];
static char      brainsPaths[LUA_BRAINS_MAX][LUA_BRAINS_PATH_MAX];
static BrainType brainsTypes[LUA_BRAINS_MAX];    /* LUA or ONNX per entry */

#if defined(HAVE_ONNXRUNTIME) && !defined(__EMSCRIPTEN__)
#include "../../bolo/ml_brain.h"
static MLBrainInstance *mlSingletonInst = NULL;  /* Active ONNX brain */
static ClientSim      *mlSingletonCS   = NULL;   /* ClientSim for ONNX brain */
#endif


/* ================================================================== */
/* Lua C closures and helpers                                          */
/* ================================================================== */

/* l_get_terrain and constants are now in braincore.c.
 * brainCoreRegisterGetTerrain() takes a pointer-to-pointer; each
 * LuaBrainInstance has its own worldPtr field for this. */

/* Original implementations removed — now in braincore.c */
#if 0
static void push_brain_info_REMOVED(lua_State *L, const BrainInfo *info) {
  int i;

  lua_newtable(L);

  /* Version */
  lua_pushinteger(L, info->BoloVersion);    lua_setfield(L, -2, "bolo_version");
  lua_pushinteger(L, info->InfoVersion);    lua_setfield(L, -2, "info_version");

  /* Player roster */
  lua_pushinteger(L, info->player_number);  lua_setfield(L, -2, "player_number");
  lua_pushinteger(L, info->num_players);    lua_setfield(L, -2, "num_players");
  lua_pushinteger(L, info->max_players);    lua_setfield(L, -2, "max_players");
  lua_pushinteger(L, info->max_pillboxes);  lua_setfield(L, -2, "max_pillboxes");
  lua_pushinteger(L, info->max_refbases);   lua_setfield(L, -2, "max_refbases");

  /* Player names: 1-based array of strings.
   * playernames is typed u_char36** but actually points at a flat
   * char[MAX_TANKS][PLAYER_NAME_LEN] array (PLAYER_NAME_LEN bytes per slot).
   * Each slot is a pascal string: byte 0 = length, bytes 1..len = chars.
   * Index by byte offset rather than pointer indirection. */
  lua_newtable(L);
  if (info->playernames != NULL) {
    const u_char *base = (const u_char *)info->playernames;
    for (i = 0; i < info->max_players; i++) {
      const u_char *ps  = base + (size_t)i * PLAYER_NAME_LEN;
      int           len = (int)ps[0];
      if (len > 0 && len < PLAYER_NAME_LEN) {
        lua_pushlstring(L, (const char *)(ps + 1), (size_t)len);
      } else {
        lua_pushstring(L, "");
      }
      lua_rawseti(L, -2, i + 1);
    }
  }
  lua_setfield(L, -2, "player_names");

  /* Alliance bitmask for our player */
  lua_pushinteger(L, info->allies ? *(info->allies) : 0);
  lua_setfield(L, -2, "allies");

  /* Tank state */
  lua_pushinteger(L, info->tankx);          lua_setfield(L, -2, "tankx");
  lua_pushinteger(L, info->tanky);          lua_setfield(L, -2, "tanky");
  lua_pushinteger(L, info->direction);      lua_setfield(L, -2, "direction");
  lua_pushinteger(L, info->speed);          lua_setfield(L, -2, "speed");
  lua_pushboolean(L, info->inboat);         lua_setfield(L, -2, "inboat");
  lua_pushboolean(L, info->hidden);         lua_setfield(L, -2, "hidden");
  lua_pushinteger(L, info->shells);         lua_setfield(L, -2, "shells");
  lua_pushinteger(L, info->mines);          lua_setfield(L, -2, "mines");
  lua_pushinteger(L, info->armour);         lua_setfield(L, -2, "armour");
  lua_pushinteger(L, info->trees);          lua_setfield(L, -2, "trees");
  lua_pushinteger(L, info->carriedpills);   lua_setfield(L, -2, "carried_pills");
  lua_pushinteger(L, info->carriedbases);   lua_setfield(L, -2, "carried_bases");
  lua_pushinteger(L, info->gunrange);       lua_setfield(L, -2, "gunrange");
  lua_pushboolean(L, info->reload != 0);    lua_setfield(L, -2, "reload");
  lua_pushboolean(L, info->newtank != 0);   lua_setfield(L, -2, "newtank");
  lua_pushboolean(L, info->tankobstructed != 0); lua_setfield(L, -2, "tank_obstructed");

  /* Nearest friendly base, or nil if none within range */
  if (info->base != NULL) {
    lua_newtable(L);
    /* base->x/y are WORLD coords (0-65535); Lua brain expects MAP coords
     * (0-255) so it can index state.baseprog[x][y] and convert back via
     * (I.base.x<<8)|0x80 when it needs a world position. */
    lua_pushinteger(L, info->base->x >> 8); lua_setfield(L, -2, "x");
    lua_pushinteger(L, info->base->y >> 8); lua_setfield(L, -2, "y");
    lua_pushinteger(L, info->base->idnum);  lua_setfield(L, -2, "idnum");
    lua_pushinteger(L, info->base_shells);  lua_setfield(L, -2, "shells");
    lua_pushinteger(L, info->base_mines);   lua_setfield(L, -2, "mines");
    lua_pushinteger(L, info->base_armour);  lua_setfield(L, -2, "armour");
  } else {
    lua_pushnil(L);
  }
  lua_setfield(L, -2, "base");

  /* Builder man (LGM) state */
  lua_pushinteger(L, info->man_status);     lua_setfield(L, -2, "man_status");
  lua_pushinteger(L, info->man_direction);  lua_setfield(L, -2, "man_direction");
  lua_pushinteger(L, info->man_x);          lua_setfield(L, -2, "man_x");
  lua_pushinteger(L, info->man_y);          lua_setfield(L, -2, "man_y");
  lua_pushinteger(L, info->manobstructed);  lua_setfield(L, -2, "man_obstructed");

  /* Current view rectangle */
  lua_pushinteger(L, info->view_top);       lua_setfield(L, -2, "view_top");
  lua_pushinteger(L, info->view_left);      lua_setfield(L, -2, "view_left");
  lua_pushinteger(L, info->view_height);    lua_setfield(L, -2, "view_height");
  lua_pushinteger(L, info->view_width);     lua_setfield(L, -2, "view_width");

  /* pillview: 0x8000 = tank-centred, otherwise = pill index being viewed */
  lua_pushinteger(L, info->pillview ? *(info->pillview) : 0x8000);
  lua_setfield(L, -2, "pillview");

  /* viewdata: raw terrain bytes as a Lua string.
   * Index with: string.byte(info.viewdata, row * info.view_width + col + 1)
   * Mask with TERRAIN_MASK to get the terrain type. */
  if (info->viewdata != NULL) {
    lua_pushlstring(L, (const char *)info->viewdata,
                    (size_t)(info->view_width * info->view_height));
  } else {
    lua_pushstring(L, "");
  }
  lua_setfield(L, -2, "viewdata");

  /* Visible objects: 1-based array of {type, x, y, idnum, direction, info} */
  lua_newtable(L);
  for (i = 0; i < info->num_objects; i++) {
    lua_newtable(L);
    lua_pushinteger(L, info->objects[i].object);    lua_setfield(L, -2, "type");
    lua_pushinteger(L, info->objects[i].x);         lua_setfield(L, -2, "x");
    lua_pushinteger(L, info->objects[i].y);         lua_setfield(L, -2, "y");
    lua_pushinteger(L, info->objects[i].idnum);     lua_setfield(L, -2, "idnum");
    lua_pushinteger(L, info->objects[i].direction); lua_setfield(L, -2, "direction");
    lua_pushinteger(L, info->objects[i].info);      lua_setfield(L, -2, "info");
    lua_rawseti(L, -2, i + 1);
  }
  lua_setfield(L, -2, "objects");

  /* Received message this tick, or nil.
   * info->message->message is a pascal string (byte 0 = length, bytes 1..N = text).
   * Convert to a C string before passing to Lua. */
  if (info->message != NULL) {
    char msgBuf[256];
    lua_newtable(L);
    lua_pushinteger(L, info->message->sender);
    lua_setfield(L, -2, "sender");
    lua_pushinteger(L, info->message->receivers ? *(info->message->receivers) : 0);
    lua_setfield(L, -2, "receivers");
    if (info->message->message != NULL && info->message->message[0] != 0) {
      utilPtoCString((char *)info->message->message, msgBuf);
      lua_pushstring(L, msgBuf);
    } else {
      lua_pushstring(L, "");
    }
    lua_setfield(L, -2, "text");
  } else {
    lua_pushnil(L);
  }
  lua_setfield(L, -2, "message");

  /* Game info */
  lua_newtable(L);
  /* mapname is a 36-byte C string (not pascal) as stored in gameinfo */
  lua_pushstring(L, (const char *)info->gameinfo.mapname.c);
  lua_setfield(L, -2, "mapname");
  lua_pushinteger(L, info->gameinfo.gametype);          lua_setfield(L, -2, "gametype");
  lua_pushinteger(L, info->gameinfo.hidden_mines);      lua_setfield(L, -2, "hidden_mines");
  lua_pushboolean(L, info->gameinfo.allow_AI);          lua_setfield(L, -2, "allow_ai");
  lua_pushboolean(L, info->gameinfo.assist_AI);         lua_setfield(L, -2, "assist_ai");
  lua_pushinteger(L, info->gameinfo.start_delay);       lua_setfield(L, -2, "start_delay");
  lua_pushinteger(L, info->gameinfo.time_limit);        lua_setfield(L, -2, "time_limit");
  lua_setfield(L, -2, "gameinfo");

  /* Current key state — brain may read these as a starting point */
  lua_pushinteger(L, info->holdkeys ? *(info->holdkeys) : 0);
  lua_setfield(L, -2, "holdkeys");
  lua_pushinteger(L, info->tapkeys ? *(info->tapkeys) : 0);
  lua_setfield(L, -2, "tapkeys");
}

/*********************************************************
*NAME:          extract_brain_output
*PURPOSE:
*  Reads the table returned by brain.think() and writes
*  the brain's output back into the BrainInfo struct.
*  Expected to be called with the return table on top of
*  the Lua stack; does not pop it.
*
*  sendmessage strings are converted from a Lua string to
*  the pascal-format (length-prefixed byte array) that
*  screenExtractBrainInfo() expects.
*
*ARGUMENTS:
*  L    - Lua state (return table on top)
*  info - BrainInfo to write outputs into
*********************************************************/
static void extract_brain_output(lua_State *L, BrainInfo *info) {
  if (!lua_istable(L, -1)) {
    return; /* brain returned nothing — leave outputs at their current values */
  }

  /* holdkeys */
  lua_getfield(L, -1, "holdkeys");
  if (lua_isinteger(L, -1) && info->holdkeys) {
    *(info->holdkeys) = (uint32_t)lua_tointeger(L, -1);
  }
  lua_pop(L, 1);

  /* tapkeys */
  lua_getfield(L, -1, "tapkeys");
  if (lua_isinteger(L, -1) && info->tapkeys) {
    *(info->tapkeys) = (uint32_t)lua_tointeger(L, -1);
  }
  lua_pop(L, 1);

  /* build: nil means no build request, table means {x, y, action} */
  lua_getfield(L, -1, "build");
  if (lua_istable(L, -1) && info->build) {
    lua_getfield(L, -1, "x");
    info->build->x = (MAP_X)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "y");
    info->build->y = (MAP_Y)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "action");
    info->build->action = (BUILDMODE)lua_tointeger(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  /* wantallies */
  lua_getfield(L, -1, "wantallies");
  if (lua_isinteger(L, -1) && info->wantallies) {
    *(info->wantallies) = (PlayerBitMap)lua_tointeger(L, -1);
  }
  lua_pop(L, 1);

  /* messagedest */
  lua_getfield(L, -1, "messagedest");
  if (lua_isinteger(L, -1) && info->messagedest) {
    *(info->messagedest) = (PlayerBitMap)lua_tointeger(L, -1);
  }
  lua_pop(L, 1);

  /* sendmessage: convert Lua string to pascal string (length byte + data).
   * screenExtractBrainInfo checks sendmessage[0] != 0 then calls
   * utilPtoCString to decode it. Max safe length is 253 characters. */
  lua_getfield(L, -1, "sendmessage");
  if (lua_isstring(L, -1) && info->sendmessage) {
    size_t      len;
    const char *s = lua_tolstring(L, -1, &len);
    if (len > 0 && len <= 253) {
      info->sendmessage[0] = (u_char)len;
      memcpy(info->sendmessage + 1, s, len);
      info->sendmessage[len + 1] = '\0';
    }
  }
  lua_pop(L, 1);
}


/* ================================================================== */
/* Brain method invocation helpers                                     */
/* ================================================================== */

/*********************************************************
*NAME:          call_brain_think
*PURPOSE:
*  Calls brain.think(info) and processes the return table
*  via extract_brain_output(). Cleans up the stack on
*  both success and failure.
*  Returns false and logs if the call fails.
*********************************************************/
static bool call_brain_think(lua_State *L) {
  int top = lua_gettop(L);

  lua_getglobal(L, "brain");
  if (!lua_istable(L, -1)) {
    WB_LOG_WARN(WB_LOG_CAT_LUA, "luaBrainsHandler: 'brain' global is not a table");
    lua_settop(L, top);
    return false;
  }

  lua_getfield(L, -1, "think");
  if (!lua_isfunction(L, -1)) {
    WB_LOG_WARN(WB_LOG_CAT_LUA, "luaBrainsHandler: brain.think is not a function");
    lua_settop(L, top);
    return false;
  }

  push_brain_info(L, &bInfo);

  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    WB_LOG_WARN(WB_LOG_CAT_LUA, "luaBrainsHandler: brain.think() error: %s", lua_tostring(L, -1));
    lua_settop(L, top);
    return false;
  }

  /* Return value is on top; brain table is below it */
  extract_brain_output(L, &bInfo);
  lua_settop(L, top);
  return true;
}

/*********************************************************
*NAME:          call_brain_method
*PURPOSE:
*  Calls a named method on the brain table (open or close)
*  passing the current BrainInfo as the sole argument.
*  Missing methods are silently ignored (they are optional).
*  Returns false and logs on Lua error.
*********************************************************/
static bool call_brain_method(lua_State *L, const char *method) {
  int top = lua_gettop(L);

  lua_getglobal(L, "brain");
  if (!lua_istable(L, -1)) {
    lua_settop(L, top);
    return false;
  }

  lua_getfield(L, -1, method);
  if (!lua_isfunction(L, -1)) {
    lua_settop(L, top);
    return true; /* optional — not an error */
  }

  push_brain_info(L, &bInfo);

  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    WB_LOG_WARN(WB_LOG_CAT_LUA, "luaBrainsHandler: brain.%s() error: %s",
            method, lua_tostring(L, -1));
    lua_settop(L, top);
    return false;
  }

  lua_settop(L, top);
  return true;
}
#endif /* #if 0 — old implementations */


/* ================================================================== */
/* Multi-instance brain API                                            */
/* ================================================================== */

/* Helper: load a file via SDL_IOFromFile into a malloc'd buffer.
 * Returns NULL on failure.  Caller must SDL_free() the result. */
static char *sdl_load_file(const char *path, size_t *outLen) {
  SDL_IOStream *io = SDL_IOFromFile(path, "rb");
  if (!io) return NULL;
  Sint64 size = SDL_GetIOSize(io);
  if (size <= 0) { SDL_CloseIO(io); return NULL; }
  char *buf = (char *)SDL_malloc((size_t)size);
  if (!buf) { SDL_CloseIO(io); return NULL; }
  size_t nread = SDL_ReadIO(io, buf, (size_t)size);
  SDL_CloseIO(io);
  if ((Sint64)nread != size) { SDL_free(buf); return NULL; }
  *outLen = (size_t)size;
  return buf;
}

/* Custom Lua searcher that uses SDL_IOFromFile so require() works on Android.
 * The brain directory is stored as an upvalue (string at index 1). */
static int sdl_lua_searcher(lua_State *L) {
  const char *modname = luaL_checkstring(L, 1);
  const char *brainDir = lua_tostring(L, lua_upvalueindex(1));
  char filepath[512];
  size_t len = 0;
  char *buf;

  /* Try brainDir/modname.lua */
  SDL_snprintf(filepath, sizeof(filepath), "%s/%s.lua", brainDir, modname);
  buf = sdl_load_file(filepath, &len);
  if (!buf) {
    /* Try brainDir/modname/init.lua */
    SDL_snprintf(filepath, sizeof(filepath), "%s/%s/init.lua", brainDir, modname);
    buf = sdl_load_file(filepath, &len);
  }
  if (!buf) {
    lua_pushfstring(L, "\n\tno file '%s/%s.lua' (SDL)", brainDir, modname);
    return 1;
  }

  if (luaL_loadbuffer(L, buf, len, filepath) != LUA_OK) {
    SDL_free(buf);
    return lua_error(L);
  }
  SDL_free(buf);
  lua_pushstring(L, filepath); /* 2nd return: file path as "extra" */
  return 2;
}

/* Helper: extract the brain directory from a path.
 * Handles paths like "brains/NewAutopilot/init.lua" and "brains/NewAutopilot/".
 * Returns true and writes into brainDir if a directory was extracted. */
static bool extract_brain_dir(const char *path, char *brainDir, size_t brainDirLen) {
  size_t pathLen = SDL_strlen(path);

  /* Case 1: path ends with /init.lua */
  if (pathLen > 9 &&
      (SDL_strcmp(path + pathLen - 9, "/init.lua") == 0 ||
       SDL_strcmp(path + pathLen - 9, "\\init.lua") == 0)) {
    size_t dirLen = pathLen - 9;
    if (dirLen < brainDirLen) {
      memcpy(brainDir, path, dirLen);
      brainDir[dirLen] = '\0';
      return true;
    }
  }

  /* Case 2: path ends with / or \ (directory path) */
  if (pathLen > 1 && (path[pathLen-1] == '/' || path[pathLen-1] == '\\')) {
    size_t dirLen = pathLen - 1;
    if (dirLen < brainDirLen) {
      memcpy(brainDir, path, dirLen);
      brainDir[dirLen] = '\0';
      return true;
    }
  }

  return false;
}

/* Helper: set up the Lua VM package.path for directory-based brains. */
static void setup_brain_package_path(lua_State *L, const char *path) {
  char brainDir[LUA_BRAINS_PATH_MAX];
  if (extract_brain_dir(path, brainDir, sizeof(brainDir))) {
    char newPath[LUA_BRAINS_PATH_MAX * 2];
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    SDL_snprintf(newPath, sizeof(newPath), "%s/?.lua;%s",
                 brainDir, lua_tostring(L, -1));
    lua_pop(L, 1); /* pop old path */
    lua_pushstring(L, newPath);
    lua_setfield(L, -2, "path");
    lua_pop(L, 1); /* pop package */
  }
}

bool luaBrainInstanceCreate(LuaBrainInstance *inst, const char *path,
                            const char *name, ClientSim *cs,
                            aiType aiMode) {
  lua_State *L;

  memset(inst, 0, sizeof(*inst));
  inst->cs = cs;
  inst->isFirst = true;

  /* BrainInfo constant header fields */
  inst->bInfo.BoloVersion  = 0x0114;
  inst->bInfo.InfoVersion  = CURRENT_BRAININFO_VERSION;
  inst->bInfo.PrefsVRefNum = 0;
  inst->bInfo.PrefsFileName = NULL;

  /* Create Lua VM */
  L = luaL_newstate();
  if (L == NULL) {
    WB_LOG_ERROR(WB_LOG_CAT_LUA, "luaBrainInstance: failed to create Lua state for '%s'", name);
    return false;
  }

  luaL_openlibs(L);
  brainCoreRegisterConstants(L);
  brainCoreRegisterGetTerrain(L, &inst->worldPtr);

  /* Create C pathfinder and register cpf_* globals */
  inst->pathfinder = brainPathfinderCreate();
  if (inst->pathfinder) {
    brainCoreRegisterPathfinder(L, &inst->pathfinder);
  }

  /* Create C world simulator and register wsim_* globals */
  inst->worldsim = brainWorldSimCreate();
  if (inst->worldsim) {
    brainCoreRegisterWorldSim(L, &inst->worldsim);
  }

  /* Per-brain overlay command buffer + register overlay_* globals.
   * The buffer is owned by this instance; the registered Lua closures
   * hold a pointer-to-pointer so the buffer can be swapped per tick
   * by the consumer (e.g. a UI replaying historical frames). */
  overlayCmdBufferInit(&inst->overlay);
  inst->overlayPtr = &inst->overlay;
  brainCoreRegisterOverlay(L, &inst->overlayPtr);

  /* braintest_viz_register binding so brains can populate the V
   * dialog rows. Routes to a callback BrainTest sets at startup;
   * NULL when the brain runs under WinBolo client → no-op. */
  brainCoreRegisterVizRegister(L);
  /* braintest_panel_register binding (sibling of viz register) for
   * the Q-toggled panel window. Same NULL-callback behavior under
   * the client. */
  brainCoreRegisterPanelRegister(L);
  /* braintest_shotsim_poi_register binding (sibling of panel register)
   * for the shot-sim panel's POI buttons. Same NULL-callback behavior
   * under the client. */
  brainCoreRegisterShotSimPoiRegister(L);
  /* viz_detail bindings (overlay_detail / _text / _clear) for the
   * interactive inspector dialog. Same null-callback no-op behavior
   * outside BrainTest. */
  brainCoreRegisterVizDetail(L);

  setup_brain_package_path(L, path);

  /* Install SDL-based searcher so require() works on Android assets.
   * Insert it at position 2 in package.searchers (before the default
   * file searcher at position 3). */
  {
    char brainDir[LUA_BRAINS_PATH_MAX];
    if (!extract_brain_dir(path, brainDir, sizeof(brainDir))) {
      /* Not a directory-based brain; derive dir from last slash */
      const char *lastSlash = SDL_strrchr(path, '/');
      if (!lastSlash) lastSlash = SDL_strrchr(path, '\\');
      if (lastSlash) {
        size_t dlen = (size_t)(lastSlash - path);
        if (dlen >= sizeof(brainDir)) dlen = sizeof(brainDir) - 1;
        memcpy(brainDir, path, dlen);
        brainDir[dlen] = '\0';
      } else {
        SDL_strlcpy(brainDir, ".", sizeof(brainDir));
      }
    }
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "searchers");
    int nSearchers = (int)lua_rawlen(L, -1);
    /* Shift existing entries up to make room at index 2 */
    for (int i = nSearchers; i >= 2; i--) {
      lua_rawgeti(L, -1, i);
      lua_rawseti(L, -2, i + 1);
    }
    lua_pushstring(L, brainDir);
    lua_pushcclosure(L, sdl_lua_searcher, 1);
    lua_rawseti(L, -2, 2);
    lua_pop(L, 2); /* pop searchers + package */
  }

  /* Load and execute the brain script.
   * Try SDL_IOFromFile first (works on Android assets), then fall back
   * to luaL_loadfile for regular filesystem paths. If the path is a
   * directory, try appending init.lua. */
  {
    size_t srcLen = 0;
    char *src = NULL;
    char resolvedPath[LUA_BRAINS_PATH_MAX];
    SDL_strlcpy(resolvedPath, path, sizeof(resolvedPath));

    /* If path ends with / or \, append init.lua */
    size_t plen = SDL_strlen(resolvedPath);
    if (plen > 0 && (resolvedPath[plen-1] == '/' || resolvedPath[plen-1] == '\\')) {
      SDL_snprintf(resolvedPath, sizeof(resolvedPath), "%sinit.lua", path);
    }

    src = sdl_load_file(resolvedPath, &srcLen);
    if (src) {
      int loadErr = luaL_loadbuffer(L, src, srcLen, resolvedPath);
      SDL_free(src);
      if (loadErr != LUA_OK) {
        const char *e = lua_tostring(L, -1);
        WB_LOG_ERROR(WB_LOG_CAT_LUA, "luaBrainInstance: failed to load '%s': %s",
                resolvedPath, e);
        FILE *ef = fopen("brain_error.log", "a");
        if (ef) { fprintf(ef, "luaBrainInstance load (buffer) '%s' error: %s\n", resolvedPath, e ? e : "(null)"); fclose(ef); }
        lua_close(L);
        return false;
      }
    } else if (luaL_loadfile(L, resolvedPath) != LUA_OK) {
      const char *e = lua_tostring(L, -1);
      WB_LOG_ERROR(WB_LOG_CAT_LUA, "luaBrainInstance: failed to load '%s': %s",
              resolvedPath, e);
      FILE *ef = fopen("brain_error.log", "a");
      if (ef) { fprintf(ef, "luaBrainInstance load (file) '%s' error: %s\n", resolvedPath, e ? e : "(null)"); fclose(ef); }
      lua_close(L);
      return false;
    }
  }
  if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
    const char *e = lua_tostring(L, -1);
    WB_LOG_ERROR(WB_LOG_CAT_LUA, "luaBrainInstance: error running '%s': %s",
            path, e);
    FILE *ef = fopen("brain_error.log", "a");
    if (ef) { fprintf(ef, "luaBrainInstance run '%s' error: %s\n", path, e ? e : "(null)"); fclose(ef); }
    lua_close(L);
    return false;
  }
  if (!lua_istable(L, -1)) {
    WB_LOG_ERROR(WB_LOG_CAT_LUA, "luaBrainInstance: '%s' did not return a table", path);
    FILE *ef = fopen("brain_error.log", "a");
    if (ef) { fprintf(ef, "luaBrainInstance: '%s' did not return a table\n", path); fclose(ef); }
    lua_close(L);
    return false;
  }
  lua_setglobal(L, "brain");

  /* Call brain.open(info) */
  screenMakeBrainInfoCS(cs, &inst->bInfo, true, aiMode);
  inst->worldPtr = inst->bInfo.theWorld;
  if (!brainCoreCallMethod(L, &inst->bInfo, "open")) {
    screenExtractBrainInfoCS(cs, &inst->bInfo);
    lua_close(L);
    return false;
  }
  screenExtractBrainInfoCS(cs, &inst->bInfo);

  inst->L = L;
  inst->running = true;
  return true;
}

bool luaBrainInstanceTick(LuaBrainInstance *inst) {
  bool ok;

  if (!inst->running) {
    return false;
  }

  /* Reset key state before brain runs (matches bot_manager) */
  inst->cs->brainHoldKeys = 0;
  inst->cs->brainTapKeys = 0;

  screenMakeBrainInfoCS(inst->cs, &inst->bInfo, inst->isFirst,
                        inst->cs->allowComputerTanks);
  inst->isFirst = false;
  inst->bInfo.operation = BRAIN_THINK;

  inst->worldPtr = inst->bInfo.theWorld;
  if (inst->pathfinder) {
    brainPathfinderSetMap(inst->pathfinder, inst->bInfo.theWorld);
  }
  if (inst->worldsim) {
    brainWorldSimSetMap(inst->worldsim, inst->bInfo.theWorld);
  }
  ok = brainCoreCallThink(inst->L, &inst->bInfo);
  screenExtractBrainInfoCS(inst->cs, &inst->bInfo);

  return ok;
}

void luaBrainInstanceDestroy(LuaBrainInstance *inst) {
  if (!inst->running || inst->L == NULL) {
    return;
  }

  screenMakeBrainInfoCS(inst->cs, &inst->bInfo, false,
                        inst->cs->allowComputerTanks);
  inst->bInfo.operation = BRAIN_CLOSE;
  inst->worldPtr = inst->bInfo.theWorld;
  brainCoreCallMethod(inst->L, &inst->bInfo, "close");
  screenExtractBrainInfoCS(inst->cs, &inst->bInfo);

  brainPathfinderDestroy(inst->pathfinder);
  inst->pathfinder = NULL;

  brainWorldSimDestroy(inst->worldsim);
  inst->worldsim = NULL;

  overlayCmdBufferDestroy(&inst->overlay);
  inst->overlayPtr = NULL;

  lua_close(inst->L);
  inst->L = NULL;
  inst->running = false;
}

LuaBrainSetting *luaBrainInstanceGetSettings(LuaBrainInstance *inst,
                                             int *out_count) {
  int              top, n, i;
  LuaBrainSetting *arr;
  lua_State       *L;

  *out_count = 0;

  if (!inst->running || inst->L == NULL) {
    return NULL;
  }
  L = inst->L;

  top = lua_gettop(L);

  lua_getglobal(L, "brain");
  if (!lua_istable(L, -1)) {
    lua_settop(L, top);
    return NULL;
  }
  lua_getfield(L, -1, "settings");
  if (!lua_isfunction(L, -1)) {
    lua_settop(L, top);
    return NULL;
  }

  if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
    WB_LOG_WARN(WB_LOG_CAT_LUA, "luaBrainInstance: brain.settings() error: %s",
            lua_tostring(L, -1));
    lua_settop(L, top);
    return NULL;
  }

  if (!lua_istable(L, -1)) {
    lua_settop(L, top);
    return NULL;
  }

  n = (int)lua_rawlen(L, -1);
  if (n <= 0) {
    lua_settop(L, top);
    return NULL;
  }

  arr = (LuaBrainSetting *)SDL_calloc((size_t)n, sizeof(LuaBrainSetting));
  if (arr == NULL) {
    lua_settop(L, top);
    return NULL;
  }

  for (i = 1; i <= n; i++) {
    LuaBrainSetting *s = &arr[i - 1];
    const char      *typestr;

    lua_rawgeti(L, -1, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      continue;
    }

    lua_getfield(L, -1, "id");
    SDL_strlcpy(s->id, lua_isstring(L, -1) ? lua_tostring(L, -1) : "",
                sizeof(s->id));
    lua_pop(L, 1);

    lua_getfield(L, -1, "label");
    SDL_strlcpy(s->label, lua_isstring(L, -1) ? lua_tostring(L, -1) : s->id,
                sizeof(s->label));
    lua_pop(L, 1);

    lua_getfield(L, -1, "type");
    typestr = lua_isstring(L, -1) ? lua_tostring(L, -1) : "bool";
    if (SDL_strcmp(typestr, "int")    == 0) s->type = LUA_BRAIN_SETTING_INT;
    else if (SDL_strcmp(typestr, "float")  == 0) s->type = LUA_BRAIN_SETTING_FLOAT;
    else if (SDL_strcmp(typestr, "string") == 0) s->type = LUA_BRAIN_SETTING_STRING;
    else                                         s->type = LUA_BRAIN_SETTING_BOOL;
    lua_pop(L, 1);

    lua_getfield(L, -1, "value");
    switch (s->type) {
      case LUA_BRAIN_SETTING_BOOL:
        s->value.b = lua_toboolean(L, -1) != 0;
        break;
      case LUA_BRAIN_SETTING_INT:
        s->value.i = (int)lua_tointeger(L, -1);
        break;
      case LUA_BRAIN_SETTING_FLOAT:
        s->value.f = (float)lua_tonumber(L, -1);
        break;
      case LUA_BRAIN_SETTING_STRING:
        SDL_strlcpy(s->value.s,
                    lua_isstring(L, -1) ? lua_tostring(L, -1) : "",
                    sizeof(s->value.s));
        break;
    }
    lua_pop(L, 1);

    lua_getfield(L, -1, "range_min");
    s->range_min = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 0.0f;
    lua_pop(L, 1);

    lua_getfield(L, -1, "range_max");
    s->range_max = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 0.0f;
    lua_pop(L, 1);

    lua_pop(L, 1); /* pop descriptor table */
  }

  lua_settop(L, top);
  *out_count = n;
  return arr;
}

void luaBrainInstanceSetSetting(LuaBrainInstance *inst,
                                const LuaBrainSetting *setting) {
  int        top;
  lua_State *L;

  if (!inst->running || inst->L == NULL || setting == NULL) {
    return;
  }
  L = inst->L;

  top = lua_gettop(L);

  lua_getglobal(L, "brain");
  if (!lua_istable(L, -1)) {
    lua_settop(L, top);
    return;
  }
  lua_getfield(L, -1, "set_setting");
  if (!lua_isfunction(L, -1)) {
    lua_settop(L, top);
    return;
  }

  lua_pushstring(L, setting->id);

  switch (setting->type) {
    case LUA_BRAIN_SETTING_BOOL:   lua_pushboolean(L, setting->value.b ? 1 : 0); break;
    case LUA_BRAIN_SETTING_INT:    lua_pushinteger(L, setting->value.i);          break;
    case LUA_BRAIN_SETTING_FLOAT:  lua_pushnumber(L,  (lua_Number)setting->value.f); break;
    case LUA_BRAIN_SETTING_STRING: lua_pushstring(L,  setting->value.s);          break;
  }

  if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
    WB_LOG_WARN(WB_LOG_CAT_LUA, "luaBrainInstance: brain.set_setting('%s') error: %s",
            setting->id, lua_tostring(L, -1));
  }

  lua_settop(L, top);
}


/* ================================================================== */
/* Directory scanning                                                  */
/* ================================================================== */

/*********************************************************
*NAME:          scan_brains_dir
*PURPOSE:
*  Scans brainsDir for .lua files (case-insensitive on
*  all platforms) and populates brainsNames / brainsPaths.
*  Uses Win32 FindFirstFile on Windows, POSIX dirent on
*  all other platforms.
*********************************************************/
static void scan_brains_in(const char *brainsDir, const char *namePrefix) {
#ifdef _WIN32
  WIN32_FIND_DATAA fd;
  HANDLE           fh;
  char             pattern[LUA_BRAINS_PATH_MAX];
  char             initPath[LUA_BRAINS_PATH_MAX];

  /* --- Pass 1: standalone .lua files --- */
  SDL_snprintf(pattern, sizeof(pattern), "%s\\*.lua", brainsDir);
  fh = FindFirstFileA(pattern, &fd);
  if (fh != INVALID_HANDLE_VALUE) {
    do {
      size_t len;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        continue;
      }
      if (brainsNum >= LUA_BRAINS_MAX) {
        break;
      }
      SDL_snprintf(brainsPaths[brainsNum], LUA_BRAINS_PATH_MAX,
                   "%s\\%s", brainsDir, fd.cFileName);
      SDL_strlcpy(brainsNames[brainsNum], fd.cFileName, LUA_BRAINS_NAME_MAX);
      len = SDL_strlen(brainsNames[brainsNum]);
      if (len >= LUA_BRAINS_EXT_LEN) {
        brainsNames[brainsNum][len - LUA_BRAINS_EXT_LEN] = '\0';
      }
      if (namePrefix && namePrefix[0]) {
        char tmp[LUA_BRAINS_NAME_MAX];
        SDL_snprintf(tmp, sizeof(tmp), "%s%s", namePrefix, brainsNames[brainsNum]);
        SDL_strlcpy(brainsNames[brainsNum], tmp, LUA_BRAINS_NAME_MAX);
      }
      brainsNum++;
    } while (FindNextFileA(fh, &fd));
    FindClose(fh);
  }

  /* --- Pass 2: directories containing init.lua --- */
  SDL_snprintf(pattern, sizeof(pattern), "%s\\*", brainsDir);
  fh = FindFirstFileA(pattern, &fd);
  if (fh != INVALID_HANDLE_VALUE) {
    do {
      DWORD attr;
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        continue;
      }
      if (fd.cFileName[0] == '.') {
        continue; /* skip . and .. */
      }
      if (brainsNum >= LUA_BRAINS_MAX) {
        break;
      }
      /* Check for init.lua inside the directory */
      SDL_snprintf(initPath, sizeof(initPath),
                   "%s\\%s\\init.lua", brainsDir, fd.cFileName);
      attr = GetFileAttributesA(initPath);
      if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        continue; /* no init.lua or it's a directory — skip */
      }
      SDL_strlcpy(brainsPaths[brainsNum], initPath, LUA_BRAINS_PATH_MAX);
      if (namePrefix && namePrefix[0]) {
        SDL_snprintf(brainsNames[brainsNum], LUA_BRAINS_NAME_MAX,
                     "%s%s", namePrefix, fd.cFileName);
      } else {
        SDL_strlcpy(brainsNames[brainsNum], fd.cFileName, LUA_BRAINS_NAME_MAX);
      }
      brainsNum++;
    } while (FindNextFileA(fh, &fd));
    FindClose(fh);
  }

#else /* POSIX */
  DIR           *dir;
  struct dirent *entry;
  char           initPath[LUA_BRAINS_PATH_MAX];
  struct stat    st;

  dir = opendir(brainsDir);
  if (dir == NULL) {
    return;
  }
  while ((entry = readdir(dir)) != NULL && brainsNum < LUA_BRAINS_MAX) {
    size_t len = SDL_strlen(entry->d_name);

    /* Skip . and .. */
    if (entry->d_name[0] == '.' && (len == 1 ||
        (len == 2 && entry->d_name[1] == '.'))) {
      continue;
    }

    /* Check if it's a .lua file (standalone brain) */
    if (len > (size_t)LUA_BRAINS_EXT_LEN &&
        SDL_strcasecmp(entry->d_name + len - LUA_BRAINS_EXT_LEN,
                       LUA_BRAINS_EXT) == 0) {
      SDL_snprintf(brainsPaths[brainsNum], LUA_BRAINS_PATH_MAX,
                   "%s/%s", brainsDir, entry->d_name);
      SDL_strlcpy(brainsNames[brainsNum], entry->d_name, LUA_BRAINS_NAME_MAX);
      brainsNames[brainsNum][len - LUA_BRAINS_EXT_LEN] = '\0';
      if (namePrefix && namePrefix[0]) {
        char tmp[LUA_BRAINS_NAME_MAX];
        SDL_snprintf(tmp, sizeof(tmp), "%s%s", namePrefix, brainsNames[brainsNum]);
        SDL_strlcpy(brainsNames[brainsNum], tmp, LUA_BRAINS_NAME_MAX);
      }
      brainsNum++;
      continue;
    }

    /* Check if it's a directory containing init.lua */
    SDL_snprintf(initPath, sizeof(initPath),
                 "%s/%s/init.lua", brainsDir, entry->d_name);
    if (stat(initPath, &st) == 0 && S_ISREG(st.st_mode)) {
      SDL_strlcpy(brainsPaths[brainsNum], initPath, LUA_BRAINS_PATH_MAX);
      if (namePrefix && namePrefix[0]) {
        SDL_snprintf(brainsNames[brainsNum], LUA_BRAINS_NAME_MAX,
                     "%s%s", namePrefix, entry->d_name);
      } else {
        SDL_strlcpy(brainsNames[brainsNum], entry->d_name, LUA_BRAINS_NAME_MAX);
      }
      brainsNum++;
    }
  }
  closedir(dir);
#endif
}


/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

/*********************************************************
*NAME:          luaBrainLoadBrains
*LAST MODIFIED: 09/03/26
*PURPOSE:
*  Initialises the BrainInfo constant fields and scans
*  the Brains/ directory for available .lua brains.
*  Returns true (scan failure is non-fatal — the brain
*  list will simply be empty).
*********************************************************/
#if defined(HAVE_ONNXRUNTIME) && !defined(__EMSCRIPTEN__)
static void scan_onnx_brains_in(const char *baseDir);
#endif

bool luaBrainLoadBrains(void) {
  memset(&singletonInst, 0, sizeof(singletonInst));
  memset(brainsTypes, 0, sizeof(brainsTypes)); /* default BRAIN_TYPE_LUA */
  brainsNum        = 0;
  brainsRunningIdx = -1;

  scan_brains_in(LUA_BRAINS_DIR, NULL);
  scan_brains_in(LUA_BRAINS_DEV_DIR, "[dev] ");

#if defined(HAVE_ONNXRUNTIME) && !defined(__EMSCRIPTEN__)
  scan_onnx_brains_in(LUA_BRAINS_DIR);
  scan_onnx_brains_in(LUA_BRAINS_DEV_DIR);
#endif

  return true;
}

/*********************************************************
*NAME:          luaBrainStart
*LAST MODIFIED: 09/03/26
*PURPOSE:
*  Creates a Lua state, loads and executes the brain
*  script, then calls brain.open(info). The script must
*  return a table assigned to the global 'brain'.
*  Returns false if loading or open() fails.
*********************************************************/
bool luaBrainStart(const char *path, const char *name, ClientSim *cs) {
  /* Shut down any currently running brain first */
  if (singletonInst.running) {
    luaBrainStop();
  }

  clientMutexWaitFor();
  if (!luaBrainInstanceCreate(&singletonInst, path, name,
                              cs,
                              cs->allowComputerTanks)) {
    clientMutexRelease();
    return false;
  }
  clientMutexRelease();

  /* Record which brain index is active (for menu tick mark) */
  brainsRunningIdx = -1;
  for (int i = 0; i < brainsNum; i++) {
    if (brainsPaths[i][0] && SDL_strcmp(brainsPaths[i], path) == 0) {
      brainsRunningIdx = i;
      break;
    }
  }

  WB_LOG_INFO(WB_LOG_CAT_LUA, "luaBrainsHandler: started brain '%s'", name);
  return true;
}

/*********************************************************
*NAME:          luaBrainRun
*LAST MODIFIED: 09/03/26
*PURPOSE:
*  Executes brain.think(info) for the current game tick.
*  Acquires the client mutex around the entire operation.
*  No-op if no brain is loaded or a think is already
*  in progress (re-entrancy guard).
*********************************************************/
void luaBrainRun(void) {
  bool ok;

  if (!singletonInst.running || brainsProcExecuting) {
    return;
  }

  clientMutexWaitFor();
  brainsProcExecuting = true;

  {
    Uint64 t0 = SDL_GetPerformanceCounter();
    ok = luaBrainInstanceTick(&singletonInst);
    Uint64 t1 = SDL_GetPerformanceCounter();
    double ms = (double)(t1 - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    if (ms > 5.0) {
      WB_LOG_DEBUG(WB_LOG_CAT_LUA, "luaBrainsHandler: think took %.3f ms", ms);
    }
  }

  brainsProcExecuting = false;
  clientMutexRelease();

  if (!ok) {
    WB_LOG_WARN(WB_LOG_CAT_LUA, "luaBrainsHandler: think failed, stopping brain");
    luaBrainStop();
  }
}

/*********************************************************
*NAME:          luaBrainStop
*LAST MODIFIED: 09/03/26
*PURPOSE:
*  Calls brain.close(info) and destroys the Lua state.
*  Safe to call when no brain is running.
*********************************************************/
void luaBrainStop(void) {
  if (!singletonInst.running) {
    return;
  }

  /* Wait for any in-progress think to finish */
  while (brainsProcExecuting) {
    SDL_Delay(1);
  }

  clientMutexWaitFor();
  brainsProcExecuting = true;
  luaBrainInstanceDestroy(&singletonInst);
  brainsProcExecuting = false;
  clientMutexRelease();

  brainsRunningIdx = -1;
  WB_LOG_INFO(WB_LOG_CAT_LUA, "luaBrainsHandler: brain stopped");
}

/*********************************************************
*NAME:          luaBrainShutdown
*LAST MODIFIED: 09/03/26
*PURPOSE:
*  Shuts down the brain subsystem on game exit.
*********************************************************/
void luaBrainShutdown(void) {
  luaBrainStop();
  mlBrainStopSingleton();
}

/*********************************************************
*NAME:          luaBrainIsRunning
*LAST MODIFIED: 09/03/26
*********************************************************/
bool luaBrainIsRunning(void) {
  return singletonInst.running || mlBrainSingletonIsRunning();
}

/*********************************************************
*NAME:          luaBrainGetNum
*LAST MODIFIED: 09/03/26
*********************************************************/
int luaBrainGetNum(void) {
  return brainsNum;
}

/*********************************************************
*NAME:          luaBrainGetName
*LAST MODIFIED: 09/03/26
*********************************************************/
const char *luaBrainGetName(int index) {
  if (index < 0 || index >= brainsNum) {
    return NULL;
  }
  return brainsNames[index];
}

/*********************************************************
*NAME:          luaBrainGetPath
*LAST MODIFIED: 09/03/26
*********************************************************/
const char *luaBrainGetPath(int index) {
  if (index < 0 || index >= brainsNum) {
    return NULL;
  }
  return brainsPaths[index];
}

/*********************************************************
*NAME:          luaBrainGetRunningIndex
*LAST MODIFIED: 09/03/26
*********************************************************/
int luaBrainGetRunningIndex(void) {
  return brainsRunningIdx;
}

/*********************************************************
*NAME:          luaBrainGetType
*********************************************************/
BrainType luaBrainGetType(int index) {
  if (index < 0 || index >= brainsNum) return BRAIN_TYPE_LUA;
  return brainsTypes[index];
}

/* ================================================================== */
/* ONNX brain scanning                                                 */
/* ================================================================== */

#if defined(HAVE_ONNXRUNTIME) && !defined(__EMSCRIPTEN__)

#define ONNX_BRAINS_SUBDIR  "onnx"
#define ONNX_EXT            ".onnx"
#define ONNX_EXT_LEN        5

static void scan_onnx_brains_in(const char *baseDir) {
    char onnxDir[LUA_BRAINS_PATH_MAX];
    SDL_snprintf(onnxDir, sizeof(onnxDir), "%s/%s", baseDir, ONNX_BRAINS_SUBDIR);

#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE fh;
    char pattern[LUA_BRAINS_PATH_MAX];

    SDL_snprintf(pattern, sizeof(pattern), "%s\\%s\\*.onnx", baseDir, ONNX_BRAINS_SUBDIR);
    fh = FindFirstFileA(pattern, &fd);
    if (fh == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (brainsNum >= LUA_BRAINS_MAX) break;

        SDL_snprintf(brainsPaths[brainsNum], LUA_BRAINS_PATH_MAX,
                     "%s\\%s\\%s", baseDir, ONNX_BRAINS_SUBDIR, fd.cFileName);

        /* Name: strip .onnx extension, add [ML] prefix */
        char rawName[LUA_BRAINS_NAME_MAX];
        SDL_strlcpy(rawName, fd.cFileName, sizeof(rawName));
        size_t len = SDL_strlen(rawName);
        if (len >= ONNX_EXT_LEN) rawName[len - ONNX_EXT_LEN] = '\0';
        SDL_snprintf(brainsNames[brainsNum], LUA_BRAINS_NAME_MAX, "[ML] %s", rawName);

        brainsTypes[brainsNum] = BRAIN_TYPE_ONNX;
        brainsNum++;
    } while (FindNextFileA(fh, &fd));
    FindClose(fh);

#else /* POSIX */
    DIR *dir = opendir(onnxDir);
    if (dir == NULL) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && brainsNum < LUA_BRAINS_MAX) {
        size_t len = SDL_strlen(entry->d_name);
        if (len <= (size_t)ONNX_EXT_LEN) continue;
        if (SDL_strcasecmp(entry->d_name + len - ONNX_EXT_LEN, ONNX_EXT) != 0) continue;

        SDL_snprintf(brainsPaths[brainsNum], LUA_BRAINS_PATH_MAX,
                     "%s/%s", onnxDir, entry->d_name);

        char rawName[LUA_BRAINS_NAME_MAX];
        SDL_strlcpy(rawName, entry->d_name, sizeof(rawName));
        rawName[len - ONNX_EXT_LEN] = '\0';
        SDL_snprintf(brainsNames[brainsNum], LUA_BRAINS_NAME_MAX, "[ML] %s", rawName);

        brainsTypes[brainsNum] = BRAIN_TYPE_ONNX;
        brainsNum++;
    }
    closedir(dir);
#endif
}

#endif /* HAVE_ONNXRUNTIME && !__EMSCRIPTEN__ */

/* ================================================================== */
/* ML brain singleton management                                       */
/* ================================================================== */

bool mlBrainStartSingleton(const char *path, const char *name, ClientSim *cs) {
#if defined(HAVE_ONNXRUNTIME) && !defined(__EMSCRIPTEN__)
    /* Stop any running brain (Lua or ONNX) */
    if (singletonInst.running) luaBrainStop();
    mlBrainStopSingleton();

    clientMutexWaitFor();
    mlSingletonInst = mlBrainCreate(path);
    clientMutexRelease();

    if (mlSingletonInst == NULL) {
        WB_LOG_ERROR(WB_LOG_CAT_LUA, "mlBrainStartSingleton: failed to load '%s'", path);
        return false;
    }

    mlSingletonCS = cs;

    /* Record which brain index is active */
    brainsRunningIdx = -1;
    for (int i = 0; i < brainsNum; i++) {
        if (brainsPaths[i][0] && SDL_strcmp(brainsPaths[i], path) == 0) {
            brainsRunningIdx = i;
            break;
        }
    }

    WB_LOG_INFO(WB_LOG_CAT_LUA, "mlBrainStartSingleton: started ML brain '%s'", name);
    return true;
#else
    (void)path; (void)name; (void)cs;
    return false;
#endif
}

void mlBrainStopSingleton(void) {
#if defined(HAVE_ONNXRUNTIME) && !defined(__EMSCRIPTEN__)
    if (mlSingletonInst == NULL) return;

    clientMutexWaitFor();
    mlBrainDestroy(mlSingletonInst);
    mlSingletonInst = NULL;
    mlSingletonCS = NULL;
    clientMutexRelease();

    brainsRunningIdx = -1;
    WB_LOG_INFO(WB_LOG_CAT_LUA, "mlBrainStopSingleton: ML brain stopped");
#endif
}

bool mlBrainRunSingleton(ClientSim *cs) {
#if defined(HAVE_ONNXRUNTIME) && !defined(__EMSCRIPTEN__)
    if (mlSingletonInst == NULL || brainsProcExecuting) return false;

    clientMutexWaitFor();
    brainsProcExecuting = true;

    /* Reset key state before brain runs (matches Lua path) */
    cs->brainHoldKeys = 0;
    cs->brainTapKeys = 0;

    BrainInfo bi;
    screenMakeBrainInfoCS(cs, &bi, false, cs->allowComputerTanks);
    bi.operation = BRAIN_THINK;

    bool ok;
    {
        Uint64 t0 = SDL_GetPerformanceCounter();
        ok = mlBrainTick(mlSingletonInst, cs, &bi);
        Uint64 t1 = SDL_GetPerformanceCounter();
        double ms = (double)(t1 - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
        if (ms > 5.0) {
            WB_LOG_DEBUG(WB_LOG_CAT_LUA, "mlBrainRunSingleton: inference took %.3f ms", ms);
        }
    }

    /* Apply outputs and free BrainInfo (same as Lua path) */
    screenExtractBrainInfoCS(cs, &bi);

    brainsProcExecuting = false;
    clientMutexRelease();

    return ok;
#else
    (void)cs;
    return false;
#endif
}

bool mlBrainSingletonIsRunning(void) {
#if defined(HAVE_ONNXRUNTIME) && !defined(__EMSCRIPTEN__)
    return mlSingletonInst != NULL;
#else
    return false;
#endif
}

/* ================================================================== */
/* Brain settings API                                                  */
/* ================================================================== */

/*********************************************************
*NAME:          luaBrainGetSettings
*PURPOSE:
*  Delegates to luaBrainInstanceGetSettings on the singleton.
*********************************************************/
LuaBrainSetting *luaBrainGetSettings(int *out_count) {
  return luaBrainInstanceGetSettings(&singletonInst, out_count);
}

/*********************************************************
*NAME:          luaBrainSetSetting
*PURPOSE:
*  Delegates to luaBrainInstanceSetSetting on the singleton.
*********************************************************/
void luaBrainSetSetting(const LuaBrainSetting *setting) {
  luaBrainInstanceSetSetting(&singletonInst, setting);
}

/*********************************************************
*NAME:          luaBrainFreeSettings
*PURPOSE:
*  Frees an array returned by luaBrainGetSettings().
*********************************************************/
void luaBrainFreeSettings(LuaBrainSetting *settings) {
  SDL_free(settings);
}

/* ================================================================== */
/* brainsHandler*()/brainHandler*() API                               */
/* Delegates to the Lua back-end above; no Win32 menu management      */
/* (ImGui handles the brain menu in sdl3imgui.cpp)                    */
/* ================================================================== */

#include "../brainsHandler.h"
#include "../winbolo.h"
#include "../lang.h"

/* Index of the currently running brain (-1 = none/manual) */
static int brainsRunningIndex = -1;

bool brainsHandlerLoadBrains(void) {
  return luaBrainLoadBrains();
}

void brainsHandlerSet(bool enabled) {
  (void)enabled;
  /* ImGui menu handles enable/disable state directly */
}

void brainsHandlerManual(void) {
  luaBrainStop();
  mlBrainStopSingleton();
  brainsRunningIndex = -1;
}

bool brainsHandlerStart(char *path, char *name, ClientSim *cs) {
#if defined(HAVE_ONNXRUNTIME) && !defined(__EMSCRIPTEN__)
  /* Route .onnx files to ML brain */
  size_t len = SDL_strlen(path);
  if (len >= 5 && SDL_strcasecmp(path + len - 5, ".onnx") == 0) {
    return mlBrainStartSingleton(path, name, cs);
  }
#endif
  if (!luaBrainStart(path, name, cs)) {
    return FALSE;
  }
  return TRUE;
}

void brainsHandlerItem(unsigned int id, ClientSim *cs) {
  int         index;
  const char *path;
  const char *name;

  index = (int)(id - BRAINS_RESOURCE_OFFSET);

  if (index == brainsRunningIndex && luaBrainIsRunning()) {
    return;
  }

  if (luaBrainIsRunning()) {
    brainsHandlerManual();
  }

  path  = luaBrainGetPath(index);
  name  = luaBrainGetName(index);
  if (path == NULL || name == NULL) {
    return;
  }

  if (luaBrainGetType(index) == BRAIN_TYPE_ONNX) {
    if (mlBrainStartSingleton(path, name, cs)) {
      brainsRunningIndex = index;
    }
  } else {
    if (brainsHandlerStart((char *)path, (char *)name, cs)) {
      brainsRunningIndex = index;
    }
  }
}

int brainsHandlerGetNum(void) {
  return luaBrainGetNum();
}

void brainsHandlerShutdown(void) {
  if (luaBrainIsRunning()) {
    brainsHandlerManual();
  }
}

bool brainHandlerIsBrainRunning(void) {
  return luaBrainIsRunning();
}

void brainHandlerRun(void) {
  if (mlBrainSingletonIsRunning()) {
#if defined(HAVE_ONNXRUNTIME) && !defined(__EMSCRIPTEN__)
    mlBrainRunSingleton(mlSingletonCS);
#endif
  } else {
    luaBrainRun();
  }
}
