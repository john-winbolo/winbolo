/*********************************************************
 * na_threat.c — NewAutopilot threat-grid C kernel
 *
 * See na_threat.h for the why. Implementation notes:
 *
 * - The two terrain caches (terrain_mult, terrain_in_trees) are
 *   plain flat arrays sized to MAP_W*MAP_W = 65536. Tile (x,y)
 *   lives at index y*MAP_W + x.
 *
 * - Per-brain working state lives in NaThreatCtx, allocated as a
 *   Lua userdata in naThreatRegister and looked up via getCtx(L)
 *   at the top of each binding. Each lua_State owns its own ctx;
 *   lua_State close fires __gc on the userdata.
 *
 * - The disk geometry (disk_dx/dy/off), proximity cache, and full-
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
/* MAX_DISK_LEN also used as the occlusion DP bounding-box size. */

/* ── Per-brain working state ──────────────────────────────────────────
 *
 * One of these per lua_State, allocated as a userdata in
 * naThreatRegister and accessed via getCtx(L). The five map-sized
 * arrays dominate the ~1 MB footprint. */
typedef struct NaThreatCtx {
    /* Lifecycle flags */
    int cfg_done;
    int disk_built;
    int terrain_built;

    /* Tunables — set by configure() */
    int   pill_range_map;
    int   min_treehide_dist;
    float pill_danger_edge_falloff;
    float pill_danger_base;
    float pill_danger_anger;
    float low_hp1_mult;
    float low_hp2_mult;
    float tree_full_hide_mult;
    float tree_partial_hide_mult;
    float forest_terrain_mult;
    float hazard_neighbor_mult;

    /* Terrain-type values (constants.lua). Set via configure(). */
    int t_building;
    int t_river;
    int t_swamp;
    int t_forest;
    int t_rubble;
    int t_halfbuild;
    int t_deepsea;

    /* Disk geometry — rebuilt by build_disk on each configure(). */
    int   disk_dx[MAX_DISK_LEN];
    int   disk_dy[MAX_DISK_LEN];
    int   disk_off[MAX_DISK_LEN];
    int   disk_len;

    /* Per-offset proximity falloff and "is far enough for full tree hide"
     * lookup. Indexed by (dy + R) * (2R+1) + (dx + R). Out-of-disk slots
     * stay zero. */
    float   prox_cache[MAX_DISK_LEN];
    uint8_t fullhide_cache[MAX_DISK_LEN];

    /* Occlusion DP predecessor tables (built alongside disk geometry).
     *
     * Slot indexing is COLUMN-MAJOR: slot = (dx+R)*size + (dy+R), matching
     * threat.lua's PRED_OFF_DX/PRED_ORDER convention so values agree.
     *
     * pred_order[] traverses disk slots in Chebyshev-distance ascending
     * order so each tile's predecessor is filled before being read.
     * pred_off_dx/dy[] is the predecessor offset for each slot —
     * Bresenham line[#line-1] from (0,0) to the slot, matching Lua. */
    int16_t pred_order[MAX_DISK_LEN];
    int8_t  pred_off_dx[MAX_DISK_LEN];
    int8_t  pred_off_dy[MAX_DISK_LEN];
    int     pred_len;

    /* Occlusion accumulators reused across pills. The Chebyshev order
     * guarantees each slot is written before read as a predecessor, so
     * no clearing is needed between pills (stale data is overwritten
     * before it can be observed). */
    int16_t occ_walls [MAX_DISK_LEN];
    int16_t occ_trees [MAX_DISK_LEN];
    int16_t occ_fpills[MAX_DISK_LEN];

    /* Map-sized arrays. */
    float   terrain_mult[MAP_TILES];      /* 256 KB — per-tile terrain factor   */
    uint8_t terrain_in_trees[MAP_TILES];  /*  64 KB — forest bitmap             */
    uint8_t friendly_set[MAP_TILES];      /*  64 KB — occlusion working set     */
    float   pill_grid_c[MAP_TILES];       /* 256 KB — pill-danger accumulator   */
    float   cov_grid_c[MAP_TILES];        /* 256 KB — coverage accumulator      */

    /* Per-state pointer-to-pointer at the host's authoritative byte map.
     * Set in naThreatRegister; each brain owns its own worldPtr (the
     * host swaps in this brain's fog-of-war discovered map every tick). */
    const unsigned char **world_ptr_ptr;
} NaThreatCtx;

/* ── Process-wide read-only state ─────────────────────────────────── */

/* TERRAIN_SPEED[tt] table — one entry per of the 16 possible terrain types.
 * Written from each brain's configure(); identical across NewAutopilot
 * clones, so concurrent writes are race-but-equivalent. */
static int g_terrain_speed[16];

/* HAZARD_TERRAIN bitset — 1 if this terrain type triggers the cardinal-
 * neighbor hazard multiplier (river / deepsea / rubble / swamp). Same
 * race-but-equivalent rationale as g_terrain_speed. */
static uint8_t g_hazard_terrain[16];

/* ── Registry-backed ctx lookup ───────────────────────────────────── */

static int naThreatCtxGc(lua_State *L) {
    /* Userdata storage is freed by Lua. The hook is here so any future
     * ctx-owned sub-allocations have a place to be released. */
    (void)L;
    return 0;
}

static NaThreatCtx *getCtx(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "na_threat_ctx");
    NaThreatCtx *ctx = (NaThreatCtx *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!ctx) luaL_error(L, "na_threat: ctx not initialized");
    return ctx;
}

static NaThreatCtx *requireConfigured(lua_State *L) {
    NaThreatCtx *ctx = getCtx(L);
    if (!ctx->cfg_done) luaL_error(L, "na_threat: configure() must be called first");
    return ctx;
}

/* ── Helpers ──────────────────────────────────────────────────────── */
static inline int in_map(int x, int y) {
    return (unsigned)x < MAP_W && (unsigned)y < MAP_W;
}

static inline int raw_tt(const NaThreatCtx *ctx, int x, int y) {
    if (!ctx->world_ptr_ptr || !*ctx->world_ptr_ptr) return 0;
    return (*ctx->world_ptr_ptr)[y * MAP_W + x] & TERRAIN_MASK;
}

static inline int tile_in_trees_c(const NaThreatCtx *ctx, int mx, int my) {
    if (raw_tt(ctx, mx, my) != ctx->t_forest) return 0;
    if (mx <= 0       || raw_tt(ctx, mx - 1, my) != ctx->t_forest) return 0;
    if (mx >= MAP_W-1 || raw_tt(ctx, mx + 1, my) != ctx->t_forest) return 0;
    if (my <= 0       || raw_tt(ctx, mx, my - 1) != ctx->t_forest) return 0;
    if (my >= MAP_W-1 || raw_tt(ctx, mx, my + 1) != ctx->t_forest) return 0;
    return 1;
}

static void compute_terrain_factor_at_c(NaThreatCtx *ctx, int mx, int my) {
    int tt = raw_tt(ctx, mx, my);
    int spd = g_terrain_speed[tt & 0x0F];
    if (spd <= 0) spd = 3; /* fallback matches Lua */
    float m = 16.0f / (float)spd;
    if (tt == ctx->t_forest) m = ctx->forest_terrain_mult;
    /* Hazard-neighbor check: any cardinal neighbor in the hazard set */
    int hazard = 0;
    if (mx > 0       && g_hazard_terrain[raw_tt(ctx, mx - 1, my) & 0x0F]) hazard = 1;
    if (!hazard && mx < MAP_W-1 && g_hazard_terrain[raw_tt(ctx, mx + 1, my) & 0x0F]) hazard = 1;
    if (!hazard && my > 0       && g_hazard_terrain[raw_tt(ctx, mx, my - 1) & 0x0F]) hazard = 1;
    if (!hazard && my < MAP_W-1 && g_hazard_terrain[raw_tt(ctx, mx, my + 1) & 0x0F]) hazard = 1;
    if (hazard) m *= ctx->hazard_neighbor_mult;

    int k = my * MAP_W + mx;
    ctx->terrain_mult[k]     = m;
    ctx->terrain_in_trees[k] = (uint8_t)tile_in_trees_c(ctx, mx, my);
}

/* Runs Bresenham from (0,0) to (tx,ty), collecting intermediate points
 * (neither start nor end — matching U.bresenham's convention).
 * Returns the SECOND-TO-LAST intermediate point in *pdx / *pdy, which is
 * Lua's line[#line-1].  Falls back to (0,0) if fewer than 2 intermediates.
 * This exactly replicates the predecessor selection in threat.lua's PRED
 * table construction so occlusion values agree between Lua and C. */
static void bresenham_second_to_last(int tx, int ty, int *pdx, int *pdy) {
    int ax = tx < 0 ? -tx : tx;
    int ay = ty < 0 ? -ty : ty;
    int sx = tx > 0 ? 1 : (tx < 0 ? -1 : 0);
    int sy = ty > 0 ? 1 : (ty < 0 ? -1 : 0);
    int err = ax - ay;
    int cx = 0, cy = 0;
    int prev_x = 0, prev_y = 0;
    int pprev_x = 0, pprev_y = 0;
    int count = 0;
    for (;;) {
        if (cx == tx && cy == ty) break;
        int e2 = 2 * err;
        if (e2 > -ay) { err -= ay; cx += sx; }
        if (e2 <  ax) { err += ax; cy += sy; }
        if (cx == tx && cy == ty) break;  /* end not yielded */
        pprev_x = prev_x; pprev_y = prev_y;
        prev_x  = cx;     prev_y  = cy;
        count++;
    }
    if (count >= 2) { *pdx = pprev_x; *pdy = pprev_y; }
    else            { *pdx = 0;       *pdy = 0; }
}

static void build_disk(NaThreatCtx *ctx) {
    int R = ctx->pill_range_map;
    if (R < 0) R = 0;
    if (R > MAX_DISK_R) R = MAX_DISK_R;
    int size = 2 * R + 1;
    int R2 = R * R;
    float inv_R = (R > 0) ? 1.0f / (float)R : 0.0f;
    float edge_fall = ctx->pill_danger_edge_falloff;
    int min_th = ctx->min_treehide_dist;

    /* Reset proximity / fullhide caches across the full bounding box.
     * Only in-disk slots get written; out-of-disk slots are unused but
     * stay deterministic. */
    int max_cells = size * size;
    if (max_cells > MAX_DISK_LEN) max_cells = MAX_DISK_LEN;
    for (int i = 0; i < max_cells; i++) {
        ctx->prox_cache[i]     = 0.0f;
        ctx->fullhide_cache[i] = 0;
    }

    ctx->disk_len = 0;
    for (int dy = -R; dy <= R; dy++) {
        for (int dx = -R; dx <= R; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > R2) continue;
            int idx = (dy + R) * size + (dx + R);
            float d = sqrtf((float)d2);
            ctx->prox_cache[idx]     = 1.0f - edge_fall * (d * inv_R);
            ctx->fullhide_cache[idx] = (d >= (float)min_th) ? 1 : 0;
            int slot = ctx->disk_len++;
            ctx->disk_dx[slot]  = dx;
            ctx->disk_dy[slot]  = dy;
            ctx->disk_off[slot] = idx;
        }
    }

    /* Step 1: predecessor offsets for every in-disk slot. */
    for (int dx = -R; dx <= R; dx++) {
        for (int dy = -R; dy <= R; dy++) {
            if (dx * dx + dy * dy > R2) continue;
            int slot = (dx + R) * size + (dy + R);   /* column-major */
            int pdx, pdy;
            if (dx == 0 && dy == 0) { pdx = pdy = 0; }
            else { bresenham_second_to_last(dx, dy, &pdx, &pdy); }
            ctx->pred_off_dx[slot] = (int8_t)pdx;
            ctx->pred_off_dy[slot] = (int8_t)pdy;
        }
    }

    /* Step 2: Chebyshev-distance ascending traversal order. */
    ctx->pred_len = 0;
    for (int d = 0; d <= R; d++) {
        for (int dx = -R; dx <= R; dx++) {
            int adx = dx < 0 ? -dx : dx;
            for (int dy = -R; dy <= R; dy++) {
                int ady = dy < 0 ? -dy : dy;
                int cheby = adx > ady ? adx : ady;
                if (cheby == d && dx * dx + dy * dy <= R2) {
                    int slot = (dx + R) * size + (dy + R);
                    ctx->pred_order[ctx->pred_len++] = (int16_t)slot;
                }
            }
        }
    }

    ctx->disk_built = 1;
}

/* ── Lua bindings ────────────────────────────────────────────────── */

/* na_threat.configure({...}) — accepts a single table with named keys.
 * Sets every tunable; missing keys revert to their default. Rebuilds
 * the disk geometry as a side effect (cheap). May be called more than
 * once on the same lua_State (brain hot-reload reuses the state); a
 * second call resets all derived state from scratch. */
static int l_naThreatConfigure(lua_State *L) {
    NaThreatCtx *ctx = getCtx(L);
    if (!lua_istable(L, 1)) {
        return luaL_error(L, "na_threat.configure: table arg required");
    }

    /* Mark unconfigured for the duration of the rebuild so any concurrent
     * binding call hits requireConfigured() and errors cleanly. */
    ctx->cfg_done = 0;

    /* Defaults — applied unconditionally so a re-configure that omits
     * keys gets the same starting point as a first-configure. */
    ctx->pill_range_map           = 9;
    ctx->min_treehide_dist        = 3;
    ctx->pill_danger_edge_falloff = 0.5f;
    ctx->pill_danger_base         = 8.0f;
    ctx->pill_danger_anger        = 200.0f;
    ctx->low_hp1_mult             = 0.8f;
    ctx->low_hp2_mult             = 0.9f;
    ctx->tree_full_hide_mult      = 0.1f;
    ctx->tree_partial_hide_mult   = 0.7f;
    ctx->forest_terrain_mult      = 1.5f;
    ctx->hazard_neighbor_mult     = 1.5f;
    ctx->t_building  = 0;
    ctx->t_river     = 1;
    ctx->t_swamp     = 2;
    ctx->t_forest    = 5;
    ctx->t_rubble    = 6;
    ctx->t_halfbuild = 8;
    ctx->t_deepsea   = 10;

    #define READ_INT(key, dst)   lua_getfield(L, 1, key); \
        if (lua_isinteger(L, -1) || lua_isnumber(L, -1)) (dst) = (int)lua_tointeger(L, -1); \
        lua_pop(L, 1);
    #define READ_NUM(key, dst)   lua_getfield(L, 1, key); \
        if (lua_isnumber(L, -1)) (dst) = (float)lua_tonumber(L, -1); \
        lua_pop(L, 1);

    READ_INT("PILL_RANGE_MAP",           ctx->pill_range_map);
    READ_INT("MIN_TREEHIDE_DIST_MAP",    ctx->min_treehide_dist);
    READ_NUM("PILL_DANGER_EDGE_FALLOFF", ctx->pill_danger_edge_falloff);
    READ_NUM("PILL_DANGER_BASE",         ctx->pill_danger_base);
    READ_NUM("PILL_DANGER_ANGER",        ctx->pill_danger_anger);
    READ_NUM("LOW_HP1_MULT",             ctx->low_hp1_mult);
    READ_NUM("LOW_HP2_MULT",             ctx->low_hp2_mult);
    READ_NUM("TREE_FULL_HIDE_MULT",      ctx->tree_full_hide_mult);
    READ_NUM("TREE_PARTIAL_HIDE_MULT",   ctx->tree_partial_hide_mult);
    READ_NUM("FOREST_TERRAIN_MULT",      ctx->forest_terrain_mult);
    READ_NUM("HAZARD_NEIGHBOR_MULT",     ctx->hazard_neighbor_mult);

    READ_INT("T_BUILDING",  ctx->t_building);
    READ_INT("T_RIVER",     ctx->t_river);
    READ_INT("T_SWAMP",     ctx->t_swamp);
    READ_INT("T_FOREST",    ctx->t_forest);
    READ_INT("T_RUBBLE",    ctx->t_rubble);
    READ_INT("T_HALFBUILD", ctx->t_halfbuild);
    READ_INT("T_DEEPSEA",   ctx->t_deepsea);

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

    /* Reset all derived per-brain state. Required for re-configure to
     * reach a clean slate; on first configure the userdata is already
     * memset to zero so these are no-ops. */
    ctx->terrain_built = 0;
    memset(ctx->terrain_mult,     0, sizeof(ctx->terrain_mult));
    memset(ctx->terrain_in_trees, 0, sizeof(ctx->terrain_in_trees));
    memset(ctx->friendly_set,     0, sizeof(ctx->friendly_set));
    memset(ctx->pill_grid_c,      0, sizeof(ctx->pill_grid_c));
    memset(ctx->cov_grid_c,       0, sizeof(ctx->cov_grid_c));
    memset(ctx->occ_walls,        0, sizeof(ctx->occ_walls));
    memset(ctx->occ_trees,        0, sizeof(ctx->occ_trees));
    memset(ctx->occ_fpills,       0, sizeof(ctx->occ_fpills));

    build_disk(ctx);  /* sets disk_built and rebuilds disk_* / prox/pred tables */

    ctx->cfg_done = 1;
    return 0;
}

/* na_threat.terrain_rebuild() — full 65k-tile sweep. */
static int l_naThreatTerrainRebuild(lua_State *L) {
    NaThreatCtx *ctx = requireConfigured(L);
    if (!ctx->world_ptr_ptr || !*ctx->world_ptr_ptr) {
        return luaL_error(L, "na_threat.terrain_rebuild: world pointer not available");
    }
    for (int my = 0; my < MAP_W; my++) {
        for (int mx = 0; mx < MAP_W; mx++) {
            compute_terrain_factor_at_c(ctx, mx, my);
        }
    }
    ctx->terrain_built = 1;
    return 0;
}

/* na_threat.terrain_update_around(mx, my) — recompute the 5-tile
 * cross at (mx,my). Mirrors threat.lua's per-tile-change update. */
static int l_naThreatTerrainUpdateAround(lua_State *L) {
    NaThreatCtx *ctx = requireConfigured(L);
    int mx = (int)luaL_checkinteger(L, 1);
    int my = (int)luaL_checkinteger(L, 2);
    if (!ctx->world_ptr_ptr || !*ctx->world_ptr_ptr) return 0;
    if (in_map(mx, my))         compute_terrain_factor_at_c(ctx, mx, my);
    if (in_map(mx - 1, my))     compute_terrain_factor_at_c(ctx, mx - 1, my);
    if (in_map(mx + 1, my))     compute_terrain_factor_at_c(ctx, mx + 1, my);
    if (in_map(mx, my - 1))     compute_terrain_factor_at_c(ctx, mx, my - 1);
    if (in_map(mx, my + 1))     compute_terrain_factor_at_c(ctx, mx, my + 1);
    return 0;
}

/* na_threat.terrain_factor_at(mx, my) -> mult, in_trees
 * Mostly for tests / debug. The inner stamp loop does not call this. */
static int l_naThreatTerrainFactorAt(lua_State *L) {
    NaThreatCtx *ctx = requireConfigured(L);
    int mx = (int)luaL_checkinteger(L, 1);
    int my = (int)luaL_checkinteger(L, 2);
    if (!in_map(mx, my)) {
        lua_pushnumber(L, 1.0);
        lua_pushboolean(L, 0);
        return 2;
    }
    int k = my * MAP_W + mx;
    lua_pushnumber(L, ctx->terrain_mult[k]);
    lua_pushboolean(L, ctx->terrain_in_trees[k] ? 1 : 0);
    return 2;
}

/* na_threat.pill_rebuild_begin()
 * Zero pill_grid_c and cov_grid_c before a pill rebuild. Called from
 * threat.lua instead of the Lua clear loop so no Lua table entries are
 * set to nil — eliminating the GC pressure from that pass. */
static int l_naThreatPillRebuildBegin(lua_State *L) {
    NaThreatCtx *ctx = requireConfigured(L);
    memset(ctx->pill_grid_c, 0, sizeof(ctx->pill_grid_c));
    memset(ctx->cov_grid_c,  0, sizeof(ctx->cov_grid_c));
    return 0;
}

/* na_threat.stamp_pill(px, py, anger, hp) -> contrib_table
 *
 * Stamps the pill at (px, py) directly into the C-side pill_grid_c and
 * cov_grid_c flat arrays (no Lua table I/O for pill_grid or coverage).
 * Returns a fresh contrib table (tile_key -> penalty) for M.pill_contrib.
 * Call na_threat.pill_rebuild_begin() before the first stamp each rebuild. */
static int l_naThreatStampPill(lua_State *L) {
    NaThreatCtx *ctx = requireConfigured(L);
    int px    = (int)luaL_checkinteger(L, 1);
    int py    = (int)luaL_checkinteger(L, 2);
    float anger = (float)luaL_checknumber(L, 3);
    int hp    = (int)luaL_checkinteger(L, 4);

    if (!ctx->disk_built) build_disk(ctx);

    float base = ctx->pill_danger_base + ctx->pill_danger_anger * anger;
    if      (hp == 1) base *= ctx->low_hp1_mult;
    else if (hp == 2) base *= ctx->low_hp2_mult;

    /* Fresh contrib table — sized hint = disk_len */
    lua_createtable(L, 0, ctx->disk_len);
    int contrib_idx = lua_gettop(L);

    for (int i = 0; i < ctx->disk_len; i++) {
        int dx = ctx->disk_dx[i];
        int dy = ctx->disk_dy[i];
        int nx = px + dx;
        int ny = py + dy;
        if (!in_map(nx, ny)) continue;
        int k = ny * MAP_W + nx;
        int off = ctx->disk_off[i];

        /* coverage: direct C write — no Lua API call */
        ctx->cov_grid_c[k] += 1.0f;

        float penalty = base * ctx->prox_cache[off];

        if (ctx->terrain_in_trees[k]) {
            penalty *= ctx->fullhide_cache[off]
                       ? ctx->tree_full_hide_mult
                       : ctx->tree_partial_hide_mult;
        }
        penalty *= ctx->terrain_mult[k];

        if (penalty > 0.0f) {
            /* pill_grid: direct C write — no Lua API call */
            ctx->pill_grid_c[k] += penalty;

            /* contrib[k] = penalty — still a Lua table for pill_contrib */
            lua_pushnumber(L, (double)penalty);
            lua_rawseti(L, contrib_idx, k);
        }
    }
    /* contrib already on top — return it */
    return 1;
}

/* na_threat.is_terrain_built() -> bool — diagnostic. */
static int l_naThreatIsTerrainBuilt(lua_State *L) {
    NaThreatCtx *ctx = requireConfigured(L);
    lua_pushboolean(L, ctx->terrain_built);
    return 1;
}

/* na_threat.apply_occlusion_all(pill_contrib,
 *                                fp_mx, fp_my, fp_n,
 *                                hp_mx, hp_my, hp_n)
 *
 * C replacement for threat.lua's per-pill apply_occlusion_to_pill loop.
 * Reads and writes pill_grid_c directly (no Lua table I/O for pill_grid).
 * Still updates pill_contrib Lua sub-tables so attack-side subtraction works.
 *
 * pill_rebuild_begin() + stamp_pill() must be called before this. */
static int l_naThreatApplyOcclusionAll(lua_State *L) {
    NaThreatCtx *ctx = requireConfigured(L);
    luaL_checktype(L, 1, LUA_TTABLE);  /* pill_contrib */
    luaL_checktype(L, 2, LUA_TTABLE);  /* fp_mx       */
    luaL_checktype(L, 3, LUA_TTABLE);  /* fp_my       */
    int fp_n = (int)luaL_checkinteger(L, 4);
    luaL_checktype(L, 5, LUA_TTABLE);  /* hp_mx       */
    luaL_checktype(L, 6, LUA_TTABLE);  /* hp_my       */
    int hp_n = (int)luaL_checkinteger(L, 7);

    if (!ctx->disk_built) build_disk(ctx);

    int R    = ctx->pill_range_map;
    int size = 2 * R + 1;

    /* Populate friendly-pill flat set from fp_mx/fp_my arrays. */
    for (int i = 1; i <= fp_n; i++) {
        lua_rawgeti(L, 2, i); int fx = (int)lua_tointeger(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, 3, i); int fy = (int)lua_tointeger(L, -1); lua_pop(L, 1);
        if (in_map(fx, fy)) ctx->friendly_set[fy * MAP_W + fx] = 1;
    }

    /* Process each hostile/neutral pill. */
    for (int hi = 1; hi <= hp_n; hi++) {
        lua_rawgeti(L, 5, hi); int px = (int)lua_tointeger(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, 6, hi); int py = (int)lua_tointeger(L, -1); lua_pop(L, 1);

        int pill_key = py * MAP_W + px;

        /* Get this pill's contrib sub-table (may be nil if stamp was skipped). */
        lua_rawgeti(L, 1, pill_key);
        int contrib_tbl = lua_gettop(L);
        int has_contrib = lua_istable(L, contrib_tbl);

        /* Occlusion DP sweep in Chebyshev order.
         * occ_walls/trees/fpills are reused without clearing — the
         * traversal order guarantees each slot is written before it is
         * read as a predecessor (same technique as Lua _occ_walls). */
        for (int oi = 0; oi < ctx->pred_len; oi++) {
            int slot = (int)ctx->pred_order[oi];
            /* Decode dx/dy from column-major slot. */
            int dx = slot / size - R;
            int dy = slot % size - R;

            int w_total, t_total, f_total;
            if (dx == 0 && dy == 0) {
                w_total = t_total = f_total = 0;
            } else {
                int pdx = (int)ctx->pred_off_dx[slot];
                int pdy = (int)ctx->pred_off_dy[slot];
                int pred_slot = (pdx + R) * size + (pdy + R);
                int prev_w = (int)ctx->occ_walls [pred_slot];
                int prev_t = (int)ctx->occ_trees [pred_slot];
                int prev_f = (int)ctx->occ_fpills[pred_slot];

                int pnx = px + pdx;
                int pny = py + pdy;
                int add_w = 0, add_t = 0, add_f = 0;
                if (in_map(pnx, pny)) {
                    int tt = raw_tt(ctx, pnx, pny);
                    if (tt == ctx->t_building || tt == ctx->t_halfbuild) { add_w = 1; }
                    else if (tt == ctx->t_forest)                         { add_t = 1; }
                    if (!(pdx == 0 && pdy == 0)) {
                        if (ctx->friendly_set[pny * MAP_W + pnx]) add_f = 1;
                    }
                }
                w_total = prev_w + add_w;
                t_total = prev_t + add_t;
                f_total = prev_f + add_f;
            }

            ctx->occ_walls [slot] = (int16_t)w_total;
            ctx->occ_trees [slot] = (int16_t)t_total;
            ctx->occ_fpills[slot] = (int16_t)f_total;

            /* Apply reduction only if d^2 >= 4 (matches Lua `d2 >= 4` skip). */
            int d2 = dx * dx + dy * dy;
            if (d2 < 4) continue;
            int nx = px + dx;
            int ny = py + dy;
            if (!in_map(nx, ny)) continue;
            int k = ny * MAP_W + nx;

            /* Direct C read — no Lua API call for pill_grid. */
            double cur = (double)ctx->pill_grid_c[k];
            if (cur <= 0.0) continue;

            int eff_walls = w_total;
            /* Front-most wall: exposed directly to pill, no self-occlusion. */
            if (w_total == 0) {
                int tt = raw_tt(ctx, nx, ny);
                if (tt == ctx->t_building || tt == ctx->t_halfbuild) eff_walls = 0;
            }
            double reduction = eff_walls * 0.20 + t_total * 0.03 + f_total * 0.40;
            if (reduction > 0.80) reduction = 0.80;
            if (reduction <= 0.0) continue;

            double factor = 1.0 - reduction;
            /* Direct C write — no Lua API call for pill_grid. */
            ctx->pill_grid_c[k] = (float)(cur * factor);

            if (has_contrib) {
                lua_rawgeti(L, contrib_tbl, k);
                if (!lua_isnil(L, -1)) {
                    double cv = lua_tonumber(L, -1);
                    lua_pop(L, 1);
                    lua_pushnumber(L, cv * factor);
                    lua_rawseti(L, contrib_tbl, k);
                } else {
                    lua_pop(L, 1);
                }
            }
        }

        lua_pop(L, 1);  /* pop contrib sub-table */
    }

    /* Clear friendly-pill set (only the positions we set). */
    for (int i = 1; i <= fp_n; i++) {
        lua_rawgeti(L, 2, i); int fx = (int)lua_tointeger(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, 3, i); int fy = (int)lua_tointeger(L, -1); lua_pop(L, 1);
        if (in_map(fx, fy)) ctx->friendly_set[fy * MAP_W + fx] = 0;
    }

    return 0;
}

/* na_threat.sync_grids() — kept for backwards compatibility, now a no-op.
 * pill_grid_c and cov_grid_c are written directly by stamp_pill and
 * apply_occlusion_all; no copy from Lua tables is needed. */
static int l_naThreatSyncGrids(lua_State *L) {
    (void)requireConfigured(L);
    return 0;
}

/* na_threat.apply_crossfire()
 * Multiply each non-zero pill_grid tile by its coverage count when > 1.
 * Replaces the Lua crossfire loop — operates on pill_grid_c directly. */
static int l_naThreatApplyCrossfire(lua_State *L) {
    NaThreatCtx *ctx = requireConfigured(L);
    for (int k = 0; k < MAP_TILES; k++) {
        if (ctx->pill_grid_c[k] > 0.0f) {
            float cov = ctx->cov_grid_c[k];
            if (cov > 1.0f) ctx->pill_grid_c[k] *= cov;
        }
    }
    return 0;
}

/* na_threat.pill_grid_at(k) -> number
 * Returns pill_grid_c[k] (0 if out of range). Replaces M.pill_grid[k]
 * reads in Lua now that pill_grid is stored exclusively in C. */
static int l_naThreatPillGridAt(lua_State *L) {
    NaThreatCtx *ctx = requireConfigured(L);
    lua_Integer k = luaL_checkinteger(L, 1);
    float v = (k >= 0 && k < MAP_TILES) ? ctx->pill_grid_c[(int)k] : 0.0f;
    lua_pushnumber(L, (double)v);
    return 1;
}

/* na_threat.cov_grid_at(k) -> integer
 * Returns cov_grid_c[k] (0 if out of range). Replaces M.coverage_grid[k]. */
static int l_naThreatCovGridAt(lua_State *L) {
    NaThreatCtx *ctx = requireConfigured(L);
    lua_Integer k = luaL_checkinteger(L, 1);
    float v = (k >= 0 && k < MAP_TILES) ? ctx->cov_grid_c[(int)k] : 0.0f;
    lua_pushinteger(L, (lua_Integer)(int)v);
    return 1;
}

/* na_threat.for_each_pill_danger(callback)
 * Iterates all non-zero pill_grid_c entries, calling callback(mx, my, v). */
static int l_naThreatForEachPillDanger(lua_State *L) {
    NaThreatCtx *ctx = requireConfigured(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    for (int k = 0; k < MAP_TILES; k++) {
        float v = ctx->pill_grid_c[k];
        if (v > 0.0f) {
            lua_pushvalue(L, 1);
            lua_pushinteger(L, k & 255);         /* mx */
            lua_pushinteger(L, (k >> 8) & 255);  /* my */
            lua_pushnumber(L, (double)v);
            lua_call(L, 3, 0);
        }
    }
    return 0;
}

/* C accessors used by na_attack.c and braincore.c. Take lua_State *L
 * so they can locate the per-state ctx. Return NULL before configure()
 * has run, matching the pre-refactor "no grid available" path callers
 * already handle. */
float *naThreatGetPillGrid(lua_State *L) {
    NaThreatCtx *ctx = getCtx(L);
    if (!ctx->cfg_done) return NULL;
    return ctx->pill_grid_c;
}

float *naThreatGetCovGrid(lua_State *L) {
    NaThreatCtx *ctx = getCtx(L);
    if (!ctx->cfg_done) return NULL;
    return ctx->cov_grid_c;
}

int naThreatRawTT(lua_State *L, int mx, int my) {
    NaThreatCtx *ctx = getCtx(L);
    return raw_tt(ctx, mx, my);
}

/* ── Module registration ─────────────────────────────────────────── */

static const luaL_Reg na_threat_lib[] = {
    { "configure",             l_naThreatConfigure },
    { "terrain_rebuild",       l_naThreatTerrainRebuild },
    { "terrain_update_around", l_naThreatTerrainUpdateAround },
    { "terrain_factor_at",     l_naThreatTerrainFactorAt },
    { "pill_rebuild_begin",    l_naThreatPillRebuildBegin },
    { "stamp_pill",            l_naThreatStampPill },
    { "apply_occlusion_all",   l_naThreatApplyOcclusionAll },
    { "apply_crossfire",       l_naThreatApplyCrossfire },
    { "pill_grid_at",          l_naThreatPillGridAt },
    { "cov_grid_at",           l_naThreatCovGridAt },
    { "for_each_pill_danger",  l_naThreatForEachPillDanger },
    { "is_terrain_built",      l_naThreatIsTerrainBuilt },
    { "sync_grids",            l_naThreatSyncGrids },
    { NULL, NULL }
};

void naThreatRegister(lua_State *L) {
    /* Allocate the ctx as a Lua-managed userdata (the userdata storage
     * IS the ctx). Lua frees it on lua_State close; the __gc metatable
     * is a no-op today but is the hook for any sub-allocation cleanup
     * the ctx might grow later. */
    NaThreatCtx *ctx = (NaThreatCtx *)lua_newuserdata(L, sizeof(NaThreatCtx));
    memset(ctx, 0, sizeof(*ctx));

    /* Cache this brain's per-state worldPtr-pointer. brainCoreRegisterGetTerrain
     * must have been called on L before naThreatRegister; if not, the caller
     * has wired the brain up incorrectly. */
    ctx->world_ptr_ptr = brainCoreGetWorldPtrPtr(L);
    if (!ctx->world_ptr_ptr) {
        luaL_error(L, "na_threat: brainCoreRegisterGetTerrain not called on this lua_State");
    }

    if (luaL_newmetatable(L, "na_threat_ctx_mt")) {
        lua_pushcfunction(L, naThreatCtxGc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);

    lua_setfield(L, LUA_REGISTRYINDEX, "na_threat_ctx");

    luaL_newlib(L, na_threat_lib);
    lua_setglobal(L, "na_threat");
}
