/*
 * gh_attack.c — C port of evaluate_pill_difficulty (detailed=false hot path)
 *
 * Replaces the per-tile Lua->C boundary crossings (~2,800 per call) with
 * direct C array reads.  The detailed=true path (plan_position / viz) is
 * kept in Lua and unchanged.
 *
 * Data flow:
 *   Brain.open  -> gh_attack.init_stamps(los, ellipse [, cfg])   [once]
 *   pills change -> gh_attack.sync_pill_at(world, pmx, pmy)      [per eval]
 *   threat rebuild -> gh_attack.sync_grids()                     [per rebuild]
 *   goal select  -> gh_attack.evaluate_pill_difficulty(...)
 *
 * TWO EVALUATION PATHS
 * --------------------
 * Only ONE brain c/ directory is compiled (brains/CMakeLists.txt,
 * brains/GoalHunter/c) and EVERY brain version links it.  GoalHunter 1.5 and
 * 1.6 are frozen and their attack.lua sweep differs from the current one's (no clear-aim
 * gate, among other things), so one C algorithm cannot be faithful to both.
 * The two paths are selected by what sync_pill_at is handed:
 *
 *   legacy  — sync_pill_at(world.pill_at, pmx, pmy).  1.5 / 1.6 call it this
 *             way.  eval_legacy() below is the historical code, unchanged,
 *             including its (wrong, see below) terrain constants.  Frozen.
 *
 *   parity  — sync_pill_at(world, pmx, pmy).  The current GoalHunter calls it this way.
 *             eval_parity() reproduces its Lua sweep rule for rule.
 *
 * The legacy path's terrain constants never matched the engine.  The brain's
 * terrain byte is the BBUILDING..TERRAIN_UNKNOWN enum from brain.h
 * (BUILDING 0, RIVER 1, SWAMP 2, CRATER 3, ROAD 4, FOREST 5, RUBBLE 6,
 * GRASS 7, HALFBUILDING 8, BOAT 9, DEEPSEA 10, REFBASE 11, PILLBOX 12,
 * UNKNOWN 13) — the numbering constants.lua uses.  The legacy TT_* defines
 * below are a different scheme entirely, so legacy reads GRASS and FOREST as
 * impassable and rejects nearly every standoff spot on a grass map.  The
 * parity path takes every terrain id from Lua instead of hardcoding any.
 */

#define _USE_MATH_DEFINES
#include "gh_attack.h"
#include "gh_threat.h"
#include "braincore.h"
#include "brain_pathfinder.h"
#include "bot_manager.h"
#include <lauxlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

/* ── Map dimensions ───────────────────────────────────────────────────── */
#define MAP_W      256
#define MAP_TILES  (MAP_W * MAP_W)
#define TT_MASK    0x0F

/* Terrain types — LEGACY PATH ONLY.  These do not match the engine (see the
 * file header); they are kept exactly as they were so GoalHunter 1.5 / 1.6,
 * which link this same object, keep playing the game they were frozen on. */
#define TT_DEEPSEA   0
#define TT_FOREST    2
#define TT_BUILDING  3
#define TT_PILLBOX   5
#define TT_RIVER     7
#define TT_SWAMP     8
#define TT_HALFBUILD 9
#define TT_BOAT      13

/* Impassable terrain (legacy path only) */
static const int s_impassable[16] = {
    1, 0, 0, 1,  /* 0=deepsea, 1=grass, 2=forest, 3=building */
    0, 1, 0, 1,  /* 4=crater,  5=pill,  6=road,   7=river    */
    1, 1, 0, 0,  /* 8=swamp,   9=half,  10=rubble, 11=unk    */
    0, 1, 0, 0   /* 12=tank,  13=boat,  14=?,      15=?      */
};

/* ── Default constants (override at runtime via init_stamps' cfg table) ─── */
#define DEF_STANDOFF        7.4f
#define DEF_SAFE_RADIUS     3.0f
#define DEF_DANGER_HOTSPOT  15.0f
#define DEF_INF_THRESHOLD   (-20.0)
#define DEF_INF_MULT        5.0f
#define COST_INF            1e29

/* Engine terrain ids (brain.h BBUILDING..TERRAIN_UNKNOWN) — the parity path's
 * fallbacks when Lua passes no cfg table.  Lua overrides all of them. */
#define ENG_T_BUILDING   0
#define ENG_T_RIVER      1
#define ENG_T_SWAMP      2
#define ENG_T_FOREST     5
#define ENG_T_HALFBUILD  8
#define ENG_T_DEEPSEA   10
#define ENG_T_PILLBOX   12

/* ── Stamp storage ────────────────────────────────────────────────────── */
#define GH_ATK_MAX_ANGLES    72
#define GH_ATK_LOS_MAX_AIMS   5
#define GH_ATK_LOS_MAX_TILES 16
#define GH_ATK_ELL_MAX_TILES 64
#define GH_ATK_AIMS           5
#define GH_ATK_SHOT_TILES    64

typedef struct { int8_t dx, dy; } LosTile;

typedef struct {
    int8_t  spot_dx, spot_dy;
    int     n_tiles[GH_ATK_LOS_MAX_AIMS];
    LosTile tiles[GH_ATK_LOS_MAX_AIMS][GH_ATK_LOS_MAX_TILES];
} LosStamp;

typedef struct {
    int8_t  dx, dy;
    int16_t key_offset;
    float   proj;
} EllTile;

typedef struct {
    int     n;
    EllTile tiles[GH_ATK_ELL_MAX_TILES];
} EllStamp;

/* ── Per-brain working state ──────────────────────────────────────────
 * Allocated as a Lua-managed userdata in naAttackRegister and accessed
 * via getCtx(L). Each brain (lua_State) owns its own ctx; concurrent
 * brains never touch each other's grids or stamps. */
typedef struct NaAttackCtx {
    LosStamp los[GH_ATK_MAX_ANGLES];
    EllStamp ell[GH_ATK_MAX_ANGLES];
    int      stamps_built;
    float    pill_grid[MAP_TILES];
    float    cov_grid[MAP_TILES];
    uint8_t  pill_at[MAP_TILES];     /* legacy: 1 = non-target pill present */
    float    self_contrib[MAP_TILES];/* pill's own contribution (temp)      */

    /* ── parity path ─────────────────────────────────────────────────
     * world_synced is set by the last sync_pill_at: 1 when it was handed
     * the whole world table (the current GoalHunter), 0 when it was handed a bare
     * world.pill_at (1.5 / 1.6).  It selects the evaluation path. */
    int      world_synced;
    uint8_t  p_any[MAP_TILES];       /* world.pill_at[k] is a non-empty list */
    uint8_t  p_friendly[MAP_TILES];  /* a live entry with owner=="friendly"  */
    uint8_t  p_blocker[MAP_TILES];   /* aim_line_trees' live-table blocker   */
    uint8_t  base_any[MAP_TILES];    /* world.base_at[k].base exists         */
    uint8_t  base_hostile[MAP_TILES];/* ...with owner == "hostile"           */

    /* Constants, all mirrored from constants.lua via init_stamps' cfg. */
    double   cfg_standoff;
    double   cfg_hotspot;
    double   cfg_inf_threshold;
    double   cfg_inf_mult;
    double   cfg_tree_penalty;
    int      t_building, t_halfbuild, t_forest, t_deepsea;
    int      t_swamp, t_river, t_pillbox;
    uint8_t  passable[16];           /* TERRAIN_COST_LAND < 9999 && !water   */
    double   aim_off[GH_ATK_AIMS][2];/* shield.AIM_OFFSETS_TILE_FIRE         */

    /* The brain's pathfinder, refreshed by getCtx on every entry. The shot
     * walk reads this sim's shell rules off it, so aim_line_trees has to
     * simulate with the same one cpf_simulate_shot would use. NULL until a
     * pathfinder is registered; the simulate call answers 0 tiles for that,
     * which reads as a blocked line. */
    BrainPathfinder *pf;
} NaAttackCtx;

/* ── Registry-backed ctx lookup ───────────────────────────────────── */

static int naAttackCtxGc(lua_State *L) {
    /* Userdata storage is freed by Lua. The hook is here so any future
     * ctx-owned sub-allocations have a place to be released. */
    (void)L;
    return 0;
}

static NaAttackCtx *getCtx(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "gh_attack_ctx");
    NaAttackCtx *ctx = (NaAttackCtx *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!ctx) luaL_error(L, "gh_attack: ctx not initialized");
    /* Re-read every entry rather than latch: the instance's pathfinder is
     * created and destroyed with the brain, and the ctx outlives neither
     * reliably. */
    ctx->pf = brainCoreGetPathfinder(L);
    return ctx;
}

static NaAttackCtx *requireStampsBuilt(lua_State *L) {
    NaAttackCtx *ctx = getCtx(L);
    if (!ctx->stamps_built) luaL_error(L, "gh_attack: init_stamps() must be called first");
    return ctx;
}

/* ── Valid-spot accumulator ───────────────────────────────────────────── */
#define MAX_VALID 80
typedef struct {
    int   mx, my, deg;
    float cx, cy, score;
} ValidSpot;

/* Parity path keeps every number in double, the width Lua computes in. */
typedef struct {
    int    mx, my, deg;
    double score;
} PValidSpot;

/* ── Helpers ──────────────────────────────────────────────────────────── */
static inline int in_map(int x, int y) {
    return (unsigned)x < (unsigned)MAP_W && (unsigned)y < (unsigned)MAP_W;
}

/* U.ttype(mx, my): get_terrain returns 0 outside the map, then & 0x0F. */
static inline int ttype_at(const BYTE *w, int x, int y) {
    if (!in_map(x, y)) return 0;
    return w[y * MAP_W + x] & TT_MASK;
}

/* Call cpf_dijkstra_cost_at(slate, mx, my, 0) -> double */
static double cpf_dij_cost(lua_State *L, int slate, int mx, int my) {
    lua_getglobal(L, "cpf_dijkstra_cost_at");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return COST_INF; }
    lua_pushinteger(L, slate);
    lua_pushinteger(L, mx);
    lua_pushinteger(L, my);
    lua_pushinteger(L, 0);
    lua_call(L, 4, 1);
    double c = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return c;
}

/* Call cpf_estimate_cost(sx, sy, dx, dy, 0) -> double */
static double cpf_est_cost(lua_State *L, int sx, int sy, int dx, int dy) {
    lua_getglobal(L, "cpf_estimate_cost");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return HUGE_VAL; }
    lua_pushinteger(L, sx);
    lua_pushinteger(L, sy);
    lua_pushinteger(L, dx);
    lua_pushinteger(L, dy);
    lua_pushinteger(L, 0);
    lua_call(L, 5, 1);
    double c = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return c;
}

/* Call cpf_influence_at(mx, my) -> double */
static double cpf_influence(lua_State *L, int mx, int my) {
    lua_getglobal(L, "cpf_influence_at");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return 0.0; }
    lua_pushinteger(L, mx);
    lua_pushinteger(L, my);
    lua_call(L, 2, 1);
    double v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

/* ── Lua bindings ─────────────────────────────────────────────────────── */

/* Read cfg[name] into *dst when present. */
static void cfg_num(lua_State *L, int idx, const char *name, double *dst) {
    lua_getfield(L, idx, name);
    if (lua_isnumber(L, -1)) *dst = lua_tonumber(L, -1);
    lua_pop(L, 1);
}

static void cfg_int(lua_State *L, int idx, const char *name, int *dst) {
    lua_getfield(L, idx, name);
    if (lua_isnumber(L, -1)) *dst = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
}

/* Defaults for the parity path.  Everything here is overwritten when Lua
 * passes a cfg table; the values only matter for a brain that calls
 * init_stamps with two arguments and then syncs a whole world table. */
static void ctx_set_cfg_defaults(NaAttackCtx *ctx) {
    ctx->cfg_standoff      = DEF_STANDOFF;
    ctx->cfg_hotspot       = DEF_DANGER_HOTSPOT;
    ctx->cfg_inf_threshold = DEF_INF_THRESHOLD;
    ctx->cfg_inf_mult      = DEF_INF_MULT;
    ctx->cfg_tree_penalty  = 8.0;
    ctx->t_building  = ENG_T_BUILDING;
    ctx->t_halfbuild = ENG_T_HALFBUILD;
    ctx->t_forest    = ENG_T_FOREST;
    ctx->t_deepsea   = ENG_T_DEEPSEA;
    ctx->t_swamp     = ENG_T_SWAMP;
    ctx->t_river     = ENG_T_RIVER;
    ctx->t_pillbox   = ENG_T_PILLBOX;
    /* TERRAIN_COST_LAND[tt] < 9999 and not is_water(tt): everything except
     * BUILDING(0), RIVER(1), HALFBUILDING(8), DEEPSEA(10) and the two ids
     * with no cost entry at all (14, 15). */
    for (int i = 0; i < 16; i++) ctx->passable[i] = 1;
    ctx->passable[ENG_T_BUILDING]  = 0;
    ctx->passable[ENG_T_RIVER]     = 0;
    ctx->passable[ENG_T_HALFBUILD] = 0;
    ctx->passable[ENG_T_DEEPSEA]   = 0;
    ctx->passable[14]              = 0;
    ctx->passable[15]              = 0;
    /* shield.AIM_OFFSETS_TILE_FIRE with AIM_INSET_FIRE = 16 (1 gu). */
    {
        const double f = 16.0 / 256.0;
        ctx->aim_off[0][0] = 0.5;       ctx->aim_off[0][1] = 0.5;
        ctx->aim_off[1][0] = f;         ctx->aim_off[1][1] = f;
        ctx->aim_off[2][0] = 1.0 - f;   ctx->aim_off[2][1] = f;
        ctx->aim_off[3][0] = f;         ctx->aim_off[3][1] = 1.0 - f;
        ctx->aim_off[4][0] = 1.0 - f;   ctx->aim_off[4][1] = 1.0 - f;
    }
}

/*
 * gh_attack.init_stamps(los_stamps_5deg, ellipse_stamps_5deg [, cfg])
 * Traverses the Lua stamp tables once and copies into C arrays.
 *
 * cfg (optional) mirrors constants.lua so no tuning value is duplicated in
 * C.  Recognised keys: standoff, danger_hotspot, inf_threshold, inf_mult,
 * tree_penalty, t_building, t_halfbuild, t_forest, t_deepsea, t_swamp,
 * t_river, t_pillbox, passable (16 booleans indexed 0..15),
 * aim_offsets (5 x {x, y}).
 */
static int l_init_stamps(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE); /* LOS_STAMPS_5DEG   */
    luaL_checktype(L, 2, LUA_TTABLE); /* ELLIPSE_STAMPS_5DEG */

    NaAttackCtx *ctx = getCtx(L);

    /* Mark unbuilt for the duration of the rebuild so any concurrent
     * binding call sees stamps_built == 0 until the new state is fully
     * built (matches Phase-2 lifecycle pattern). */
    ctx->stamps_built = 0;
    memset(ctx->los, 0, sizeof(ctx->los));
    memset(ctx->ell, 0, sizeof(ctx->ell));
    ctx_set_cfg_defaults(ctx);

    /* LOS stamps: keyed by degree (0,5,10,...355) */
    for (int deg = 0, ai = 0; deg < 360; deg += 5, ai++) {
        if (ai >= GH_ATK_MAX_ANGLES) break;
        lua_rawgeti(L, 1, deg);  /* try integer key first */
        if (lua_isnil(L, -1)) { lua_pop(L, 1); continue; }

        LosStamp *ls = &ctx->los[ai];

        lua_getfield(L, -1, "spot_dx");
        ls->spot_dx = (int8_t)lua_tointeger(L, -1); lua_pop(L, 1);
        lua_getfield(L, -1, "spot_dy");
        ls->spot_dy = (int8_t)lua_tointeger(L, -1); lua_pop(L, 1);

        lua_getfield(L, -1, "aims");
        for (int aim = 0; aim < GH_ATK_LOS_MAX_AIMS; aim++) {
            lua_rawgeti(L, -1, aim + 1);
            if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
            int n = 0;
            int tlen = (int)lua_rawlen(L, -1);
            for (int t = 1; t <= tlen && n < GH_ATK_LOS_MAX_TILES; t++) {
                lua_rawgeti(L, -1, t);
                lua_getfield(L, -1, "dx");
                ls->tiles[aim][n].dx = (int8_t)lua_tointeger(L, -1); lua_pop(L, 1);
                lua_getfield(L, -1, "dy");
                ls->tiles[aim][n].dy = (int8_t)lua_tointeger(L, -1); lua_pop(L, 1);
                lua_pop(L, 1); /* tile table */
                n++;
            }
            ls->n_tiles[aim] = n;
            lua_pop(L, 1); /* aim array */
        }
        lua_pop(L, 1); /* aims table */
        lua_pop(L, 1); /* stamp table */
    }

    /* Ellipse stamps: keyed by degree */
    for (int deg = 0, ai = 0; deg < 360; deg += 5, ai++) {
        if (ai >= GH_ATK_MAX_ANGLES) break;
        lua_rawgeti(L, 2, deg);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); continue; }

        EllStamp *es = &ctx->ell[ai];
        int tlen = (int)lua_rawlen(L, -1);
        int n = 0;
        for (int t = 1; t <= tlen && n < GH_ATK_ELL_MAX_TILES; t++) {
            lua_rawgeti(L, -1, t);
            EllTile *et = &es->tiles[n];
            lua_getfield(L, -1, "dx");
            et->dx = (int8_t)lua_tointeger(L, -1); lua_pop(L, 1);
            lua_getfield(L, -1, "dy");
            et->dy = (int8_t)lua_tointeger(L, -1); lua_pop(L, 1);
            lua_getfield(L, -1, "key_offset");
            et->key_offset = (int16_t)lua_tointeger(L, -1); lua_pop(L, 1);
            lua_getfield(L, -1, "proj");
            et->proj = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            lua_pop(L, 1); /* tile table */
            n++;
        }
        es->n = n;
        lua_pop(L, 1); /* stamp array */
    }

    /* Optional constants table. */
    if (lua_istable(L, 3)) {
        cfg_num(L, 3, "standoff",       &ctx->cfg_standoff);
        cfg_num(L, 3, "danger_hotspot", &ctx->cfg_hotspot);
        cfg_num(L, 3, "inf_threshold",  &ctx->cfg_inf_threshold);
        cfg_num(L, 3, "inf_mult",       &ctx->cfg_inf_mult);
        cfg_num(L, 3, "tree_penalty",   &ctx->cfg_tree_penalty);
        cfg_int(L, 3, "t_building",     &ctx->t_building);
        cfg_int(L, 3, "t_halfbuild",    &ctx->t_halfbuild);
        cfg_int(L, 3, "t_forest",       &ctx->t_forest);
        cfg_int(L, 3, "t_deepsea",      &ctx->t_deepsea);
        cfg_int(L, 3, "t_swamp",        &ctx->t_swamp);
        cfg_int(L, 3, "t_river",        &ctx->t_river);
        cfg_int(L, 3, "t_pillbox",      &ctx->t_pillbox);

        lua_getfield(L, 3, "passable");
        if (lua_istable(L, -1)) {
            for (int i = 0; i < 16; i++) {
                lua_rawgeti(L, -1, i);
                ctx->passable[i] = (uint8_t)(lua_toboolean(L, -1) ? 1 : 0);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);

        lua_getfield(L, 3, "aim_offsets");
        if (lua_istable(L, -1)) {
            for (int i = 0; i < GH_ATK_AIMS; i++) {
                lua_rawgeti(L, -1, i + 1);
                if (lua_istable(L, -1)) {
                    lua_rawgeti(L, -1, 1);
                    ctx->aim_off[i][0] = lua_tonumber(L, -1); lua_pop(L, 1);
                    lua_rawgeti(L, -1, 2);
                    ctx->aim_off[i][1] = lua_tonumber(L, -1); lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

    ctx->stamps_built = 1;
    return 0;
}

/* Fill the legacy pill_at occupancy array from a world.pill_at table at
 * stack index `idx`, excluding the target pill's own tile. */
static void sync_legacy_pill_at(lua_State *L, NaAttackCtx *ctx, int idx,
                                int target_key) {
    memset(ctx->pill_at, 0, sizeof(ctx->pill_at));
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        /* key = tile index (integer), value = array of pill entries */
        if (lua_isinteger(L, -2)) {
            int k = (int)lua_tointeger(L, -2);
            if (k >= 0 && k < MAP_TILES && k != target_key) {
                /* Check if the array has at least one pill entry */
                if (lua_istable(L, -1) && lua_rawlen(L, -1) > 0) {
                    ctx->pill_at[k] = 1;
                }
            }
        }
        lua_pop(L, 1); /* pop value, keep key */
    }
}

/* Field readers for the table on the top of the stack. */
static double tbl_num(lua_State *L, const char *name, double dflt) {
    lua_getfield(L, -1, name);
    double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : dflt;
    lua_pop(L, 1);
    return v;
}

static int tbl_is_true(lua_State *L, const char *name) {
    lua_getfield(L, -1, name);
    int v = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return v;
}

static int tbl_str_eq(lua_State *L, const char *name, const char *want) {
    lua_getfield(L, -1, name);
    const char *s = lua_tostring(L, -1);
    int eq = (s != NULL && strcmp(s, want) == 0);
    lua_pop(L, 1);
    return eq;
}

/* One pill_at tile: fold its entry list into the three pill flag arrays. */
static void scan_pill_entries(lua_State *L, NaAttackCtx *ctx,
                              int list_idx, int pills_idx, int k) {
    int n = (int)lua_rawlen(L, list_idx);
    int tile_mx = k % MAP_W, tile_my = k / MAP_W;
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, list_idx, i);          /* entry */
        int ei = lua_gettop(L);
        if (!lua_istable(L, ei)) { lua_pop(L, 1); continue; }

        /* Scan B's terrain penalty reads the INDEX entry's own copy. */
        lua_getfield(L, ei, "pill");
        if (lua_istable(L, -1)) {
            if (tbl_str_eq(L, "owner", "friendly") &&
                tbl_num(L, "health", 0.0) > 0.0)
                ctx->p_friendly[k] = 1;
        }
        lua_pop(L, 1);

        /* aim_line_trees' live-table check:
         *   p = (e.id and pills and pills[e.id]) or e.pill */
        int pushed = 0;
        if (pills_idx) {
            lua_getfield(L, ei, "id");
            if (!lua_isnil(L, -1)) {
                lua_gettable(L, pills_idx);   /* consumes id, pushes pills[id] */
                pushed = 1;
                if (lua_isnil(L, -1)) { lua_pop(L, 1); pushed = 0; }
            } else {
                lua_pop(L, 1);
            }
        }
        if (!pushed) lua_getfield(L, ei, "pill");

        if (lua_istable(L, -1)) {
            double hp   = tbl_num(L, "health", 0.0);
            int in_tank = tbl_is_true(L, "in_tank");
            lua_getfield(L, -1, "mx");
            int has_mx = !lua_isnil(L, -1);
            int p_mx   = has_mx ? (int)lua_tointeger(L, -1) : 0;
            lua_pop(L, 1);
            lua_getfield(L, -1, "my");
            int p_my = lua_isnil(L, -1) ? 0 : (int)lua_tointeger(L, -1);
            lua_pop(L, 1);
            if (!in_tank && hp > 0.0 &&
                (!has_mx || (p_mx == tile_mx && p_my == tile_my)))
                ctx->p_blocker[k] = 1;
        }
        lua_pop(L, 1);   /* p */
        lua_pop(L, 1);   /* entry */
    }
}

/* Build the parity path's per-tile flags from the whole world table
 * (stack index `widx`).  Mirrors, in one pass:
 *   LOS stamp        world.pill_at[k]                        -> p_any
 *   scan B penalty   any entry .pill.owner=="friendly", hp>0 -> p_friendly
 *   aim_line_trees   live-table pill blocker                 -> p_blocker
 *   aim_line_trees   world.base_at[k].base                   -> base_any
 *   scan B penalty   ...with .owner == "hostile"             -> base_hostile
 */
static void sync_world_flags(lua_State *L, NaAttackCtx *ctx, int widx) {
    memset(ctx->p_any,        0, sizeof(ctx->p_any));
    memset(ctx->p_friendly,   0, sizeof(ctx->p_friendly));
    memset(ctx->p_blocker,    0, sizeof(ctx->p_blocker));
    memset(ctx->base_any,     0, sizeof(ctx->base_any));
    memset(ctx->base_hostile, 0, sizeof(ctx->base_hostile));

    lua_getfield(L, widx, "pills");            /* +1 (may be nil) */
    int pills_slot = lua_gettop(L);
    int pills_idx  = lua_istable(L, pills_slot) ? pills_slot : 0;

    lua_getfield(L, widx, "pill_at");          /* +1 */
    if (lua_istable(L, -1)) {
        int pa = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, pa)) {
            if (lua_isinteger(L, -2) && lua_istable(L, -1)) {
                int k = (int)lua_tointeger(L, -2);
                if (k >= 0 && k < MAP_TILES && lua_rawlen(L, -1) > 0) {
                    ctx->p_any[k] = 1;
                    scan_pill_entries(L, ctx, lua_gettop(L), pills_idx, k);
                }
            }
            lua_pop(L, 1);                     /* value, keep key */
        }
    }
    lua_pop(L, 1);                             /* pill_at */

    lua_getfield(L, widx, "base_at");          /* +1 */
    if (lua_istable(L, -1)) {
        int ba = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, ba)) {
            if (lua_isinteger(L, -2) && lua_istable(L, -1)) {
                int k = (int)lua_tointeger(L, -2);
                if (k >= 0 && k < MAP_TILES) {
                    lua_getfield(L, -1, "base");
                    if (lua_istable(L, -1)) {
                        ctx->base_any[k] = 1;
                        if (tbl_str_eq(L, "owner", "hostile"))
                            ctx->base_hostile[k] = 1;
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);                             /* base_at */
    lua_pop(L, 1);                             /* pills   */
}

/*
 * gh_attack.sync_pill_at(world_or_pill_at, target_pmx, target_pmy)
 *
 * Handed the whole `world` table (the current GoalHunter) it builds the parity
 * path's flag arrays and selects the parity evaluator.  Handed a bare
 * `world.pill_at` (GoalHunter 1.5 / 1.6) it rebuilds only the legacy
 * occupancy array and selects the legacy evaluator.
 */
static int l_sync_pill_at(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int target_pmx = (int)luaL_checkinteger(L, 2);
    int target_pmy = (int)luaL_checkinteger(L, 3);
    int target_key = target_pmy * MAP_W + target_pmx;

    NaAttackCtx *ctx = requireStampsBuilt(L);

    lua_getfield(L, 1, "pill_at");
    int is_world = lua_istable(L, -1);
    lua_pop(L, 1);

    if (is_world) {
        ctx->world_synced = 1;
        sync_world_flags(L, ctx, 1);
    } else {
        ctx->world_synced = 0;
        sync_legacy_pill_at(L, ctx, 1, target_key);
    }
    return 0;
}

/*
 * gh_attack.sync_grids()
 * Copies pill_grid and cov_grid from gh_threat into ctx local arrays.
 * pill_grid and cov_grid are now written directly to C by stamp_pill and
 * apply_occlusion_all, so this is a plain memcpy — no Lua table iteration.
 */
static int l_sync_grids(lua_State *L) {
    NaAttackCtx *ctx = requireStampsBuilt(L);
    float *pg = naThreatGetPillGrid(L);
    float *cg = naThreatGetCovGrid(L);
    if (pg) memcpy(ctx->pill_grid, pg, MAP_TILES * sizeof(float));
    else    memset(ctx->pill_grid, 0, sizeof(ctx->pill_grid));
    if (cg) memcpy(ctx->cov_grid,  cg, MAP_TILES * sizeof(float));
    else    memset(ctx->cov_grid,  0, sizeof(ctx->cov_grid));
    return 0;
}

/* Load the self-contribution table (arg 7) into ctx->self_contrib. */
static void load_self_contrib(lua_State *L, NaAttackCtx *ctx) {
    memset(ctx->self_contrib, 0, sizeof(ctx->self_contrib));
    if (!lua_istable(L, 7)) return;
    lua_pushnil(L);
    while (lua_next(L, 7)) {
        if (lua_isinteger(L, -2)) {
            int k = (int)lua_tointeger(L, -2);
            if (k >= 0 && k < MAP_TILES)
                ctx->self_contrib[k] = (float)lua_tonumber(L, -1);
        }
        lua_pop(L, 1);
    }
}

static int push_no_spot(lua_State *L) {
    lua_pushnumber(L, 1e30);
    lua_pushinteger(L, -1);
    lua_pushinteger(L, -1);
    lua_pushinteger(L, -1);
    return 4;
}

/* ── Parity path: attack.lua's clear-aim gate ─────────────────────────────
 * One aim point, one shell simulation.  Returns the forest-tile count on the
 * line (>= 0), or -1 when the line is BLOCKED / never reaches the pill.
 * Mirrors attack.lua's aim_line_trees. */
static int aim_line_trees(NaAttackCtx *ctx, const BYTE *w,
                          int ox, int oy, int omx, int omy,
                          int pmx, int pmy, int i,
                          int *out_awx, int *out_awy) {
    int awx = (pmx << 8) + (int)floor(ctx->aim_off[i][0] * 256.0);
    int awy = (pmy << 8) + (int)floor(ctx->aim_off[i][1] * 256.0);

    BrainShotTile tiles[GH_ATK_SHOT_TILES];
    /* The same pathfinder cpf_simulate_shot uses: the shot walk reads this
     * sim's shell rules off it, so an aim line simulated without it is not
     * the line the engine will fire. It answers 0 tiles when there is none,
     * which this treats as a blocked line — the safe reading. */
    int n = brainPathfinderSimulateShot(ctx->pf,
                                        (WORLD)ox, (WORLD)oy,
                                        (WORLD)awx, (WORLD)awy,
                                        BRAIN_SHOT_SHOOTER_TANK, 0,
                                        tiles, GH_ATK_SHOT_TILES);
    int blocked = 0, reached = 0, trees = 0;
    for (int ti = 0; ti < n; ti++) {
        int tx = tiles[ti].mx, ty = tiles[ti].my;
        if (tx == pmx && ty == pmy) { reached = 1; break; }
        /* Our own tile never obstructs our own shot. */
        if (tx != omx || ty != omy) {
            int tt = ttype_at(w, tx, ty);
            if (tt == ctx->t_building || tt == ctx->t_halfbuild) { blocked = 1; break; }
            else if (tt == ctx->t_forest) trees++;
            int key = ty * MAP_W + tx;
            if (key >= 0 && key < MAP_TILES) {
                if (ctx->base_any[key])  { blocked = 1; break; }
                if (ctx->p_blocker[key]) { blocked = 1; break; }
            }
        }
    }
    if (reached && !blocked) { *out_awx = awx; *out_awy = awy; return trees; }
    return -1;
}

/* attack.lua's clear_aim_from_world with prefer_idx = nil.  Returns the
 * 0-based aim index (-1 when every line is blocked) and writes the tree
 * count of the chosen line to *out_trees. */
static int clear_aim_from_world(NaAttackCtx *ctx, const BYTE *w,
                                int ox, int oy, int pmx, int pmy,
                                int *out_trees) {
    int omx = ox >> 8, omy = oy >> 8;
    int best_i = -1, best_trees = -1, awx, awy;
    for (int i = 0; i < GH_ATK_AIMS; i++) {
        int t = aim_line_trees(ctx, w, ox, oy, omx, omy, pmx, pmy, i, &awx, &awy);
        if (t >= 0) {
            if (i == 0 && t == 0) { *out_trees = 0; return 0; }
            if (best_trees < 0 || t < best_trees) {
                best_i = i; best_trees = t;
                if (t == 0) break;
            }
        }
    }
    if (best_i < 0) return -1;
    *out_trees = best_trees;
    return best_i;
}

/* ── Parity evaluator ─────────────────────────────────────────────────────
 * Rule-for-rule port of the current GoalHunter's M.evaluate_pill_difficulty sweep
 * (detailed = false, step_deg = 5, no banned angles) plus
 * M.finalize_pill_eval.  Every arithmetic step is done in double, the width
 * Lua uses, so the 50-point bucket boundary falls the same way. */
static int eval_parity(lua_State *L, NaAttackCtx *ctx, const BYTE *w,
                       int pmx, int pmy, int step, int phase_not_opening,
                       int tmx, int tmy) {
    const double R   = ctx->cfg_standoff;
    const double pcx = (double)pmx + 0.5;
    const double pcy = (double)pmy + 0.5;

    PValidSpot valid[MAX_VALID];
    int n_valid = 0;

    for (int deg = 0; deg < 360; deg += step) {
        /* Cooperative abort — the Lua sweep is unwound by the budget hook
         * instead; with -brain-no-budget-kill neither fires. */
        if (botManagerShouldAbort(L)) break;

        double rad = (double)deg * M_PI / 180.0;
        double cx  = pcx + sin(rad) * R;
        double cy  = pcy - cos(rad) * R;
        int    mx  = (int)floor(cx);
        int    my  = (int)floor(cy);

        if (!in_map(mx, my)) continue;

        int base_key = my * MAP_W + mx;
        int tt = w[base_key] & TT_MASK;
        if (!ctx->passable[tt]) continue;

        /* ── LOS stamp ── */
        int stamp_idx = deg / 5;   /* stamps are always 5-deg spaced */
        if (stamp_idx >= GH_ATK_MAX_ANGLES) stamp_idx = GH_ATK_MAX_ANGLES - 1;
        const LosStamp *ls = &ctx->los[stamp_idx];

        int has_los = 0;
        for (int ai = 0; ai < GH_ATK_LOS_MAX_AIMS; ai++) {
            int blocked = 0;
            int nt = ls->n_tiles[ai];
            for (int ti = 0; ti < nt; ti++) {
                int tx = pmx + ls->tiles[ai][ti].dx;
                int ty = pmy + ls->tiles[ai][ti].dy;
                int ttt = ttype_at(w, tx, ty);
                if (ttt == ctx->t_building || ttt == ctx->t_halfbuild) { blocked = 1; break; }
                int tk = ty * MAP_W + tx;
                if (tk >= 0 && tk < MAP_TILES && ctx->p_any[tk]) { blocked = 1; break; }
            }
            if (!blocked) { has_los = 1; break; }
        }
        if (!has_los) continue;

        /* ── Clear-aim gate ──
         * A spot with no shootable line to any of the five aim points is no
         * spot at all: it never enters all_valid. */
        int ox = (int)floor(cx * 256.0 + 0.5);
        int oy = (int)floor(cy * 256.0 + 0.5);
        int aim_trees = 0;
        int aim_idx = clear_aim_from_world(ctx, w, ox, oy, pmx, pmy, &aim_trees);
        if (aim_idx < 0) continue;

        const EllStamp *es = &ctx->ell[stamp_idx];

        /* ── Crossfire (scan A) ──
         * gh_threat.cov_grid_at truncates to an integer on the way out, so
         * the Lua sweep only ever sees whole numbers here. */
        int max_coverage = 0;
        for (int ti = 0; ti < es->n; ti++) {
            const EllTile *et = &es->tiles[ti];
            if (et->proj > 0.0f) continue;
            int k = base_key + et->key_offset;
            int cov = (k >= 0 && k < MAP_TILES) ? (int)ctx->cov_grid[k] : 0;
            if (cov > max_coverage) max_coverage = cov;
        }
        double score_e = (max_coverage > 1) ? 100.0 * (double)(max_coverage - 1) : 0.0;

        /* ── Maneuver area (scan B) ── */
        double total_danger    = 0.0;
        double terrain_penalty = 0.0;
        int    safe_tiles      = 0;
        int    forest_count    = 0;

        for (int ti = 0; ti < es->n; ti++) {
            const EllTile *et = &es->tiles[ti];
            if (et->proj > 0.0f) continue;

            int sx = mx + (int)et->dx;
            int sy = my + (int)et->dy;
            int k  = base_key + et->key_offset;
            int k_ok = (k >= 0 && k < MAP_TILES);

            double d = 0.0;
            if (k_ok) {
                d = (double)ctx->pill_grid[k] - (double)ctx->self_contrib[k];
                if (d < 0.0) d = 0.0;
            }

            int stt = ttype_at(w, sx, sy);
            if (stt == ctx->t_forest) forest_count++;

            total_danger += d;
            safe_tiles++;

            if (stt == ctx->t_deepsea || (k_ok && ctx->base_hostile[k])) {
                terrain_penalty += 1000.0;
            } else if (stt == ctx->t_building || stt == ctx->t_halfbuild ||
                       stt == ctx->t_swamp    || stt == ctx->t_river     ||
                       stt == ctx->t_pillbox) {
                terrain_penalty += 100.0;
            } else if (k_ok && ctx->p_friendly[k]) {
                terrain_penalty += 100.0;
            }
        }

        if (forest_count > 0) {
            total_danger -= (double)forest_count * 0.75;
            if (total_danger < 0.0) total_danger = 0.0;
        }

        double score_a = (safe_tiles > 0) ? (total_danger / (double)safe_tiles) : 999.0;
        double denom   = (safe_tiles > 0) ? (double)safe_tiles : 1.0;  /* math.max(1, n) */
        double score_b = (total_danger / denom >= ctx->cfg_hotspot) ? 10.0 : 0.0;
        double total_score = score_a + score_b + terrain_penalty + score_e;

        if (aim_trees > 0)
            total_score += (double)aim_trees * ctx->cfg_tree_penalty;

        if (phase_not_opening) {
            double infl = cpf_influence(L, mx, my);
            if (infl <= ctx->cfg_inf_threshold)
                total_score *= ctx->cfg_inf_mult;
        }

        if (n_valid < MAX_VALID) {
            PValidSpot *vs = &valid[n_valid++];
            vs->mx = mx; vs->my = my; vs->deg = deg; vs->score = total_score;
        }
    }

    if (n_valid == 0) return push_no_spot(L);

    /* ── finalize_pill_eval: 50-point bucket, then cheapest to reach ── */
    double min_score = valid[0].score;
    for (int i = 1; i < n_valid; i++)
        if (valid[i].score < min_score) min_score = valid[i].score;

    double bucket_floor = floor(min_score / 50.0) * 50.0;
    double bucket_ceil  = bucket_floor + 50.0;

    int bucket_idx[MAX_VALID];
    int n_bucket = 0;
    for (int i = 0; i < n_valid; i++)
        if (valid[i].score >= bucket_floor && valid[i].score < bucket_ceil)
            bucket_idx[n_bucket++] = i;

    /* `costs` is Lua's one shared table: a slate that bails part-way leaves
     * the entries it did write behind, and the entries it never reached keep
     * whatever an earlier slate wrote (or stay unset -> math.huge). */
    double costs[MAX_VALID];
    int    costs_set[MAX_VALID];
    for (int i = 0; i < n_bucket; i++) costs_set[i] = 0;

    static const int slates[4] = { 0, 1, 2, 3 };
    int slate_used = -1;
    for (int si = 0; si < 4; si++) {
        int ok = 1;
        for (int bi = 0; bi < n_bucket; bi++) {
            double c = cpf_dij_cost(L, slates[si],
                                    valid[bucket_idx[bi]].mx,
                                    valid[bucket_idx[bi]].my);
            if (c >= COST_INF) { ok = 0; break; }
            costs[bi] = c; costs_set[bi] = 1;
        }
        if (ok) { slate_used = slates[si]; break; }
    }

    if (slate_used < 0 && tmx >= 0) {
        for (int bi = 0; bi < n_bucket; bi++) {
            costs[bi] = cpf_est_cost(L, tmx, tmy,
                                     valid[bucket_idx[bi]].mx,
                                     valid[bucket_idx[bi]].my);
            costs_set[bi] = 1;
        }
    }

    double best_dij = HUGE_VAL;
    int    best_bi  = -1;
    for (int bi = 0; bi < n_bucket; bi++) {
        double dij = costs_set[bi] ? costs[bi] : HUGE_VAL;
        if (dij < best_dij) { best_dij = dij; best_bi = bi; }
    }

    /* Lua's best_spot starts nil and only a strictly-smaller dij replaces it,
     * so an all-infinite bucket leaves best_spot nil -> no spot. */
    if (best_bi < 0) return push_no_spot(L);

    PValidSpot *best = &valid[bucket_idx[best_bi]];
    lua_pushnumber(L,  best->score);
    lua_pushinteger(L, best->mx);
    lua_pushinteger(L, best->my);
    lua_pushinteger(L, best->deg);
    return 4;
}

/* ── Legacy evaluator (GoalHunter 1.5 / 1.6) ─────────────────────────────
 * Unchanged from before the parity work.  Do not "fix" anything here: those
 * brains are frozen and their Lua sweep is the 1.5/1.6 one, not the current one. */
static int eval_legacy(lua_State *L, NaAttackCtx *ctx, const BYTE *world,
                       int pmx, int pmy, int step, int phase_not_opening,
                       int tmx, int tmy) {
    (void)tmx; (void)tmy;
    float R   = DEF_STANDOFF;
    float pcx = (float)pmx + 0.5f;
    float pcy = (float)pmy + 0.5f;

    ValidSpot valid[MAX_VALID];
    int       n_valid = 0;

    for (int deg = 0; deg < 360; deg += step) {
        if (botManagerShouldAbort(L)) break;
        float rad = (float)(deg * M_PI / 180.0);
        float cx  = pcx + sinf(rad) * R;
        float cy  = pcy - cosf(rad) * R;
        int   mx  = (int)floorf(cx);
        int   my  = (int)floorf(cy);

        if (!in_map(mx, my)) continue;

        int base_key = my * MAP_W + mx;
        int tt = world[base_key] & TT_MASK;
        if (s_impassable[tt]) continue;

        /* LOS check */
        int stamp_idx = deg / 5;   /* stamps are always 5-deg spaced */
        if (stamp_idx >= GH_ATK_MAX_ANGLES) stamp_idx = GH_ATK_MAX_ANGLES - 1;

        const LosStamp *ls = &ctx->los[stamp_idx];
        int pill_key = pmy * MAP_W + pmx;
        int has_los = 0;

        for (int ai = 0; ai < GH_ATK_LOS_MAX_AIMS; ai++) {
            int blocked = 0;
            int nt = ls->n_tiles[ai];
            for (int ti = 0; ti < nt; ti++) {
                int tx = pmx + ls->tiles[ai][ti].dx;
                int ty = pmy + ls->tiles[ai][ti].dy;
                if (!in_map(tx, ty)) { blocked = 1; break; }
                int tk = ty * MAP_W + tx;
                int ttt = world[tk] & TT_MASK;
                if (ttt == TT_BUILDING || ttt == TT_HALFBUILD) { blocked = 1; break; }
                /* Non-target pill blocks LOS */
                if (tk != pill_key && ctx->pill_at[tk]) { blocked = 1; break; }
            }
            if (!blocked) { has_los = 1; break; }
        }

        if (!has_los) continue;

        /* Ellipse scan (back half: proj <= 0) */
        const EllStamp *es = &ctx->ell[stamp_idx];

        float max_coverage  = 0.0f;
        float total_danger  = 0.0f;
        int   safe_tiles    = 0;
        float terrain_penalty = 0.0f;
        int   forest_count  = 0;

        for (int ti = 0; ti < es->n; ti++) {
            const EllTile *et = &es->tiles[ti];
            if (et->proj > 0.0f) continue;  /* skip front half */

            int k = base_key + et->key_offset;
            if (k < 0 || k >= MAP_TILES) continue;

            /* scan_a: coverage (crossfire) */
            float cov = ctx->cov_grid[k];
            if (cov > max_coverage) max_coverage = cov;

            /* scan_b: danger + terrain */
            int   sx  = mx + (int)et->dx;
            int   sy  = my + (int)et->dy;
            if (!in_map(sx, sy)) continue;

            int stt = world[k] & TT_MASK;
            if (stt == TT_FOREST) forest_count++;

            float d = ctx->pill_grid[k];
            float sc = ctx->self_contrib[k];
            if (sc > 0.0f) d -= sc;
            if (d < 0.0f) d = 0.0f;

            total_danger += d;
            safe_tiles++;

            /* Terrain penalty */
            if (stt == TT_DEEPSEA) {
                terrain_penalty += 1000.0f;
            } else if (stt == TT_BUILDING || stt == TT_HALFBUILD ||
                       stt == TT_SWAMP   || stt == TT_RIVER      ||
                       stt == TT_PILLBOX) {
                terrain_penalty += 100.0f;
            } else if (ctx->pill_at[k]) {
                /* friendly pill presence — add terrain penalty */
                terrain_penalty += 100.0f;
            }
        }

        if (forest_count > 0)
            total_danger -= (float)forest_count * 0.75f;
        if (total_danger < 0.0f) total_danger = 0.0f;

        float score_a = (safe_tiles > 0) ? (total_danger / (float)safe_tiles) : 999.0f;
        float denom   = (safe_tiles > 0) ? (float)safe_tiles : 1.0f;
        float score_b = (total_danger / denom >= DEF_DANGER_HOTSPOT) ? 10.0f : 0.0f;
        float score_e = (max_coverage > 1.0f) ? 100.0f * (max_coverage - 1.0f) : 0.0f;
        float total_score = score_a + score_b + terrain_penalty + score_e;

        /* Hostile influence multiplier (calls back into Lua — only for LOS-passing angles) */
        if (phase_not_opening) {
            double infl = cpf_influence(L, mx, my);
            if (infl <= DEF_INF_THRESHOLD)
                total_score *= DEF_INF_MULT;
        }

        if (n_valid < MAX_VALID) {
            ValidSpot *vs = &valid[n_valid++];
            vs->mx    = mx;
            vs->my    = my;
            vs->cx    = cx;
            vs->cy    = cy;
            vs->deg   = deg;
            vs->score = total_score;
        }
    }

    if (n_valid == 0) return push_no_spot(L);

    /* Two-pass bucket selection */
    float min_score = valid[0].score;
    for (int i = 1; i < n_valid; i++)
        if (valid[i].score < min_score) min_score = valid[i].score;

    float bucket_floor = floorf(min_score / 50.0f) * 50.0f;
    float bucket_ceil  = bucket_floor + 50.0f;

    /* Collect bucket members */
    int bucket_idx[MAX_VALID];
    int n_bucket = 0;
    for (int i = 0; i < n_valid; i++) {
        if (valid[i].score >= bucket_floor && valid[i].score < bucket_ceil)
            bucket_idx[n_bucket++] = i;
    }

    /* Try slates 0,1,2,3 — use first where all bucket spots have finite cost */
    double costs[MAX_VALID];
    int slate_ok = 0;
    static const int slates[4] = { 0, 1, 2, 3 };

    for (int si = 0; si < 4 && !slate_ok; si++) {
        int ok = 1;
        for (int bi = 0; bi < n_bucket; bi++) {
            double c = cpf_dij_cost(L, slates[si],
                                    valid[bucket_idx[bi]].mx,
                                    valid[bucket_idx[bi]].my);
            if (c >= COST_INF) { ok = 0; break; }
            costs[bi] = c;
        }
        if (ok) slate_ok = 1;
        if (!ok && si == 3) {
            /* No slate covers all — fill with huge costs so we still pick something */
            for (int bi = 0; bi < n_bucket; bi++) costs[bi] = COST_INF;
        }
    }

    /* Pick bucket spot with lowest travel cost */
    double   best_cost  = COST_INF * 2.0;
    int      best_bi    = 0;
    for (int bi = 0; bi < n_bucket; bi++) {
        if (costs[bi] < best_cost) { best_cost = costs[bi]; best_bi = bi; }
    }

    ValidSpot *best = &valid[bucket_idx[best_bi]];
    lua_pushnumber(L,  (double)best->score);
    lua_pushinteger(L, best->mx);
    lua_pushinteger(L, best->my);
    lua_pushinteger(L, best->deg);
    return 4;
}

/*
 * gh_attack.evaluate_pill_difficulty(
 *     pmx, pmy, step_deg, phase_not_opening, tmx, tmy,
 *     self_contrib_table_or_nil)
 * -> best_score, best_mx, best_my, best_deg
 *
 * Returns math.huge, -1, -1, -1 when no valid spot is found.
 */
static int l_evaluate_pill_difficulty(lua_State *L) {
    NaAttackCtx *ctx = getCtx(L);
    if (!ctx->stamps_built) return push_no_spot(L);

    int   pmx              = (int)luaL_checkinteger(L, 1);
    int   pmy              = (int)luaL_checkinteger(L, 2);
    int   step_deg         = (int)luaL_checkinteger(L, 3);
    int   phase_not_opening = lua_toboolean(L, 4);
    int   tmx              = (int)luaL_checkinteger(L, 5);
    int   tmy              = (int)luaL_checkinteger(L, 6);

    /* World terrain pointer */
    const BYTE **wpp = brainCoreGetWorldPtrPtr(L);
    if (!wpp || !*wpp) return push_no_spot(L);
    const BYTE *world = *wpp;

    load_self_contrib(L, ctx);

    /* Avoid looking up stamp_idx 0 for step_deg != 5 when stamps are 5-deg only */
    int step = (step_deg > 0) ? step_deg : 5;

    if (ctx->world_synced)
        return eval_parity(L, ctx, world, pmx, pmy, step, phase_not_opening, tmx, tmy);
    return eval_legacy(L, ctx, world, pmx, pmy, step, phase_not_opening, tmx, tmy);
}

/* ── Registration ─────────────────────────────────────────────────────── */
static const luaL_Reg gh_attack_lib[] = {
    { "init_stamps",              l_init_stamps              },
    { "sync_pill_at",             l_sync_pill_at             },
    { "sync_grids",               l_sync_grids               },
    { "evaluate_pill_difficulty", l_evaluate_pill_difficulty },
    { NULL, NULL }
};

void naAttackRegister(lua_State *L) {
    /* Allocate the ctx as a Lua-managed userdata (the userdata storage
     * IS the ctx). Lua frees it on lua_State close; the __gc metatable
     * is a no-op today but is the hook for any sub-allocation cleanup
     * the ctx might grow later. */
    NaAttackCtx *ctx = (NaAttackCtx *)lua_newuserdata(L, sizeof(NaAttackCtx));
    memset(ctx, 0, sizeof(*ctx));
    ctx_set_cfg_defaults(ctx);

    if (luaL_newmetatable(L, "gh_attack_ctx_mt")) {
        lua_pushcfunction(L, naAttackCtxGc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);

    lua_setfield(L, LUA_REGISTRYINDEX, "gh_attack_ctx");

    luaL_newlib(L, gh_attack_lib);
    lua_setglobal(L, "gh_attack");
}
