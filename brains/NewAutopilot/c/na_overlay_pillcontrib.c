/*********************************************************
 * na_overlay_pillcontrib.c
 *
 * See na_overlay_pillcontrib.h for the rationale: NewAutopilot's
 * BrainTest overlay should not pollute the engine's generic
 * brain runtime, so its Lua bindings live in the bot's own C
 * directory.
 *
 * Each bot has its own Lua state with these bindings registered.
 * The bot's player_num is captured as a Lua upvalue at register
 * time so the bindings know which slot range to write into —
 * there is no inter-bot race because every bot only touches its
 * own range.
 *********************************************************/

#include "na_overlay_pillcontrib.h"
#include <lauxlib.h>

static NaPillContribBeginPillFunc  g_beginPillCb = NULL;
static NaPillContribAddTileFunc    g_addTileCb   = NULL;

void naPillContribSetBeginPillCallback(NaPillContribBeginPillFunc cb) { g_beginPillCb = cb; }
void naPillContribSetAddTileCallback(NaPillContribAddTileFunc cb)     { g_addTileCb   = cb; }

/* pillcontrib_begin_pill(pill_id, mx, my) -> slot (opaque; -1 on
 * overflow). The bot's player_num is the closure's upvalue. */
static int l_pillcontrib_begin_pill(lua_State *L) {
  int bot     = (int)lua_tointeger(L, lua_upvalueindex(1));
  int pill_id = (int)luaL_checkinteger(L, 1);
  int mx      = (int)luaL_checkinteger(L, 2);
  int my      = (int)luaL_checkinteger(L, 3);
  int slot = -1;
  if (g_beginPillCb) {
    slot = g_beginPillCb(bot, pill_id, mx, my);
  }
  lua_pushinteger(L, slot);
  return 1;
}

/* pillcontrib_add_tile(slot, tile_x, tile_y, value).  Slot is
 * the opaque encoded handle returned by begin_pill — bot index
 * lives inside it, so this binding doesn't need its own upvalue. */
static int l_pillcontrib_add_tile(lua_State *L) {
  int   slot  = (int)luaL_checkinteger(L, 1);
  int   tx    = (int)luaL_checkinteger(L, 2);
  int   ty    = (int)luaL_checkinteger(L, 3);
  float value = (float)luaL_checknumber(L, 4);
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

void naPillContribRegister(lua_State *L, int player_num) {
  /* begin_pill carries player_num as an upvalue. add_tile/add_all
   * don't need it — the bot identity is baked into the slot they
   * receive from begin_pill. Register each closure separately
   * because lua_pushcclosure consumes the upvalues. */
  lua_pushinteger(L, player_num);
  lua_pushcclosure(L, l_pillcontrib_begin_pill, 1);
  lua_setglobal(L, "pillcontrib_begin_pill");

  lua_pushcfunction(L, l_pillcontrib_add_tile);
  lua_setglobal(L, "pillcontrib_add_tile");

  lua_pushcfunction(L, l_pillcontrib_add_all);
  lua_setglobal(L, "pillcontrib_add_all");
}
