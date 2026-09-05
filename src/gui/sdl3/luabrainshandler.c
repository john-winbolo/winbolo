/*
 * $Id$
 *
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

#if defined(WINBOLO_LUAJIT) && defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>  /* pthread_jit_write_protect_np — see luaBrainInstanceTick */
#endif

#ifndef _WIN32
#  include <dirent.h>
#  include <sys/stat.h>
#endif

/* Lua 5.4 — path resolved via CMake include_directories */
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "../../common/wb_log.h"
#include "global.h"
#include "brain.h"
#include "brain_data.h"
#include "client_sim.h"
#include "util.h"
#include "gh_overlay_pillcontrib.h"
#include "gh_threat.h"
#include "gh_shield_stamp.h"
#include "gh_opt_log.h"
#include "gh_attack.h"
#include "../clientmutex.h"
#include "../gamefront.h"
#include "luabrainshandler.h"


/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

/* Path passed by --run-script; injected as RUN_SCRIPT_PATH Lua global. */
static char s_run_script_path[1024] = "";

/* Set by --profile / --profile-log (or always-on in dev mode). Captured
 * as BRAIN_PROFILE / BRAIN_PROFILE_LOG Lua globals at brain init. */
/* Base seed for every brain's math.random; 0 leaves the VM default. The
 * per-instance seed is this plus the bot's player number, so bots still differ
 * from each other while being identical across runs. Host-controlled default
 * rather than a create parameter, so it reaches the seeding point inside
 * luaBrainInstanceCreate without threading an argument through every caller --
 * the same shape the debug-mode default uses. */
static long s_defaultRandomSeed = 0;
static void seedRandomOnState(lua_State *L, long seed);

static int s_profile     = 0;
static int s_profile_log = 0;
/* Pool-viz capture (BRAIN_POOL_VIZ). Decoupled from profile_log so a
 * headless profiling run can ask for timings ONLY: BrainTest's profile-log
 * sets this to feed its "P" replay window, but winbolods -profile-log leaves
 * it off to avoid the pool-string GC cost (and the extra .btr bytes) skewing
 * the numbers it's there to measure. */
static int s_pool_viz    = 0;

void luaBrainsSetProfile(int profile, int profile_log, int pool_viz) {
    s_profile     = profile     ? 1 : 0;
    s_profile_log = profile_log ? 1 : 0;
    s_pool_viz    = pool_viz    ? 1 : 0;
}

/* Selective brain-debug parts (winbolods -bd-noviz / -bd-nopool /
 * -bd-noprint2 / -bd-nojsonl). -brain-debug implies ALL FOUR on; these
 * default-on statics let the operator turn individual recording streams
 * off to keep a many-bot debug game playable. Captured at brain init:
 *   print2 → _PRINT2_ENABLED
 *   pool   → BRAIN_POOL_VIZ (the per-tick pool-breakdown JSON build)
 *   viz    → _BT_VIZ_COLLECT="off" (viz.lua wrappers emit nothing)
 *   jsonl  → BRAIN_LOG_JSON / _JSONL_LOGGER_ENABLED
 * BRAIN_DEBUG_MODE itself stays on — the brain still runs its debug
 * logic; only the corresponding output stream is silenced. */
static int s_bd_print2 = 1;
static int s_bd_pool   = 1;
static int s_bd_viz    = 1;
static int s_bd_jsonl  = 1;

void luaBrainsSetDebugParts(int print2_on, int pool_on, int viz_on, int jsonl_on) {
    s_bd_print2 = print2_on ? 1 : 0;
    s_bd_pool   = pool_on   ? 1 : 0;
    s_bd_viz    = viz_on    ? 1 : 0;
    s_bd_jsonl  = jsonl_on  ? 1 : 0;
}

/* Set by --instr-profile. Captured as the BRAIN_INSTR_PROFILE Lua global at
 * brain init; when set, GoalHunter arms its sampling profiler around think. */
static int s_instr_profile = 0;

void luaBrainsSetInstrProfile(int enabled) { s_instr_profile = enabled ? 1 : 0; }

/* Set by --log-json (or always-on in dev mode). Captured as
 * BRAIN_LOG_JSON Lua global at brain init. */
static int s_log_json = 0;

void luaBrainsSetLogJson(int enable) { s_log_json = enable ? 1 : 0; }

/* Opt-out for the brain Lua sandbox. 0 = sandboxed (restricted stdlib +
 * path-jailed file access), 1 = full luaL_openlibs (legacy/trusted). The
 * GUI client and dedicated server default to 0; BrainTest sets 1. */
static int s_allow_unsafe = 0;

void luaBrainsSetAllowUnsafe(int enable) { s_allow_unsafe = enable ? 1 : 0; }

void luaBrainsSetRunScript(const char *path) {
    if (path && path[0])
        SDL_strlcpy(s_run_script_path, path, sizeof(s_run_script_path));
    else
        s_run_script_path[0] = '\0';
}

/* Staged per-bot init arg from -bot-init's [..] suffix. Consumed (cleared)
 * by the next luaBrainInstanceCreate, which injects BRAIN_INIT_ARG. */
static char s_next_init_arg[128] = "";

void luaBrainsSetNextInitArg(const char *arg) {
    if (arg && arg[0])
        SDL_strlcpy(s_next_init_arg, arg, sizeof(s_next_init_arg));
    else
        s_next_init_arg[0] = '\0';
}

/* Staged engine tick for the NEXT brain instance created. Injected as the
 * BRAIN_START_ENGINE_TICK Lua global so a brain born mid-game (a Survival
 * wave respawn) can seed its own tick counter from the game clock instead
 * of restarting at 0 every life. Consume-once like s_next_init_arg: a host
 * that never stages one (BrainTest's in-process create) gets 0, which is
 * exactly the game-start value, so the global is never nil. */
static unsigned int s_next_start_engine_tick = 0;

void luaBrainsSetNextStartEngineTick(unsigned int tick) {
    s_next_start_engine_tick = tick;
}

bool luaBrainsParseBotInitSpec(const char *spec, BotInitSlot *slots, int maxN) {
    const char *p = spec;
    while (*p) {
        while (*p == ',' || *p == ' ') p++;   /* skip separators */
        if (!*p) break;

        /* Range: "a" or "a-b", terminated by '='. */
        char *endp = NULL;
        long lo = strtol(p, &endp, 10);
        if (endp == p) {
            fprintf(stderr, "-bot-init: expected a player id near '%s'\n", p);
            return false;
        }
        long hi = lo;
        if (*endp == '-') {
            const char *q = endp + 1;
            hi = strtol(q, &endp, 10);
            if (endp == q) {
                fprintf(stderr, "-bot-init: expected end of id range near '%s'\n", q);
                return false;
            }
        }
        while (*endp == ' ') endp++;
        if (*endp != '=') {
            fprintf(stderr, "-bot-init: expected '=' after id range near '%s'\n", p);
            return false;
        }

        /* Value: <path>[<arg>], up to the next comma (paths can't contain ','). */
        const char *val = endp + 1;
        const char *end = strchr(val, ',');
        if (!end) end = val + strlen(val);

        char vbuf[512 + 128];
        size_t vlen = (size_t)(end - val);
        if (vlen >= sizeof(vbuf)) vlen = sizeof(vbuf) - 1;
        memcpy(vbuf, val, vlen);
        vbuf[vlen] = '\0';

        char argbuf[128] = "";
        char *lb = strchr(vbuf, '[');
        if (lb) {
            *lb = '\0';                       /* path ends at '[' */
            char *rb = strchr(lb + 1, ']');
            if (rb) *rb = '\0';
            SDL_strlcpy(argbuf, lb + 1, sizeof(argbuf));
        }
        if (vbuf[0] == '\0') {
            fprintf(stderr, "-bot-init: empty path for id range %ld-%ld\n", lo, hi);
            return false;
        }

        if (lo > hi) { long t = lo; lo = hi; hi = t; }
        for (long id = lo; id <= hi; id++) {
            if (id < 0 || id >= maxN) {
                fprintf(stderr, "-bot-init: id %ld out of range (0-%d), ignoring\n",
                        id, maxN - 1);
                continue;
            }
            SDL_strlcpy(slots[id].path, vbuf, sizeof(slots[id].path));
            SDL_strlcpy(slots[id].arg, argbuf, sizeof(slots[id].arg));
            slots[id].covered = 1;
        }

        p = (*end == ',') ? end + 1 : end;
    }
    return true;
}

static LuaBrainInstance singletonInst;           /* The GUI client's brain  */
static int        brainsNum          = 0;        /* Discovered brain count  */
static int        brainsRunningIdx   = -1;       /* Index of active brain   */
static bool       brainsProcExecuting = false;   /* Re-entrancy guard       */

static char      brainsNames[LUA_BRAINS_MAX][LUA_BRAINS_NAME_MAX];
static char      brainsPaths[LUA_BRAINS_MAX][LUA_BRAINS_PATH_MAX];
static BrainType brainsTypes[LUA_BRAINS_MAX];    /* LUA or ONNX per entry */

#if defined(HAVE_ONNXRUNTIME) && !defined(__EMSCRIPTEN__)
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

  /* Received messages this tick — array of {sender, receivers, text}
   * tables. info.message (singular) is the legacy alias to messages[1].
   * Pascal-string conversion is the same as before. */
  {
    char msgBuf[256];
    lua_newtable(L);
    for (u_short mi = 0; mi < info->num_messages; mi++) {
      const MessageInfo *m = &info->messages[mi];
      lua_newtable(L);
      lua_pushinteger(L, m->sender);
      lua_setfield(L, -2, "sender");
      lua_pushinteger(L, m->receivers ? *(m->receivers) : 0);
      lua_setfield(L, -2, "receivers");
      if (m->message != NULL && m->message[0] != 0) {
        utilPtoCString((char *)m->message, msgBuf);
        lua_pushstring(L, msgBuf);
      } else {
        lua_pushstring(L, "");
      }
      lua_setfield(L, -2, "text");
      lua_rawseti(L, -2, mi + 1);
    }
    lua_setfield(L, -2, "messages");
  }
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

  /* holdkeys / tapkeys.
   *
   * These pointers alias straight into ClientSim storage (brainDataMakeInfo
   * sets info->holdkeys = clientSimGetBrainHoldKeys(cs), which returns
   * &cs->brainHoldKeys), so writing through them here IS the write that
   * drives the tank -- it lands before brainDataExtractInfo ever runs. Any
   * gate on a dead brain's movement therefore has to be here; a check down
   * in the extract would be copying the value onto itself.
   *
   * A dead brain still runs (to reset its own state for respawn, and to
   * broadcast), but must not steer. GoalHunter already returns zeroed keys
   * on its dead branch and the caller zeroes both sets before each think, so
   * this is belt-and-braces for that brain -- but it is the only thing
   * stopping some other Lua brain from driving a corpse. */
  if (!info->dead) {
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
  }

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

  /* When sandboxed, require() must not escape the brain directory. require()
   * passes the module name to searchers verbatim (the "." -> "/" rewrite is
   * done inside the stock searchers, not here), so a name containing a path
   * separator or ".." would let `require("../../etc/foo")` resolve outside the
   * brain dir. Reject those; brain modules are always flat names. */
  if (!s_allow_unsafe &&
      (SDL_strchr(modname, '/')  || SDL_strchr(modname, '\\') ||
       SDL_strstr(modname, ".."))) {
    lua_pushfstring(L, "\n\tmodule '%s' rejected (sandboxed)", modname);
    return 1;
  }

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

  /* Sandboxed: load as TEXT only so a brain cannot require precompiled
   * bytecode (Lua does not verify bytecode; crafted .luac escapes the VM).
   * Unsafe hosts (BrainTest) keep the default "bt" mode for dev tooling. */
  if (luaL_loadbufferx(L, buf, len, filepath,
                       s_allow_unsafe ? NULL : "t") != LUA_OK) {
    SDL_free(buf);
    return lua_error(L);
  }
  SDL_free(buf);
  lua_pushstring(L, filepath); /* 2nd return: file path as "extra" */
  return 2;
}

/* Helper: extract the brain directory from a path.
 * Handles paths like "brains/GoalHunter/init.lua" and "brains/GoalHunter/".
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

    /* Expose the brain directory so the brain can locate opt/ at runtime. */
    lua_pushstring(L, brainDir);
    lua_setglobal(L, "BRAIN_DIR");
  }
}

/* Lua print() replacement for the release client: routes brain output
 * through wb_log (category LUA, debug level) instead of stdout, so it
 * stays silent unless WINBOLO_LOG enables it (e.g. WINBOLO_LOG=lua=debug).
 * Concatenates its arguments tab-separated, matching stock print(). */
static int l_brain_log_print(lua_State *L) {
  char buf[1024];
  int pos = 0;
  int n = lua_gettop(L);
  for (int i = 1; i <= n; i++) {
    if (i > 1 && pos < (int)sizeof(buf) - 1) buf[pos++] = '\t';
    const char *s = luaL_tolstring(L, i, NULL);
    if (s) {
      int slen = (int)strlen(s);
      int room = (int)sizeof(buf) - pos - 1;
      if (slen > room) slen = room;
      memcpy(buf + pos, s, (size_t)slen);
      pos += slen;
    }
    lua_pop(L, 1); /* pop the luaL_tolstring result */
  }
  buf[pos] = '\0';
  WB_LOG_DEBUG(WB_LOG_CAT_LUA, "%s", buf);
  return 0;
}


/* ================================================================== */
/* Brain Lua sandbox                                                   */
/*                                                                     */
/* When s_allow_unsafe is 0 (the default for the GUI client and        */
/* dedicated server), brain_apply_sandbox() locks the freshly-opened   */
/* standard libraries down so an untrusted brain .lua cannot run        */
/* programs, load native code, escape the VM, or touch the filesystem   */
/* outside its own directory:                                          */
/*   - io.open / loadfile / os.remove are replaced with wrappers that  */
/*     confine every path to the brain directory (read AND write).     */
/*   - io.popen / io.lines / io.input / io.output / io.tmpfile,        */
/*     os.execute / os.exit / os.rename / os.getenv / os.tmpname /      */
/*     os.setlocale, load / dofile, package.loadlib and the native      */
/*     package loaders, and the debug-library introspection set are     */
/*     removed outright.                                               */
/* The shipped GoalHunter brain needs no changes: its loadfile() of    */
/* los_stamp_cache.lua and its io.open() cache paths all resolve under  */
/* the brain directory, and everything it actually removes is dev-only */
/* tooling that only runs under BrainTest (which sets s_allow_unsafe).  */
/* ================================================================== */

/* True if canonPath is canonRoot itself or a path beneath it. The
 * separator check stops "/a/brainX" from matching root "/a/brain". */
static bool brain_under_root(const char *canonRoot, const char *canonPath) {
  size_t rl = SDL_strlen(canonRoot);
  if (rl == 0) return false;
#ifdef _WIN32
  if (SDL_strncasecmp(canonPath, canonRoot, rl) != 0) return false;
#else
  if (SDL_strncmp(canonPath, canonRoot, rl) != 0) return false;
#endif
  char sep = canonPath[rl];
  return sep == '\0' || sep == '/' || sep == '\\';
}

/* Canonicalize `path` (resolving .., symlinks, and relative-to-cwd) and
 * verify it lands inside `root`. Writes the resolved absolute path into
 * `out` and returns true on success. For a not-yet-existing file (write
 * mode) the parent directory is canonicalized instead, so new files under
 * the root are allowed while escapes are still rejected. */
static bool brain_resolve_in_root(const char *root, const char *path,
                                  char *out, size_t outlen) {
  if (!path || !path[0]) return false;
#ifdef _WIN32
  char  rootCanon[LUA_BRAINS_PATH_MAX];
  char  canon[LUA_BRAINS_PATH_MAX * 2];
  DWORD rn = GetFullPathNameA(root, sizeof(rootCanon), rootCanon, NULL);
  DWORD cn = GetFullPathNameA(path, sizeof(canon), canon, NULL);
  if (rn == 0 || rn >= sizeof(rootCanon)) return false;
  if (cn == 0 || cn >= sizeof(canon)) return false;
  if (!brain_under_root(rootCanon, canon)) return false;
  SDL_strlcpy(out, canon, outlen);
  return true;
#else
  char *rootCanon = realpath(root, NULL);
  if (!rootCanon) return false;

  bool  ok    = false;
  char *canon = realpath(path, NULL);     /* full resolve if it exists */
  if (canon) {
    ok = brain_under_root(rootCanon, canon);
    if (ok) SDL_strlcpy(out, canon, outlen);
    free(canon);
  } else {
    /* New file: canonicalize the parent dir and re-append the basename. */
    char  tmp[LUA_BRAINS_PATH_MAX * 2];
    char *parent;
    const char *base;
    char *slash;
    SDL_strlcpy(tmp, path, sizeof(tmp));
    slash = SDL_strrchr(tmp, '/');
    if (slash) { *slash = '\0'; base = slash + 1; parent = realpath(tmp[0] ? tmp : ".", NULL); }
    else       { base = tmp;    parent = realpath(".", NULL); }
    if (parent) {
      char full[LUA_BRAINS_PATH_MAX * 2];
      SDL_snprintf(full, sizeof(full), "%s/%s", parent, base);
      ok = brain_under_root(rootCanon, full);
      if (ok) SDL_strlcpy(out, full, outlen);
      free(parent);
    }
  }
  free(rootCanon);
  return ok;
#endif
}

/* Jailed io.open(filename [, mode]): path-check, then delegate to the real
 * io.open (upvalue 1) with the resolved path. upvalue 2 is the brain root. */
static int l_jailed_io_open(lua_State *L) {
  const char *fname = luaL_checkstring(L, 1);
  const char *mode  = luaL_optstring(L, 2, "r");
  const char *root  = lua_tostring(L, lua_upvalueindex(2));
  char resolved[LUA_BRAINS_PATH_MAX * 2];
  int  base;

  if (!brain_resolve_in_root(root, fname, resolved, sizeof(resolved))) {
    lua_pushnil(L);
    lua_pushfstring(L, "io.open: '%s' is outside the brain directory", fname);
    return 2;
  }
  base = lua_gettop(L);
  lua_pushvalue(L, lua_upvalueindex(1)); /* the real io.open */
  lua_pushstring(L, resolved);
  lua_pushstring(L, mode);
  lua_call(L, 2, LUA_MULTRET);
  return lua_gettop(L) - base;
}

/* Jailed loadfile(filename): path-check, then load as TEXT only (rejects
 * bytecode) with the normal sandboxed _ENV. upvalue 1 is the brain root. */
static int l_jailed_loadfile(lua_State *L) {
  const char *fname = luaL_checkstring(L, 1);
  const char *root  = lua_tostring(L, lua_upvalueindex(1));
  char   resolved[LUA_BRAINS_PATH_MAX * 2];
  size_t len = 0;
  char  *src;

  if (!brain_resolve_in_root(root, fname, resolved, sizeof(resolved))) {
    lua_pushnil(L);
    lua_pushfstring(L, "loadfile: '%s' is outside the brain directory", fname);
    return 2;
  }
  src = sdl_load_file(resolved, &len);
  if (!src) {
    lua_pushnil(L);
    lua_pushfstring(L, "loadfile: cannot open '%s'", fname);
    return 2;
  }
  if (luaL_loadbufferx(L, src, len, resolved, "t") != LUA_OK) {
    SDL_free(src);
    lua_pushnil(L);
    lua_insert(L, -2); /* nil, errmsg */
    return 2;
  }
  SDL_free(src);
  return 1; /* the loaded chunk */
}

/* Jailed os.remove(filename): path-check, then remove. upvalue 1 = root. */
static int l_jailed_os_remove(lua_State *L) {
  const char *fname = luaL_checkstring(L, 1);
  const char *root  = lua_tostring(L, lua_upvalueindex(1));
  char resolved[LUA_BRAINS_PATH_MAX * 2];

  if (!brain_resolve_in_root(root, fname, resolved, sizeof(resolved))) {
    lua_pushnil(L);
    lua_pushfstring(L, "os.remove: '%s' is outside the brain directory", fname);
    return 2;
  }
  if (remove(resolved) != 0) {
    lua_pushnil(L);
    lua_pushfstring(L, "os.remove: cannot remove '%s'", fname);
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

/* Generic jail for a C binding whose first argument is a filesystem path
 * (e.g. gh_opt_log.open / gh_opt_log.append / gh_shield.load). Validates the
 * path against the brain root, then forwards the call to the original function
 * (upvalue 1) with the sanitized path substituted for arg 1 and the remaining
 * arguments unchanged. upvalue 2 is the brain root. */
static int l_jailed_path_arg1(lua_State *L) {
  const char *p    = lua_tostring(L, 1);
  const char *root = lua_tostring(L, lua_upvalueindex(2));
  char resolved[LUA_BRAINS_PATH_MAX * 2];
  int  n, i;

  if (p == NULL || !brain_resolve_in_root(root, p, resolved, sizeof(resolved))) {
    lua_pushnil(L);
    lua_pushfstring(L, "blocked: path '%s' is outside the brain directory",
                    p ? p : "(nil)");
    return 2;
  }
  n = lua_gettop(L);
  lua_pushvalue(L, lua_upvalueindex(1));        /* original function */
  lua_pushstring(L, resolved);                  /* sanitized arg 1 */
  for (i = 2; i <= n; i++) lua_pushvalue(L, i); /* original args 2..n */
  lua_call(L, n, LUA_MULTRET);
  return lua_gettop(L) - n;
}

/* Replace tbl[method] with l_jailed_path_arg1 wrapping the original, if the
 * global `tbl` exists and `method` is a function. No-op otherwise (brains that
 * don't register the binding are unaffected). */
static void brain_jail_table_method(lua_State *L, const char *tbl,
                                     const char *method, const char *root) {
  lua_getglobal(L, tbl);
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, method);
    if (lua_isfunction(L, -1)) {
      lua_pushstring(L, root);
      lua_pushcclosure(L, l_jailed_path_arg1, 2); /* upvalues: original, root */
      lua_setfield(L, -2, method);
    } else {
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
}

/* Lock down the standard libraries of a freshly-opened brain VM. `root`
 * is the brain's own directory: the only place file access is permitted. */
static void brain_apply_sandbox(lua_State *L, const char *root) {
  static const char *kill_io[]    = { "popen", "lines", "input", "output",
                                      "tmpfile", NULL };
  static const char *kill_os[]    = { "execute", "exit", "rename", "getenv",
                                      "tmpname", "setlocale", NULL };
  static const char *kill_debug[] = { "sethook", "gethook", "getupvalue",
                                      "setupvalue", "upvalueid", "upvaluejoin",
                                      "getlocal", "setlocal", "getregistry",
                                      "setmetatable", "debug", NULL };
  int i;

  /* io: jail io.open, drop the arbitrary-path / shell members. */
  lua_getglobal(L, "io");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "open");           /* real io.open */
    lua_pushstring(L, root);
    lua_pushcclosure(L, l_jailed_io_open, 2);
    lua_setfield(L, -2, "open");
    for (i = 0; kill_io[i]; i++) {
      lua_pushnil(L);
      lua_setfield(L, -2, kill_io[i]);
    }
  }
  lua_pop(L, 1);

  /* base loaders: jail loadfile (text-only), remove load/dofile. */
  lua_pushstring(L, root);
  lua_pushcclosure(L, l_jailed_loadfile, 1);
  lua_setglobal(L, "loadfile");
  lua_pushnil(L); lua_setglobal(L, "load");
  lua_pushnil(L); lua_setglobal(L, "dofile");

  /* os: jail os.remove, drop process/env mutators; keep time/clock/date. */
  lua_getglobal(L, "os");
  if (lua_istable(L, -1)) {
    lua_pushstring(L, root);
    lua_pushcclosure(L, l_jailed_os_remove, 1);
    lua_setfield(L, -2, "remove");
    for (i = 0; kill_os[i]; i++) {
      lua_pushnil(L);
      lua_setfield(L, -2, kill_os[i]);
    }
  }
  lua_pop(L, 1);

  /* debug: keep only traceback + getinfo; drop the introspection that
   * could reach the originals we just replaced or hook the VM. */
  lua_getglobal(L, "debug");
  if (lua_istable(L, -1)) {
    for (i = 0; kill_debug[i]; i++) {
      lua_pushnil(L);
      lua_setfield(L, -2, kill_debug[i]);
    }
  }
  lua_pop(L, 1);

  /* package: no native code. Drop loadlib + cpath and the two C loader
   * searchers (slots 3 and 4 of the freshly-opened searchers table). The
   * Lua + SDL source searchers remain, so require() of brain modules works. */
  lua_getglobal(L, "package");
  if (lua_istable(L, -1)) {
    lua_pushnil(L); lua_setfield(L, -2, "loadlib");
    lua_pushnil(L); lua_setfield(L, -2, "cpath");
    lua_getfield(L, -1, "searchers");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_getfield(L, -1, "loaders"); } /* LuaJIT/5.1 name */
    if (lua_istable(L, -1)) {
      lua_pushnil(L); lua_rawseti(L, -2, 4);
      lua_pushnil(L); lua_rawseti(L, -2, 3);
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  /* C brain bindings that fopen() a brain-supplied path are a parallel file
   * API; jail them to the brain directory too. These globals exist only for
   * brains that register them (e.g. GoalHunter); the wrapper is a no-op
   * otherwise. */
  brain_jail_table_method(L, "gh_opt_log", "open",   root);
  brain_jail_table_method(L, "gh_opt_log", "append", root);
  brain_jail_table_method(L, "gh_shield",  "load",   root);
}


bool luaBrainInstanceCreate(LuaBrainInstance *inst, const char *path,
                            const char *name, ClientSim *cs,
                            aiType aiMode, bool debug_mode,
                            int player_num) {
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

  /* Zero the BotContext* slot before any C binding can run. Lua does not
   * zero-init extraspace, and brain.open() below can reach botFromLua via
   * gh_threat / cpf bindings. botManagerAddBot writes the real pointer
   * after this function returns. */
  *(void **)lua_getextraspace(L) = NULL;

  luaL_openlibs(L);

  /* Replace stock print() (writes to stdout) with one that routes brain
   * output through wb_log, so brains stay silent unless WINBOLO_LOG asks
   * for them. */
  lua_pushcfunction(L, l_brain_log_print);
  lua_setglobal(L, "print");

  brainCoreRegisterConstants(L);

  /* Signal to the brain whether it's running under BrainTest (debug) or
   * a release host (WinBolo / WinBoloDS).  Brains use this to skip debug
   * output and overlay calls when running in production. */
  lua_pushboolean(L, debug_mode);
  lua_setglobal(L, "BRAIN_DEBUG_MODE");

  /* print2 mirrors BRAIN_DEBUG_MODE: writes the per-tick log only in
   * debug mode. The Lua side gates every print2 call on this so opt
   * builds and non-debug runs pay zero I/O cost. s_bd_print2 (winbolods
   * -bd-noprint2) silences the stream without turning debug mode off. */
  lua_pushboolean(L, debug_mode && s_bd_print2);
  lua_setglobal(L, "_PRINT2_ENABLED");

  /* -bd-noviz: force this bot's viz collect mode OFF so viz.lua's draw
   * wrappers emit nothing (the overlay Lua→C calls are the cost, not just
   * the .btr bytes). Same mechanism BrainTest's V-window per-bot radio
   * uses, so is_on() gates the viz-only precompute too. */
  if (debug_mode && !s_bd_viz) {
    lua_pushstring(L, "off");
    lua_setglobal(L, "_BT_VIZ_COLLECT");
  }

  lua_pushboolean(L, s_profile);
  lua_setglobal(L, "BRAIN_PROFILE");

  lua_pushboolean(L, s_profile_log);
  lua_setglobal(L, "BRAIN_PROFILE_LOG");

  lua_pushboolean(L, s_instr_profile);
  lua_setglobal(L, "BRAIN_INSTR_PROFILE");

  /* BRAIN_LOG_JSON drives the brain's JSONL behavior log. Force it on
   * whenever debug mode is on — there's no scenario where you'd want
   * debug logging without the structured trace too. _JSONL_LOGGER_ENABLED
   * is the parallel player-0 gate inside init.lua's log-open; set it
   * here too so player 0's brain_p0.jsonl actually opens. */
  bool log_json_eff = (s_log_json || debug_mode) && s_bd_jsonl;
  lua_pushboolean(L, log_json_eff);
  lua_setglobal(L, "BRAIN_LOG_JSON");
  lua_pushboolean(L, log_json_eff);
  lua_setglobal(L, "_JSONL_LOGGER_ENABLED");

  /* Pool visualizer strings (desc, loc_reason, etc.) — on in debug mode,
   * off in --opt production mode to eliminate GC pressure. Also driven by the
   * explicit s_pool_viz flag (luaBrainsSetProfile's third arg): BrainTest's
   * profile-log sets it so the pool breakdown is captured into the .btr and
   * its "P" replay window has real data. A headless winbolods -profile-log
   * leaves it off — it wants timings only, not the pool-string GC cost. The
   * pool-viz code is runtime-gated (not stripped from opt), so enabling the
   * global is enough. */
  lua_pushboolean(L, (debug_mode && s_bd_pool) || s_pool_viz);
  lua_setglobal(L, "BRAIN_POOL_VIZ");

  /* Per-category debug log gates. All require BRAIN_DEBUG_MODE to be on
   * (debug builds strip the whole block via lua_strip's --strip-block
   * "if BRAIN_DEBUG_MODE" prefix). Signal-rich categories default ON
   * when debug is on; chatty ones default OFF so print2_bot<N>.log
   * stays grep-able. */
  lua_pushboolean(L, debug_mode);  lua_setglobal(L, "BRAIN_LOG_GOALS");   /* goal transitions */
  lua_pushboolean(L, debug_mode);  lua_setglobal(L, "BRAIN_LOG_BUILDER"); /* wall/build decisions */
  lua_pushboolean(L, false);       lua_setglobal(L, "BRAIN_LOG_SCORES");  /* FINAL_SCORES dump every replan */
  lua_pushboolean(L, false);       lua_setglobal(L, "BRAIN_LOG_SWERVE");  /* per-tick swerve trace */

  /* RUN_SCRIPT_PATH: non-empty string = script to run after Brain.open; nil otherwise. */
  if (s_run_script_path[0]) {
    lua_pushstring(L, s_run_script_path);
  } else {
    lua_pushnil(L);
  }
  lua_setglobal(L, "RUN_SCRIPT_PATH");

  /* BRAIN_INIT_ARG: optional per-bot text from -bot-init's [..] suffix; string
   * when staged, nil otherwise. Consume-once so it applies only to this brain —
   * the next create defaults back to nil unless luaBrainsSetNextInitArg re-stages. */
  if (s_next_init_arg[0]) {
    lua_pushstring(L, s_next_init_arg);
  } else {
    lua_pushnil(L);
  }
  lua_setglobal(L, "BRAIN_INIT_ARG");
  s_next_init_arg[0] = '\0';

  /* BRAIN_START_ENGINE_TICK: the server sim's tick at the moment this brain
   * was created. Always a number (0 = created at game start), never nil, so
   * every host — including BrainTest's in-process create, which stages
   * nothing — sees a usable value. The brain seeds state.tick with half of
   * it (brains think once per two engine ticks), so a wave bot's print2 and
   * jsonl tick numbers continue the session clock instead of restarting at 0
   * and colliding with every earlier life. Consume-once. */
  lua_pushinteger(L, (lua_Integer)s_next_start_engine_tick);
  lua_setglobal(L, "BRAIN_START_ENGINE_TICK");
  s_next_start_engine_tick = 0;

  brainCoreRegisterGetTerrain(L, &inst->worldPtr);

  /* Create C pathfinder and register cpf_* globals */
  inst->pathfinder = brainPathfinderCreate();
  if (inst->pathfinder) {
    /* Preheat slate arrays: malloc + page-commit so the first
     * dijkstra_start (typically tick 1) doesn't pay ~1-2 ms of
     * lazy page-fault cost on a fresh process. */
    brainPathfinderDijkstraPreheat(inst->pathfinder);
    brainCoreRegisterPathfinder(L, &inst->pathfinder);
  }

  /* Create C world simulator and register wsim_* globals */
  inst->worldsim = brainWorldSimCreate();
  if (inst->worldsim) {
    brainCoreRegisterWorldSim(L, &inst->worldsim);
  }

  /* Overlay bindings ARE registered on both hosts. BrainTest uses
   * this code path (it links luaBrainInstanceCreate, not a parallel
   * variant), so omitting the bindings left every brain's viz.* call
   * short-circuited at the Lua wrapper (`if not overlay_text then
   * return end`) — V-dialog rows toggled but no map overlays drew.
   *
   * The per-tick cost the previous "omit the binding" path was
   * avoiding is now killed at the Lua layer instead: in the release
   * client (debug_mode == false) we set _BT_VIZ_SUPPRESS_ALL=true,
   * which makes viz.is_on() return false for every id except
   * hud_resources. The brain's viz wrappers gate every draw call on
   * viz.is_on, so suppressed draws never reach the C closure — same
   * effective cost as before, without BrainTest collateral damage. */
  overlayCmdBufferInit(&inst->overlay);
  inst->overlayPtr = &inst->overlay;
  brainCoreRegisterOverlay(L, &inst->overlayPtr);

  /* Release-client suppression: makes viz.is_on() report off for
   * everything except hud_resources, so viz.* draw calls early-return
   * at the Lua layer before crossing into the C overlay closures. */
  lua_pushboolean(L, !debug_mode);
  lua_setglobal(L, "_BT_VIZ_SUPPRESS_ALL");

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
  /* pill_contrib bindings (pillcontrib_clear / _begin_pill / _add_tile)
   * for the per-pill danger overlay (shift-2 in BrainTest). NULL-callback
   * no-op outside BrainTest. GoalHunter-specific — lives in the bot's
   * own C directory so the engine's brain runtime stays generic. */
  naPillContribRegister(L, player_num);
  /* gh_threat — GoalHunter threat-grid C kernel. Provides terrain
   * factor cache + pill stamping. Tunables are set from Lua via
   * gh_threat.configure so cloners can tweak constants without
   * recompiling. */
  naThreatRegister(L);
  naShieldStampRegister(L);
  naOptLogRegister(L);
  /* The crash-log writer lives in bolo_static, which cannot call into the
   * brain C bindings directly; hand it the flush entry point instead so a
   * brain crash report is written after the queued print2 output, not before
   * it. Idempotent — every brain instance installs the same function. */
  brainCoreSetLogFlushHook(naOptLogFlushSync);
  naAttackRegister(L);

  /* Compute brain directory once at function scope so it can be reused for
   * the SDL searcher, BRAIN_DIR global, and opt/ detection below. */
  char brainDir[LUA_BRAINS_PATH_MAX];
  if (!extract_brain_dir(path, brainDir, sizeof(brainDir))) {
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

  /* If not in debug mode and opt/init.lua exists, treat opt/ as the
   * effective brain directory — require() and the init load both use it.
   * Any brain that ships an opt/ directory gets this automatically. */
  char effectiveDir[LUA_BRAINS_PATH_MAX];
  SDL_strlcpy(effectiveDir, brainDir, sizeof(effectiveDir));
  if (!debug_mode) {
    char optInit[LUA_BRAINS_PATH_MAX];
    SDL_snprintf(optInit, sizeof(optInit), "%s/opt/init.lua", brainDir);
    SDL_IOStream *check = SDL_IOFromFile(optInit, "r");
    if (check) {
      SDL_CloseIO(check);
      SDL_snprintf(effectiveDir, sizeof(effectiveDir), "%s/opt", brainDir);
    }
  }

  /* Lock the VM down unless the host opted out (--allow-unsafe-brains /
   * BrainTest). The jail root is the brain's own directory with any trailing
   * /opt stripped, so both the base and opt/ trees — and the committed
   * los_stamp_cache.lua, which lives in the non-opt dir — resolve under it.
   * Run before the searcher shuffle below so dropping the native package
   * searchers composes with inserting the SDL source searcher. */
  if (!s_allow_unsafe) {
    char brainRoot[LUA_BRAINS_PATH_MAX];
    size_t rl;
    SDL_strlcpy(brainRoot, brainDir, sizeof(brainRoot));
    rl = SDL_strlen(brainRoot);
    if (rl >= 4 &&
        (SDL_strcasecmp(brainRoot + rl - 4, "/opt") == 0 ||
         SDL_strcasecmp(brainRoot + rl - 4, "\\opt") == 0)) {
      brainRoot[rl - 4] = '\0';
    }
    brain_apply_sandbox(L, brainRoot);
  }

  setup_brain_package_path(L, path);

  /* Override package.path to load from effectiveDir first. */
  {
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    const char *curPath = lua_tostring(L, -1);
    char newPath[LUA_BRAINS_PATH_MAX * 2];
    SDL_snprintf(newPath, sizeof(newPath), "%s/?.lua;%s", effectiveDir, curPath);
    lua_pop(L, 1);
    lua_pushstring(L, newPath);
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);
  }

  /* Install SDL-based searcher so require() works on Android assets.
   * Insert it at position 2 in package.searchers (before the default
   * file searcher at position 3). */
  {
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "searchers");
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_getfield(L, -1, "loaders"); } /* LuaJIT/5.1 name */
    int nSearchers = (int)lua_rawlen(L, -1);
    /* Shift existing entries up to make room at index 2 */
    for (int i = nSearchers; i >= 2; i--) {
      lua_rawgeti(L, -1, i);
      lua_rawseti(L, -2, i + 1);
    }
    lua_pushstring(L, effectiveDir);
    lua_pushcclosure(L, sdl_lua_searcher, 1);
    lua_rawseti(L, -2, 2);

    /* Sandboxed: drop every searcher past the SDL one (slot 2). The stock
     * Lua searcher would otherwise survive at slot 3 and load modules via
     * package.path — which still carries the openlibs system defaults and
     * "./?.lua" (cwd), letting require() read .lua/.luac outside the brain
     * dir and load bytecode. Leaving only preload (slot 1) + SDL (slot 2)
     * confines require() to the path-jailed, text-only SDL searcher. The
     * native C searchers were already removed in brain_apply_sandbox. Cap
     * the loop generously; the table never holds more than ~5 entries. */
    if (!s_allow_unsafe) {
      for (int i = 3; i <= 16; i++) {
        lua_pushnil(L);
        lua_rawseti(L, -2, i);
      }
    }
    lua_pop(L, 2); /* pop searchers + package */
  }

#ifdef WINBOLO_LUAJIT
  /* Runtime 5.4->5.1 compatibility for brains on LuaJIT: swallow the GC modes
   * LuaJIT lacks (generational/incremental) and backfill stdlib functions the
   * 5.4 brains may call. Runs after the sandbox/searcher setup, just before the
   * brain loads, so nothing clobbers it. */
  (void)luaL_dostring(L,
    "local _cg=collectgarbage\n"
    "collectgarbage=function(o,...) if o=='generational' or o=='incremental' then return 0 end return _cg(o,...) end\n"
    "if not math.type then math.type=function(x) if type(x)~='number' then return nil end return (x%1==0) and 'integer' or 'float' end end\n"
    "table.unpack=table.unpack or unpack\n"
    "if not table.move then table.move=function(a1,f,e,t,a2) a2=a2 or a1 for i=0,e-f do a2[t+i]=a1[f+i] end return a2 end end\n"
    "math.maxinteger=math.maxinteger or 9223372036854775807\n"
    "math.mininteger=math.mininteger or -9223372036854775808\n");

  /* Larger JIT mcode areas than LuaJIT's defaults (sizemcode: KB per
   * area, maxmcode: KB total per state). Every time an area fills, the
   * allocator probes for a new one within arm64's +/-128MB branch range
   * of the interpreter — a window every brain's lua_State competes for.
   * Small default areas mean frequent allocations, fragmentation, and
   * eventually probe exhaustion, which aborts the trace being compiled
   * (silently costing brains JIT coverage in long sessions) and is the
   * path where the macOS hardened-runtime W^X leak lived (2.02 Sentry
   * 24dae975; see cmake/patches/luajit_fix_osx_hrt_thread_leak.cmake).
   * Fewer, larger areas make that path rare on every platform. */
  (void)luaL_dostring(L,
    "if jit and jit.opt then jit.opt.start('sizemcode=256','maxmcode=4096') end\n");
#endif

  /* Load and execute the brain script.
   * Try SDL_IOFromFile first (works on Android assets), then fall back
   * to luaL_loadfile for regular filesystem paths. If the path is a
   * directory, try appending init.lua. */
  {
    size_t srcLen = 0;
    char *src = NULL;
    char resolvedPath[LUA_BRAINS_PATH_MAX];
    SDL_strlcpy(resolvedPath, path, sizeof(resolvedPath));

    /* If path ends with / or \, load init.lua from effectiveDir. */
    size_t plen = SDL_strlen(resolvedPath);
    if (plen > 0 && (resolvedPath[plen-1] == '/' || resolvedPath[plen-1] == '\\')) {
      SDL_snprintf(resolvedPath, sizeof(resolvedPath), "%s/init.lua", effectiveDir);
    } else if (effectiveDir[0] != brainDir[0] ||
               SDL_strcmp(effectiveDir, brainDir) != 0) {
      /* Non-directory path: rebase onto effectiveDir if it differs. */
      const char *fname = SDL_strrchr(path, '/');
      if (!fname) fname = SDL_strrchr(path, '\\');
      fname = fname ? fname + 1 : path;
      SDL_snprintf(resolvedPath, sizeof(resolvedPath), "%s/%s", effectiveDir, fname);
    }

    src = sdl_load_file(resolvedPath, &srcLen);
    if (src) {
      int loadErr = luaL_loadbuffer(L, src, srcLen, resolvedPath);
      SDL_free(src);
      if (loadErr != LUA_OK) {
        const char *e = lua_tostring(L, -1);
        WB_LOG_ERROR(WB_LOG_CAT_LUA, "luaBrainInstance: failed to load '%s': %s",
                resolvedPath, e);
        if (debug_mode) { FILE *ef = fopen("brain_error.log", "a");
        if (ef) { fprintf(ef, "luaBrainInstance load (buffer) '%s' error: %s\n", resolvedPath, e ? e : "(null)"); fclose(ef); } }
        lua_close(L);
        return false;
      }
    } else if (luaL_loadfile(L, resolvedPath) != LUA_OK) {
      const char *e = lua_tostring(L, -1);
      WB_LOG_ERROR(WB_LOG_CAT_LUA, "luaBrainInstance: failed to load '%s': %s",
              resolvedPath, e);
      if (debug_mode) { FILE *ef = fopen("brain_error.log", "a");
      if (ef) { fprintf(ef, "luaBrainInstance load (file) '%s' error: %s\n", resolvedPath, e ? e : "(null)"); fclose(ef); } }
      lua_close(L);
      return false;
    }
  }
  if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
    const char *e = lua_tostring(L, -1);
    WB_LOG_ERROR(WB_LOG_CAT_LUA, "luaBrainInstance: error running '%s': %s",
            path, e);
    if (debug_mode) { FILE *ef = fopen("brain_error.log", "a");
    if (ef) { fprintf(ef, "luaBrainInstance run '%s' error: %s\n", path, e ? e : "(null)"); fclose(ef); } }
    lua_close(L);
    return false;
  }
  if (!lua_istable(L, -1)) {
    WB_LOG_ERROR(WB_LOG_CAT_LUA, "luaBrainInstance: '%s' did not return a table", path);
    if (debug_mode) { FILE *ef = fopen("brain_error.log", "a");
    if (ef) { fprintf(ef, "luaBrainInstance: '%s' did not return a table\n", path); fclose(ef); } }
    lua_close(L);
    return false;
  }
  lua_setglobal(L, "brain");

  /* Call brain.open(info) */
  brainDataMakeInfo(cs, &inst->bInfo, true, aiMode);
  inst->worldPtr = inst->bInfo.theWorld;
  /* Set the map pointer now so Brain.open can pre-warm the edge-cost table
   * via cpf.rebuild_edge_costs() — same map pointer set each tick. */
  if (inst->pathfinder) {
    brainPathfinderSetMap(inst->pathfinder, inst->bInfo.theWorld);
  }
  /* Seed math.random BEFORE brain.open runs. This ordering is the whole
   * point: GoalHunter draws replan_offset inside open (init.lua), and that
   * single draw staggers the bot's entire replan cadence for the rest of the
   * game. Seeding after create returns would leave exactly that draw coming
   * from PUC-Lua's per-process auto-seed, which is the divergence the knob
   * exists to remove. */
  if (s_defaultRandomSeed != 0) {
    /* Note: the local L, not inst->L -- inst->L is not assigned until after
     * brain.open succeeds, further down. */
    seedRandomOnState(L, s_defaultRandomSeed + (long)player_num);
  }

  if (!brainCoreCallMethod(L, &inst->bInfo, "open")) {
    brainDataExtractInfo(cs, &inst->bInfo);
    lua_close(L);
    return false;
  }
  brainDataExtractInfo(cs, &inst->bInfo);

  /* Install the per-think allocation counter now that every early-exit
   * lua_close path is behind us, so a failed create can't leak the
   * counter context. Deliberately after brain.open — script load and
   * open-time allocations are not part of the per-think churn we measure. */
  brainCoreInstallAllocCounter(L);

  inst->L = L;
  inst->running = true;
  return true;
}

bool luaBrainInstanceTick(LuaBrainInstance *inst) {
  bool ok;

  if (!inst->running) {
    return false;
  }

#if defined(WINBOLO_LUAJIT) && defined(__APPLE__) && defined(__aarch64__)
  /* LuaJIT's hardened-runtime path toggles per-thread JIT write
   * protection; a longjmp out of mcode allocation can leave this thread
   * in write mode, which SIGBUSes the next trace entry (2.02 Sentry
   * 24dae975). Re-arm execute mode at the tick boundary so a stray
   * write-mode thread self-heals — a per-thread register write, so
   * effectively free. Root cause is patched in
   * cmake/patches/luajit_fix_osx_hrt_thread_leak.cmake; this is the
   * belt-and-braces layer. */
  pthread_jit_write_protect_np(1);
#endif

  /* Reset key state before brain runs (matches bot_manager) */
  *clientSimGetBrainHoldKeys(inst->cs) = 0;
  *clientSimGetBrainTapKeys(inst->cs) = 0;

  brainDataMakeInfo(inst->cs, &inst->bInfo, inst->isFirst,
                    *clientSimGetAllowComputerTanks(inst->cs));
  inst->isFirst = false;
  inst->bInfo.operation = BRAIN_THINK;

  inst->worldPtr = inst->bInfo.theWorld;
  if (inst->pathfinder) {
    brainPathfinderSetMap(inst->pathfinder, inst->bInfo.theWorld);
  }
  if (inst->worldsim) {
    brainWorldSimSetMap(inst->worldsim, inst->bInfo.theWorld);
  }
  /* wasKilled is recorded on the instance so the caller (bot_manager
   * runBotThinkJobImpl) can disambiguate the budget-abort recovery
   * path from a real Lua error after the call returns. Reset to false
   * here so a successful tick clears stale state from a prior abort. */
  inst->wasKilled = false;
  ok = brainCoreCallThink(inst->L, &inst->bInfo, &inst->wasKilled);
  /* Always extract, including on a dead tick. The extract is not only about
   * key/build outputs: it frees every per-tick array brainDataMakeInfo just
   * allocated (allies, player_bots, base, pillview, viewdata, events,
   * messages), so skipping it leaked all of them on every tick a bot spent
   * dead. It is also the only path that delivers sendmessage, and a dying bot
   * needs to broadcast a goal-clear so teammates stop counting the enemy it
   * was engaging as taken care of -- exactly when that enemy has just become
   * free. A dead player can still type in the real game.
   *
   * The one output that must NOT be applied while dead is the tank controls;
   * that is gated inside brainDataExtractInfo on value->dead, next to the
   * build request which already self-gated on dead armour. */
  brainDataExtractInfo(inst->cs, &inst->bInfo);

  return ok;
}

void luaBrainSetTickInputs(LuaBrainInstance *inst,
                           double lastThinkMs,
                           double targetMs,
                           bool   wasKilled,
                           int    tierOverride) {
    lua_State *L;
    int top;

    if (inst == NULL || !inst->running || inst->L == NULL) {
        return;
    }
    L = inst->L;
    top = lua_gettop(L);

    /* Capacity-tier pin for measurement runs. Written as the global the brain
     * already honours (_BT_TIER_OVERRIDE, read every tick), so no brain change
     * is needed.
     *
     * Only ever WRITTEN, never cleared. BrainTest's tier panel evals this same
     * global into the bot, and this runs on the producer before every think --
     * so clearing it when the server-side override is off would erase the
     * user's panel selection every tick, before the brain could read it. The
     * server parses its args once, so there is no on->off transition here to
     * serve; a future mid-session control would want edge detection in
     * botManagerTick, which knows the previous value. */
    if (tierOverride >= 1 && tierOverride <= 10) {
        lua_pushinteger(L, tierOverride);
        lua_setglobal(L, "_BT_TIER_OVERRIDE");
    }

    lua_getglobal(L, "brain");
    if (!lua_istable(L, -1)) {
        lua_settop(L, top);
        return;
    }
    lua_pushnumber(L, lastThinkMs);
    lua_setfield(L, -2, "lastThinkMs");
    lua_pushnumber(L, targetMs);
    lua_setfield(L, -2, "targetMs");
    lua_pushboolean(L, wasKilled ? 1 : 0);
    lua_setfield(L, -2, "wasKilled");
    lua_settop(L, top);
}

void luaBrainSetDefaultRandomSeed(long base) { s_defaultRandomSeed = base; }

void luaBrainSeedRandom(LuaBrainInstance *inst, long seed) {
    if (inst == NULL) return;
    seedRandomOnState(inst->L, seed);
}

/* File-local, and takes the lua_State directly, because the caller that
 * matters runs inside luaBrainInstanceCreate BEFORE inst->L is assigned --
 * going through the instance there hits the NULL guard and silently does
 * nothing, which is exactly the bug this split fixes. Not in the header:
 * lua_State is not a visible type to every consumer of it. */
static void seedRandomOnState(lua_State *L, long seed) {
    int top;

    if (L == NULL) {
        return;
    }
    top = lua_gettop(L);

    /* math.randomseed(seed). PUC-Lua 5.4 auto-seeds per process, so without
     * this the brain's draws differ every run -- and they are not cosmetic:
     * replan_offset staggers a bot's entire replan cadence off one draw. The
     * caller combines a fixed seed with the player number so bots still differ
     * from one another while being identical across runs. */
    lua_getglobal(L, "math");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "randomseed");
        if (lua_isfunction(L, -1)) {
            lua_pushinteger(L, (lua_Integer)seed);
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                lua_pop(L, 1); /* discard the error; seeding is best-effort */
            }
        }
    }
    lua_settop(L, top);
}

void luaBrainInstanceDestroy(LuaBrainInstance *inst) {
  if (!inst->running || inst->L == NULL) {
    return;
  }

  brainDataMakeInfo(inst->cs, &inst->bInfo, false,
                    *clientSimGetAllowComputerTanks(inst->cs));
  inst->bInfo.operation = BRAIN_CLOSE;
  inst->worldPtr = inst->bInfo.theWorld;
  brainCoreCallMethod(inst->L, &inst->bInfo, "close");
  brainDataExtractInfo(inst->cs, &inst->bInfo);

  brainPathfinderDestroy(inst->pathfinder);
  inst->pathfinder = NULL;

  brainWorldSimDestroy(inst->worldsim);
  inst->worldsim = NULL;

  overlayCmdBufferDestroy(&inst->overlay);
  inst->overlayPtr = NULL;

  /* Restore the original allocator and free the counter context before
   * tearing down the state, so the wrapper isn't left pointing at freed
   * memory during lua_close's final sweep. */
  brainCoreUninstallAllocCounter(inst->L);

  lua_close(inst->L);
  inst->L = NULL;
  inst->running = false;
}

void luaBrainInstanceSetDebugMode(LuaBrainInstance *inst, bool enabled) {
  if (!inst || !inst->L) return;
  lua_pushboolean(inst->L, enabled);
  lua_setglobal(inst->L, "BRAIN_DEBUG_MODE");
  /* Keep print2 gate in sync — flipping debug mode mid-run should
   * also start/stop the per-tick log file. */
  lua_pushboolean(inst->L, enabled);
  lua_setglobal(inst->L, "_PRINT2_ENABLED");
  /* Per-category gates follow the master debug flag for the signal-rich
   * categories; chatty ones (SCORES/SWERVE) stay off unless user toggles
   * them in their Lua state separately. */
  lua_pushboolean(inst->L, enabled); lua_setglobal(inst->L, "BRAIN_LOG_GOALS");
  lua_pushboolean(inst->L, enabled); lua_setglobal(inst->L, "BRAIN_LOG_BUILDER");
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

  /* User-supplied brains: SDL_GetPrefPath("WinBolo","WinBolo")/Brains —
     ~/Library/Application Support/WinBolo/WinBolo/Brains on macOS. Writable,
     survives app reinstall/upgrade (the bundle is read-only/code-signed).
     These are untrusted (a player drops in arbitrary .lua); they load through
     luaBrainInstanceCreate like every other brain, so the default sandbox
     (no --allow-unsafe-brains) jails each one to its own directory. */
  char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
  if (prefDir) {
    char userBrains[LUA_BRAINS_PATH_MAX];
    SDL_snprintf(userBrains, sizeof(userBrains), "%sBrains", prefDir);
    SDL_CreateDirectory(userBrains);  /* make the drop location discoverable; no-op if it exists */
    scan_brains_in(userBrains, "[user] ");
  #if defined(HAVE_ONNXRUNTIME) && !defined(__EMSCRIPTEN__)
    scan_onnx_brains_in(userBrains);
  #endif
    SDL_free(prefDir);
  }

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
  /* Singleton path (legacy "human player loads a script directly")
   * has no notion of a bot player_num — pillcontrib bindings will
   * write into slot 0 and the BrainTest viewer (which only ever
   * follows real bot indices) won't read it. Pass 0 explicitly. */
  if (!luaBrainInstanceCreate(&singletonInst, path, name,
                              cs,
                              *clientSimGetAllowComputerTanks(cs), false,
                              0)) {
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
    *clientSimGetBrainHoldKeys(cs) = 0;
    *clientSimGetBrainTapKeys(cs) = 0;

    BrainInfo bi;
    brainDataMakeInfo(cs, &bi, false, *clientSimGetAllowComputerTanks(cs));
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
    brainDataExtractInfo(cs, &bi);

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
