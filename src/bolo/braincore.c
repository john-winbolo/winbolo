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
 *Filename:      braincore.c
 *Author:        John Morrison
 *Purpose:
 *  Shared Lua brain helpers used by both client and server.
 *  Extracted from luabrainshandler.c — no client globals.
 *********************************************************/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#ifdef _WIN32
#include <process.h>  /* _getpid */
#define brc_getpid() _getpid()
#else
#include <unistd.h>
#define brc_getpid() getpid()
#endif

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "global.h"
#include "brain.h"
#include "brain_overlay.h"
#include "shells.h"
#include "client_enums.h"  /* sndEffects */
#include "util.h"
#include "braincore.h"
#include "brain_pathfinder.h"
#include "tank.h"  /* tankModPct. The movement rates themselves arrive on
                    * the pathfinder each think, so the stop predictor and
                    * the engine cannot drift apart */

/* C-side pill_grid from the gh_threat brain module (same link unit). */
extern float *naThreatGetPillGrid(lua_State *L);

/* ------------------------------------------------------------------ */
/* clock_us — high-resolution timer for Lua profiling                  */
/* ------------------------------------------------------------------ */

/* bt_yield — calls host event-pump callback; no-op if not set. */
static void (*g_yield_cb)(void) = NULL;
void brainCoreSetYieldCallback(void (*cb)(void)) { g_yield_cb = cb; }
static int l_bt_yield(lua_State *L) {
  (void)L;
  if (g_yield_cb) g_yield_cb();
  return 0;
}

static int l_clock_us(lua_State *L) {
  Uint64 now  = SDL_GetPerformanceCounter();
  Uint64 freq = SDL_GetPerformanceFrequency();
  /* Return microseconds as a Lua number (double has 53 bits of mantissa,
   * good for ~285 years of microseconds before precision loss). */
  double us = (double)now / (double)freq * 1000000.0;
  lua_pushnumber(L, us);
  return 1;
}

/* ------------------------------------------------------------------ */
/* get_terrain C closure with upvalue                                   */
/* ------------------------------------------------------------------ */

static int l_get_terrain_upvalue(lua_State *L) {
  const BYTE **worldPtrPtr = (const BYTE **)lua_touserdata(L, lua_upvalueindex(1));
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  if (worldPtrPtr == NULL || *worldPtrPtr == NULL || x < 0 || x > 255 || y < 0 || y > 255) {
    lua_pushinteger(L, 0);
  } else {
    lua_pushinteger(L, (*worldPtrPtr)[y * 256 + x]);
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* Per-brain init table                                                */
/* ------------------------------------------------------------------ */

void brainCoreSetInitTable(lua_State *L, const ScnTable *init) {
  int n = (init != NULL) ? (int)init->count : 0;
  int i;

  if (n > SCN_TABLE_MAX) n = SCN_TABLE_MAX;
  lua_createtable(L, 0, n);
  for (i = 0; i < n; i++) {
    if (init->kv[i].key[0] == '\0') continue;
    lua_pushstring(L, init->kv[i].value);
    lua_setfield(L, -2, init->kv[i].key);
  }
  lua_setglobal(L, "BRAIN_INIT");
}

/* ------------------------------------------------------------------ */
/* Constant registration                                               */
/* ------------------------------------------------------------------ */

void brainCoreRegisterConstants(lua_State *L) {
  /* Key command bits */
  lua_pushinteger(L, 1 << KEY_faster);    lua_setglobal(L, "KEY_FASTER");
  lua_pushinteger(L, 1 << KEY_slower);    lua_setglobal(L, "KEY_SLOWER");
  lua_pushinteger(L, 1 << KEY_turnleft);  lua_setglobal(L, "KEY_TURNLEFT");
  lua_pushinteger(L, 1 << KEY_turnright); lua_setglobal(L, "KEY_TURNRIGHT");
  lua_pushinteger(L, 1 << KEY_morerange); lua_setglobal(L, "KEY_MORERANGE");
  lua_pushinteger(L, 1 << KEY_lessrange); lua_setglobal(L, "KEY_LESSRANGE");
  lua_pushinteger(L, 1 << KEY_shoot);     lua_setglobal(L, "KEY_SHOOT");
  lua_pushinteger(L, 1 << KEY_dropmine);  lua_setglobal(L, "KEY_DROPMINE");
  lua_pushinteger(L, 1 << KEY_TankView);  lua_setglobal(L, "KEY_TANKVIEW");
  lua_pushinteger(L, 1 << KEY_PillView);  lua_setglobal(L, "KEY_PILLVIEW");

  /* Build modes */
  lua_pushinteger(L, BUILDMODE_FARM);     lua_setglobal(L, "BUILDMODE_FARM");
  lua_pushinteger(L, BUILDMODE_ROAD);     lua_setglobal(L, "BUILDMODE_ROAD");
  lua_pushinteger(L, BUILDMODE_BUILD);    lua_setglobal(L, "BUILDMODE_BUILD");
  lua_pushinteger(L, BUILDMODE_PBOX);     lua_setglobal(L, "BUILDMODE_PBOX");
  lua_pushinteger(L, BUILDMODE_MINE);     lua_setglobal(L, "BUILDMODE_MINE");

  /* Terrain type values */
  lua_pushinteger(L, BBUILDING);          lua_setglobal(L, "TERRAIN_BUILDING");
  lua_pushinteger(L, BRIVER);             lua_setglobal(L, "TERRAIN_RIVER");
  lua_pushinteger(L, BSWAMP);             lua_setglobal(L, "TERRAIN_SWAMP");
  lua_pushinteger(L, BCRATER);            lua_setglobal(L, "TERRAIN_CRATER");
  lua_pushinteger(L, BROAD);              lua_setglobal(L, "TERRAIN_ROAD");
  lua_pushinteger(L, BFOREST);            lua_setglobal(L, "TERRAIN_FOREST");
  lua_pushinteger(L, BRUBBLE);            lua_setglobal(L, "TERRAIN_RUBBLE");
  lua_pushinteger(L, BGRASS);             lua_setglobal(L, "TERRAIN_GRASS");
  lua_pushinteger(L, BHALFBUILDING);      lua_setglobal(L, "TERRAIN_HALFBUILDING");
  lua_pushinteger(L, BBOAT);              lua_setglobal(L, "TERRAIN_BOAT");
  lua_pushinteger(L, BDEEPSEA);           lua_setglobal(L, "TERRAIN_DEEPSEA");
  lua_pushinteger(L, BREFBASE_T);         lua_setglobal(L, "TERRAIN_REFBASE");
  lua_pushinteger(L, BPILLBOX_T);         lua_setglobal(L, "TERRAIN_PILLBOX");

  /* Terrain flag bits */
  lua_pushinteger(L, TERRAIN_MASK);       lua_setglobal(L, "TERRAIN_MASK");
  lua_pushinteger(L, TERRAIN_TANK_VIS);   lua_setglobal(L, "TERRAIN_TANK_VIS");
  lua_pushinteger(L, TERRAIN_PILL_VIS);   lua_setglobal(L, "TERRAIN_PILL_VIS");
  lua_pushinteger(L, TERRAIN_MINE);       lua_setglobal(L, "TERRAIN_MINE_FLAG");

  /* Object types */
  lua_pushinteger(L, OBJECT_TANK);        lua_setglobal(L, "OBJECT_TANK");
  lua_pushinteger(L, OBJECT_SHOT);        lua_setglobal(L, "OBJECT_SHOT");
  lua_pushinteger(L, OBJECT_PILLBOX);     lua_setglobal(L, "OBJECT_PILLBOX");
  lua_pushinteger(L, OBJECT_REFBASE);     lua_setglobal(L, "OBJECT_REFBASE");
  lua_pushinteger(L, OBJECT_BUILDMAN);    lua_setglobal(L, "OBJECT_BUILDMAN");
  lua_pushinteger(L, OBJECT_PARACHUTE);   lua_setglobal(L, "OBJECT_PARACHUTE");

  /* Object info flags */
  lua_pushinteger(L, OBJECT_HOSTILE);     lua_setglobal(L, "OBJECT_HOSTILE");
  lua_pushinteger(L, OBJECT_NEUTRAL);     lua_setglobal(L, "OBJECT_NEUTRAL");

  /* Neutral player sentinel */
  lua_pushinteger(L, NEUTRAL_PLAYER);     lua_setglobal(L, "NEUTRAL_PLAYER");

  /* Game event type constants */
  lua_pushinteger(L, EVENT_PILL_CAPTURED);   lua_setglobal(L, "EVENT_PILL_CAPTURED");
  lua_pushinteger(L, EVENT_BASE_CAPTURED);   lua_setglobal(L, "EVENT_BASE_CAPTURED");
  lua_pushinteger(L, EVENT_TANK_KILLED);     lua_setglobal(L, "EVENT_TANK_KILLED");
  lua_pushinteger(L, EVENT_SOUND);           lua_setglobal(L, "EVENT_SOUND");
  lua_pushinteger(L, EVENT_PILL_UPDATE);     lua_setglobal(L, "EVENT_PILL_UPDATE");
  lua_pushinteger(L, EVENT_BASE_UPDATE);     lua_setglobal(L, "EVENT_BASE_UPDATE");
  lua_pushinteger(L, EVENT_BASE_STOCK);      lua_setglobal(L, "EVENT_BASE_STOCK");
  lua_pushinteger(L, EVENT_PLAYER_LEAVE);    lua_setglobal(L, "EVENT_PLAYER_LEAVE");
  lua_pushinteger(L, EVENT_ASSISTANT_MSG);   lua_setglobal(L, "EVENT_ASSISTANT_MSG");
  lua_pushinteger(L, EVENT_LGM_LOST);        lua_setglobal(L, "EVENT_LGM_LOST");
  lua_pushinteger(L, EVENT_SOUND_TANK_HIT);  lua_setglobal(L, "EVENT_SOUND_TANK_HIT");
  lua_pushinteger(L, EVENT_SOUND_SHOOT);     lua_setglobal(L, "EVENT_SOUND_SHOOT");
  lua_pushinteger(L, EVENT_PING);            lua_setglobal(L, "EVENT_PING");

  /* Smart-ping kinds. A brain reads them off an EVENT_PING's data[2] — the
     event carries [sender, kind, xHi, xLo, yHi, yLo] in WORLD units — so a
     bot can act on the Bot Command ping its team sends it. */
  lua_pushinteger(L, PING_KIND_STANDARD);    lua_setglobal(L, "PING_KIND_STANDARD");
  lua_pushinteger(L, PING_KIND_CAUTION);     lua_setglobal(L, "PING_KIND_CAUTION");
  lua_pushinteger(L, PING_KIND_ASSIST);      lua_setglobal(L, "PING_KIND_ASSIST");
  lua_pushinteger(L, PING_KIND_ATTACK);      lua_setglobal(L, "PING_KIND_ATTACK");
  lua_pushinteger(L, PING_KIND_ON_MY_WAY);   lua_setglobal(L, "PING_KIND_ON_MY_WAY");
  lua_pushinteger(L, PING_KIND_BOT_COMMAND); lua_setglobal(L, "PING_KIND_BOT_COMMAND");

  /* Assistant message ID constants */
  lua_pushinteger(L, ASSIST_MSG_MAN_DEAD);           lua_setglobal(L, "ASSIST_MSG_MAN_DEAD");
  lua_pushinteger(L, ASSIST_MSG_NO_TREE);            lua_setglobal(L, "ASSIST_MSG_NO_TREE");
  lua_pushinteger(L, ASSIST_MSG_NO_BUILD);           lua_setglobal(L, "ASSIST_MSG_NO_BUILD");
  lua_pushinteger(L, ASSIST_MSG_NO_BUILD_BOAT);      lua_setglobal(L, "ASSIST_MSG_NO_BUILD_BOAT");
  lua_pushinteger(L, ASSIST_MSG_INSUFFICIENT_TREES);  lua_setglobal(L, "ASSIST_MSG_INSUFFICIENT_TREES");
  lua_pushinteger(L, ASSIST_MSG_BUILDTANK);          lua_setglobal(L, "ASSIST_MSG_BUILDTANK");
  lua_pushinteger(L, ASSIST_MSG_PILL_NO_REPAIR);     lua_setglobal(L, "ASSIST_MSG_PILL_NO_REPAIR");
  lua_pushinteger(L, ASSIST_MSG_NO_PILLS);           lua_setglobal(L, "ASSIST_MSG_NO_PILLS");
  lua_pushinteger(L, ASSIST_MSG_INSUFFICIENT_MINES);  lua_setglobal(L, "ASSIST_MSG_INSUFFICIENT_MINES");
  lua_pushinteger(L, ASSIST_MSG_PILL_ON_MINE);       lua_setglobal(L, "ASSIST_MSG_PILL_ON_MINE");
  lua_pushinteger(L, ASSIST_MSG_TANK_SUNK);          lua_setglobal(L, "ASSIST_MSG_TANK_SUNK");

  /* Sound effect constants */
  lua_pushinteger(L, shootSelf);          lua_setglobal(L, "SND_SHOOT_SELF");
  lua_pushinteger(L, shootNear);          lua_setglobal(L, "SND_SHOOT_NEAR");
  lua_pushinteger(L, shotTreeNear);       lua_setglobal(L, "SND_SHOT_TREE_NEAR");
  lua_pushinteger(L, shotTreeFar);        lua_setglobal(L, "SND_SHOT_TREE_FAR");
  lua_pushinteger(L, shotBuildingNear);   lua_setglobal(L, "SND_SHOT_BUILDING_NEAR");
  lua_pushinteger(L, shotBuildingFar);    lua_setglobal(L, "SND_SHOT_BUILDING_FAR");
  lua_pushinteger(L, hitTankNear);        lua_setglobal(L, "SND_HIT_TANK_NEAR");
  lua_pushinteger(L, hitTankFar);         lua_setglobal(L, "SND_HIT_TANK_FAR");
  lua_pushinteger(L, hitTankSelf);        lua_setglobal(L, "SND_HIT_TANK_SELF");
  lua_pushinteger(L, bubbles);            lua_setglobal(L, "SND_BUBBLES");
  lua_pushinteger(L, tankSinkNear);       lua_setglobal(L, "SND_TANK_SINK_NEAR");
  lua_pushinteger(L, tankSinkFar);        lua_setglobal(L, "SND_TANK_SINK_FAR");
  lua_pushinteger(L, bigExplosionNear);   lua_setglobal(L, "SND_BIG_EXPLOSION_NEAR");
  lua_pushinteger(L, bigExplosionFar);    lua_setglobal(L, "SND_BIG_EXPLOSION_FAR");
  lua_pushinteger(L, farmingTreeNear);    lua_setglobal(L, "SND_FARMING_TREE_NEAR");
  lua_pushinteger(L, farmingTreeFar);     lua_setglobal(L, "SND_FARMING_TREE_FAR");
  lua_pushinteger(L, manBuildingNear);    lua_setglobal(L, "SND_MAN_BUILDING_NEAR");
  lua_pushinteger(L, manBuildingFar);     lua_setglobal(L, "SND_MAN_BUILDING_FAR");
  lua_pushinteger(L, manDyingNear);       lua_setglobal(L, "SND_MAN_DYING_NEAR");
  lua_pushinteger(L, manDyingFar);        lua_setglobal(L, "SND_MAN_DYING_FAR");
  lua_pushinteger(L, manLayingMineNear);  lua_setglobal(L, "SND_MAN_LAYING_MINE_NEAR");
  lua_pushinteger(L, mineExplosionNear);  lua_setglobal(L, "SND_MINE_EXPLOSION_NEAR");
  lua_pushinteger(L, mineExplosionFar);   lua_setglobal(L, "SND_MINE_EXPLOSION_FAR");
  lua_pushinteger(L, shootFar);           lua_setglobal(L, "SND_SHOOT_FAR");

  /* High-resolution timer for profiling */
  lua_pushcfunction(L, l_clock_us);       lua_setglobal(L, "clock_us");

  /* bt_yield() — calls the host event-pump callback if set.
   * BrainTest registers SDL_PumpEvents so Brain.open() stays responsive.
   * Non-BrainTest hosts leave the callback NULL; bt_yield() is a no-op. */
  lua_pushcfunction(L, l_bt_yield);       lua_setglobal(L, "bt_yield");
}

void brainCoreRegisterGetTerrain(lua_State *L, const BYTE **worldPtr) {
  lua_pushlightuserdata(L, (void *)worldPtr);
  lua_pushcclosure(L, l_get_terrain_upvalue, 1);
  lua_setglobal(L, "get_terrain");
  /* Also stash the worldPtr-pointer in the Lua registry under a known
   * key so other C modules (e.g. brains/<bot>/c / *) can read raw terrain
   * without going through the get_terrain Lua closure. */
  lua_pushlightuserdata(L, (void *)worldPtr);
  lua_setfield(L, LUA_REGISTRYINDEX, "winbolo_world_ptr_ptr");
}

const BYTE **brainCoreGetWorldPtrPtr(lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, "winbolo_world_ptr_ptr");
  const BYTE **p = (const BYTE **)lua_touserdata(L, -1);
  lua_pop(L, 1);
  return p;
}

/* ------------------------------------------------------------------ */
/* Counting allocator wrapper (per-think allocation profiling)         */
/* ------------------------------------------------------------------ */

/* Context threaded as the ud of the wrapping lua_Alloc. Delegates every
 * request to the original allocator and tallies fresh allocations and
 * growths so a benchmark can read per-think heap churn. */
typedef struct BrcAllocCounter {
  lua_Alloc   origAlloc;   /* allocator this wrapper delegates to */
  void       *origUd;      /* ud the original allocator expects */
  lua_Integer allocN;      /* fresh + grow events since last reset */
  lua_Integer allocBytes;  /* net new bytes since last reset */
} BrcAllocCounter;

/* lua_Alloc wrapper: delegate faithfully, then count. Only successful
 * fresh allocations and growths are tallied; shrinks and frees are not.
 * In Lua 5.4 osize carries the object type tag (not a byte size) when
 * ptr is NULL, so a fresh alloc counts the whole nsize. */
static void *brc_counting_alloc(void *ud, void *ptr, size_t osize,
                                size_t nsize) {
  BrcAllocCounter *ctx = (BrcAllocCounter *)ud;
  void *res = ctx->origAlloc(ctx->origUd, ptr, osize, nsize);
  if (nsize == 0) {
    return res; /* free — not counted */
  }
  if (res != NULL) {
    if (ptr == NULL) {
      ctx->allocN++;
      ctx->allocBytes += (lua_Integer)nsize;
    } else if (nsize > osize) {
      ctx->allocN++;
      ctx->allocBytes += (lua_Integer)(nsize - osize);
    }
    /* shrink (nsize <= osize): counted as neither */
  }
  return res;
}

/* brain_alloc_stats() Lua global — returns (allocN, allocBytes) tallied
 * since the last reset. The counter context rides as a light-userdata
 * upvalue so this closure needs no globals. */
static int brc_alloc_stats(lua_State *L) {
  BrcAllocCounter *ctx =
      (BrcAllocCounter *)lua_touserdata(L, lua_upvalueindex(1));
  lua_pushinteger(L, ctx->allocN);
  lua_pushinteger(L, ctx->allocBytes);
  return 2;
}

void brainCoreInstallAllocCounter(lua_State *L) {
  BrcAllocCounter *ctx = (BrcAllocCounter *)malloc(sizeof(BrcAllocCounter));
  if (ctx == NULL) return;
  ctx->origAlloc = lua_getallocf(L, &ctx->origUd);
  ctx->allocN = 0;
  ctx->allocBytes = 0;
  lua_setallocf(L, brc_counting_alloc, ctx);
  lua_pushlightuserdata(L, ctx);
  lua_pushcclosure(L, brc_alloc_stats, 1);
  lua_setglobal(L, "brain_alloc_stats");
}

void brainCoreUninstallAllocCounter(lua_State *L) {
  void *ud = NULL;
  lua_Alloc cur = lua_getallocf(L, &ud);
  if (cur != brc_counting_alloc) return; /* wrapper not installed */
  BrcAllocCounter *ctx = (BrcAllocCounter *)ud;
  lua_setallocf(L, ctx->origAlloc, ctx->origUd);
  free(ctx);
}

/* ------------------------------------------------------------------ */
/* BrainInfo marshaling                                                */
/* ------------------------------------------------------------------ */

/* Per-state scratch pool for brainCorePushInfo.
 *
 * brainCorePushInfo runs once per think for every brain, and the info tree it
 * marshals is entirely transient — nothing survives to the next push. Building
 * the whole tree fresh each call (outer table, player_names, one table per
 * visible object, per-message and per-event tables with a nested data array
 * each, gameinfo, base, singular message) turned every push into a wave of
 * short-lived tables charged to that brain's Lua GC.
 *
 * Instead each lua_State keeps one scratch table T in its registry, keyed by
 * the address of the file-static below. The registry is per-State and each
 * brain has its own State, so T is naturally per-brain. T holds:
 *   T.info    — the pooled info table left on the stack for the caller. Its
 *               array/table fields (player_names, gameinfo, objects, messages,
 *               events, and each event's nested data array) are themselves
 *               persistent and reused.
 *   T.base    — pooled table for info.base (a nil-or-table field). Anchored in
 *               T so it survives while info.base is toggled to nil.
 *   T.message — pooled table for info.message (singular; nil-or-table),
 *               likewise anchored in T.
 * The first call per State builds everything; later calls fetch T and overwrite
 * in place — lua_setfield on an existing key allocates nothing, so the only
 * per-call allocations are viewdata and any genuinely new (non-interned)
 * strings.
 *
 * Contract preserved: this function still leaves exactly one value on the stack
 * — the info table — with the same field names, types and nil semantics a
 * freshly built tree produced, so callers are unchanged.
 *
 * Hazard: because the tables are overwritten in place on the next push, a brain
 * must not retain a reference to an info sub-table (info.objects, info.messages,
 * info.events, info.base, info.message, info.player_names, info.gameinfo, ...)
 * across thinks and expect it to keep that think's snapshot — the next push
 * mutates it. Reading fields and copying scalars/strings out remains safe.
 */
static const char brc_info_pool_key;

void brainCorePushInfo(lua_State *L, const BrainInfo *info) {
  int i;
  int t_idx;

  /* Fetch (or lazily build) this State's scratch pool, leaving T on top. */
  lua_rawgetp(L, LUA_REGISTRYINDEX, (void *)&brc_info_pool_key);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);                                  /* T */
    lua_newtable(L);                                  /* T.info */
    lua_newtable(L); lua_setfield(L, -2, "player_names");
    lua_newtable(L); lua_setfield(L, -2, "gameinfo");
    lua_newtable(L); lua_setfield(L, -2, "objects");
    lua_newtable(L); lua_setfield(L, -2, "messages");
    lua_newtable(L); lua_setfield(L, -2, "events");
    lua_setfield(L, -2, "info");                      /* T.info = info */
    lua_newtable(L); lua_setfield(L, -2, "base");     /* T.base */
    lua_newtable(L); lua_setfield(L, -2, "message");  /* T.message */
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, (void *)&brc_info_pool_key);
  }
  t_idx = lua_gettop(L);                 /* T */
  lua_getfield(L, t_idx, "info");        /* info — kept on the stack top below */

  /* Version */
  lua_pushinteger(L, info->BoloVersion);    lua_setfield(L, -2, "bolo_version");
  lua_pushinteger(L, info->InfoVersion);    lua_setfield(L, -2, "info_version");

  /* Player roster */
  lua_pushinteger(L, info->player_number);  lua_setfield(L, -2, "player_number");
  lua_pushinteger(L, info->num_players);    lua_setfield(L, -2, "num_players");
  lua_pushinteger(L, info->max_players);    lua_setfield(L, -2, "max_players");
  lua_pushinteger(L, info->max_pillboxes);  lua_setfield(L, -2, "max_pillboxes");
  lua_pushinteger(L, info->max_refbases);   lua_setfield(L, -2, "max_refbases");

  /* Player names — persistent array; overwrite 1..max_players and nil any tail
   * left by a previous, larger roster (max_players is stable in practice). */
  {
    int old_n, count = 0;
    lua_getfield(L, -1, "player_names");
    old_n = (int)lua_rawlen(L, -1);
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
      count = info->max_players;
    }
    for (i = count; i < old_n; i++) {
      lua_pushnil(L);
      lua_rawseti(L, -2, i + 1);
    }
    lua_pop(L, 1);
  }

  /* Alliance bitmask */
  lua_pushinteger(L, info->allies ? *(info->allies) : 0);
  lua_setfield(L, -2, "allies");

  /* Per-slot PLAYER_FLAG_BOT bitmap (bit N = slot N is a brain).
   * Brains intersect with `allies` (and invert) to address allied
   * humans only — see the human-goal-change broadcast in
   * GoalHunter. */
  lua_pushinteger(L, info->player_bots ? *(info->player_bots) : 0);
  lua_setfield(L, -2, "player_bots");

  /* Tank state */
  lua_pushinteger(L, info->tankx);          lua_setfield(L, -2, "tankx");
  lua_pushinteger(L, info->tanky);          lua_setfield(L, -2, "tanky");
  lua_pushinteger(L, info->direction);      lua_setfield(L, -2, "direction");
  lua_pushnumber(L,  info->tank_angle);     lua_setfield(L, -2, "tank_angle");
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

  /* Nearest friendly base, or nil — toggle the pooled T.base in/out of info. */
  if (info->base != NULL) {
    lua_getfield(L, t_idx, "base");
    lua_pushinteger(L, info->base->x >> 8); lua_setfield(L, -2, "x");
    lua_pushinteger(L, info->base->y >> 8); lua_setfield(L, -2, "y");
    lua_pushinteger(L, info->base->idnum);  lua_setfield(L, -2, "idnum");
    lua_pushinteger(L, info->base_shells);  lua_setfield(L, -2, "shells");
    lua_pushinteger(L, info->base_mines);   lua_setfield(L, -2, "mines");
    lua_pushinteger(L, info->base_armour);  lua_setfield(L, -2, "armour");
    lua_setfield(L, -2, "base");            /* info.base = T.base */
  } else {
    lua_pushnil(L);
    lua_setfield(L, -2, "base");
  }

  /* Builder man (LGM) state */
  lua_pushinteger(L, info->man_status);     lua_setfield(L, -2, "man_status");
  lua_pushinteger(L, info->man_direction);  lua_setfield(L, -2, "man_direction");
  lua_pushinteger(L, info->man_x);          lua_setfield(L, -2, "man_x");
  lua_pushinteger(L, info->man_y);          lua_setfield(L, -2, "man_y");
  lua_pushinteger(L, info->manobstructed);  lua_setfield(L, -2, "man_obstructed");

  /* View rectangle */
  lua_pushinteger(L, info->view_top);       lua_setfield(L, -2, "view_top");
  lua_pushinteger(L, info->view_left);      lua_setfield(L, -2, "view_left");
  lua_pushinteger(L, info->view_height);    lua_setfield(L, -2, "view_height");
  lua_pushinteger(L, info->view_width);     lua_setfield(L, -2, "view_width");

  /* pillview */
  lua_pushinteger(L, info->pillview ? *(info->pillview) : 0x8000);
  lua_setfield(L, -2, "pillview");

  /* viewdata — a fresh per-tick string (accepted allocation) */
  if (info->viewdata != NULL) {
    lua_pushlstring(L, (const char *)info->viewdata,
                    (size_t)(info->view_width * info->view_height));
  } else {
    lua_pushstring(L, "");
  }
  lua_setfield(L, -2, "viewdata");

  /* Visible objects — persistent array of per-object tables. Overwrite each
   * element's fields in place, creating an element table only when the array
   * grows; nil any slots beyond this tick's count. */
  {
    int old_n, n = info->num_objects;
    lua_getfield(L, -1, "objects");
    old_n = (int)lua_rawlen(L, -1);
    for (i = 0; i < n; i++) {
      lua_rawgeti(L, -1, i + 1);
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_rawseti(L, -3, i + 1);
      }
      lua_pushinteger(L, info->objects[i].object);    lua_setfield(L, -2, "type");
      lua_pushinteger(L, info->objects[i].x);         lua_setfield(L, -2, "x");
      lua_pushinteger(L, info->objects[i].y);         lua_setfield(L, -2, "y");
      lua_pushinteger(L, info->objects[i].idnum);     lua_setfield(L, -2, "idnum");
      lua_pushinteger(L, info->objects[i].direction); lua_setfield(L, -2, "direction");
      lua_pushinteger(L, info->objects[i].info);      lua_setfield(L, -2, "info");
      lua_pushinteger(L, info->objects[i].speed);     lua_setfield(L, -2, "speed");
      lua_pop(L, 1);
    }
    for (i = n; i < old_n; i++) {
      lua_pushnil(L);
      lua_rawseti(L, -2, i + 1);
    }
    lua_pop(L, 1);
  }

  /* Received messages — full per-tick inbox, a persistent array of
   * {sender=N, receivers=N, text="..."} tables (same overwrite/nil-tail scheme
   * as objects). info.message (singular) below is a separate nil-or-table field
   * for legacy brains not yet ported to iterate info.messages. */
  {
    char msgBuf[256];
    int old_n, n = (int)info->num_messages;
    lua_getfield(L, -1, "messages");
    old_n = (int)lua_rawlen(L, -1);
    for (i = 0; i < n; i++) {
      const MessageInfo *m = &info->messages[i];
      lua_rawgeti(L, -1, i + 1);
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_rawseti(L, -3, i + 1);
      }
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
      lua_pop(L, 1);
    }
    for (i = n; i < old_n; i++) {
      lua_pushnil(L);
      lua_rawseti(L, -2, i + 1);
    }
    lua_pop(L, 1);
  }
  /* info.message (singular) — toggle the separate pooled T.message in/out,
   * matching the freshly built tree (never aliased to messages[1]). */
  if (info->message != NULL) {
    char msgBuf[256];
    lua_getfield(L, t_idx, "message");
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
    lua_setfield(L, -2, "message");         /* info.message = T.message */
  } else {
    lua_pushnil(L);
    lua_setfield(L, -2, "message");
  }

  /* Game info — persistent table, overwrite its fields. */
  {
    lua_getfield(L, -1, "gameinfo");
    lua_pushstring(L, (const char *)info->gameinfo.mapname.c);
    lua_setfield(L, -2, "mapname");
    lua_pushinteger(L, info->gameinfo.gametype);          lua_setfield(L, -2, "gametype");
    lua_pushinteger(L, info->gameinfo.hidden_mines);      lua_setfield(L, -2, "hidden_mines");
    lua_pushboolean(L, info->gameinfo.allow_AI);          lua_setfield(L, -2, "allow_ai");
    lua_pushboolean(L, info->gameinfo.assist_AI);         lua_setfield(L, -2, "assist_ai");
    lua_pushinteger(L, info->gameinfo.start_delay);       lua_setfield(L, -2, "start_delay");
    lua_pushinteger(L, info->gameinfo.time_limit);        lua_setfield(L, -2, "time_limit");
    lua_pop(L, 1);
  }

  /* Current key state */
  lua_pushinteger(L, info->holdkeys ? *(info->holdkeys) : 0);
  lua_setfield(L, -2, "holdkeys");
  lua_pushinteger(L, info->tapkeys ? *(info->tapkeys) : 0);
  lua_setfield(L, -2, "tapkeys");

  /* Server tick and assistant message */
  lua_pushinteger(L, info->server_tick);
  lua_setfield(L, -2, "server_tick");
  lua_pushinteger(L, info->assistant_msg);
  lua_setfield(L, -2, "assistant_msg");
  /* Dead-tick hook: brain runs while the tank is dead so it can reset its own
   * state for a clean respawn; it should early-return without acting. */
  lua_pushboolean(L, info->dead);
  lua_setfield(L, -2, "dead");

  /* Game events — persistent array; each element holds a persistent nested
   * data array. Overwrite type and data[1..dlen] in place, nil each data tail
   * (dlen varies by event type) and the events tail. */
  {
    int old_n, n = info->num_events;
    lua_getfield(L, -1, "events");
    old_n = (int)lua_rawlen(L, -1);
    for (i = 0; i < n; i++) {
      int dlen, old_d, j;
      lua_rawgeti(L, -1, i + 1);
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);                    /* event table */
        lua_newtable(L);                    /* its data array */
        lua_setfield(L, -2, "data");
        lua_pushvalue(L, -1);
        lua_rawseti(L, -3, i + 1);
      }
      lua_pushinteger(L, info->events[i].type);
      lua_setfield(L, -2, "type");
      lua_getfield(L, -1, "data");
      dlen  = gameEventDataSize(info->events[i].type);
      old_d = (int)lua_rawlen(L, -1);
      for (j = 0; j < dlen; j++) {
        lua_pushinteger(L, info->events[i].data[j]);
        lua_rawseti(L, -2, j + 1);
      }
      for (j = dlen; j < old_d; j++) {
        lua_pushnil(L);
        lua_rawseti(L, -2, j + 1);
      }
      lua_pop(L, 1);                         /* drop data */
      lua_pop(L, 1);                         /* drop event table */
    }
    for (i = n; i < old_n; i++) {
      lua_pushnil(L);
      lua_rawseti(L, -2, i + 1);
    }
    lua_pop(L, 1);
  }

  /* Drop the scratch pool T, leaving exactly the info table on the stack top. */
  lua_remove(L, t_idx);
}

/* ------------------------------------------------------------------ */
/* Output extraction                                                   */
/* ------------------------------------------------------------------ */

void brainCoreExtractOutput(lua_State *L, BrainInfo *info) {
  if (!lua_istable(L, -1)) {
    return;
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

  /* build */
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

  /* sendmessage */
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

/* ------------------------------------------------------------------ */
/* Brain crash logging                                                 */
/* ------------------------------------------------------------------ */
/* Lua error in brain.think() causes the producer in bot_manager.c to
 * remove the bot from the game (j->needRemove). Without crash logging
 * the disappearance is silent. These helpers:
 *   1. Install a debug.traceback message handler so pcall returns the
 *      full Lua stack.
 *   2. Write a unique file per crash — never overwritten — with a
 *      searchable banner "===== BRAIN CRASH =====" and [BRAIN_CRASH]
 *      tagged lines. Filename embeds UTC timestamp, PID, lua_State
 *      pointer, and (if readable) state.bot_index.
 *   3. Also tag stderr lines with [BRAIN_CRASH] so debugger consoles
 *      surface the failure without grepping log files.
 * No CLI switch — always on.
 */
static int brc_traceback_msgh(lua_State *L) {
  const char *msg = lua_tostring(L, 1);
  if (msg == NULL) {
    if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING) {
      return 1;
    }
    msg = lua_pushfstring(L, "(error object is a %s value)",
                           luaL_typename(L, 1));
  }
  luaL_traceback(L, L, msg, 1);
  return 1;
}

/* Best-effort read of state.bot_index (Lua-side identifier matching
 * print2_botN.log / playerN.jsonl). Returns -1 if unavailable. Never
 * raises. */
static int brc_read_bot_index(lua_State *L) {
  int top = lua_gettop(L);
  int idx = -1;
  lua_getglobal(L, "state");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "bot_index");
    if (lua_isnumber(L, -1)) {
      idx = (int)lua_tointeger(L, -1);
    }
  }
  lua_settop(L, top);
  return idx;
}

/* Best-effort read of an integer field from the global "state" table.
 * Returns -1 if missing / not a number. */
static int brc_read_state_int(lua_State *L, const char *field) {
  int top = lua_gettop(L);
  int v = -1;
  lua_getglobal(L, "state");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, field);
    if (lua_isnumber(L, -1)) v = (int)lua_tointeger(L, -1);
  }
  lua_settop(L, top);
  return v;
}

/* Best-effort copy of _G.DEBUG_SESSION_DIR into out (NUL-terminated).
 * Returns true if set & non-empty. */
static bool brc_read_session_dir(lua_State *L, char *out, size_t outsz) {
  int top = lua_gettop(L);
  bool ok = false;
  out[0] = '\0';
  lua_getglobal(L, "DEBUG_SESSION_DIR");
  if (lua_isstring(L, -1)) {
    const char *s = lua_tostring(L, -1);
    if (s && s[0]) {
      strncpy(out, s, outsz - 1);
      out[outsz - 1] = '\0';
      ok = true;
    }
  }
  lua_settop(L, top);
  return ok;
}

/* Best-effort read of _G.BRAIN_DEBUG_MODE. Returns true only when the
 * global is explicitly truthy. Production hosts (WinBolo / WinBoloDS)
 * set it false at brain creation, so brains there write NO crash/error
 * files to disk — stderr surfacing still fires regardless. */
static bool brc_debug_mode(lua_State *L) {
  int top = lua_gettop(L);
  lua_getglobal(L, "BRAIN_DEBUG_MODE");
  bool on = lua_toboolean(L, -1) != 0;
  lua_settop(L, top);
  return on;
}

/* Rate-limit key in the Lua registry: stores the last UNIX timestamp
 * a crash file was written for this brain. Per-brain (each brain has
 * its own lua_State + registry) so two brains crashing at the same
 * time both get logged; only repeats from the SAME brain are throttled.
 * Address-of a static is the standard light-userdata key idiom. */
static const char kBrcCrashRateLimitKey;

/* Crashes within this many seconds of a previous crash on the same
 * brain get suppressed (stderr still fires, file is skipped). 30s
 * is short enough that a transient crash from one tick re-arms quickly
 * but long enough that a chronic per-tick crash can't fill the disk
 * (50 ticks/sec × 30s = at most 1 file per 1500 crashes). */
#define BRC_CRASH_RATE_LIMIT_SECS 30

/* Returns true if this crash should be suppressed (rate-limited).
 * Updates the registry timestamp on a non-suppressed call so the next
 * caller can see we just logged. */
static bool brc_should_suppress_crash_file(lua_State *L, time_t now) {
  lua_pushlightuserdata(L, (void *)&kBrcCrashRateLimitKey);
  lua_rawget(L, LUA_REGISTRYINDEX);
  time_t last = (time_t)lua_tointeger(L, -1);
  lua_pop(L, 1);

  if (last != 0 && (now - last) < BRC_CRASH_RATE_LIMIT_SECS) {
    return true;
  }
  lua_pushlightuserdata(L, (void *)&kBrcCrashRateLimitKey);
  lua_pushinteger(L, (lua_Integer)now);
  lua_rawset(L, LUA_REGISTRYINDEX);
  return false;
}

/* Write a brain crash report. err_or_traceback is the pcall payload
 * (error string + "\nstack traceback:\n..." from the msgh). method is
 * "think" / "init" / whatever — used in the banner and filename. Not
 * static so the unit test in tests/unit/test_brain_crash_log.c can
 * invoke it directly without standing up a full BrainInfo. */
void brc_write_crash_log(lua_State *L,
                         const char *method,
                         const char *err_or_traceback) {
  if (err_or_traceback == NULL) err_or_traceback = "(no error message)";
  if (method == NULL) method = "?";

  /* Timestamps: UTC (for filenames + cross-host comparison) and local
   * (for at-a-glance reading next to other session logs). */
  time_t now = time(NULL);
  struct tm tm_utc, tm_local;
#ifdef _WIN32
  gmtime_s(&tm_utc, &now);
  localtime_s(&tm_local, &now);
#else
  gmtime_r(&now, &tm_utc);
  localtime_r(&now, &tm_local);
#endif
  char ts_utc[32], ts_local[32];
  strftime(ts_utc,   sizeof(ts_utc),   "%Y%m%d_%H%M%SZ",     &tm_utc);
  strftime(ts_local, sizeof(ts_local), "%Y-%m-%d %H:%M:%S",  &tm_local);

  int bot_idx = brc_read_bot_index(L);
  int tick    = brc_read_state_int(L, "tick");
  int pid     = brc_getpid();
  uintptr_t lptr = (uintptr_t)L;

  char session_dir[512];
  bool has_session = brc_read_session_dir(L, session_dir, sizeof(session_dir));

  /* File writes are gated to debug hosts (BRAIN_DEBUG_MODE, set by -braindebug);
   * production WinBolo / WinBoloDS write NO crash files to disk. The stderr
   * surface below still fires so a production crash is never silent. */
  bool debug_mode = brc_debug_mode(L);

  /* Rate-limit: if this brain crashed within the last
   * BRC_CRASH_RATE_LIMIT_SECS, skip the file write so a perpetually-
   * crashing brain (we no longer remove the bot on Lua error — see
   * bot_manager.c runBotThinkJobImpl) can't fill the disk. Stderr
   * still fires below so the crash is never invisible. */
  bool suppress_file = brc_should_suppress_crash_file(L, now);

  /* Filename: bot index if known, else lua_State pointer. Goes inside
   * DEBUG_SESSION_DIR if BrainTest set one (so each session's crashes
   * are colocated with its other logs); else CWD. Unique per crash. */
  char path[1024];
  const char *prefix = has_session ? session_dir : ".";
  if (bot_idx >= 0) {
    SDL_snprintf(path, sizeof(path),
                 "%s/brain_crash_%s_pid%d_bot%d.log",
                 prefix, ts_utc, pid, bot_idx);
  } else {
    SDL_snprintf(path, sizeof(path),
                 "%s/brain_crash_%s_pid%d_L%p.log",
                 prefix, ts_utc, pid, (void *)lptr);
  }

  /* Per-crash file: debug hosts only, and rate-limited so a chronically-
   * crashing brain can't fill the disk. Production (debug off) writes nothing;
   * the stderr surface below still fires. */
  FILE *f = (suppress_file || !debug_mode) ? NULL : fopen(path, "wb");
  if (f) {
    fprintf(f, "===== BRAIN CRASH =====\n");
    fprintf(f, "[BRAIN_CRASH] method=brain.%s\n", method);
    fprintf(f, "[BRAIN_CRASH] timestamp_utc=%s\n", ts_utc);
    fprintf(f, "[BRAIN_CRASH] timestamp_local=%s\n", ts_local);
    fprintf(f, "[BRAIN_CRASH] debug_session_dir=%s\n",
            has_session ? session_dir : "(none)");
    fprintf(f, "[BRAIN_CRASH] pid=%d\n", pid);
    fprintf(f, "[BRAIN_CRASH] lua_state=%p\n", (void *)lptr);
    fprintf(f, "[BRAIN_CRASH] bot_index=%d\n", bot_idx);
    fprintf(f, "[BRAIN_CRASH] state.tick=%d\n", tick);
    fprintf(f, "[BRAIN_CRASH] ----- error + traceback below -----\n");
    fprintf(f, "%s\n", err_or_traceback);
    fprintf(f, "===== END BRAIN CRASH =====\n");
    fflush(f);
    fclose(f);
  }

  /* Combined append-only crash log: EVERY Lua crash's full traceback appended
   * here (ignoring the per-crash-file rate-limit) so one file is the complete
   * chronological record of a run. Debug hosts only (-braindebug); one growing
   * file rather than 1500 separate ones, bounded in practice by bot_manager
   * kicking a brain that crashes every tick. Same prefix as the per-crash files
   * (session dir if set, else CWD). */
  if (debug_mode) {
    char all_path[1024];
    SDL_snprintf(all_path, sizeof(all_path), "%s/brain_crashes.log", prefix);
    FILE *af = fopen(all_path, "ab");
    if (af) {
      fprintf(af, "===== BRAIN CRASH =====\n");
      fprintf(af, "[BRAIN_CRASH] method=brain.%s\n", method);
      fprintf(af, "[BRAIN_CRASH] timestamp_utc=%s\n", ts_utc);
      fprintf(af, "[BRAIN_CRASH] timestamp_local=%s\n", ts_local);
      fprintf(af, "[BRAIN_CRASH] pid=%d bot_index=%d state.tick=%d lua_state=%p\n",
              pid, bot_idx, tick, (void *)lptr);
      fprintf(af, "[BRAIN_CRASH] ----- error + traceback below -----\n");
      fprintf(af, "%s\n", err_or_traceback);
      fprintf(af, "===== END BRAIN CRASH =====\n\n");
      fflush(af);
      fclose(af);
    }
  }

  /* Stderr surface so the failure is visible without grepping. Always
   * fires (even when the file was suppressed) so a chronically-crashing
   * brain stays loud in the console — just with a clear note that the
   * file isn't being re-emitted. */
  if (suppress_file) {
    fprintf(stderr,
            "[BRAIN_CRASH] brain.%s() crashed at %s (bot_index=%d tick=%d L=%p) "
            "— file SUPPRESSED (rate-limit: same brain crashed within %ds)\n",
            method, ts_local, bot_idx, tick, (void *)lptr,
            BRC_CRASH_RATE_LIMIT_SECS);
  } else {
    fprintf(stderr,
            "[BRAIN_CRASH] brain.%s() crashed at %s (bot_index=%d tick=%d L=%p) — see %s\n",
            method, ts_local, bot_idx, tick, (void *)lptr, path);
  }
  fprintf(stderr, "[BRAIN_CRASH] %s\n", err_or_traceback);
  fflush(stderr);

  /* One-line index entry in brain_error.log (inside the session dir if
   * applicable) for tail-watchers; back-points at the full crash file.
   * Skipped on rate-limit so the index doesn't grow without bound, and
   * skipped entirely in production (BRAIN_DEBUG_MODE off). */
  if (!suppress_file && debug_mode) {
    char idx_path[1024];
    SDL_snprintf(idx_path, sizeof(idx_path), "%s/brain_error.log", prefix);
    FILE *idx = fopen(idx_path, "a");
    if (idx) {
      fprintf(idx,
              "[BRAIN_CRASH] %s (%s) bot=%d tick=%d pid=%d L=%p method=%s file=%s\n",
              ts_utc, ts_local, bot_idx, tick, pid, (void *)lptr, method, path);
      fclose(idx);
    }
  }
}

/* ------------------------------------------------------------------ */
/* Brain method invocation                                             */
/* ------------------------------------------------------------------ */

bool brainCoreCallThink(lua_State *L, BrainInfo *info, bool *out_killed) {
  int top = lua_gettop(L);

  if (out_killed) *out_killed = false;

  /* Reset the per-think allocation counters, but only on states that
   * actually carry the counting wrapper — identity-checked so states
   * without it (e.g. the lua_strip tool) are untouched. */
  {
    void *allocUd = NULL;
    if (lua_getallocf(L, &allocUd) == brc_counting_alloc && allocUd) {
      BrcAllocCounter *ctx = (BrcAllocCounter *)allocUd;
      ctx->allocN = 0;
      ctx->allocBytes = 0;
    }
  }

  lua_getglobal(L, "brain");
  if (!lua_istable(L, -1)) {
    fprintf(stderr, "brainCore: 'brain' global is not a table\n");
    if (brc_debug_mode(L)) { FILE *ef = fopen("brain_error.log", "a"); if (ef) { fprintf(ef, "brain global is not a table (type=%d)\n", lua_type(L, -1)); fclose(ef); } }
    lua_settop(L, top);
    return false;
  }

  lua_getfield(L, -1, "think");
  if (!lua_isfunction(L, -1)) {
    fprintf(stderr, "brainCore: brain.think is not a function\n");
    if (brc_debug_mode(L)) { FILE *ef = fopen("brain_error.log", "a"); if (ef) { fprintf(ef, "brain.think is not a function (type=%d)\n", lua_type(L, -1)); fclose(ef); } }
    lua_settop(L, top);
    return false;
  }

  /* Push traceback msgh BELOW the function so lua_pcall reports full
   * stack on error. Stack before pcall:
   *   [brain, msgh, brain.think, info_arg]
   * msgh_idx is the absolute index of msgh. */
  lua_pushcfunction(L, brc_traceback_msgh);  /* [brain, think, msgh] */
  lua_insert(L, -2);                          /* [brain, msgh, think] */
  int msgh_idx = lua_gettop(L) - 1;           /* msgh is one below top */

  brainCorePushInfo(L, info);                 /* [brain, msgh, think, info] */

  if (lua_pcall(L, 1, 1, msgh_idx) != LUA_OK) {
    const char *errMsg = lua_tostring(L, -1);
    /* Budget-hook sentinel: Lua prepends "<chunkname>:<line>: " to
     * luaL_error messages, so the suffix is the stable match point.
     * On a kill we report the abort to the producer (returns false
     * with *out_killed = true) but do NOT log to brain_error.log or
     * pipe through Lua print() — the bot is going to keep running,
     * and a chronically slow brain would otherwise flood the log
     * with the same line every tick. The producer's rate-limited
     * overrun warning covers operator visibility. */
    bool killed = (out_killed != NULL && errMsg != NULL &&
                   strstr(errMsg, "tick_budget_exceeded") != NULL);
    if (killed) {
      *out_killed = true;
      lua_settop(L, top);
      return false;
    }
    /* Full crash report: timestamped per-bot file with traceback + a
     * stderr/[BRAIN_CRASH] surface. errMsg here is the value returned
     * by brc_traceback_msgh, i.e. "<err>\nstack traceback:\n...". */
    brc_write_crash_log(L, "think", errMsg);
    /* Route error through Lua print() so BrainTest Print Output captures it.
     * Wrap in pcall so a busted print() can't re-enter this same path. */
    lua_getglobal(L, "print");
    if (lua_isfunction(L, -1)) {
      lua_pushfstring(L, "[BRAIN_CRASH] brain.think() error: %s",
                      errMsg ? errMsg : "(unknown)");
      lua_pcall(L, 1, 0, 0);
    } else {
      lua_pop(L, 1);
    }
    lua_settop(L, top);
    return false;
  }

  brainCoreExtractOutput(L, info);
  lua_settop(L, top);
  return true;
}

bool brainCoreCallMethod(lua_State *L, BrainInfo *info, const char *method) {
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

  /* Install traceback msgh below the function so errors include the
   * full Lua stack. Stack: [brain, msgh, method_fn, info_arg]. */
  lua_pushcfunction(L, brc_traceback_msgh);
  lua_insert(L, -2);
  int msgh_idx = lua_gettop(L) - 1;

  brainCorePushInfo(L, info);

  if (lua_pcall(L, 1, 0, msgh_idx) != LUA_OK) {
    const char *errMsg = lua_tostring(L, -1);
    brc_write_crash_log(L, method, errMsg);
    lua_settop(L, top);
    return false;
  }

  lua_settop(L, top);
  return true;
}

/* ------------------------------------------------------------------ */
/* C Pathfinder Lua wrappers (cpf_* globals)                           */
/* ------------------------------------------------------------------ */

/* Helper: extract BrainPathfinder* from upvalue 1 (pointer-to-pointer) */
#define CPF_GET(L) \
  BrainPathfinder **ppf = (BrainPathfinder **)lua_touserdata(L, lua_upvalueindex(1)); \
  BrainPathfinder *pf = (ppf && *ppf) ? *ppf : NULL; \
  if (!pf) return 0

static int l_cpf_set_terrain_cost(lua_State *L) {
  CPF_GET(L);
  int type = (int)luaL_checkinteger(L, 1);
  float cost = (float)luaL_checknumber(L, 2);
  brainPathfinderSetTerrainCost(pf, type, cost);
  return 0;
}

static int l_cpf_set_boat_cost(lua_State *L) {
  CPF_GET(L);
  int type = (int)luaL_checkinteger(L, 1);
  float cost = (float)luaL_checknumber(L, 2);
  brainPathfinderSetBoatCost(pf, type, cost);
  return 0;
}

static int l_cpf_set_terrain_speed(lua_State *L) {
  CPF_GET(L);
  int type = (int)luaL_checkinteger(L, 1);
  float speed = (float)luaL_checknumber(L, 2);
  brainPathfinderSetTerrainSpeed(pf, type, speed);
  return 0;
}

static int l_cpf_set_config(lua_State *L) {
  CPF_GET(L);
  const char *key = luaL_checkstring(L, 1);
  float value = (float)luaL_checknumber(L, 2);
  brainPathfinderSetConfig(pf, key, value);
  return 0;
}

static int l_cpf_clear_danger(lua_State *L) {
  CPF_GET(L);
  brainPathfinderClearDanger(pf);
  return 0;
}

static int l_cpf_stamp_pill(lua_State *L) {
  CPF_GET(L);
  int cx = (int)luaL_checkinteger(L, 1);
  int cy = (int)luaL_checkinteger(L, 2);
  int radius = (int)luaL_checkinteger(L, 3);
  float base_danger = (float)luaL_checknumber(L, 4);
  float anger = (float)luaL_checknumber(L, 5);
  brainPathfinderStampPill(pf, cx, cy, radius, base_danger, anger);
  return 0;
}

static int l_cpf_set_danger(lua_State *L) {
  CPF_GET(L);
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  float value = (float)luaL_checknumber(L, 3);
  brainPathfinderSetDanger(pf, x, y, value);
  return 0;
}

/* cpf_load_pill_danger_from_threat() — C-to-C danger load.
 * Reads naThreatGetPillGrid() directly (no Lua table iteration).
 * Replaces cpf_load_danger(threat.pill_grid) now that pill_grid lives in C. */
static int l_cpf_load_pill_danger_from_threat(lua_State *L) {
  CPF_GET(L);
  float *pg = naThreatGetPillGrid(L);
  brainPathfinderClearDanger(pf);
  if (!pg) return 0;
  for (int k = 0; k < 65536; k++) {
    if (pg[k] > 0.0f) {
      int x = k & 255;
      int y = (k >> 8) & 255;
      brainPathfinderSetDanger(pf, x, y, pg[k]);
    }
  }
  return 0;
}

/* Batch-load the entire danger grid from a Lua table keyed by mkey
 * (my*256 + mx) -> value. Clears the grid first. One C call replaces
 * the per-entry cpf_set_danger loop driven from Lua. */
static int l_cpf_load_danger(lua_State *L) {
  CPF_GET(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  brainPathfinderClearDanger(pf);
  lua_pushnil(L);
  while (lua_next(L, 1) != 0) {
    lua_Integer k = luaL_checkinteger(L, -2);
    float v = (float)luaL_checknumber(L, -1);
    int x = (int)(k & 255);
    int y = (int)((k >> 8) & 255);
    brainPathfinderSetDanger(pf, x, y, v);
    lua_pop(L, 1);
  }
  return 0;
}

static int l_cpf_set_overlay(lua_State *L) {
  CPF_GET(L);
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  float value = (float)luaL_checknumber(L, 3);
  brainPathfinderSetOverlay(pf, x, y, value);
  return 0;
}

static int l_cpf_clear_overlay(lua_State *L) {
  CPF_GET(L);
  brainPathfinderClearOverlay(pf);
  return 0;
}

static int l_cpf_get_overlay(lua_State *L) {
  CPF_GET(L);
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  lua_pushnumber(L, (double)brainPathfinderGetOverlay(pf, x, y));
  return 1;
}

static int l_cpf_get_danger(lua_State *L) {
  CPF_GET(L);
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  lua_pushnumber(L, (double)brainPathfinderGetDanger(pf, x, y));
  return 1;
}

static int l_cpf_astar_log_enable(lua_State *L) {
  const char *path = luaL_optstring(L, 1, NULL);
  brainPathfinderEnableLogPath(path);
  return 0;
}

static int l_cpf_astar_log_set_tick(lua_State *L) {
  int tick = (int)luaL_checkinteger(L, 1);
  brainPathfinderSetLogTick(tick);
  return 0;
}

/* cpf_set_danger_offset(x, y, value) — set per-tile danger offset (negative = subtract) */
static int l_cpf_set_danger_offset(lua_State *L) {
  CPF_GET(L);
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  int v = (int)luaL_checknumber(L, 3);
  if (v < -32768) v = -32768;
  if (v >  32767) v =  32767;
  brainPathfinderSetDangerOffset(pf, x, y, (int16_t)v);
  return 0;
}

/* cpf_clear_danger_offset() — zero all danger offsets */
static int l_cpf_clear_danger_offset(lua_State *L) {
  CPF_GET(L);
  brainPathfinderClearDangerOffset(pf);
  return 0;
}

/* cpf_load_danger_offset(table [, scale]) — bulk-load offsets from mkey-keyed
 * Lua table. Optional scale multiplier (default 1.0); pass -1 to subtract a
 * pill contrib table. Clears first. */
static int l_cpf_load_danger_offset(lua_State *L) {
  CPF_GET(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  float scale = (float)luaL_optnumber(L, 2, 1.0);
  brainPathfinderClearDangerOffset(pf);
  lua_pushnil(L);
  while (lua_next(L, 1) != 0) {
    lua_Integer k = luaL_checkinteger(L, -2);
    int v = (int)((float)luaL_checknumber(L, -1) * scale);
    int x = (int)(k & 255);
    int y = (int)((k >> 8) & 255);
    if (v < -32768) v = -32768;
    if (v >  32767) v =  32767;
    brainPathfinderSetDangerOffset(pf, x, y, (int16_t)v);
    lua_pop(L, 1);
  }
  return 0;
}

static int l_cpf_clear_influence(lua_State *L) {
  CPF_GET(L);
  brainPathfinderClearInfluence(pf);
  return 0;
}

static int l_cpf_stamp_influence(lua_State *L) {
  CPF_GET(L);
  int cx = (int)luaL_checkinteger(L, 1);
  int cy = (int)luaL_checkinteger(L, 2);
  int radius = (int)luaL_checkinteger(L, 3);
  int strength = (int)luaL_checkinteger(L, 4);
  brainPathfinderStampInfluence(pf, cx, cy, radius, strength);
  return 0;
}

static int l_cpf_influence_at(lua_State *L) {
  CPF_GET(L);
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  int16_t value = brainPathfinderInfluenceAt(pf, x, y);
  lua_pushinteger(L, (int)value);
  return 1;
}

static int l_cpf_path_to(lua_State *L) {
  int next_x = -1, next_y = -1;
  int status;
  CPF_GET(L);
  int sx = (int)luaL_checkinteger(L, 1);
  int sy = (int)luaL_checkinteger(L, 2);
  int dx = (int)luaL_checkinteger(L, 3);
  int dy = (int)luaL_checkinteger(L, 4);
  int in_boat = (int)luaL_checkinteger(L, 5);
  int shells = (int)luaL_checkinteger(L, 6);
  int trees = (int)luaL_checkinteger(L, 7);
  int mines = (int)luaL_optinteger(L, 8, 0);
  int armour = (int)luaL_optinteger(L, 9, 40);
  int budget = (int)luaL_optinteger(L, 10, 1500);

  /* Log Lua stack trace to A* log if enabled */
  if (pf->astarLog) {
    FILE *alog = (FILE *)pf->astarLog;
    luaL_traceback(L, L, NULL, 1);
    fprintf(alog, "CALLER: boat=%d %s\n", in_boat, lua_tostring(L, -1));
    lua_pop(L, 1);
    fflush(alog);
  }

  status = brainPathfinderPathTo(pf, sx, sy, dx, dy,
                                  in_boat, shells, trees,
                                  mines, armour, budget,
                                  &next_x, &next_y);
  lua_pushinteger(L, status);
  lua_pushinteger(L, next_x);
  lua_pushinteger(L, next_y);
  return 3;
}

static int l_cpf_cost_to(lua_State *L) {
  CPF_GET(L);
  int sx = (int)luaL_checkinteger(L, 1);
  int sy = (int)luaL_checkinteger(L, 2);
  int dx = (int)luaL_checkinteger(L, 3);
  int dy = (int)luaL_checkinteger(L, 4);
  int in_boat = (int)luaL_checkinteger(L, 5);
  int shells = (int)luaL_checkinteger(L, 6);
  int trees = (int)luaL_checkinteger(L, 7);
  int mines = (int)luaL_checkinteger(L, 8);
  int armour = (int)luaL_checkinteger(L, 9);
  int budget = (int)luaL_optinteger(L, 10, 4000);
  int allow_boat = (int)luaL_optinteger(L, 11, 1);

  /* Capture Lua traceback so the astar log shows who called this.
   * Only when logging is actually enabled — building a traceback string
   * walks the whole Lua stack and allocates, and cost_to is a hot path
   * (bulk candidate eval), so this must be free when the log is off. */
  if (brainPathfinderIsLogEnabled()) {
    lua_getglobal(L, "debug");
    if (lua_istable(L, -1)) {
      lua_getfield(L, -1, "traceback");
      if (lua_isfunction(L, -1)) {
        lua_pushstring(L, "");
        lua_pushinteger(L, 2);
        if (lua_pcall(L, 2, 1, 0) == LUA_OK && lua_isstring(L, -1)) {
          brainPathfinderSetLogCaller(lua_tostring(L, -1));
        }
        lua_pop(L, 1);  /* traceback result or error */
      } else {
        lua_pop(L, 1);  /* non-function */
      }
    }
    lua_pop(L, 1);  /* debug table */
  }

  float cost = brainPathfinderCostToEx(pf, sx, sy, dx, dy, in_boat,
                                        shells, trees, mines, armour,
                                        budget, allow_boat);
  lua_pushnumber(L, (double)cost);
  return 1;
}

static int l_cpf_cost_to_reset(lua_State *L) {
  CPF_GET(L);
  int sx = (int)luaL_checkinteger(L, 1);
  int sy = (int)luaL_checkinteger(L, 2);
  int in_boat = (int)luaL_checkinteger(L, 3);
  int shells = (int)luaL_checkinteger(L, 4);
  int trees = (int)luaL_checkinteger(L, 5);
  int mines = (int)luaL_checkinteger(L, 6);
  int armour = (int)luaL_checkinteger(L, 7);
  brainPathfinderCostToReset(pf, sx, sy, in_boat, shells, trees, mines, armour);
  return 0;
}

static int l_cpf_cost_to_incremental(lua_State *L) {
  CPF_GET(L);
  int dx = (int)luaL_checkinteger(L, 1);
  int dy = (int)luaL_checkinteger(L, 2);
  int budget = (int)luaL_optinteger(L, 3, 16000);
  float cost = brainPathfinderCostToIncremental(pf, dx, dy, budget);
  lua_pushnumber(L, (double)cost);
  return 1;
}

/* cpf_dijkstra_from(sx, sy, in_boat, shells, trees, mines, armour)
 * Runs full Dijkstra from the source. Returns three values:
 *   us       - wall-clock microseconds the search took
 *   expanded - number of nodes expanded
 *   peak_open - peak heap size during the search
 * After it returns, internal g_cost holds min cost to every reachable tile,
 * but no lookup helper is exposed yet — this is a perf-measurement entry. */
static int l_cpf_dijkstra_from(lua_State *L) {
  CPF_GET(L);
  int sx = (int)luaL_checkinteger(L, 1);
  int sy = (int)luaL_checkinteger(L, 2);
  int in_boat = (int)luaL_checkinteger(L, 3);
  int shells = (int)luaL_checkinteger(L, 4);
  int trees = (int)luaL_checkinteger(L, 5);
  int mines = (int)luaL_checkinteger(L, 6);
  int armour = (int)luaL_checkinteger(L, 7);
  int expanded = 0, peak_open = 0;
  double us = brainPathfinderDijkstraFrom(pf, sx, sy, in_boat,
                                           shells, trees, mines, armour,
                                           &expanded, &peak_open);
  lua_pushnumber(L, us);
  lua_pushinteger(L, expanded);
  lua_pushinteger(L, peak_open);
  return 3;
}

/* cpf_dijkstra_start(slate, tick, sx, sy, in_boat, shells, trees, mines, armour,
 *                    max_cost, exact, danger_scale, kind[, allow_boat])
 * allow_boat defaults to 1 (boat transitions enabled). Pass 0 to restrict
 * the search to land nodes only (no T_BOAT pickup, no water expansion). */
static int l_cpf_dijkstra_start(lua_State *L) {
  CPF_GET(L);
  int slate = (int)luaL_checkinteger(L, 1);
  uint32_t tick = (uint32_t)luaL_checkinteger(L, 2);
  int sx = (int)luaL_checkinteger(L, 3);
  int sy = (int)luaL_checkinteger(L, 4);
  int in_boat = (int)luaL_checkinteger(L, 5);
  int shells = (int)luaL_checkinteger(L, 6);
  int trees = (int)luaL_checkinteger(L, 7);
  int mines = (int)luaL_checkinteger(L, 8);
  int armour = (int)luaL_checkinteger(L, 9);
  float max_cost = (float)luaL_optnumber(L, 10, 0.0);
  int exact;
  if (lua_isnoneornil(L, 11)) {
    exact = 1;
  } else if (lua_isboolean(L, 11)) {
    exact = lua_toboolean(L, 11) ? 1 : 0;
  } else {
    exact = (int)luaL_checkinteger(L, 11) != 0 ? 1 : 0;
  }
  float danger_scale = (float)luaL_optnumber(L, 12, 1.0);
  int kind = (int)luaL_optinteger(L, 13, 0);
  int allow_boat = (int)luaL_optinteger(L, 14, 1);
  brainPathfinderDijkstraStart(pf, slate, tick, sx, sy, in_boat,
                                shells, trees, mines, armour,
                                max_cost, exact, danger_scale, kind, allow_boat);
  return 0;
}

/* cpf_dijkstra_step(slate, tick, budget) */
static int l_cpf_dijkstra_step(lua_State *L) {
  CPF_GET(L);
  int slate = (int)luaL_checkinteger(L, 1);
  uint32_t tick = (uint32_t)luaL_checkinteger(L, 2);
  int budget = (int)luaL_checkinteger(L, 3);
  int done = brainPathfinderDijkstraStep(pf, slate, tick, budget);
  int expanded = 0, peak_open = 0, d = 0;
  brainPathfinderDijkstraStatus(pf, slate, &expanded, &peak_open, &d);
  lua_pushboolean(L, done);
  lua_pushinteger(L, expanded);
  lua_pushinteger(L, peak_open);
  return 3;
}

/* cpf_dijkstra_cost_at(slate, x, y, boat) */
static int l_cpf_dijkstra_cost_at(lua_State *L) {
  CPF_GET(L);
  int slate = (int)luaL_checkinteger(L, 1);
  int x = (int)luaL_checkinteger(L, 2);
  int y = (int)luaL_checkinteger(L, 3);
  int boat = (int)luaL_optinteger(L, 4, 0);
  float cost = brainPathfinderDijkstraCostAt(pf, slate, x, y, boat);
  lua_pushnumber(L, (double)cost);
  return 1;
}

/* cpf_dijkstra_lookup_by_kind(kind, x, y, boat)
 * Searches all slates of given kind in started_tick descending order,
 * returns first finite cost. Newer searches win even if still running. */
static int l_cpf_dijkstra_lookup_by_kind(lua_State *L) {
  CPF_GET(L);
  int kind = (int)luaL_checkinteger(L, 1);
  int x = (int)luaL_checkinteger(L, 2);
  int y = (int)luaL_checkinteger(L, 3);
  int boat = (int)luaL_optinteger(L, 4, 0);
  float cost = brainPathfinderDijkstraLookupByKind(pf, kind, x, y, boat);
  lua_pushnumber(L, (double)cost);
  return 1;
}

/* Pcontrib lookup callback: reads pcontrib[tile_key] from the Lua
 * table at the configured stack index. Numeric value or 0 if absent. */
typedef struct {
  lua_State *L;
  int        table_idx; /* absolute stack index */
} PcontribLuaCtx;

static float pcontrib_lua_lookup(void *user, int tile_key) {
  PcontribLuaCtx *ctx = (PcontribLuaCtx *)user;
  lua_rawgeti(ctx->L, ctx->table_idx, tile_key);
  float v = 0.0f;
  if (lua_isnumber(ctx->L, -1)) v = (float)lua_tonumber(ctx->L, -1);
  lua_pop(ctx->L, 1);
  return v;
}

/* cpf_dijkstra_lookup_subtract_by_kind(kind, x, y, boat, pcontrib_table)
 * Same as lookup_by_kind, but also walks the slate's realized path back
 * to the source and subtracts pcontrib_table[tile_key] * danger_scale *
 * inv_speed[tile] at each non-source tile — the exact "as-if-target-
 * pill-were-dead" cost. pcontrib_table is the per-pill contribution map
 * the brain already produces in M.pill_contrib. Pass nil to skip the
 * subtraction (equivalent to lookup_by_kind). Result is clamped >= 0. */
static int l_cpf_dijkstra_lookup_subtract_by_kind(lua_State *L) {
  CPF_GET(L);
  int kind = (int)luaL_checkinteger(L, 1);
  int x = (int)luaL_checkinteger(L, 2);
  int y = (int)luaL_checkinteger(L, 3);
  int boat = (int)luaL_optinteger(L, 4, 0);

  if (lua_isnoneornil(L, 5)) {
    float c = brainPathfinderDijkstraLookupByKind(pf, kind, x, y, boat);
    lua_pushnumber(L, (double)c);
    return 1;
  }

  luaL_checktype(L, 5, LUA_TTABLE);
  PcontribLuaCtx ctx;
  ctx.L = L;
  ctx.table_idx = 5;
  float cost = brainPathfinderDijkstraLookupSubtractByKind(
      pf, kind, x, y, boat, pcontrib_lua_lookup, &ctx);
  lua_pushnumber(L, (double)cost);
  return 1;
}

/* cpf_dijkstra_next_step(kind, sx, sy, dx, dy [, obstacles, penalty]) → nx, ny or nil
 * obstacles: optional flat array of packed tile keys (y*256+x) to dodge at trace
 * time; penalty: extra cost added to those tiles (default large). */
#define CPF_MAX_OBSTACLES 64
static int l_cpf_dijkstra_next_step(lua_State *L) {
  CPF_GET(L);
  int kind = (int)luaL_checkinteger(L, 1);
  int sx = (int)luaL_checkinteger(L, 2);
  int sy = (int)luaL_checkinteger(L, 3);
  int dx = (int)luaL_checkinteger(L, 4);
  int dy = (int)luaL_checkinteger(L, 5);
  int obstacles[CPF_MAX_OBSTACLES];
  int n_obs = 0;
  float penalty = (float)luaL_optnumber(L, 7, 1.0e6);
  if (lua_istable(L, 6)) {
    int len = (int)lua_rawlen(L, 6);
    if (len > CPF_MAX_OBSTACLES) len = CPF_MAX_OBSTACLES;
    for (int i = 1; i <= len; i++) {
      lua_rawgeti(L, 6, i);
      obstacles[n_obs++] = (int)lua_tointeger(L, -1);
      lua_pop(L, 1);
    }
  }
  int nx = -1, ny = -1;
  if (brainPathfinderDijkstraNextStep(pf, kind, sx, sy, dx, dy,
                                      n_obs > 0 ? obstacles : NULL, n_obs, penalty,
                                      &nx, &ny)) {
    lua_pushinteger(L, nx);
    lua_pushinteger(L, ny);
    return 2;
  }
  lua_pushnil(L);
  return 1;
}

/* cpf_dijkstra_trace_path(kind, dx, dy) → flat array {x1,y1,x2,y2,...} or nil */
static int l_cpf_dijkstra_trace_path(lua_State *L) {
  CPF_GET(L);
  int kind = (int)luaL_checkinteger(L, 1);
  int dx = (int)luaL_checkinteger(L, 2);
  int dy = (int)luaL_checkinteger(L, 3);
  int slate = brainPathfinderDijkstraFindBest(pf, kind);
  if (slate < 0) { lua_pushnil(L); return 1; }
  int path_x[64], path_y[64];
  int n = brainPathfinderDijkstraTracePath(pf, slate, dx, dy, path_x, path_y, 64);
  if (n <= 0) { lua_pushnil(L); return 1; }
  lua_createtable(L, n * 2, 0);
  for (int i = 0; i < n; i++) {
    lua_pushinteger(L, path_x[i]);
    lua_rawseti(L, -2, 2 * i + 1);
    lua_pushinteger(L, path_y[i]);
    lua_rawseti(L, -2, 2 * i + 2);
  }
  return 1;
}

/* cpf_dijkstra_trace_path_by_kind(kind, dx, dy) → flat array {x1,y1,...} or nil.
 * Multi-slate: walks slates of given kind in recency order, picks the
 * first where (dx,dy) is reachable, traces from THAT slate. Use when
 * the cost was found via lookup_by_kind's older-slate fallback — the
 * single-slate trace_path picks "best" which may be a newer slate that
 * hasn't reached (dx,dy) yet, returning nil. */
static int l_cpf_dijkstra_trace_path_by_kind(lua_State *L) {
  CPF_GET(L);
  int kind = (int)luaL_checkinteger(L, 1);
  int dx   = (int)luaL_checkinteger(L, 2);
  int dy   = (int)luaL_checkinteger(L, 3);
  int path_x[64], path_y[64];
  int n = brainPathfinderDijkstraTracePathByKind(pf, kind, dx, dy,
                                                  path_x, path_y, 64);
  if (n <= 0) { lua_pushnil(L); return 1; }
  lua_createtable(L, n * 2, 0);
  for (int i = 0; i < n; i++) {
    lua_pushinteger(L, path_x[i]);
    lua_rawseti(L, -2, 2 * i + 1);
    lua_pushinteger(L, path_y[i]);
    lua_rawseti(L, -2, 2 * i + 2);
  }
  return 1;
}

/* cpf_dijkstra_pick_reuse_slate(kind)
 * Returns the slate index the brain should reuse next when starting a
 * search of the given kind. Picks an unused slate first, then the
 * oldest slate of matching kind, then the oldest slate overall. */
static int l_cpf_dijkstra_pick_reuse_slate(lua_State *L) {
  CPF_GET(L);
  int kind = (int)luaL_checkinteger(L, 1);
  lua_pushinteger(L, brainPathfinderDijkstraPickReuseSlate(pf, kind));
  return 1;
}

/* cpf_dijkstra_find_best(kind) → slate index of freshest slate, or -1. */
static int l_cpf_dijkstra_find_best(lua_State *L) {
  CPF_GET(L);
  int kind = (int)luaL_checkinteger(L, 1);
  lua_pushinteger(L, brainPathfinderDijkstraFindBest(pf, kind));
  return 1;
}

/* cpf_dijkstra_status(slate)
 * Returns: started, done, kind, started_tick, completed_tick,
 *          expanded, peak_open, src_x, src_y, in_boat, danger_scale.
 * started: true iff brainPathfinderDijkstraStart has been called at
 * least once on this slate. (Pre-preheat this was equivalent to
 * "g_cost allocated"; after preheat that allocation happens at brain
 * creation time, so we use started_tick > 0 instead — otherwise Lua
 * code that checks `if not active then start() end` skips the very
 * first start because preheat already allocated g_cost.) */
static int l_cpf_dijkstra_status(lua_State *L) {
  CPF_GET(L);
  int slate = (int)luaL_checkinteger(L, 1);
  const DijkstraSlate *s = brainPathfinderDijkstraGetSlate(pf, slate);
  if (!s) {
    /* Push 11 nils so the caller can rely on the count */
    for (int i = 0; i < 11; i++) lua_pushnil(L);
    return 11;
  }
  lua_pushboolean(L, s->started_tick > 0);
  lua_pushboolean(L, s->done);
  lua_pushinteger(L, s->kind);
  lua_pushinteger(L, s->started_tick);
  lua_pushinteger(L, s->completed_tick);
  lua_pushinteger(L, s->expanded);
  lua_pushinteger(L, s->peak_open);
  lua_pushinteger(L, s->src_x);
  lua_pushinteger(L, s->src_y);
  lua_pushinteger(L, s->in_boat);
  lua_pushnumber(L, s->danger_scale);
  return 11;
}

/* cpf_dijkstra_copy_slate(src, dst) — copy src's cost arrays + metadata to
 * dst so dst can serve as a read-only fallback while src recomputes. */
static int l_cpf_dijkstra_copy_slate(lua_State *L) {
  CPF_GET(L);
  int src = (int)luaL_checkinteger(L, 1);
  int dst = (int)luaL_checkinteger(L, 2);
  brainPathfinderDijkstraCopySlate(pf, src, dst);
  return 0;
}

/* cpf_rebuild_edge_costs()
 * Force a rebuild of the precomputed neighbor edge cost grid. Normally
 * happens lazily on the first Dijkstra start after a map change; this
 * lets the brain trigger it explicitly (e.g. after a base capture or
 * pillbox demolition that changes terrain). */
static int l_cpf_rebuild_edge_costs(lua_State *L) {
  CPF_GET(L);
  brainPathfinderRebuildEdgeCosts(pf);
  return 0;
}

/* cpf_simulate_shot(origin_wx, origin_wy, target_wx, target_wy,
 *                   shooter_type=TANK, sight_len=0)
 *   -> { {mx=..., my=...}, ... }
 * Wrapper over brainPathfinderSimulateShot. It keeps no state of its
 * own, but it does read this sim's shell rules off the pathfinder the
 * bot manager pushes to each think — the same upvalue cpf_predict_stop
 * takes its movement rules from. The angle is derived from
 * origin → target via atan2 + lroundf. For sub-brad precision matching
 * the engine's actual shell flight, use cpf_simulate_shot_angle with
 * BrainInfo.tank_angle (a float). */
static int l_cpf_simulate_shot(lua_State *L) {
  CPF_GET(L);
  WORLD ox = (WORLD)luaL_checkinteger(L, 1);
  WORLD oy = (WORLD)luaL_checkinteger(L, 2);
  WORLD tx = (WORLD)luaL_checkinteger(L, 3);
  WORLD ty = (WORLD)luaL_checkinteger(L, 4);
  int shooter   = (int)luaL_optinteger(L, 5, BRAIN_SHOT_SHOOTER_TANK);
  int sight_len = (int)luaL_optinteger(L, 6, 0);

  BrainShotTile tiles[64];
  int n = brainPathfinderSimulateShot(pf, ox, oy, tx, ty, shooter, sight_len,
                                      tiles, (int)(sizeof(tiles)/sizeof(tiles[0])));
  lua_createtable(L, n, 0);
  for (int i = 0; i < n; i++) {
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, tiles[i].mx); lua_setfield(L, -2, "mx");
    lua_pushinteger(L, tiles[i].my); lua_setfield(L, -2, "my");
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

/* cpf_simulate_shot_angle(origin_wx, origin_wy, angle,
 *                          shooter_type=TANK, sight_len=0)
 *   -> { {mx=..., my=...}, ... }
 * Same as cpf_simulate_shot but takes the firing angle directly
 * (0..255 bradians, FLOAT). Use this with BrainInfo.tank_angle
 * for a bit-exact match to a real shell — the engine's
 * shellsAddItem fires at the float tank.angle, so a brain that
 * predicts using the BYTE-floored direction will be off by up to
 * one brad. */
static int l_cpf_simulate_shot_angle(lua_State *L) {
  CPF_GET(L);
  WORLD ox = (WORLD)luaL_checkinteger(L, 1);
  WORLD oy = (WORLD)luaL_checkinteger(L, 2);
  float angle  = (float)luaL_checknumber(L, 3);
  int shooter  = (int)luaL_optinteger(L, 4, BRAIN_SHOT_SHOOTER_TANK);
  int sight_len= (int)luaL_optinteger(L, 5, 0);

  BrainShotTile tiles[64];
  int n = brainPathfinderSimulateShotAngle(pf, ox, oy, angle, shooter, sight_len,
                                           tiles, (int)(sizeof(tiles)/sizeof(tiles[0])));
  lua_createtable(L, n, 0);
  for (int i = 0; i < n; i++) {
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, tiles[i].mx); lua_setfield(L, -2, "mx");
    lua_pushinteger(L, tiles[i].my); lua_setfield(L, -2, "my");
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

/* cpf_predict_stop(tankx, tanky, angle, speed [, terrain_cap])
 *   -> stop_wx, stop_wy
 * Predicts where the tank comes to rest if it begins braking THIS tick,
 * mirroring the engine's exact decel + residual-move model (tank.c tankAccel
 * + tankMoveUnified) so the brain can decide whether a stop here lands it in
 * firing range of a pill:
 *   - brake = tank_brake_rate per tick; a further tank_decel_rate
 *     applies WHILE speed > terrain_cap (terrain only drags speed down to its
 *     cap, never below). Auto-slowdown is the same rate and does NOT stack
 *     with the brake key, so a brake-to-stop is a flat ramp on uniform
 *     terrain. Both rates take this tank's acceleration modifier, so a
 *     modified bot predicts against the rate it will actually brake at.
 *   - each tick (decel first, then move): residual += floor(speed); when
 *     residual >= tank_min_move advance `residual` wu along
 *     utilGet16Dir(angle) (16-dir quantized) via utilCalcDistance, reset.
 * residualSpeed is assumed 0 at entry (the brain can't observe it → the stop
 * can be up to one sub-move, <6 wu, short of reality). terrain_cap defaults to
 * 255 (no terrain term: uniform terrain at/under cap) and is treated as
 * constant for the short stop — a mid-stop speed-boundary crossing isn't
 * modeled. */
static int l_cpf_predict_stop(lua_State *L) {
  /* Both rates take this tank's acceleration modifier, as tankAccel and the
   * auto-slow do; the bot manager pushed it in before the think. The minimum
   * move is sub-move granularity and stays absolute, as it does in tank.c.
   * The terrain cap arrives as an argument, so no speed modifier is needed
   * here. */
  BrainPathfinder **ppfRates = (BrainPathfinder **)lua_touserdata(L, lua_upvalueindex(1));
  BrainPathfinder *pfRates = (ppfRates && *ppfRates) ? *ppfRates : NULL;
  const int accelPct = tankModPct(pfRates ? pfRates->accel_pct : 0);
  const double BRAKE_RATE   = (pfRates ? pfRates->brake_rate : 0.0f) * accelPct / 100;
  const double TERRAIN_RATE = (pfRates ? pfRates->terrain_decel_rate : 0.0f) * accelPct / 100;
  const int    MIN_MOVE     = pfRates ? (int) pfRates->min_move : 0;

  WORLD x  = (WORLD)luaL_checkinteger(L, 1);
  WORLD y  = (WORLD)luaL_checkinteger(L, 2);
  float angle  = (float)luaL_checknumber(L, 3);
  double speed = luaL_checknumber(L, 4);
  double cap   = luaL_optnumber(L, 5, 255.0);
  /* min_speed (7th arg): stop the sim once speed drops to/below this instead of
   * all the way to 0, dropping the slow sub-MIN_MOVE creep tail that the brain
   * can't really observe anyway. 0 = full ramp to rest (original behaviour). */
  double min_speed = luaL_optnumber(L, 7, 0.0);

  /* Optional per-step trace (6th arg true): returns a 3rd value, an array of
   * {speed, decel, dist, after, resid} sub-tables — one per simulation tick —
   * so the brain can print exactly how the brake ramp + 16-dir residual moves
   * played out. Off by default (existing callers pass 5 args). */
  int want_trace = lua_toboolean(L, 6);
  int trace_idx = 0, n = 0;
  if (want_trace) { lua_newtable(L); trace_idx = lua_gettop(L); }

  BYTE dir = utilGet16Dir((TURNTYPE)angle);
  int residual = 0;
  int guard = 0;
  while (speed > min_speed && guard++ < 4096) {
    double s_before = speed;
    double decel = BRAKE_RATE;
    if (speed > cap) { speed -= TERRAIN_RATE; decel += TERRAIN_RATE; } /* over-cap terrain drag */
    speed -= BRAKE_RATE;                       /* brake key (== auto-slow) */
    if (speed < 0.0) speed = 0.0;
    residual += (int)speed;                    /* (BYTE)speed → floor */
    int moved = 0;
    if (residual >= MIN_MOVE) {
      int dx = 0, dy = 0;
      utilCalcDistance(&dx, &dy, (TURNTYPE)dir, residual);
      x = (WORLD)((int)x + dx);
      y = (WORLD)((int)y + dy);
      moved = residual;
      residual = 0;
    }
    if (want_trace) {
      lua_newtable(L);
      lua_pushnumber(L, s_before);  lua_setfield(L, -2, "speed");  /* speed entering this tick */
      lua_pushnumber(L, decel);     lua_setfield(L, -2, "decel");  /* total decel applied */
      lua_pushinteger(L, moved);    lua_setfield(L, -2, "dist");   /* wu advanced this tick (0 = sub-move held) */
      lua_pushnumber(L, speed);     lua_setfield(L, -2, "after");  /* speed after decel */
      lua_pushinteger(L, residual); lua_setfield(L, -2, "resid");  /* residual carried to next tick */
      lua_pushinteger(L, (lua_Integer)x); lua_setfield(L, -2, "x"); /* tank wu pos after this tick */
      lua_pushinteger(L, (lua_Integer)y); lua_setfield(L, -2, "y");
      lua_rawseti(L, trace_idx, ++n);
    }
  }
  lua_pushinteger(L, (lua_Integer)x);
  lua_pushinteger(L, (lua_Integer)y);
  if (want_trace) { lua_pushvalue(L, trace_idx); return 3; }  /* x, y, trace */
  return 2;
}

/* cpf_simulate_shot_with_tanks(origin_wx, origin_wy, target_wx, target_wy,
 *                              shooter_type, sight_len, tanks_table, owner_player)
 *   -> { {mx=, my=, hit_type=, hit_id=}, ... }
 * tanks_table is an array of {wx=, wy=, player_num=} entries.
 * hit_type: 0=tile, 1=tank hit. hit_id: player number (when hit_type==1). */
static int l_cpf_simulate_shot_with_tanks(lua_State *L) {
  CPF_GET(L);
  WORLD ox = (WORLD)luaL_checkinteger(L, 1);
  WORLD oy = (WORLD)luaL_checkinteger(L, 2);
  WORLD tx = (WORLD)luaL_checkinteger(L, 3);
  WORLD ty = (WORLD)luaL_checkinteger(L, 4);
  int shooter    = (int)luaL_optinteger(L, 5, BRAIN_SHOT_SHOOTER_TANK);
  int sight_len  = (int)luaL_optinteger(L, 6, 0);
  luaL_checktype(L, 7, LUA_TTABLE);
  uint8_t owner  = (uint8_t)luaL_checkinteger(L, 8);

  /* Read tanks table */
  BrainShotTankPos tanks[MAX_TANKS];
  int num_tanks = 0;
  int tlen = (int)lua_rawlen(L, 7);
  for (int i = 1; i <= tlen && num_tanks < MAX_TANKS; i++) {
    lua_rawgeti(L, 7, i);
    lua_getfield(L, -1, "wx");
    lua_getfield(L, -2, "wy");
    lua_getfield(L, -3, "player_num");
    tanks[num_tanks].wx         = (WORLD)lua_tointeger(L, -3);
    tanks[num_tanks].wy         = (WORLD)lua_tointeger(L, -2);
    tanks[num_tanks].player_num = (uint8_t)lua_tointeger(L, -1);
    lua_pop(L, 4);  /* pop 3 fields + table entry */
    num_tanks++;
  }

  BrainShotTile tiles[64];
  int n = brainPathfinderSimulateShotWithTanks(pf, ox, oy, tx, ty,
            shooter, sight_len, tanks, num_tanks, owner,
            tiles, (int)(sizeof(tiles)/sizeof(tiles[0])));
  lua_createtable(L, n, 0);
  for (int i = 0; i < n; i++) {
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, tiles[i].mx);       lua_setfield(L, -2, "mx");
    lua_pushinteger(L, tiles[i].my);       lua_setfield(L, -2, "my");
    lua_pushinteger(L, tiles[i].hit_type); lua_setfield(L, -2, "hit_type");
    lua_pushinteger(L, tiles[i].hit_id);   lua_setfield(L, -2, "hit_id");
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

static int l_cpf_estimate_cost(lua_State *L) {
  CPF_GET(L);
  int sx = (int)luaL_checkinteger(L, 1);
  int sy = (int)luaL_checkinteger(L, 2);
  int dx = (int)luaL_checkinteger(L, 3);
  int dy = (int)luaL_checkinteger(L, 4);
  int in_boat = (int)luaL_checkinteger(L, 5);
  float cost = brainPathfinderEstimateCost(pf, sx, sy, dx, dy, in_boat);
  lua_pushnumber(L, (double)cost);
  return 1;
}

static int l_cpf_danger_at(lua_State *L) {
  CPF_GET(L);
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  float value = brainPathfinderDangerAt(pf, x, y);
  lua_pushnumber(L, (double)value);
  return 1;
}

static int l_cpf_lgm_travel_ticks(lua_State *L) {
  CPF_GET(L);
  WORLD sx = (WORLD)luaL_checkinteger(L, 1);
  WORLD sy = (WORLD)luaL_checkinteger(L, 2);
  WORLD dx = (WORLD)luaL_checkinteger(L, 3);
  WORLD dy = (WORLD)luaL_checkinteger(L, 4);
  BYTE blessX = (BYTE)luaL_checkinteger(L, 5);
  BYTE blessY = (BYTE)luaL_checkinteger(L, 6);
  int maxTicks = (int)luaL_checkinteger(L, 7);
  int stuckTicks = (int)luaL_checkinteger(L, 8);
  int result = brainPathfinderLgmTravelTicks(pf, sx, sy, dx, dy,
                                              blessX, blessY,
                                              maxTicks, stuckTicks);
  lua_pushinteger(L, result);
  return 1;
}

static int l_cpf_lgm_travel_ticks_map(lua_State *L) {
  CPF_GET(L);
  BYTE smx = (BYTE)luaL_checkinteger(L, 1);
  BYTE smy = (BYTE)luaL_checkinteger(L, 2);
  BYTE dmx = (BYTE)luaL_checkinteger(L, 3);
  BYTE dmy = (BYTE)luaL_checkinteger(L, 4);
  BYTE blessX = (BYTE)luaL_checkinteger(L, 5);
  BYTE blessY = (BYTE)luaL_checkinteger(L, 6);
  int maxTicks = (int)luaL_checkinteger(L, 7);
  int stuckTicks = (int)luaL_checkinteger(L, 8);
  int result = brainPathfinderLgmTravelTicksMap(pf, smx, smy, dmx, dmy,
                                                 blessX, blessY,
                                                 maxTicks, stuckTicks);
  lua_pushinteger(L, result);
  return 1;
}

/* cpf_set_lgm_blocked(tiles) — tiles is an array of { mx, my } pairs (each a
 * 2-element table). Clears the LGM-impassable overlay, then marks each tile so
 * the LGM travel sim treats it as a wall. The bot's brain map is PURE TERRAIN
 * (botUpdateBrainMap stamps no pills/bases), so the sim can't see them — the
 * brain stamps both live pills and enemy bases here each tick to match the
 * engine's mapGetManSpeed (see brain_pathfinder.c lgmGetBrainManSpeed). */
static int l_cpf_set_lgm_blocked(lua_State *L) {
  CPF_GET(L);
  brainPathfinderClearLgmBlock(pf);
  if (lua_istable(L, 1)) {
    int n = (int)lua_rawlen(L, 1);
    int i;
    for (i = 1; i <= n; i++) {
      lua_rawgeti(L, 1, i);          /* push tiles[i] */
      if (lua_istable(L, -1)) {
        BYTE mx, my;
        lua_rawgeti(L, -1, 1); mx = (BYTE)luaL_checkinteger(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, -1, 2); my = (BYTE)luaL_checkinteger(L, -1); lua_pop(L, 1);
        brainPathfinderSetLgmBlock(pf, mx, my);
      }
      lua_pop(L, 1);                 /* pop tiles[i] */
    }
  }
  return 0;
}

static int l_cpf_estimate_tank_travel_ticks(lua_State *L) {
  CPF_GET(L);
  int sx = (int)luaL_checkinteger(L, 1);
  int sy = (int)luaL_checkinteger(L, 2);
  int dx = (int)luaL_checkinteger(L, 3);
  int dy = (int)luaL_checkinteger(L, 4);
  int in_boat = (int)luaL_checkinteger(L, 5);
  int maxTicks = (int)luaL_optinteger(L, 6, 4000);
  int stuckTicks = (int)luaL_optinteger(L, 7, 200);
  int result = brainPathfinderEstimateTankTravelTicks(pf, sx, sy, dx, dy,
                                                       in_boat,
                                                       maxTicks, stuckTicks);
  lua_pushinteger(L, result);
  return 1;
}

static int l_cpf_find_front_line(lua_State *L) {
  int i;
  int max_points = 512;
  int out_x[512], out_y[512];
  int count;
  CPF_GET(L);
  count = brainPathfinderFindFrontLine(pf, out_x, out_y, max_points);

  lua_createtable(L, count * 2, 0);
  for (i = 0; i < count; i++) {
    lua_pushinteger(L, out_x[i]);
    lua_rawseti(L, -2, i * 2 + 1);
    lua_pushinteger(L, out_y[i]);
    lua_rawseti(L, -2, i * 2 + 2);
  }
  return 1;
}

/* cpf_dijkstra_shells_at(kind, x, y)
 * Returns shells remaining on arrival at (x,y) from the freshest Dijkstra
 * slate of the given kind, or nil if no slate has a finite cost there.
 * Valid after smart_cost when Dijkstra was the source of the cost. */
static int l_cpf_dijkstra_shells_at(lua_State *L) {
  CPF_GET(L);
  int kind  = (int)luaL_checkinteger(L, 1);
  int x     = (int)luaL_checkinteger(L, 2);
  int y     = (int)luaL_checkinteger(L, 3);
  int best  = brainPathfinderDijkstraFindBest(pf, kind);
  if (best < 0) { lua_pushnil(L); return 1; }
  DijkstraSlate *s = &pf->dij_slates[best];
  if (!s->shells_at) { lua_pushnil(L); return 1; }
  int node = y * 256 + x;  /* land node (boat=0) */
  float g = s->g_cost ? s->g_cost[node] : 1e30f;
  if (g >= 1e29f) { lua_pushnil(L); return 1; }  /* slate hasn't reached tile */
  lua_pushinteger(L, (lua_Integer)s->shells_at[node]);
  return 1;
}

/* cpf_astar_shells_at(x, y)
 * Returns shells remaining on arrival at (x,y) from the last cost_to call.
 * Valid after smart_cost when A* was used (Dijkstra off or tile not yet reached). */
static int l_cpf_astar_shells_at(lua_State *L) {
  CPF_GET(L);
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  int node = y * 256 + x;  /* land node */
  lua_pushinteger(L, (lua_Integer)pf->shells_at[node]);
  return 1;
}

/* shell_debug_hits() → array of tables {wx=, wy=, owner=}
 * Returns the ring buffer of recent shell-collision world positions as
 * recorded by shellsUpdate in the C sim. The brain calls this each tick
 * and redraws overlays — because overlay commands are recorded per-frame,
 * replay scrubbing shows the exact set known at that historical tick. */
static int l_shell_debug_hits(lua_State *L) {
  int n = shellsDebugHitLogCount();
  lua_createtable(L, n, 0);
  for (int i = 0; i < n; i++) {
    int wx = 0, wy = 0;
    uint32_t tk = 0;
    uint8_t owner = 0;
    if (!shellsDebugHitLogGet(i, &wx, &wy, &tk, &owner)) continue;
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, wx);    lua_setfield(L, -2, "wx");
    lua_pushinteger(L, wy);    lua_setfield(L, -2, "wy");
    lua_pushinteger(L, owner); lua_setfield(L, -2, "owner");
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

static int l_cpf_trace_path(lua_State *L) {
  CPF_GET(L);
  int path_x[64], path_y[64];
  int count = brainPathfinderTracePath(pf, path_x, path_y, 64);
  lua_createtable(L, count * 2, 0);
  for (int i = 0; i < count; i++) {
    lua_pushinteger(L, path_x[i]);
    lua_rawseti(L, -2, 2 * i + 1);
    lua_pushinteger(L, path_y[i]);
    lua_rawseti(L, -2, 2 * i + 2);
  }
  return 1;
}

/* cpf_trace_last_search(dx, dy) -> flat array {x1,y1,x2,y2,...}
 * Trace the most-recent A* search's parent chain to (dx, dy) without
 * the status==1 gate. Use after cost_to() — its end-of-call cleanup
 * zaps status/dest so cpf_trace_path() returns empty, but the
 * closed/parent state is still good enough to reconstruct the path. */
static int l_cpf_trace_last_search(lua_State *L) {
  CPF_GET(L);
  int dx = (int)luaL_checkinteger(L, 1);
  int dy = (int)luaL_checkinteger(L, 2);
  int path_x[64], path_y[64];
  int count = brainPathfinderTraceLastSearchPath(pf, dx, dy, path_x, path_y, 64);
  lua_createtable(L, count * 2, 0);
  for (int i = 0; i < count; i++) {
    lua_pushinteger(L, path_x[i]);
    lua_rawseti(L, -2, 2 * i + 1);
    lua_pushinteger(L, path_y[i]);
    lua_rawseti(L, -2, 2 * i + 2);
  }
  return 1;
}

/* cpf_serialize() -> string
 * Returns binary blob of the full pathfinder state (grids + Dijkstra slates). */
static int l_cpf_serialize(lua_State *L) {
  BrainPathfinder **pfPtr = (BrainPathfinder **)lua_touserdata(L, lua_upvalueindex(1));
  BrainPathfinder *pf = pfPtr ? *pfPtr : NULL;
  if (!pf) { lua_pushnil(L); return 1; }
  size_t blobSize = 0;
  unsigned char *blob = brainPathfinderSerialize(pf, &blobSize);
  if (!blob) { lua_pushnil(L); return 1; }
  lua_pushlstring(L, (const char *)blob, blobSize);
  free(blob);
  return 1;
}

/* cpf_deserialize(string) -> bool
 * Restores pathfinder state from a blob returned by cpf_serialize(). */
static int l_cpf_deserialize(lua_State *L) {
  BrainPathfinder **pfPtr = (BrainPathfinder **)lua_touserdata(L, lua_upvalueindex(1));
  BrainPathfinder *pf = pfPtr ? *pfPtr : NULL;
  if (!pf) { lua_pushboolean(L, 0); return 1; }
  size_t len = 0;
  const char *data = luaL_checklstring(L, 1, &len);
  int ok = brainPathfinderDeserialize(pf, (const unsigned char *)data, len);
  lua_pushboolean(L, ok);
  return 1;
}

void brainCoreRegisterPathfinder(lua_State *L, BrainPathfinder **pfPtr) {
  static const struct { const char *name; lua_CFunction func; } funcs[] = {
    { "cpf_set_terrain_cost",  l_cpf_set_terrain_cost },
    { "cpf_set_boat_cost",     l_cpf_set_boat_cost },
    { "cpf_set_terrain_speed", l_cpf_set_terrain_speed },
    { "cpf_set_config",        l_cpf_set_config },
    { "cpf_clear_danger",      l_cpf_clear_danger },
    { "cpf_stamp_pill",        l_cpf_stamp_pill },
    { "cpf_set_danger",        l_cpf_set_danger },
    { "cpf_load_danger",                    l_cpf_load_danger },
    { "cpf_load_pill_danger_from_threat",   l_cpf_load_pill_danger_from_threat },
    { "cpf_set_overlay",          l_cpf_set_overlay },
    { "cpf_get_overlay",          l_cpf_get_overlay },
    { "cpf_get_danger",           l_cpf_get_danger },
    { "cpf_astar_log_enable",     l_cpf_astar_log_enable },
    { "cpf_astar_log_set_tick",   l_cpf_astar_log_set_tick },
    { "cpf_clear_overlay",        l_cpf_clear_overlay },
    { "cpf_set_danger_offset",    l_cpf_set_danger_offset },
    { "cpf_clear_danger_offset",  l_cpf_clear_danger_offset },
    { "cpf_load_danger_offset",   l_cpf_load_danger_offset },
    { "cpf_clear_influence",   l_cpf_clear_influence },
    { "cpf_stamp_influence",   l_cpf_stamp_influence },
    { "cpf_influence_at",      l_cpf_influence_at },
    { "cpf_path_to",           l_cpf_path_to },
    { "cpf_cost_to",           l_cpf_cost_to },
    { "cpf_cost_to_reset",     l_cpf_cost_to_reset },
    { "cpf_cost_to_incremental", l_cpf_cost_to_incremental },
    { "cpf_dijkstra_from",     l_cpf_dijkstra_from },
    { "cpf_dijkstra_start",         l_cpf_dijkstra_start },
    { "cpf_dijkstra_step",          l_cpf_dijkstra_step },
    { "cpf_dijkstra_cost_at",       l_cpf_dijkstra_cost_at },
    { "cpf_dijkstra_lookup_by_kind", l_cpf_dijkstra_lookup_by_kind },
    { "cpf_dijkstra_lookup_subtract_by_kind", l_cpf_dijkstra_lookup_subtract_by_kind },
    { "cpf_dijkstra_next_step",     l_cpf_dijkstra_next_step },
    { "cpf_dijkstra_trace_path",    l_cpf_dijkstra_trace_path },
    { "cpf_dijkstra_trace_path_by_kind", l_cpf_dijkstra_trace_path_by_kind },
    { "cpf_dijkstra_pick_reuse_slate", l_cpf_dijkstra_pick_reuse_slate },
    { "cpf_dijkstra_find_best",     l_cpf_dijkstra_find_best },
    { "cpf_dijkstra_status",        l_cpf_dijkstra_status },
    { "cpf_dijkstra_copy_slate",    l_cpf_dijkstra_copy_slate },
    { "cpf_rebuild_edge_costs", l_cpf_rebuild_edge_costs },
    { "cpf_estimate_cost",     l_cpf_estimate_cost },
    { "cpf_simulate_shot",        l_cpf_simulate_shot },
    { "cpf_simulate_shot_angle",  l_cpf_simulate_shot_angle },
    { "cpf_predict_stop",         l_cpf_predict_stop },
    { "cpf_simulate_shot_with_tanks", l_cpf_simulate_shot_with_tanks },
    { "cpf_danger_at",             l_cpf_danger_at },
    { "cpf_lgm_travel_ticks",      l_cpf_lgm_travel_ticks },
    { "cpf_lgm_travel_ticks_map",  l_cpf_lgm_travel_ticks_map },
    { "cpf_set_lgm_blocked",       l_cpf_set_lgm_blocked },
    { "cpf_estimate_tank_travel_ticks", l_cpf_estimate_tank_travel_ticks },
    { "cpf_dijkstra_shells_at",    l_cpf_dijkstra_shells_at },
    { "cpf_astar_shells_at",       l_cpf_astar_shells_at },
    { "cpf_trace_path",            l_cpf_trace_path },
    { "cpf_trace_last_search",     l_cpf_trace_last_search },
    { "shell_debug_hits",          l_shell_debug_hits },
    { "cpf_find_front_line",       l_cpf_find_front_line },
    { "cpf_serialize",             l_cpf_serialize },
    { "cpf_deserialize",           l_cpf_deserialize },
    { NULL, NULL }
  };
  int i;
  for (i = 0; funcs[i].name != NULL; i++) {
    lua_pushlightuserdata(L, (void *)pfPtr);
    lua_pushcclosure(L, funcs[i].func, 1);
    lua_setglobal(L, funcs[i].name);
  }
}

/* ------------------------------------------------------------------ */
/* C World Simulator Lua wrappers (wsim_* globals)                     */
/* ------------------------------------------------------------------ */

#define WSIM_GET(L) \
  BrainWorldSim **pps = (BrainWorldSim **)lua_touserdata(L, lua_upvalueindex(1)); \
  BrainWorldSim *ws = (pps && *pps) ? *pps : NULL; \
  if (!ws) return 0

static int l_wsim_clear(lua_State *L) {
  WSIM_GET(L);
  brainWorldSimClear(ws);
  return 0;
}

static int l_wsim_set_terrain_speed(lua_State *L) {
  WSIM_GET(L);
  int type = (int)luaL_checkinteger(L, 1);
  float speed = (float)luaL_checknumber(L, 2);
  brainWorldSimSetTerrainSpeed(ws, type, speed);
  return 0;
}

static int l_wsim_add_pill(lua_State *L) {
  WSIM_GET(L);
  int mx     = (int)luaL_checkinteger(L, 1);
  int my     = (int)luaL_checkinteger(L, 2);
  int health = (int)luaL_checkinteger(L, 3);
  float anger = (float)luaL_checknumber(L, 4);
  int owner  = (int)luaL_checkinteger(L, 5);
  int pill_id = (int)luaL_checkinteger(L, 6);
  brainWorldSimAddPill(ws, mx, my, health, anger, owner, pill_id);
  return 0;
}

static int l_wsim_add_tank(lua_State *L) {
  WSIM_GET(L);
  int wx      = (int)luaL_checkinteger(L, 1);
  int wy      = (int)luaL_checkinteger(L, 2);
  int dir     = (int)luaL_checkinteger(L, 3);
  int speed   = (int)luaL_checkinteger(L, 4);
  int is_ours = lua_toboolean(L, 5);
  int owner   = (int)luaL_checkinteger(L, 6);
  int armour  = (int)luaL_checkinteger(L, 7);
  brainWorldSimAddTank(ws, wx, wy, dir, speed, is_ours, owner, armour);
  return 0;
}

static int l_wsim_set_path(lua_State *L) {
  WSimPathPoint pts[WSIM_MAX_PATH];
  int i;
  WSIM_GET(L);

  luaL_checktype(L, 1, LUA_TTABLE);
  int total = (int)lua_rawlen(L, 1);  /* flat: x1,y1,x2,y2,... */
  int npts = total / 2;
  if (npts > WSIM_MAX_PATH) npts = WSIM_MAX_PATH;

  for (i = 0; i < npts; i++) {
    lua_rawgeti(L, 1, 2 * i + 1);
    pts[i].mx = (uint8_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_rawgeti(L, 1, 2 * i + 2);
    pts[i].my = (uint8_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
  }

  brainWorldSimSetPath(ws, pts, npts);
  return 0;
}

static int l_wsim_set_attack_target(lua_State *L) {
  WSIM_GET(L);
  int idx = (int)luaL_checkinteger(L, 1);
  brainWorldSimSetAttackTarget(ws, idx);
  return 0;
}

static int l_wsim_set_lgm(lua_State *L) {
  WSIM_GET(L);
  int dispatch_tick = (int)luaL_checkinteger(L, 1);
  int dest_mx       = (int)luaL_checkinteger(L, 2);
  int dest_my       = (int)luaL_checkinteger(L, 3);
  int speed         = (int)luaL_optinteger(L, 4, 4);
  brainWorldSimSetLGM(ws, dispatch_tick, dest_mx, dest_my, speed);
  return 0;
}

static int l_wsim_run(lua_State *L) {
  WSimResult r;
  int i;
  WSIM_GET(L);

  int max_ticks = (int)luaL_optinteger(L, 1, 300);
  r = brainWorldSimRun(ws, max_ticks);

  /* Build result table */
  lua_createtable(L, 0, 10);

  lua_pushinteger(L, r.armour_remaining);
  lua_setfield(L, -2, "armour");

  lua_pushinteger(L, r.damage_taken);
  lua_setfield(L, -2, "damage");

  lua_pushinteger(L, r.ticks_simulated);
  lua_setfield(L, -2, "ticks");

  lua_pushinteger(L, r.arrival_tick);
  lua_setfield(L, -2, "arrival");

  lua_pushboolean(L, r.killed);
  lua_setfield(L, -2, "killed");

  /* LGM fields: only present if LGM was dispatched */
  if (r.lgm_survived > 0) {
    lua_pushboolean(L, r.lgm_survived == 1);
    lua_setfield(L, -2, "lgm_survived");

    if (r.lgm_arrival_tick >= 0) {
      lua_pushinteger(L, r.lgm_arrival_tick);
      lua_setfield(L, -2, "lgm_arrival");
    }
    if (r.lgm_death_tick >= 0) {
      lua_pushinteger(L, r.lgm_death_tick);
      lua_setfield(L, -2, "lgm_death");
    }
  }

  /* Per-pill results array (only pills that were added) */
  lua_createtable(L, ws->num_pills, 0);
  for (i = 0; i < ws->num_pills; i++) {
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, ws->pills[i].pill_id);
    lua_setfield(L, -2, "id");
    lua_pushinteger(L, r.pill_shots[i]);
    lua_setfield(L, -2, "shots");
    lua_pushinteger(L, r.pill_final_health[i]);
    lua_setfield(L, -2, "health");
    lua_pushinteger(L, r.pill_final_speed[i]);
    lua_setfield(L, -2, "speed");
    lua_rawseti(L, -2, i + 1);
  }
  lua_setfield(L, -2, "pills");

  /* Per-hit position log */
  lua_createtable(L, r.num_hits, 0);
  for (i = 0; i < r.num_hits; i++) {
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, r.hits[i].mx);
    lua_setfield(L, -2, "mx");
    lua_pushinteger(L, r.hits[i].my);
    lua_setfield(L, -2, "my");
    lua_pushinteger(L, r.hits[i].armour_after);
    lua_setfield(L, -2, "armour");
    lua_pushinteger(L, r.hits[i].tick);
    lua_setfield(L, -2, "tick");
    lua_pushinteger(L, r.hits[i].pill_idx);
    lua_setfield(L, -2, "pill");
    lua_rawseti(L, -2, i + 1);
  }
  lua_setfield(L, -2, "hits");

  return 1;
}

void brainCoreRegisterWorldSim(lua_State *L, BrainWorldSim **wsPtr) {
  static const struct { const char *name; lua_CFunction func; } funcs[] = {
    { "wsim_clear",             l_wsim_clear },
    { "wsim_set_terrain_speed", l_wsim_set_terrain_speed },
    { "wsim_add_pill",          l_wsim_add_pill },
    { "wsim_add_tank",          l_wsim_add_tank },
    { "wsim_set_path",          l_wsim_set_path },
    { "wsim_set_attack_target", l_wsim_set_attack_target },
    { "wsim_set_lgm",           l_wsim_set_lgm },
    { "wsim_run",               l_wsim_run },
    { NULL, NULL }
  };
  int i;
  for (i = 0; funcs[i].name != NULL; i++) {
    lua_pushlightuserdata(L, (void *)wsPtr);
    lua_pushcclosure(L, funcs[i].func, 1);
    lua_setglobal(L, funcs[i].name);
  }
}

/* ------------------------------------------------------------------ */
/* Overlay drawing Lua wrappers (overlay_* globals)                    */
/* ------------------------------------------------------------------ */

#define OVL_GET(L) \
  OverlayCmdBuffer **ppb = (OverlayCmdBuffer **)lua_touserdata(L, lua_upvalueindex(1)); \
  OverlayCmdBuffer *buf = (ppb && *ppb) ? *ppb : NULL; \
  if (!buf) return 0

/* overlay_clear() */
static int l_overlay_clear(lua_State *L) {
  OVL_GET(L);
  overlayCmdBufferClear(buf);
  return 0;
}

/* overlay_line(x1, y1, x2, y2, r, g, b [, a [, viz_idx]])
 * viz_idx: optional uint8 index into BrainTest's VIZ_TOGGLES array,
 *          set by viz.lua wrappers so the renderer can filter
 *          recorded commands during playback. Defaults to 0xFF
 *          ("no viz_id" — never filtered). */
static int l_overlay_line(lua_State *L) {
  OVL_GET(L);
  float x1 = (float)luaL_checknumber(L, 1);
  float y1 = (float)luaL_checknumber(L, 2);
  float x2 = (float)luaL_checknumber(L, 3);
  float y2 = (float)luaL_checknumber(L, 4);
  int r = luaL_checkinteger(L, 5);
  int g = luaL_checkinteger(L, 6);
  int b = luaL_checkinteger(L, 7);
  int a = luaL_optinteger(L, 8, 255);
  int viz_idx = luaL_optinteger(L, 9, OVERLAY_VIZ_IDX_NONE);
  overlayCmdLine(buf, x1, y1, x2, y2, r, g, b, a);
  overlayCmdSetLastVizIdx(buf, (uint8_t)viz_idx);
  return 0;
}

/* overlay_rect(x1, y1, x2, y2, r, g, b [, a [, filled]]) */
static int l_overlay_rect(lua_State *L) {
  OVL_GET(L);
  float x1 = (float)luaL_checknumber(L, 1);
  float y1 = (float)luaL_checknumber(L, 2);
  float x2 = (float)luaL_checknumber(L, 3);
  float y2 = (float)luaL_checknumber(L, 4);
  int r = luaL_checkinteger(L, 5);
  int g = luaL_checkinteger(L, 6);
  int b = luaL_checkinteger(L, 7);
  int a = luaL_optinteger(L, 8, 255);
  int filled = lua_toboolean(L, 9);
  /* 10th arg (optional, default false): if true and rect is filled,
   * the overlay is drawn at full 1/256-tile precision instead of
   * being floored to the game-pixel grid. Used by overlays that
   * track sub-wu sprites (e.g. the LGM, which renders at sub-wu). */
  int subpixel = lua_toboolean(L, 10);
  /* 11th arg (optional): viz_idx for playback-time filtering. */
  int viz_idx = luaL_optinteger(L, 11, OVERLAY_VIZ_IDX_NONE);
  overlayCmdRect(buf, x1, y1, x2, y2, r, g, b, a, filled);
  if (subpixel && filled && buf && buf->count > 0) {
    buf->cmds[buf->count - 1].type = OVERLAY_CMD_RECT_FILL_SUBPIXEL;
  }
  overlayCmdSetLastVizIdx(buf, (uint8_t)viz_idx);
  return 0;
}

/* overlay_circle(cx, cy, radius, r, g, b [, a [, viz_idx [, subpixel [, filled]]]])
 * subpixel (optional, default false): if true the circle's center
 * uses 1/256-tile precision instead of being floored to the game-pixel
 * grid. Use for markers that pin to a sub-game-pixel position (e.g.
 * shell hit dot) where the standard 1-gp quantization is visible.
 * filled (optional, default false): draw a filled disc instead of an
 * outline ring. Mutually exclusive with subpixel (filled wins). */
static int l_overlay_circle(lua_State *L) {
  OVL_GET(L);
  float cx = (float)luaL_checknumber(L, 1);
  float cy = (float)luaL_checknumber(L, 2);
  float radius = (float)luaL_checknumber(L, 3);
  int r = luaL_checkinteger(L, 4);
  int g = luaL_checkinteger(L, 5);
  int b = luaL_checkinteger(L, 6);
  int a = luaL_optinteger(L, 7, 255);
  int viz_idx  = luaL_optinteger(L, 8, OVERLAY_VIZ_IDX_NONE);
  int subpixel = lua_toboolean(L, 9);
  int filled   = lua_toboolean(L, 10);
  overlayCmdCircle(buf, cx, cy, radius, r, g, b, a);
  if (buf && buf->count > 0) {
    if (filled) {
      buf->cmds[buf->count - 1].type = OVERLAY_CMD_CIRCLE_FILL;
    } else if (subpixel) {
      buf->cmds[buf->count - 1].type = OVERLAY_CMD_CIRCLE_SUBPIXEL;
    }
  }
  overlayCmdSetLastVizIdx(buf, (uint8_t)viz_idx);
  return 0;
}

/* overlay_text(x, y, text [, anchor [, r, g, b [, a]]])
 * anchor: "topleft", "topright", "bottomleft", "bottomright", "center" */
static int l_overlay_text(lua_State *L) {
  OVL_GET(L);
  float x = (float)luaL_checknumber(L, 1);
  float y = (float)luaL_checknumber(L, 2);
  const char *text = luaL_checkstring(L, 3);
  const char *anchorStr = luaL_optstring(L, 4, "topleft");
  int r = luaL_optinteger(L, 5, 255);
  int g = luaL_optinteger(L, 6, 255);
  int b = luaL_optinteger(L, 7, 255);
  int a = luaL_optinteger(L, 8, 255);
  float scale = (float)luaL_optnumber(L, 9, 1.0);
  int viz_idx = luaL_optinteger(L, 10, OVERLAY_VIZ_IDX_NONE);

  uint8_t anchor = OVERLAY_ANCHOR_TOPLEFT;
  if (strcmp(anchorStr, "topright") == 0)         anchor = OVERLAY_ANCHOR_TOPRIGHT;
  else if (strcmp(anchorStr, "bottomleft") == 0)  anchor = OVERLAY_ANCHOR_BOTTOMLEFT;
  else if (strcmp(anchorStr, "bottomright") == 0) anchor = OVERLAY_ANCHOR_BOTTOMRIGHT;
  else if (strcmp(anchorStr, "center") == 0)      anchor = OVERLAY_ANCHOR_CENTER;

  overlayCmdText(buf, x, y, text, anchor, r, g, b, a);
  /* Store scale in the radius field (unused for text) */
  if (buf && buf->count > 0) {
    buf->cmds[buf->count - 1].radius = scale;
  }
  overlayCmdSetLastVizIdx(buf, (uint8_t)viz_idx);
  return 0;
}

/* overlay_hud_text(offset_x, offset_y, text, anchor, r, g, b [, a])
 * anchor: "topleft", "topright", "bottomleft", "bottomright"
 * x/y are pixel offsets from the chosen corner */
static int l_overlay_hud_text(lua_State *L) {
  OVL_GET(L);
  float x = (float)luaL_checknumber(L, 1);
  float y = (float)luaL_checknumber(L, 2);
  const char *text = luaL_checkstring(L, 3);
  const char *anchorStr = luaL_optstring(L, 4, "topleft");
  int r = luaL_optinteger(L, 5, 255);
  int g = luaL_optinteger(L, 6, 255);
  int b = luaL_optinteger(L, 7, 255);
  int a = luaL_optinteger(L, 8, 255);
  int viz_idx = luaL_optinteger(L, 9, OVERLAY_VIZ_IDX_NONE);

  uint8_t anchor = OVERLAY_ANCHOR_TOPLEFT;
  if (strcmp(anchorStr, "topright") == 0)         anchor = OVERLAY_ANCHOR_TOPRIGHT;
  else if (strcmp(anchorStr, "bottomleft") == 0)  anchor = OVERLAY_ANCHOR_BOTTOMLEFT;
  else if (strcmp(anchorStr, "bottomright") == 0) anchor = OVERLAY_ANCHOR_BOTTOMRIGHT;
  else if (strcmp(anchorStr, "center") == 0)      anchor = OVERLAY_ANCHOR_CENTER;

  overlayCmdHudText(buf, x, y, text, anchor, r, g, b, a);
  overlayCmdSetLastVizIdx(buf, (uint8_t)viz_idx);
  return 0;
}

/* overlay_hud_rect(offset_x, offset_y, w, h, anchor, r, g, b [, a [, filled [, viz_idx]]])
 * Same anchor scheme / pixel-offset coords as overlay_hud_text. filled != 0
 * draws a solid fill (background), else a 1px outline (border). */
static int l_overlay_hud_rect(lua_State *L) {
  OVL_GET(L);
  float x = (float)luaL_checknumber(L, 1);
  float y = (float)luaL_checknumber(L, 2);
  float w = (float)luaL_checknumber(L, 3);
  float h = (float)luaL_checknumber(L, 4);
  const char *anchorStr = luaL_optstring(L, 5, "topleft");
  int r = luaL_optinteger(L, 6, 255);
  int g = luaL_optinteger(L, 7, 255);
  int b = luaL_optinteger(L, 8, 255);
  int a = luaL_optinteger(L, 9, 255);
  int filled  = luaL_optinteger(L, 10, 0);
  int viz_idx = luaL_optinteger(L, 11, OVERLAY_VIZ_IDX_NONE);

  uint8_t anchor = OVERLAY_ANCHOR_TOPLEFT;
  if (strcmp(anchorStr, "topright") == 0)         anchor = OVERLAY_ANCHOR_TOPRIGHT;
  else if (strcmp(anchorStr, "bottomleft") == 0)  anchor = OVERLAY_ANCHOR_BOTTOMLEFT;
  else if (strcmp(anchorStr, "bottomright") == 0) anchor = OVERLAY_ANCHOR_BOTTOMRIGHT;
  else if (strcmp(anchorStr, "center") == 0)      anchor = OVERLAY_ANCHOR_CENTER;

  overlayCmdHudRect(buf, x, y, w, h, anchor, r, g, b, a, filled);
  overlayCmdSetLastVizIdx(buf, (uint8_t)viz_idx);
  return 0;
}

/* UNUSED ON THIS BRANCH — kept in lockstep with the BrainTest source
 * line so future merges don't conflict. Registers overlay_* Lua
 * globals (debug-shape drawing) backed by a per-brain OverlayCmdBuffer.
 * No caller wires up `bufPtr` here, so the brain's viz.lua wrappers
 * see overlay_* as nil and silently no-op (`if not overlay_text then
 * return end`). When BrainTest is wired in on a future branch, it
 * calls this from luabrainshandler.c after creating the brain instance. */
void brainCoreRegisterOverlay(lua_State *L, OverlayCmdBuffer **bufPtr) {
  static const struct { const char *name; lua_CFunction func; } funcs[] = {
    { "overlay_clear",    l_overlay_clear },
    { "overlay_line",     l_overlay_line },
    { "overlay_rect",     l_overlay_rect },
    { "overlay_circle",   l_overlay_circle },
    { "overlay_text",     l_overlay_text },
    { "overlay_hud_text", l_overlay_hud_text },
    { "overlay_hud_rect", l_overlay_hud_rect },
    { NULL, NULL }
  };
  int i;
  for (i = 0; funcs[i].name != NULL; i++) {
    lua_pushlightuserdata(L, (void *)bufPtr);
    lua_pushcclosure(L, funcs[i].func, 1);
    lua_setglobal(L, funcs[i].name);
  }
}

/* ── viz_detail registry hook ─────────────────────────────────────── */
/*
 * The brain calls overlay_detail / overlay_detail_text each tick to
 * register clickable map primitives with rich text bodies. The host
 * (BrainTest) implements the actual registry; under non-host runtimes
 * (game client, headless server) the callbacks stay NULL and these
 * bindings silently no-op so the brain doesn't have to gate on
 * "am I in BrainTest". */

static BrainVizDetailRegisterFunc g_vizDetailRegisterCb = NULL;
static BrainVizDetailAppendBodyFunc g_vizDetailAppendBodyCb = NULL;
static BrainVizDetailClearFunc g_vizDetailClearCb = NULL;

void brainCoreSetVizDetailRegisterCallback(BrainVizDetailRegisterFunc cb) {
  g_vizDetailRegisterCb = cb;
}
void brainCoreSetVizDetailAppendBodyCallback(BrainVizDetailAppendBodyFunc cb) {
  g_vizDetailAppendBodyCb = cb;
}
void brainCoreSetVizDetailClearCallback(BrainVizDetailClearFunc cb) {
  g_vizDetailClearCb = cb;
}

/* overlay_detail(detail_id, kind, x1, y1, x2, y2, label) */
static int l_overlay_detail(lua_State *L) {
  const char *id    = luaL_checkstring(L, 1);
  const char *kind  = luaL_checkstring(L, 2);
  float       x1    = (float)luaL_checknumber(L, 3);
  float       y1    = (float)luaL_checknumber(L, 4);
  float       x2    = (float)luaL_checknumber(L, 5);
  float       y2    = (float)luaL_checknumber(L, 6);
  const char *label = luaL_optstring(L, 7, "");
  int idx = -1;
  if (g_vizDetailRegisterCb) {
    idx = g_vizDetailRegisterCb(id, kind, x1, y1, x2, y2, label);
  }
  lua_pushinteger(L, idx);
  return 1;
}

/* overlay_detail_text(detail_id, line) */
static int l_overlay_detail_text(lua_State *L) {
  const char *id   = luaL_checkstring(L, 1);
  const char *line = luaL_checkstring(L, 2);
  int idx = -1;
  if (g_vizDetailAppendBodyCb) {
    idx = g_vizDetailAppendBodyCb(id, line);
  }
  lua_pushinteger(L, idx);
  return 1;
}

/* overlay_detail_clear() — wipe the entire registry. Brains call
 * once at the top of think() so each tick rebuilds from scratch. */
static int l_overlay_detail_clear(lua_State *L) {
  (void)L;
  if (g_vizDetailClearCb) g_vizDetailClearCb();
  return 0;
}

void brainCoreRegisterVizDetail(lua_State *L) {
  lua_pushcfunction(L, l_overlay_detail);       lua_setglobal(L, "overlay_detail");
  lua_pushcfunction(L, l_overlay_detail_text);  lua_setglobal(L, "overlay_detail_text");
  lua_pushcfunction(L, l_overlay_detail_clear); lua_setglobal(L, "overlay_detail_clear");
}

/* NOTE: pill_contrib bindings moved to brains/GoalHunter/c/
 * gh_overlay_pillcontrib.c — they were specific to GoalHunter's
 * BrainTest overlay and shouldn't live in the generic brain runtime. */

/* ------------------------------------------------------------------ */
/* Print capture (override Lua's print to also call a callback)        */
/* ------------------------------------------------------------------ */

typedef struct {
    BrainPrintCaptureFunc cb;
    void *ud;
    const uint32_t *tickPtr;
} PrintCaptureCtx;

static int l_captured_print(lua_State *L) {
  PrintCaptureCtx *ctx = (PrintCaptureCtx *)lua_touserdata(L, lua_upvalueindex(1));

  /* Build the output string (same as Lua's default print) */
  int n = lua_gettop(L);
  char buf[4096];
  int pos = 0;
  for (int i = 1; i <= n; i++) {
    if (i > 1 && pos < (int)sizeof(buf) - 1) buf[pos++] = '\t';
    const char *s = luaL_tolstring(L, i, NULL);
    if (s) {
      int slen = (int)strlen(s);
      int room = (int)sizeof(buf) - pos - 1;
      if (slen > room) slen = room;
      memcpy(buf + pos, s, slen);
      pos += slen;
    }
    lua_pop(L, 1); /* pop the tostring result */
  }
  buf[pos] = '\0';

  /* Write to stderr as usual */
  fprintf(stderr, "%s\n", buf);

  /* Call the capture callback */
  if (ctx->cb) {
    uint32_t tick = ctx->tickPtr ? *ctx->tickPtr : 0;
    ctx->cb(tick, buf, ctx->ud);
  }

  return 0;
}

/* Global print capture — applied to all new brain instances */
/* UNUSED ON THIS BRANCH — kept in lockstep with the BrainTest source.
 * Lua print() override that mirrors output to a callback (used by
 * BrainTest's log window to capture per-bot prints with tick context).
 * No caller invokes brainCoreSetGlobalPrintCapture or
 * brainCoreRegisterPrintCapture here, so Lua's print() retains its
 * default stderr behavior. */
BrainPrintCaptureFunc g_printCb = NULL;
void *g_printCbUd = NULL;
const uint32_t *g_printTickPtr = NULL;

void brainCoreSetGlobalPrintCapture(BrainPrintCaptureFunc cb, void *ud,
                                     const uint32_t *tickPtr) {
  g_printCb = cb;
  g_printCbUd = ud;
  g_printTickPtr = tickPtr;
}

void brainCoreRegisterPrintCapture(lua_State *L, BrainPrintCaptureFunc cb,
                                    void *ud, const uint32_t *tickPtr) {
  /* Allocate context as Lua userdata so it lives as long as the Lua state */
  PrintCaptureCtx *ctx = (PrintCaptureCtx *)lua_newuserdata(L, sizeof(PrintCaptureCtx));
  ctx->cb = cb;
  ctx->ud = ud;
  ctx->tickPtr = tickPtr;
  lua_pushcclosure(L, l_captured_print, 1);
  lua_setglobal(L, "print");
}

/* ── braintest_viz_register binding + callback hook ─────────────────
 * Brains call `braintest_viz_register(id, label, short, long, default)`
 * to surface their viz_ids in BrainTest's V dialog. The host
 * (BrainTest) sets a callback that actually populates the registry;
 * other hosts (WinBolo client, headless server) leave the callback
 * NULL and the binding silently returns -1, which the Lua wrapper
 * treats as "not running under BrainTest, fine, do nothing". */
static BrainVizRegisterFunc g_vizRegisterCb = NULL;

void brainCoreSetVizRegisterCallback(BrainVizRegisterFunc cb) {
  g_vizRegisterCb = cb;
}

static int l_braintest_viz_register(lua_State *L) {
  const char *id         = luaL_checkstring(L, 1);
  const char *label      = luaL_optstring(L, 2, id);
  const char *short_desc = luaL_optstring(L, 3, "");
  const char *long_desc  = luaL_optstring(L, 4, "");
  int default_on         = lua_toboolean(L, 5);
  /* When the 5th arg isn't supplied, lua_toboolean returns 0 (off).
   * Treat "missing" as "default to ON" — most viz_ids start visible.
   * Detect via lua_isnoneornil. */
  if (lua_isnoneornil(L, 5)) default_on = 1;
  int idx = -1;
  if (g_vizRegisterCb) {
    idx = g_vizRegisterCb(id, label, short_desc, long_desc, default_on);
  }
  lua_pushinteger(L, idx);
  return 1;
}

void brainCoreRegisterVizRegister(lua_State *L) {
  lua_pushcfunction(L, l_braintest_viz_register);
  lua_setglobal(L, "braintest_viz_register");
}

/* ── braintest_panel_register host hook (parallel of viz register) ── */
static BrainPanelRegisterFunc g_panelRegisterCb = NULL;

void brainCoreSetPanelRegisterCallback(BrainPanelRegisterFunc cb) {
  g_panelRegisterCb = cb;
}

static int l_braintest_panel_register(lua_State *L) {
  /* Signature: braintest_panel_register(name, type, lua_expr [, opts]).
   *   `type`     — optional ("text" if nil); namespaced by host
   *   `lua_expr` — required; Lua chunk that returns the body string
   *   `opts`     — optional table; recognized keys:
   *                  shortcut = "T"   → panel gets its own SDL window
   *                                      toggled by this key. Empty /
   *                                      missing → tab in the P window.
   *
   * Two-arg form (name, lua_expr) still supported for older brains:
   * type defaults to "text", no opts. */
  const char *name = luaL_checkstring(L, 1);
  const char *type = NULL;
  const char *lua_expr = NULL;
  const char *shortcut = NULL;
  int top = lua_gettop(L);
  if (top >= 3) {
    if (!lua_isnoneornil(L, 2)) type = luaL_checkstring(L, 2);
    lua_expr = luaL_checkstring(L, 3);
    if (top >= 4 && lua_istable(L, 4)) {
      lua_getfield(L, 4, "shortcut");
      if (lua_isstring(L, -1)) shortcut = lua_tostring(L, -1);
      lua_pop(L, 1);
    }
  } else {
    lua_expr = luaL_checkstring(L, 2);
  }
  int idx = -1;
  if (g_panelRegisterCb) {
    idx = g_panelRegisterCb(name, type, lua_expr, shortcut);
  }
  lua_pushinteger(L, idx);
  return 1;
}

void brainCoreRegisterPanelRegister(lua_State *L) {
  lua_pushcfunction(L, l_braintest_panel_register);
  lua_setglobal(L, "braintest_panel_register");
}

/* ── braintest_shotsim_poi_register host hook ─────────────────────── */
static BrainShotSimPoiRegisterFunc g_shotSimPoiRegisterCb = NULL;

void brainCoreSetShotSimPoiRegisterCallback(BrainShotSimPoiRegisterFunc cb) {
  g_shotSimPoiRegisterCb = cb;
}

static int l_braintest_shotsim_poi_register(lua_State *L) {
  /* Signature: braintest_shotsim_poi_register(name, lua_expr).
   *   lua_expr — Lua chunk that returns (wx, wy) or nil.
   * Returns the slot index, or -1 if the host isn't listening. */
  const char *name     = luaL_checkstring(L, 1);
  const char *lua_expr = luaL_checkstring(L, 2);
  int idx = -1;
  if (g_shotSimPoiRegisterCb) {
    idx = g_shotSimPoiRegisterCb(name, lua_expr);
  }
  lua_pushinteger(L, idx);
  return 1;
}

void brainCoreRegisterShotSimPoiRegister(lua_State *L) {
  lua_pushcfunction(L, l_braintest_shotsim_poi_register);
  lua_setglobal(L, "braintest_shotsim_poi_register");
}
