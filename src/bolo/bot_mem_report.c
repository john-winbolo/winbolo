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

/* Memory figures for the dedicated server's memory report — see
 * bot_mem_report.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>

#include "global.h"
#include "shells.h"
#include "explosions.h"
#include "tankexp.h"
#include "minesexp.h"
#include "floodfill.h"
#include "building.h"
#include "rubble.h"
#include "swamp.h"
#include "grass.h"
#include "game_sim.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "bot_manager.h"
#include "server_sim_internal.h"
#include "bot_mem_report.h"

/* Every world list node starts with its `next` pointer and every list
 * ends on NULL (the heads start NULL; see shellsCreate, grassCreate), so
 * one walk counts them all. The cap stops a corrupt, looped list from
 * hanging the server inside a diagnostic. */
typedef struct BotMemNode {
  struct BotMemNode *next;
} BotMemNode;

#define BOT_MEM_LIST_CAP 10000000u

static uint32_t botMemCountList(const void *head) {
  const BotMemNode *n = (const BotMemNode *)head;
  uint32_t count = 0;
  while (n != NULL && count < BOT_MEM_LIST_CAP) {
    count++;
    n = n->next;
  }
  return count;
}

void botMemCountLists(const struct GameSim *gs, BotMemListCounts *out) {
  memset(out, 0, sizeof(*out));
  if (gs == NULL) {
    return;
  }
  out->shells          = botMemCountList(gs->shs);
  out->explosions      = botMemCountList(gs->expl);
  out->tankExplosions  = botMemCountList(gs->tankExplosions);
  out->minesExplosions = botMemCountList(gs->minesExplosions);
  out->floodFill       = botMemCountList(gs->ff);
  out->building        = botMemCountList(gs->blds);
  out->rubble          = botMemCountList(gs->rbl);
  out->swamp           = botMemCountList(gs->swp);
  out->grass           = botMemCountList(gs->grs);
}

void botMemAddLists(BotMemListCounts *a, const BotMemListCounts *b) {
  a->shells          += b->shells;
  a->explosions      += b->explosions;
  a->tankExplosions  += b->tankExplosions;
  a->minesExplosions += b->minesExplosions;
  a->floodFill       += b->floodFill;
  a->building        += b->building;
  a->rubble          += b->rubble;
  a->swamp           += b->swamp;
  a->grass           += b->grass;
}

/* The bot's lua_State, or NULL when the seat has no running brain. Only
 * active bots: a parked runner is not ticked, so it cannot be growing,
 * and its heap is not what a reader of the report is asking about. */
static lua_State *botMemState(struct ServerSim *sim, BYTE playerNum) {
  BotContext *bot;
  if (sim == NULL || playerNum >= MAX_TANKS) {
    return NULL;
  }
  bot = &sim->botMgr.bots[playerNum];
  if (!bot->active || !bot->brain.running) {
    return NULL;
  }
  return bot->brain.L;
}

bool botMemLuaHeapBytes(struct ServerSim *sim, BYTE playerNum,
                        size_t *outBytes) {
  lua_State *L = botMemState(sim, playerNum);
  if (L == NULL) {
    return false;
  }
  *outBytes = (size_t)lua_gc(L, LUA_GCCOUNT, 0) * 1024u +
              (size_t)lua_gc(L, LUA_GCCOUNTB, 0);
  return true;
}

int botMemSumBotClientLists(struct ServerSim *sim, BotMemListCounts *out) {
  int n = 0;
  memset(out, 0, sizeof(*out));
  if (sim == NULL) {
    return 0;
  }
  for (int i = 0; i < MAX_TANKS; i++) {
    BotContext *bot = &sim->botMgr.bots[i];
    BotMemListCounts one;
    if (!bot->active || bot->cs == NULL) {
      continue;
    }
    botMemCountLists(clientSimGetGameSim(bot->cs), &one);
    botMemAddLists(out, &one);
    n++;
  }
  return n;
}

/* ------------------------------------------------------------------ */
/* Deep walk                                                           */
/* ------------------------------------------------------------------ */

/* Breadth first, so every table is named by the shortest path that
 * reaches it: "loaded.goals.cache" rather than the chain of upvalues a
 * depth-first walk happens to go down first. The queue of objects lives
 * in a Lua table inside the bot's own state (it must hold references so
 * the objects stay put); the paths live in C. */

#define BOT_MEM_TOP_MAX   64
#define BOT_MEM_PATH_LEN  256
#define BOT_MEM_MAX_OBJS  4000000

#if LUA_VERSION_NUM >= 502
  #define BOT_MEM_LEN lua_rawlen
#else
  #define BOT_MEM_LEN lua_objlen
#endif

typedef struct {
  char     path[BOT_MEM_PATH_LEN];
  uint32_t entries;
  size_t   strBytes;
} BotMemTop;

typedef struct {
  lua_State *L;
  int        visited;   /* absolute stack index of the visited set */
  int        queue;     /* absolute stack index of the object queue */
  char     **paths;     /* paths[i] names queue entry i + 1 */
  int        count;     /* entries queued so far */
  int        cap;       /* room in paths */
  BotMemTop  top[BOT_MEM_TOP_MAX];
  int        topN;
  int        topCount;
  uint64_t   tables;
  uint64_t   entries;
  uint64_t   functions;
  size_t     strBytes;
} BotMemWalk;

static void botMemTopInsert(BotMemWalk *w, const char *path,
                            uint32_t entries, size_t strBytes) {
  int pos;
  if (w->topCount == w->topN &&
      entries <= w->top[w->topCount - 1].entries) {
    return;
  }
  pos = (w->topCount < w->topN) ? w->topCount : w->topN - 1;
  while (pos > 0 && w->top[pos - 1].entries < entries) {
    w->top[pos] = w->top[pos - 1];
    pos--;
  }
  snprintf(w->top[pos].path, sizeof(w->top[pos].path), "%s", path);
  w->top[pos].entries = entries;
  w->top[pos].strBytes = strBytes;
  if (w->topCount < w->topN) {
    w->topCount++;
  }
}

/* Queue the value on top of the stack (left in place) under `path`,
 * unless it holds no references or has been queued already. */
static void botMemEnqueue(BotMemWalk *w, const char *path) {
  lua_State *L = w->L;
  int vt = lua_type(L, -1);
  size_t len;

  if (vt != LUA_TTABLE && vt != LUA_TFUNCTION && vt != LUA_TUSERDATA) {
    return;
  }
  if (w->count >= BOT_MEM_MAX_OBJS) {
    return;
  }
  lua_pushvalue(L, -1);
  lua_rawget(L, w->visited);
  if (!lua_isnil(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  lua_pop(L, 1);
  if (w->count == w->cap) {
    int ncap = w->cap ? w->cap * 2 : 4096;
    char **np = (char **)realloc(w->paths, (size_t)ncap * sizeof(char *));
    if (np == NULL) {
      return;
    }
    w->paths = np;
    w->cap = ncap;
  }
  len = strlen(path);
  w->paths[w->count] = (char *)malloc(len + 1);
  if (w->paths[w->count] == NULL) {
    return;
  }
  memcpy(w->paths[w->count], path, len + 1);
  lua_pushvalue(L, -1);
  lua_pushboolean(L, 1);
  lua_rawset(L, w->visited);
  lua_pushvalue(L, -1);
  lua_rawseti(L, w->queue, w->count + 1);
  w->count++;
}

/* Append a key to a path. Reads the key at stack index `idx` without
 * converting it in place (lua_tostring on a number key would break the
 * lua_next walk it came from). */
static void botMemChildPath(lua_State *L, int idx, const char *parent,
                            char *out, size_t outSize) {
  switch (lua_type(L, idx)) {
  case LUA_TSTRING:
    snprintf(out, outSize, "%s.%s", parent, lua_tostring(L, idx));
    break;
  case LUA_TNUMBER:
    snprintf(out, outSize, "%s[%.14g]", parent, (double)lua_tonumber(L, idx));
    break;
  default:
    snprintf(out, outSize, "%s[<%s>]", parent, luaL_typename(L, idx));
    break;
  }
}

/* Count a table's entries and queue everything the object on top of the
 * stack refers to. */
static void botMemExpand(BotMemWalk *w, const char *path) {
  lua_State *L = w->L;
  int obj = lua_gettop(L);
  int vt = lua_type(L, obj);
  char child[BOT_MEM_PATH_LEN];

  if (vt == LUA_TTABLE) {
    uint32_t entries = 0;
    size_t strBytes = 0;
    w->tables++;
    lua_pushnil(L);
    while (lua_next(L, obj) != 0) {
      /* key at -2, value at -1 */
      entries++;
      if (lua_type(L, -1) == LUA_TSTRING) {
        strBytes += BOT_MEM_LEN(L, -1);
      }
      if (lua_type(L, -2) == LUA_TSTRING) {
        strBytes += BOT_MEM_LEN(L, -2);
      }
      botMemChildPath(L, -2, path, child, sizeof(child));
      botMemEnqueue(w, child);
      if (lua_type(L, -2) == LUA_TTABLE) {
        /* A table used as a key holds memory alive too. */
        lua_pushvalue(L, -2);
        snprintf(child, sizeof(child), "%s[<key>]", path);
        botMemEnqueue(w, child);
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
    w->entries += entries;
    w->strBytes += strBytes;
    botMemTopInsert(w, path, entries, strBytes);
  } else if (vt == LUA_TFUNCTION) {
    /* Module-local state lives in upvalues of the module's functions. */
    w->functions++;
    for (int i = 1; ; i++) {
      const char *name = lua_getupvalue(L, obj, i);
      if (name == NULL) {
        break;
      }
      snprintf(child, sizeof(child), "%s<%s>", path,
               (name[0] != '\0') ? name : "upv");
      botMemEnqueue(w, child);
      lua_pop(L, 1);
    }
  }
  if (vt != LUA_TFUNCTION && lua_getmetatable(L, obj)) {
    snprintf(child, sizeof(child), "%s<mt>", path);
    botMemEnqueue(w, child);
    lua_pop(L, 1);
  }
}

bool botMemLuaDeepWalk(struct ServerSim *sim, BYTE playerNum, int topN,
                       void (*emit)(void *ctx, const char *line),
                       void *ctx) {
  lua_State *L = botMemState(sim, playerNum);
  BotMemWalk *w;
  int top;
  size_t before, after;
  char line[BOT_MEM_PATH_LEN + 96];

  if (L == NULL || emit == NULL) {
    return false;
  }
  w = (BotMemWalk *)calloc(1, sizeof(BotMemWalk));
  if (w == NULL) {
    return false;
  }
  if (topN < 1) topN = 1;
  if (topN > BOT_MEM_TOP_MAX) topN = BOT_MEM_TOP_MAX;
  w->L = L;
  w->topN = topN;

  before = (size_t)lua_gc(L, LUA_GCCOUNT, 0) * 1024u +
           (size_t)lua_gc(L, LUA_GCCOUNTB, 0);
  top = lua_gettop(L);
  lua_checkstack(L, 16);
  lua_newtable(L);
  w->visited = lua_gettop(L);
  lua_newtable(L);
  w->queue = lua_gettop(L);
  /* The walk's own tables are not the brain's; keep them out. */
  lua_pushvalue(L, w->visited);
  lua_pushboolean(L, 1);
  lua_rawset(L, w->visited);
  lua_pushvalue(L, w->queue);
  lua_pushboolean(L, 1);
  lua_rawset(L, w->visited);

  /* Loaded modules first, so a module's tables are named after it, then
   * the globals, then whatever else the registry holds. */
  lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
  botMemEnqueue(w, "loaded");
  lua_pop(L, 1);
#if LUA_VERSION_NUM >= 502
  lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
#else
  lua_pushvalue(L, LUA_GLOBALSINDEX);
#endif
  botMemEnqueue(w, "_G");
  lua_pop(L, 1);
  lua_pushvalue(L, LUA_REGISTRYINDEX);
  botMemEnqueue(w, "REG");
  lua_pop(L, 1);

  for (int i = 0; i < w->count; i++) {
    lua_rawgeti(L, w->queue, i + 1);
    botMemExpand(w, w->paths[i]);
    lua_settop(L, w->queue);
    free(w->paths[i]);
    w->paths[i] = NULL;
  }
  lua_settop(L, top);

  after = (size_t)lua_gc(L, LUA_GCCOUNT, 0) * 1024u +
          (size_t)lua_gc(L, LUA_GCCOUNTB, 0);
  snprintf(line, sizeof(line),
           "[memdeep] bot p%u: heap %.1f KB, %llu tables, %llu functions, "
           "%llu table entries, %.1f KB of strings in tables "
           "(walk itself used %.1f KB, freed at next GC)",
           (unsigned)playerNum, before / 1024.0,
           (unsigned long long)w->tables, (unsigned long long)w->functions,
           (unsigned long long)w->entries, w->strBytes / 1024.0,
           (after > before) ? (after - before) / 1024.0 : 0.0);
  emit(ctx, line);
  for (int i = 0; i < w->topCount; i++) {
    snprintf(line, sizeof(line),
             "[memdeep] bot p%u: %8u entries %9.1f KB str  %s",
             (unsigned)playerNum, w->top[i].entries,
             w->top[i].strBytes / 1024.0, w->top[i].path);
    emit(ctx, line);
  }
  free(w->paths);
  free(w);
  return true;
}
