/*********************************************************
 * na_threat.c — NewAutopilot threat-grid C kernel
 *
 * See na_threat.h for the why. Implementation notes:
 *
 * - The two terrain caches (terrain_mult, terrain_in_trees) are
 *   plain flat arrays sized to MAP_W*MAP_W = 65536. Tile (x,y)
 *   lives at index y*MAP_W + x.
 *
 * - The disk geometry (DISK_DX/DY/OFF), proximity cache, and full-
 *   hide cache are built once on first naThreatConfigure call.
 *   The disk radius (PILL_RANGE_MAP) and proximity falloff are
 *   passed in from Lua, so cloners tweaking constants.lua get the
 *   same disk shape on the C side without recompiling.
 *
 * - Raw terrain reads use brainCoreGetWorldPtrPtr (engine helper)
 *   to dereference the host's authoritative byte map. The pointer
 *   is cached lazily on first rebuild and refreshed if the host
 *   swaps maps between bot lifetimes.
 *
 * - HAZARD_TERRAIN is stored as a 16-entry boolean table (terrain
 *   types are 4 bits) so the cardinal-neighbor hazard check is a
 *   handful of array reads.
 *********************************************************/

#include "na_threat.h"
#include "../../../src/bolo/braincore.h"
#include <lauxlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>

/* ── Map dimensions (matches MAP_W in constants.lua) ──────────────── */
#define MAP_W      256
#define MAP_TILES  (MAP_W * MAP_W)
#define TERRAIN_MASK 0x0F  /* low 4 bits of the engine's BYTE map = terrain type */

/* ── Disk geometry size cap.
 * For radius R, disk has up to (2R+1)^2 tiles. R=10 → 441; the
 * actual in-disk count is ~314. We cap R at 15 (961 tiles) which
 * is plenty of headroom — current PILL_RANGE_MAP is 9. */
#define MAX_DISK_R   15
#define MAX_DISK_LEN ((2*MAX_DISK_R + 1) * (2*MAX_DISK_R + 1))

/* ── Module state ─────────────────────────────────────────────────── */
static float   g_terrain_mult[MAP_TILES];
static uint8_t g_terrain_in_trees[MAP_TILES];

/* Tunables — set by naThreatConfigure */
static int   g_pill_range_map         = 9;
static int   g_min_treehide_dist      = 3;
static float g_pill_danger_edge_falloff = 0.5f;
static float g_pill_danger_base       = 8.0f;
static float g_pill_danger_anger      = 200.0f;
static float g_low_hp1_mult           = 0.8f;
static float g_low_hp2_mult           = 0.9f;
static float g_tree_full_hide_mult    = 0.1f;
static float g_tree_partial_hide_mult = 0.7f;
static float g_forest_terrain_mult    = 1.5f;
static float g_hazard_neighbor_mult   = 1.5f;

/* Terrain-type values (constants.lua). Set via naThreatConfigure. */
static int g_t_building  = 0;
static int g_t_river     = 1;
static int g_t_swamp     = 2;
static int g_t_forest    = 5;
static int g_t_rubble    = 6;
static int g_t_halfbuild = 8;
static int g_t_deepsea   = 10;

/* TERRAIN_SPEED[tt] table — one entry per of the 16 possible terrain types. */
static int g_terrain_speed[16];

/* HAZARD_TERRAIN bitset — 1 if this terrain type triggers the cardinal-
 * neighbor hazard multiplier (river / deepsea / rubble / swamp). */
static uint8_t g_hazard_terrain[16];

/* Disk geometry (built once per Configure). */
static int   g_disk_dx[MAX_DISK_LEN];
static int   g_disk_dy[MAX_DISK_LEN];
static int   g_disk_off[MAX_DISK_LEN];
static int   g_disk_len = 0;

/* Per-offset proximity falloff and "is far enough for full tree hide" lookup.
 * Indexed by (dy + R) * (2R+1) + (dx + R). Not all entries are used
 * (only those inside the disk) — out-of-disk slots stay zero. */
static float   g_prox_cache[MAX_DISK_LEN];
static uint8_t g_fullhide_cache[MAX_DISK_LEN];

static int g_disk_built = 0;
static int g_terrain_built = 0;

/* Cached host worldPtr-pointer (lazy-fetched on first read). */
static const unsigned char **g_world_ptr_ptr = NULL;

/* ── Helpers ──────────────────────────────────────────────────────── */
static inline int in_map(int x, int y) {
    return (unsigned)x < MAP_W && (unsigned)y < MAP_W;
}

static inline int raw_tt(int x, int y) {
    if (!g_world_ptr_ptr || !*g_world_ptr_ptr) return 0;
    return (*g_world_ptr_ptr)[y * MAP_W + x] & TERRAIN_MASK;
}

static inline int tile_in_trees_c(int mx, int my) {
    if (raw_tt(mx, my) != g_t_forest) return 0;
    if (mx <= 0       || raw_tt(mx - 1, my) != g_t_forest) return 0;
    if (mx >= MAP_W-1 || raw_tt(mx + 1, my) != g_t_forest) return 0;
    if (my <= 0       || raw_tt(mx, my - 1) != g_t_forest) return 0;
    if (my >= MAP_W-1 || raw_tt(mx, my + 1) != g_t_forest) return 0;
    return 1;
}

static void compute_terrain_factor_at_c(int mx, int my) {
    int tt = raw_tt(mx, my);
    int spd = g_terrain_speed[tt & 0x0F];
    if (spd <= 0) spd = 3; /* fallback matches Lua */
    float m = 16.0f / (float)spd;
    if (tt == g_t_forest) m = g_forest_terrain_mult;
    /* Hazard-neighbor check: any cardinal neighbor in the hazard set */
    int hazard = 0;
    if (mx > 0       && g_hazard_terrain[raw_tt(mx - 1, my) & 0x0F]) hazard = 1;
    if (!hazard && mx < MAP_W-1 && g_hazard_terrain[raw_tt(mx + 1, my) & 0x0F]) hazard = 1;
    if (!hazard && my > 0       && g_hazard_terrain[raw_tt(mx, my - 1) & 0x0F]) hazard = 1;
    if (!hazard && my < MAP_W-1 && g_hazard_terrain[raw_tt(mx, my + 1) & 0x0F]) hazard = 1;
    if (hazard) m *= g_hazard_neighbor_mult;

    int k = my * MAP_W + mx;
    g_terrain_mult[k]     = m;
    g_terrain_in_trees[k] = (uint8_t)tile_in_trees_c(mx, my);
}

static void build_disk(void) {
    int R = g_pill_range_map;
    if (R < 0) R = 0;
    if (R > MAX_DISK_R) R = MAX_DISK_R;
    int size = 2 * R + 1;
    int R2 = R * R;
    float inv_R = (R > 0) ? 1.0f / (float)R : 0.0f;
    float edge_fall = g_pill_danger_edge_falloff;
    int min_th = g_min_treehide_dist;

    /* Reset proximity / fullhide caches across the full bounding box.
     * Only in-disk slots get written; out-of-disk slots are unused but
     * stay deterministic. */
    int max_cells = size * size;
    if (max_cells > MAX_DISK_LEN) max_cells = MAX_DISK_LEN;
    for (int i = 0; i < max_cells; i++) {
        g_prox_cache[i]     = 0.0f;
        g_fullhide_cache[i] = 0;
    }

    g_disk_len = 0;
    for (int dy = -R; dy <= R; dy++) {
        for (int dx = -R; dx <= R; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > R2) continue;
            int idx = (dy + R) * size + (dx + R);
            float d = sqrtf((float)d2);
            g_prox_cache[idx]     = 1.0f - edge_fall * (d * inv_R);
            g_fullhide_cache[idx] = (d >= (float)min_th) ? 1 : 0;
            int slot = g_disk_len++;
            g_disk_dx[slot]  = dx;
            g_disk_dy[slot]  = dy;
            g_disk_off[slot] = idx;
        }
    }
    g_disk_built = 1;
}

/* ── Lua bindings ────────────────────────────────────────────────── */

/* na_threat.configure({...}) — accepts a single table with named keys.
 * Sets every tunable; missing keys keep their default. Rebuilds the
 * disk geometry as a side effect (cheap). */
static int l_naThreatConfigure(lua_State *L) {
    if (!lua_istable(L, 1)) {
        return luaL_error(L, "na_threat.configure: table arg required");
    }

    #define READ_INT(key, dst)   lua_getfield(L, 1, key); \
        if (lua_isinteger(L, -1) || lua_isnumber(L, -1)) (dst) = (int)lua_tointeger(L, -1); \
        lua_pop(L, 1);
    #define READ_NUM(key, dst)   lua_getfield(L, 1, key); \
        if (lua_isnumber(L, -1)) (dst) = (float)lua_tonumber(L, -1); \
        lua_pop(L, 1);

    READ_INT("PILL_RANGE_MAP",         g_pill_range_map);
    READ_INT("MIN_TREEHIDE_DIST_MAP",  g_min_treehide_dist);
    READ_NUM("PILL_DANGER_EDGE_FALLOFF", g_pill_danger_edge_falloff);
    READ_NUM("PILL_DANGER_BASE",       g_pill_danger_base);
    READ_NUM("PILL_DANGER_ANGER",      g_pill_danger_anger);
    READ_NUM("LOW_HP1_MULT",           g_low_hp1_mult);
    READ_NUM("LOW_HP2_MULT",           g_low_hp2_mult);
    READ_NUM("TREE_FULL_HIDE_MULT",    g_tree_full_hide_mult);
    READ_NUM("TREE_PARTIAL_HIDE_MULT", g_tree_partial_hide_mult);
    READ_NUM("FOREST_TERRAIN_MULT",    g_forest_terrain_mult);
    READ_NUM("HAZARD_NEIGHBOR_MULT",   g_hazard_neighbor_mult);

    READ_INT("T_BUILDING",  g_t_building);
    READ_INT("T_RIVER",     g_t_river);
    READ_INT("T_SWAMP",     g_t_swamp);
    READ_INT("T_FOREST",    g_t_forest);
    READ_INT("T_RUBBLE",    g_t_rubble);
    READ_INT("T_HALFBUILD", g_t_halfbuild);
    READ_INT("T_DEEPSEA",   g_t_deepsea);

    /* TERRAIN_SPEED table — keyed by terrain type integer (0..15).
     * Defaults to 3 for entries Lua doesn't supply. */
    for (int i = 0; i < 16; i++) g_terrain_speed[i] = 3;
    lua_getfield(L, 1, "TERRAIN_SPEED");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            int tt = (int)luaL_optinteger(L, -2, -1);
            int sp = (int)luaL_optinteger(L, -1, 3);
            if (tt >= 0 && tt < 16) g_terrain_speed[tt] = sp;
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    /* HAZARD_TERRAIN table — keys are terrain types, value is true. */
    for (int i = 0; i < 16; i++) g_hazard_terrain[i] = 0;
    lua_getfield(L, 1, "HAZARD_TERRAIN");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            int tt = (int)luaL_optinteger(L, -2, -1);
            if (tt >= 0 && tt < 16) g_hazard_terrain[tt] = 1;
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    #undef READ_INT
    #undef READ_NUM

    build_disk();
    g_terrain_built = 0;  /* force rebuild on next call */
    return 0;
}

/* na_threat.terrain_rebuild() — full 65k-tile sweep. */
static int l_naThreatTerrainRebuild(lua_State *L) {
    if (!g_world_ptr_ptr) g_world_ptr_ptr = brainCoreGetWorldPtrPtr(L);
    if (!g_world_ptr_ptr || !*g_world_ptr_ptr) {
        return luaL_error(L, "na_threat.terrain_rebuild: world pointer not available");
    }
    for (int my = 0; my < MAP_W; my++) {
        for (int mx = 0; mx < MAP_W; mx++) {
            compute_terrain_factor_at_c(mx, my);
        }
    }
    g_terrain_built = 1;
    return 0;
}

/* na_threat.terrain_update_around(mx, my) — recompute the 5-tile
 * cross at (mx,my). Mirrors threat.lua's per-tile-change update. */
static int l_naThreatTerrainUpdateAround(lua_State *L) {
    int mx = (int)luaL_checkinteger(L, 1);
    int my = (int)luaL_checkinteger(L, 2);
    if (!g_world_ptr_ptr) g_world_ptr_ptr = brainCoreGetWorldPtrPtr(L);
    if (!g_world_ptr_ptr || !*g_world_ptr_ptr) return 0;
    if (in_map(mx, my))         compute_terrain_factor_at_c(mx, my);
    if (in_map(mx - 1, my))     compute_terrain_factor_at_c(mx - 1, my);
    if (in_map(mx + 1, my))     compute_terrain_factor_at_c(mx + 1, my);
    if (in_map(mx, my - 1))     compute_terrain_factor_at_c(mx, my - 1);
    if (in_map(mx, my + 1))     compute_terrain_factor_at_c(mx, my + 1);
    return 0;
}

/* na_threat.terrain_factor_at(mx, my) -> mult, in_trees
 * Mostly for tests / debug. The inner stamp loop does not call this. */
static int l_naThreatTerrainFactorAt(lua_State *L) {
    int mx = (int)luaL_checkinteger(L, 1);
    int my = (int)luaL_checkinteger(L, 2);
    if (!in_map(mx, my)) {
        lua_pushnumber(L, 1.0);
        lua_pushboolean(L, 0);
        return 2;
    }
    int k = my * MAP_W + mx;
    lua_pushnumber(L, g_terrain_mult[k]);
    lua_pushboolean(L, g_terrain_in_trees[k] ? 1 : 0);
    return 2;
}

/* na_threat.stamp_pill(pill_grid, coverage, px, py, anger, hp) -> contrib_table
 *
 * Stamps the pill at (px, py) into pill_grid (Lua table) and increments
 * coverage[k] (Lua table) for each disk tile. Returns a fresh contrib
 * table mapping tile_key -> penalty for this pill so the caller can
 * store it into M.pill_contrib[pill_pos_key]. Matches Lua stamp_pill's
 * semantics exactly except it skips occlusion (occlusion stays in Lua).
 */
static int l_naThreatStampPill(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);  /* pill_grid */
    luaL_checktype(L, 2, LUA_TTABLE);  /* coverage  */
    int px    = (int)luaL_checkinteger(L, 3);
    int py    = (int)luaL_checkinteger(L, 4);
    float anger = (float)luaL_checknumber(L, 5);
    int hp    = (int)luaL_checkinteger(L, 6);

    if (!g_disk_built) build_disk();

    float base = g_pill_danger_base + g_pill_danger_anger * anger;
    if      (hp == 1) base *= g_low_hp1_mult;
    else if (hp == 2) base *= g_low_hp2_mult;

    /* Fresh contrib table — sized hint = disk_len */
    lua_createtable(L, 0, g_disk_len);
    int contrib_idx = lua_gettop(L);

    /* Stack layout:
     *   1 = pill_grid
     *   2 = coverage
     *   contrib_idx (top) = contrib */
    int pg = 1;
    int cv = 2;

    for (int i = 0; i < g_disk_len; i++) {
        int dx = g_disk_dx[i];
        int dy = g_disk_dy[i];
        int nx = px + dx;
        int ny = py + dy;
        if (!in_map(nx, ny)) continue;
        int k = ny * MAP_W + nx;
        int off = g_disk_off[i];

        /* coverage[k] = (coverage[k] or 0) + 1 */
        lua_rawgeti(L, cv, k);
        int prev_cov = (int)lua_tointegerx(L, -1, NULL);
        lua_pop(L, 1);
        lua_pushinteger(L, prev_cov + 1);
        lua_rawseti(L, cv, k);

        float penalty = base * g_prox_cache[off];

        if (g_terrain_in_trees[k]) {
            penalty *= g_fullhide_cache[off]
                       ? g_tree_full_hide_mult
                       : g_tree_partial_hide_mult;
        }
        penalty *= g_terrain_mult[k];

        if (penalty > 0.0f) {
            /* pill_grid[k] = (pill_grid[k] or 0) + penalty */
            lua_rawgeti(L, pg, k);
            double prev_pg = lua_tonumberx(L, -1, NULL);
            lua_pop(L, 1);
            lua_pushnumber(L, prev_pg + (double)penalty);
            lua_rawseti(L, pg, k);

            /* contrib[k] = penalty */
            lua_pushnumber(L, (double)penalty);
            lua_rawseti(L, contrib_idx, k);
        }
    }
    /* contrib already on top — return it */
    return 1;
}

/* na_threat.is_terrain_built() -> bool — diagnostic. */
static int l_naThreatIsTerrainBuilt(lua_State *L) {
    lua_pushboolean(L, g_terrain_built);
    return 1;
}

/* ── Module registration ─────────────────────────────────────────── */

static const luaL_Reg na_threat_lib[] = {
    { "configure",            l_naThreatConfigure },
    { "terrain_rebuild",      l_naThreatTerrainRebuild },
    { "terrain_update_around", l_naThreatTerrainUpdateAround },
    { "terrain_factor_at",    l_naThreatTerrainFactorAt },
    { "stamp_pill",           l_naThreatStampPill },
    { "is_terrain_built",     l_naThreatIsTerrainBuilt },
    { NULL, NULL }
};

void naThreatRegister(lua_State *L) {
    luaL_newlib(L, na_threat_lib);
    lua_setglobal(L, "na_threat");
}
