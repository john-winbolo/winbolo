/*********************************************************
 * na_overlay_pillcontrib.c — moved from src/bolo/braincore.c
 *
 * See na_overlay_pillcontrib.h for the rationale: NewAutopilot's
 * BrainTest overlay should not pollute the engine's generic
 * brain runtime, so its Lua bindings live in the bot's own C
 * directory.
 *********************************************************/

#include "na_overlay_pillcontrib.h"
#include "../../../src/bolo/bot_manager.h"
#include <lauxlib.h>

static NaPillContribClearFunc      g_clearCb     = NULL;
static NaPillContribBeginPillFunc  g_beginPillCb = NULL;
static NaPillContribAddTileFunc    g_addTileCb   = NULL;

void naPillContribSetClearCallback(NaPillContribClearFunc cb)         { g_clearCb     = cb; }
void naPillContribSetBeginPillCallback(NaPillContribBeginPillFunc cb) { g_beginPillCb = cb; }
void naPillContribSetAddTileCallback(NaPillContribAddTileFunc cb)     { g_addTileCb   = cb; }

/* pillcontrib_clear() — reset the per-tick registry. Brains call
 * once before pushing this tick's pills.
 *
 * The pillcontrib overlay is single-bot-only. The BrainTest registry
 * the callbacks write into is a process-wide structure with no
 * locking, so concurrent writes from multiple brains corrupt it.
 * When more than one bot is active we no-op all four bindings; the
 * overlay simply goes blank in multi-brain BrainTest runs. Mutexing
 * (or rebuilding as per-brain layered overlays) is deferred until
 * someone wants multi-brain viz. */
static int l_pillcontrib_clear(lua_State *L) {
  (void)L;
  if (botManagerGetActiveBotCount() > 1) return 0;
  if (g_clearCb) g_clearCb();
  return 0;
}

/* pillcontrib_begin_pill(pill_id, mx, my) -> slot (or -1 on overflow) */
static int l_pillcontrib_begin_pill(lua_State *L) {
  int pill_id = (int)luaL_checkinteger(L, 1);
  int mx      = (int)luaL_checkinteger(L, 2);
  int my      = (int)luaL_checkinteger(L, 3);
  if (botManagerGetActiveBotCount() > 1) {
    lua_pushinteger(L, -1);
    return 1;
  }
  int slot = -1;
  if (g_beginPillCb) {
    slot = g_beginPillCb(pill_id, mx, my);
  }
  lua_pushinteger(L, slot);
  return 1;
}

/* pillcontrib_add_tile(slot, tile_x, tile_y, value) */
static int l_pillcontrib_add_tile(lua_State *L) {
  int   slot  = (int)luaL_checkinteger(L, 1);
  int   tx    = (int)luaL_checkinteger(L, 2);
  int   ty    = (int)luaL_checkinteger(L, 3);
  float value = (float)luaL_checknumber(L, 4);
  if (botManagerGetActiveBotCount() > 1) return 0;
  if (g_addTileCb) {
    g_addTileCb(slot, tx, ty, value);
  }
  return 0;
}

/* pillcontrib_add_all(slot, contrib_table)
 * Batch variant: iterates the contrib table (tkey->val map where
 * tkey = ty*256+tx) in C, calling g_addTileCb for each entry
 * with val > 0.  Replaces a Lua loop of pillcontrib_add_tile calls,
 * reducing Lua→C boundary crossings from N_tiles to 1. */
static int l_pillcontrib_add_all(lua_State *L) {
  int slot = (int)luaL_checkinteger(L, 1);
  if (botManagerGetActiveBotCount() > 1) return 0;
  if (!g_addTileCb || !lua_istable(L, 2)) return 0;
  lua_pushnil(L);
  while (lua_next(L, 2) != 0) {
    float val = (float)lua_tonumber(L, -1);
    if (val > 0.0f) {
      lua_Integer tkey = lua_tointeger(L, -2);
      int tx = (int)(tkey % 256);
      int ty = (int)(tkey / 256);
      g_addTileCb(slot, tx, ty, val);
    }
    lua_pop(L, 1);
  }
  return 0;
}

void naPillContribRegister(lua_State *L) {
  lua_pushcfunction(L, l_pillcontrib_clear);      lua_setglobal(L, "pillcontrib_clear");
  lua_pushcfunction(L, l_pillcontrib_begin_pill); lua_setglobal(L, "pillcontrib_begin_pill");
  lua_pushcfunction(L, l_pillcontrib_add_tile);   lua_setglobal(L, "pillcontrib_add_tile");
  lua_pushcfunction(L, l_pillcontrib_add_all);    lua_setglobal(L, "pillcontrib_add_all");
}
