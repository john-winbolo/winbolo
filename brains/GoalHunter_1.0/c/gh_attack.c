/*
 * gh_attack.c — C port of evaluate_pill_difficulty (detailed=false hot path)
 *
 * Replaces the per-tile Lua->C boundary crossings (~2,800 per call) with
 * direct C array reads.  The detailed=true path (plan_position / viz) is
 * kept in Lua and unchanged.
 *
 * Data flow:
 *   Brain.open  -> gh_attack.init_stamps(los, ellipse)   [once]
 *   pills change -> gh_attack.sync_pill_at(world.pill_at) [per change]
 *   threat rebuild -> gh_attack.sync_grids(pill_grid, cov_grid) [per rebuild]
 *   goal select  -> gh_attack.evaluate_pill_difficulty(...)
 */

#define _USE_MATH_DEFINES
#include "gh_attack.h"
#include "gh_threat.h"
#include "braincore.h"
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

/* Terrain types */
#define TT_DEEPSEA   0
#define TT_FOREST    2
#define TT_BUILDING  3
#define TT_PILLBOX   5
#define TT_RIVER     7
#define TT_SWAMP     8
#define TT_HALFBUILD 9
#define TT_BOAT      13

/* Impassable terrain: deepsea, building, pillbox, river, swamp, halfbuild, boat */
static const int s_impassable[16] = {
    1, 0, 0, 1,  /* 0=deepsea, 1=grass, 2=forest, 3=building */
    0, 1, 0, 1,  /* 4=crater,  5=pill,  6=road,   7=river    */
    1, 1, 0, 0,  /* 8=swamp,   9=half,  10=rubble, 11=unk    */
    0, 1, 0, 0   /* 12=tank,  13=boat,  14=?,      15=?      */
};

/* ── Default constants (override at runtime via function args) ─────────── */
#define DEF_STANDOFF        7.4f
#define DEF_SAFE_RADIUS     3.0f
#define DEF_DANGER_HOTSPOT  15.0f
#define DEF_INF_THRESHOLD   (-20.0)
#define DEF_INF_MULT        5.0f
#define COST_INF            1e29

/* ── Stamp storage ────────────────────────────────────────────────────── */
#define GH_ATK_MAX_ANGLES    72
#define GH_ATK_LOS_MAX_AIMS   5
#define GH_ATK_LOS_MAX_TILES 16
#define GH_ATK_ELL_MAX_TILES 64

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
    uint8_t  pill_at[MAP_TILES];     /* 1 = non-target pill present    */
    float    self_contrib[MAP_TILES];/* pill's own contribution (temp) */
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

/* ── Helpers ──────────────────────────────────────────────────────────── */
static inline int in_map(int x, int y) {
    return (unsigned)x < (unsigned)MAP_W && (unsigned)y < (unsigned)MAP_W;
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

/*
 * gh_attack.init_stamps(los_stamps_5deg, ellipse_stamps_5deg)
 * Traverses the Lua stamp tables once and copies into C arrays.
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

    ctx->stamps_built = 1;
    return 0;
}

/*
 * gh_attack.sync_pill_at(world_pill_at_table, target_pmx, target_pmy)
 * Rebuilds ctx->pill_at from world.pill_at, excluding the target pill tile.
 */
static int l_sync_pill_at(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int target_pmx = (int)luaL_checkinteger(L, 2);
    int target_pmy = (int)luaL_checkinteger(L, 3);
    int target_key = target_pmy * MAP_W + target_pmx;

    NaAttackCtx *ctx = requireStampsBuilt(L);
    memset(ctx->pill_at, 0, sizeof(ctx->pill_at));

    lua_pushnil(L);
    while (lua_next(L, 1)) {
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
    if (!ctx->stamps_built) {
        lua_pushnumber(L, 1e30);
        lua_pushinteger(L, -1);
        lua_pushinteger(L, -1);
        lua_pushinteger(L, -1);
        return 4;
    }

    int   pmx              = (int)luaL_checkinteger(L, 1);
    int   pmy              = (int)luaL_checkinteger(L, 2);
    int   step_deg         = (int)luaL_checkinteger(L, 3);
    int   phase_not_opening = lua_toboolean(L, 4);
    int   tmx              = (int)luaL_checkinteger(L, 5);
    int   tmy              = (int)luaL_checkinteger(L, 6);
    /* arg 7: self_contrib table or nil */
    int   has_self_contrib  = lua_istable(L, 7);

    /* World terrain pointer */
    const BYTE **wpp = brainCoreGetWorldPtrPtr(L);
    if (!wpp || !*wpp) {
        lua_pushnumber(L, 1e30);
        lua_pushinteger(L, -1);
        lua_pushinteger(L, -1);
        lua_pushinteger(L, -1);
        return 4;
    }
    const BYTE *world = *wpp;

    /* Build self_contrib from Lua table */
    memset(ctx->self_contrib, 0, sizeof(ctx->self_contrib));
    if (has_self_contrib) {
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

    float R       = DEF_STANDOFF;
    float pcx     = (float)pmx + 0.5f;
    float pcy     = (float)pmy + 0.5f;

    ValidSpot valid[MAX_VALID];
    int       n_valid = 0;

    /* Avoid looking up stamp_idx 0 for step_deg != 5 when stamps are 5-deg only */
    int step = (step_deg > 0) ? step_deg : 5;

    for (int deg = 0; deg < 360; deg += step) {
        /* Cooperative abort. Per-degree is the natural checkpoint —
         * each iteration runs an independent LOS+ellipse scan and
         * appends to `valid[]`. On hit we fall through to the
         * post-loop block; if any spots were collected the brain
         * gets the best of them, otherwise the no-valid path returns
         * the existing 1e30 sentinel which it already handles. */
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

    if (n_valid == 0) {
        lua_pushnumber(L, 1e30);
        lua_pushinteger(L, -1);
        lua_pushinteger(L, -1);
        lua_pushinteger(L, -1);
        return 4;
    }

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

    if (luaL_newmetatable(L, "gh_attack_ctx_mt")) {
        lua_pushcfunction(L, naAttackCtxGc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);

    lua_setfield(L, LUA_REGISTRYINDEX, "gh_attack_ctx");

    luaL_newlib(L, gh_attack_lib);
    lua_setglobal(L, "gh_attack");
}
