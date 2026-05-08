/*
 * na_shield_stamp.c — precomputed shield-scan lookup table + full C scan
 *
 * Binary file layout (version 2):
 *   ShieldStampHeader  (32 bytes)
 *   ShieldStampEntry   [n_angles * n_nudges]  (240 bytes each)
 *
 * Entry index: angle_key * n_nudges + nudge_idx
 *
 * Each ShieldStampEntry holds 6 ShieldShotPath structs:
 *   aim[0..4]    — tank → aim point (5 outgoing AIM_OFFSETS)
 *   pill_to_tank — pill → tank standoff (return fire)
 *
 * Each ShieldShotPath (40 bytes):
 *   source (int16 x,y) — shooter world-unit offset from pill_wx/pill_wy
 *   target (int16 x,y) — aim-point world-unit offset from pill_wx/pill_wy
 *   has_pill (uint8)   — shot reaches the pill tile
 *   n_tiles  (uint8)   — entries used in tiles[] (max 15)
 *   tiles[15] (int8 x,y) — path tile offsets from pill tile (signed)
 *
 * Per-brain working state lives in NaShieldStampCtx, allocated as a
 * Lua userdata in naShieldStampRegister and looked up via getCtx(L)
 * at the top of each binding. The disk-loaded entries[] table is
 * shared read-only across brains; first caller into l_load wins a
 * CAS and performs the read, losers spin until a result flag flips.
 */

#include "na_shield_stamp.h"
#include "na_threat.h"
#include <SDL3/SDL.h>
#include <lauxlib.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Binary format ────────────────────────────────────────────────── */
#define STAMP_MAGIC         0x444C4853u   /* "SHLD" little-endian */
#define STAMP_VERSION       2u
#define STAMP_MAX_ANGLES    1440
#define STAMP_MAX_NUDGES    33   /* nudge steps 0..32 inclusive = 33 entries */
#define STAMP_MAX_TILES     15
#define STAMP_N_AIMS        5             /* outgoing aims */
#define STAMP_N_PATHS       6             /* 5 outgoing + 1 return fire */

#pragma pack(push, 1)
typedef struct { int8_t  x, y; } ShieldCoord;
typedef struct { int16_t x, y; } ShieldPreciseCoord;

typedef struct {
    ShieldPreciseCoord source;        /*  4 bytes */
    ShieldPreciseCoord target;        /*  4 bytes */
    uint8_t            has_pill;      /*  1 byte  */
    uint8_t            n_tiles;       /*  1 byte  */
    ShieldCoord        tiles[15];     /* 30 bytes */
                                      /* = 40 bytes total */
} ShieldShotPath;

typedef struct {
    ShieldShotPath aim[5];            /* 5 * 40 = 200 bytes */
    ShieldShotPath pill_to_tank;      /*     40 bytes */
                                      /* = 240 bytes total */
} ShieldStampEntry;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t n_angles;
    uint32_t n_nudges;
    uint32_t nudge_step_wu;
    float    standoff;
    float    aim_inset;
    float    step_deg;
                          /* = 32 bytes */
} ShieldStampHeader;
#pragma pack(pop)

/* ── Slate / scan working types ───────────────────────────────────── */
#define SLATE_MAX_CANDS    32
#define SLATE_MAX_NUDGES   33   /* 0=base, 1..32=nudge steps */
#define SLATE_MAX_AIMS      5
#define SLATE_MAX_BLOCKERS  5
#define RESULT_STRIDE      29   /* per candidate: 9 scalars + 5*2 actual + 5*2 potential */

typedef struct {
    uint8_t n_actual;
    uint8_t n_potential;
    uint8_t blocked;
    uint8_t _pad;
    int8_t  actual_dx[SLATE_MAX_BLOCKERS];
    int8_t  actual_dy[SLATE_MAX_BLOCKERS];
    int8_t  potential_dx[SLATE_MAX_BLOCKERS];
    int8_t  potential_dy[SLATE_MAX_BLOCKERS];
    /* = 4 + 10 + 10 = 24 bytes */
} SlateEntry;

typedef struct {
    int    best_aim;       /* 1-based, 0 = none */
    double best_total;
    double best_score_nb, best_score_act, best_score_pot;
    int    best_chain, best_bcnt, best_n_act, best_n_pot;
    int8_t best_adx[SLATE_MAX_BLOCKERS], best_ady[SLATE_MAX_BLOCKERS];
    int8_t best_pdx[SLATE_MAX_BLOCKERS], best_pdy[SLATE_MAX_BLOCKERS];
} NbResult;

#define SCAN_MAX_CANDS   32   /* 1 standoff + 28 ring + headroom */
#define SCAN_N_AIMS       5
#define SCAN_MAX_BLOCKERS 5

typedef struct {
    int8_t dx[SCAN_MAX_BLOCKERS];
    int8_t dy[SCAN_MAX_BLOCKERS];
    int     n;
} BlockerList;

typedef struct {
    int8_t  dx[STAMP_MAX_TILES];
    int8_t  dy[STAMP_MAX_TILES];
    int      n;
} OutgoingSet;

typedef struct {
    float   cx, cy;    /* tile-space float position */
    int     mx, my;    /* tile coords */
    float   deg;
    int     ak;        /* angle key */
    int     valid;
} ScanCand;

typedef struct {
    int          blocked;
    int          nudge_step;   /* 0=no nudge, 1..32 */
    float        score;
    BlockerList  actual;
    BlockerList  potential;
} ScanAimResult;

typedef struct {
    ScanCand      cand;
    ScanAimResult aims[SCAN_N_AIMS];
    int           best_aim;   /* 0-based, -1=none */
    float         best_score;
} ScanResult;

/* Per-call HP-dependent neighbor-bonus tunables. Built on the stack
 * inside l_scan_c / l_run_neighbor_bonus and threaded by const-pointer
 * into run_nb_for_cand — never stored on the ctx, never global. */
typedef struct {
    int    n_fav;
    int    fav_size[2];
    double fav_bon[2];
    int    min_chain;
    double max_bonus;
} ScanCfg;

/* ── Per-brain state ──────────────────────────────────────────────── */
typedef struct NaShieldStampCtx {
    int cfg_done;

    /* Tunables — set by configure_scan(). */
    int     t_building;
    int     t_halfbuild;
    uint8_t non_build[16];      /* non_build[tt] = 1 if non-buildable terrain */
    float   approach_offset;
    float   blocker_min_dist;
    double  score_per_slot;
    double  built_bonus;
    double  neighbor_bonus;

    /* Stored Lua ref to cpf.lgm_travel_ticks_map for LGM reachability check.
     * Per-state because a LUA_REGISTRYINDEX ref is only valid in the
     * lua_State that issued it. */
    int     lgm_ref;

    /* Per-scan slate state — filled by scan_c / slate_set, read by
     * run_nb_for_cand. */
    SlateEntry slate[SLATE_MAX_CANDS][SLATE_MAX_NUDGES][SLATE_MAX_AIMS];
    uint8_t    slate_ni_used[SLATE_MAX_CANDS][SLATE_MAX_AIMS];
    int        slate_ncands;

    /* Per-scan candidate scoring buffer. memset at the top of every
     * l_scan_c so prior content is overwritten before any read. */
    ScanResult scan_buf[SCAN_MAX_CANDS];
} NaShieldStampCtx;

/* ── Shared read-only stamp data ──────────────────────────────────── */
/* Loaded once from disk via the atomic-guarded l_load below. After
 * s_entries_state observes 1, these are read-only for the rest of
 * process lifetime. */
static ShieldStampEntry *s_entries    = NULL;
static int               s_n_angles   = 0;
static int               s_n_nudges   = 0;
static int               s_nudge_step = 0;  /* wu per nudge index */

/* Atomic guards for the one-shot disk load.
 *   s_entries_loading: 0 = unclaimed, 1 = a thread won the CAS and is loading.
 *   s_entries_state:   0 = pending, 1 = loaded successfully, 2 = load failed. */
static SDL_AtomicInt s_entries_loading = { 0 };
static SDL_AtomicInt s_entries_state   = { 0 };

static int entries_ready(void) {
    return SDL_GetAtomicInt(&s_entries_state) == 1;
}

/* ── Ctx lookup ───────────────────────────────────────────────────── */

static int naShieldStampCtxGc(lua_State *L) {
    NaShieldStampCtx *ctx = (NaShieldStampCtx *)lua_touserdata(L, 1);
    if (ctx && ctx->lgm_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ctx->lgm_ref);
        ctx->lgm_ref = LUA_NOREF;
    }
    return 0;
}

static NaShieldStampCtx *getCtx(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "na_shield_stamp_ctx");
    NaShieldStampCtx *ctx = (NaShieldStampCtx *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!ctx) luaL_error(L, "na_shield_stamp: ctx not initialized");
    return ctx;
}

static NaShieldStampCtx *requireConfigured(lua_State *L) {
    NaShieldStampCtx *ctx = getCtx(L);
    if (!ctx->cfg_done) luaL_error(L, "na_shield_stamp: configure_scan() must be called first");
    return ctx;
}

/* ── Helpers ──────────────────────────────────────────────────────── */

static int nudge_wu_to_idx(int wu) {
    if (s_nudge_step <= 0) return -1;
    if (wu % s_nudge_step != 0) return -1;
    int idx = wu / s_nudge_step;
    if (idx < 0 || idx >= s_n_nudges) return -1;
    return idx;
}

static int clamp_angle_key(int ak) {
    if (ak < 0) ak = 0;
    if (ak >= s_n_angles) ak = s_n_angles - 1;
    return ak;
}

/* ── Load ─────────────────────────────────────────────────────────── */

/* Build the per-lua_State na_shield.pill_hit lookup table from the
 * already-loaded shared s_entries[]. Caller has confirmed entries_ready(). */
static void publish_pill_hit_table(lua_State *L) {
    size_t n = (size_t)s_n_angles * (size_t)s_n_nudges;

    /* Lua convention: aim_idx 0=return fire, 1..5=outgoing aims.
     * Index (1-based): (angle_key * n_nudges + nudge_idx) * STAMP_N_PATHS + aim_idx + 1 */
    lua_Integer total = (lua_Integer)n * STAMP_N_PATHS;
    lua_createtable(L, (int)total, 0);
    for (size_t ei = 0; ei < n; ei++) {
        const ShieldStampEntry *e = &s_entries[ei];
        lua_Integer base = (lua_Integer)ei * STAMP_N_PATHS;
        lua_pushboolean(L, e->pill_to_tank.has_pill);
        lua_rawseti(L, -2, base + 1);
        for (int ai = 0; ai < STAMP_N_AIMS; ai++) {
            lua_pushboolean(L, e->aim[ai].has_pill);
            lua_rawseti(L, -2, base + ai + 2);
        }
    }
    lua_getglobal(L, "na_shield");
    lua_insert(L, -2);
    lua_setfield(L, -2, "pill_hit");
    lua_pop(L, 1);
}

/* Disk read for the winner of the load CAS. Returns 1 on success
 * (s_entries / sizes populated), 0 on failure (sizes left at 0). */
static int do_disk_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    ShieldStampHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return 0; }
    if (hdr.magic   != STAMP_MAGIC   ||
        hdr.version != STAMP_VERSION ||
        hdr.n_angles == 0 || hdr.n_nudges == 0 ||
        hdr.n_angles > STAMP_MAX_ANGLES ||
        hdr.n_nudges > STAMP_MAX_NUDGES) {
        fclose(f); return 0;
    }

    size_t n = (size_t)hdr.n_angles * hdr.n_nudges;
    ShieldStampEntry *buf = (ShieldStampEntry *)malloc(n * sizeof(ShieldStampEntry));
    if (!buf) { fclose(f); return 0; }

    if (fread(buf, sizeof(ShieldStampEntry), n, f) != n) {
        free(buf); fclose(f); return 0;
    }
    fclose(f);

    s_entries    = buf;
    s_n_angles   = (int)hdr.n_angles;
    s_n_nudges   = (int)hdr.n_nudges;
    s_nudge_step = (int)hdr.nudge_step_wu;
    return 1;
}

static int l_load(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);

    int state = SDL_GetAtomicInt(&s_entries_state);
    if (state == 0) {
        if (SDL_CompareAndSwapAtomicInt(&s_entries_loading, 0, 1)) {
            /* Winner: do the actual disk read, then publish the result.
             * Losers are spinning on s_entries_state; we MUST set it
             * before returning so they can make progress. */
            int ok = do_disk_load(path);
            SDL_SetAtomicInt(&s_entries_state, ok ? 1 : 2);
        } else {
            /* Loser: spin until the winner publishes a terminal result. */
            do {
                state = SDL_GetAtomicInt(&s_entries_state);
            } while (state == 0);
        }
        state = SDL_GetAtomicInt(&s_entries_state);
    }

    if (state != 1) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* The pill_hit Lua table is per-lua_State (it lives on this state's
     * na_shield global), so build it here for every successful caller. */
    publish_pill_hit_table(L);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_is_loaded(lua_State *L) {
    lua_pushboolean(L, entries_ready());
    return 1;
}

/* na_shield.get_flat(angle_key, nudge_wu, aim_idx) -> flat {dx1,dy1,...} or nil
 * aim_idx: 0..4 = outgoing, 5 = return fire (pill_to_tank) */
static int l_get_flat(lua_State *L) {
    if (!entries_ready()) { lua_pushnil(L); return 1; }
    int angle_key = (int)luaL_checkinteger(L, 1);
    int nudge_wu  = (int)luaL_checkinteger(L, 2);
    int aim_idx   = (int)luaL_checkinteger(L, 3);
    int ni = nudge_wu_to_idx(nudge_wu);
    /* aim_idx Lua convention: 0=return fire (pill_to_tank), 1..5=outgoing aims */
    if (ni < 0 || angle_key < 0 || angle_key >= s_n_angles ||
        aim_idx < 0 || aim_idx > STAMP_N_AIMS) {
        lua_pushnil(L); return 1;
    }
    const ShieldStampEntry *e = &s_entries[angle_key * s_n_nudges + ni];
    const ShieldShotPath *p = (aim_idx == 0) ? &e->pill_to_tank
                                              : &e->aim[aim_idx - 1];
    int n = (int)p->n_tiles;
    if (n == 0) { lua_pushnil(L); return 1; }
    lua_createtable(L, n * 2, 0);
    for (int i = 0; i < n; i++) {
        lua_pushinteger(L, (lua_Integer)p->tiles[i].x);
        lua_rawseti(L, -2, 2 * i + 1);
        lua_pushinteger(L, (lua_Integer)p->tiles[i].y);
        lua_rawseti(L, -2, 2 * i + 2);
    }
    return 1;
}

/* ── Slate ────────────────────────────────────────────────────────── */

static int l_slate_clear(lua_State *L) {
    NaShieldStampCtx *ctx = getCtx(L);
    int n = (int)luaL_checkinteger(L, 1);
    if (n > SLATE_MAX_CANDS) n = SLATE_MAX_CANDS;
    ctx->slate_ncands = n;
    memset(ctx->slate,         0, sizeof(ctx->slate));
    memset(ctx->slate_ni_used, 0, sizeof(ctx->slate_ni_used));
    return 0;
}

/* na_shield.slate_set(ci, ni, ai, blocked,
 *   n_act, dx1,dy1, dx2,dy2, dx3,dy3, dx4,dy4, dx5,dy5,
 *   n_pot, pdx1,pdy1, pdx2,pdy2, pdx3,pdy3, pdx4,pdy4, pdx5,pdy5) */
static int l_slate_set(lua_State *L) {
    NaShieldStampCtx *ctx = getCtx(L);
    int ci = (int)luaL_checkinteger(L, 1) - 1;
    int ni = (int)luaL_checkinteger(L, 2);
    int ai = (int)luaL_checkinteger(L, 3);
    if (ci < 0 || ci >= SLATE_MAX_CANDS ||
        ni < 0 || ni >= SLATE_MAX_NUDGES ||
        ai < 0 || ai >= SLATE_MAX_AIMS) return 0;
    SlateEntry *e = &ctx->slate[ci][ni][ai];
    e->blocked    = (uint8_t)lua_toboolean(L, 4);
    e->n_actual   = (uint8_t)luaL_checkinteger(L, 5);
    for (int k = 0; k < SLATE_MAX_BLOCKERS; k++) {
        e->actual_dx[k] = (int8_t)luaL_checkinteger(L, 6 + k * 2);
        e->actual_dy[k] = (int8_t)luaL_checkinteger(L, 7 + k * 2);
    }
    int pot_base  = 6 + SLATE_MAX_BLOCKERS * 2;
    e->n_potential = (uint8_t)luaL_checkinteger(L, pot_base);
    for (int k = 0; k < SLATE_MAX_BLOCKERS; k++) {
        e->potential_dx[k] = (int8_t)luaL_checkinteger(L, pot_base + 1 + k * 2);
        e->potential_dy[k] = (int8_t)luaL_checkinteger(L, pot_base + 2 + k * 2);
    }
    return 0;
}

static int l_slate_set_nudge_used(lua_State *L) {
    NaShieldStampCtx *ctx = getCtx(L);
    int ci = (int)luaL_checkinteger(L, 1) - 1;
    int ai = (int)luaL_checkinteger(L, 2);
    int ni = (int)luaL_checkinteger(L, 3);
    if (ci >= 0 && ci < SLATE_MAX_CANDS &&
        ai >= 0 && ai < SLATE_MAX_AIMS  &&
        ni >= 0 && ni < SLATE_MAX_NUDGES)
        ctx->slate_ni_used[ci][ai] = (uint8_t)ni;
    return 0;
}

static int covers_subset(const NaShieldStampCtx *ctx, int j, int ni, int ai,
                          const int8_t *sub_dx, const int8_t *sub_dy, int sub_n) {
    if (j < 0 || j >= ctx->slate_ncands) return 0;
    const SlateEntry *e = &ctx->slate[j][ni][ai];
    if (e->blocked) return 0;
    for (int k = 0; k < sub_n; k++) {
        int found = 0;
        for (int m = 0; !found && m < (int)e->n_actual && m < SLATE_MAX_BLOCKERS; m++)
            found = (e->actual_dx[m] == sub_dx[k] && e->actual_dy[m] == sub_dy[k]);
        for (int m = 0; !found && m < (int)e->n_potential && m < SLATE_MAX_BLOCKERS; m++)
            found = (e->potential_dx[m] == sub_dx[k] && e->potential_dy[m] == sub_dy[k]);
        if (!found) return 0;
    }
    return 1;
}

/* Internal neighbor bonus used by both l_run_neighbor_bonus and scan_c.
 * Writes RESULT_STRIDE values per candidate into out[] (caller provides). */
static void run_nb_for_cand(const NaShieldStampCtx *ctx, const ScanCfg *cfg,
                             int i, int n_cands, int n_aims,
                             double score_per_slot, double built_bonus,
                             double neighbor_bonus,
                             NbResult *out) {
    out->best_aim   = 0;
    out->best_total = -1.0;
    out->best_chain = 0; out->best_bcnt = 0;
    out->best_n_act = 0; out->best_n_pot = 0;
    out->best_score_nb = 0; out->best_score_act = 0; out->best_score_pot = 0;
    memset(out->best_adx, 0, sizeof(out->best_adx));
    memset(out->best_ady, 0, sizeof(out->best_ady));
    memset(out->best_pdx, 0, sizeof(out->best_pdx));
    memset(out->best_pdy, 0, sizeof(out->best_pdy));

    for (int ai = 0; ai < n_aims; ai++) {
        int ni = (int)ctx->slate_ni_used[i][ai];
        const SlateEntry *e = &ctx->slate[i][ni][ai];
        if (e->blocked) continue;
        int na = (int)e->n_actual, np = (int)e->n_potential;
        int n_tot = na + np;
        if (n_tot == 0) continue;

        int8_t all_dx[SLATE_MAX_BLOCKERS * 2], all_dy[SLATE_MAX_BLOCKERS * 2];
        int    all_act[SLATE_MAX_BLOCKERS * 2];
        int n_all = 0;
        for (int k = 0; k < na && k < SLATE_MAX_BLOCKERS; k++) {
            all_dx[n_all] = e->actual_dx[k]; all_dy[n_all] = e->actual_dy[k];
            all_act[n_all++] = 1;
        }
        for (int k = 0; k < np && k < SLATE_MAX_BLOCKERS; k++) {
            all_dx[n_all] = e->potential_dx[k]; all_dy[n_all] = e->potential_dy[k];
            all_act[n_all++] = 0;
        }

        int n_iter   = n_all < SLATE_MAX_BLOCKERS ? n_all : SLATE_MAX_BLOCKERS;
        int mask_max = (1 << n_iter) - 1;

        for (int mask = 1; mask <= mask_max; mask++) {
            int8_t sub_dx[SLATE_MAX_BLOCKERS], sub_dy[SLATE_MAX_BLOCKERS];
            int    sub_act[SLATE_MAX_BLOCKERS];
            int sub_n = 0, sub_na = 0, sub_np = 0;
            for (int idx = 0; idx < n_iter; idx++) {
                if (mask & (1 << idx)) {
                    sub_dx[sub_n] = all_dx[idx]; sub_dy[sub_n] = all_dy[idx];
                    sub_act[sub_n] = all_act[idx];
                    if (all_act[idx]) sub_na++; else sub_np++;
                    sub_n++;
                }
            }

            double aim_score = (score_per_slot + built_bonus) * sub_na
                             +  score_per_slot                 * sub_np;

            int left = 0;
            for (int j = i - 1; j >= 0; j--) {
                if (!covers_subset(ctx, j, ni, ai, sub_dx, sub_dy, sub_n)) break;
                left++;
            }
            int right = 0;
            for (int j = i + 1; j < n_cands; j++) {
                if (!covers_subset(ctx, j, ni, ai, sub_dx, sub_dy, sub_n)) break;
                right++;
            }
            int sym   = left < right ? left : right;
            int chain = sym * 2;

            double bias = 0.0;
            int    sz   = sub_na + sub_np;
            for (int fi = 0; fi < cfg->n_fav && fi < 2; fi++)
                if (sz == cfg->fav_size[fi]) bias += cfg->fav_bon[fi];
            if (cfg->min_chain > 0 && chain < cfg->min_chain)
                bias -= cfg->max_bonus * 100.0;

            double total = aim_score + chain * neighbor_bonus + bias;
            if (total > out->best_total) {
                out->best_total    = total;
                out->best_aim      = ai + 1;
                out->best_chain    = chain;
                out->best_n_act    = sub_na; out->best_n_pot = sub_np;
                out->best_bcnt     = sub_na + sub_np;
                out->best_score_act = (score_per_slot + built_bonus) * sub_na;
                out->best_score_pot =  score_per_slot * sub_np;
                out->best_score_nb  = chain * neighbor_bonus;
                int ao = 0, po = 0;
                memset(out->best_adx, 0, sizeof(out->best_adx));
                memset(out->best_ady, 0, sizeof(out->best_ady));
                memset(out->best_pdx, 0, sizeof(out->best_pdx));
                memset(out->best_pdy, 0, sizeof(out->best_pdy));
                for (int k = 0; k < sub_n; k++) {
                    if (sub_act[k]) {
                        out->best_adx[ao] = sub_dx[k];
                        out->best_ady[ao++] = sub_dy[k];
                    } else {
                        out->best_pdx[po] = sub_dx[k];
                        out->best_pdy[po++] = sub_dy[k];
                    }
                }
            }
        }
    }
}

static int l_run_neighbor_bonus(lua_State *L) {
    NaShieldStampCtx *ctx = getCtx(L);
    int    n_cands        = (int)luaL_checkinteger(L, 1);
    int    n_aims         = (int)luaL_checkinteger(L, 2);
    double score_per_slot = luaL_checknumber(L, 3);
    double built_bonus    = luaL_checknumber(L, 4);
    double neighbor_bonus = luaL_checknumber(L, 5);

    ScanCfg cfg;
    cfg.n_fav        = (int)luaL_checkinteger(L, 6);
    cfg.fav_size[0]  = (int)luaL_checkinteger(L, 7);
    cfg.fav_bon[0]   = luaL_checknumber(L, 8);
    cfg.fav_size[1]  = (int)luaL_checkinteger(L, 9);
    cfg.fav_bon[1]   = luaL_checknumber(L, 10);
    cfg.min_chain    = (int)luaL_checkinteger(L, 11);
    cfg.max_bonus    = luaL_checknumber(L, 12);

    if (n_cands > SLATE_MAX_CANDS) n_cands = SLATE_MAX_CANDS;
    if (n_aims  > SLATE_MAX_AIMS)  n_aims  = SLATE_MAX_AIMS;

    lua_createtable(L, n_cands * RESULT_STRIDE, 0);
    int out = 1;
    NbResult r;

    for (int i = 0; i < n_cands; i++) {
        run_nb_for_cand(ctx, &cfg, i, n_cands, n_aims,
                        score_per_slot, built_bonus, neighbor_bonus, &r);

        lua_pushinteger(L, r.best_aim);                               lua_rawseti(L,-2,out++);
        lua_pushnumber (L, r.best_aim ? r.best_total : 0.0);         lua_rawseti(L,-2,out++);
        lua_pushnumber (L, r.best_score_nb);                         lua_rawseti(L,-2,out++);
        lua_pushinteger(L, r.best_chain);                            lua_rawseti(L,-2,out++);
        lua_pushnumber (L, r.best_score_act);                        lua_rawseti(L,-2,out++);
        lua_pushnumber (L, r.best_score_pot);                        lua_rawseti(L,-2,out++);
        lua_pushinteger(L, r.best_bcnt);                             lua_rawseti(L,-2,out++);
        lua_pushinteger(L, r.best_n_act);                            lua_rawseti(L,-2,out++);
        lua_pushinteger(L, r.best_n_pot);                            lua_rawseti(L,-2,out++);
        for (int k = 0; k < SLATE_MAX_BLOCKERS; k++) {
            lua_pushinteger(L, r.best_adx[k]); lua_rawseti(L,-2,out++);
            lua_pushinteger(L, r.best_ady[k]); lua_rawseti(L,-2,out++);
        }
        for (int k = 0; k < SLATE_MAX_BLOCKERS; k++) {
            lua_pushinteger(L, r.best_pdx[k]); lua_rawseti(L,-2,out++);
            lua_pushinteger(L, r.best_pdy[k]); lua_rawseti(L,-2,out++);
        }
    }
    return 1;
}

/* ── scan_c configuration ─────────────────────────────────────────── */

/* na_shield.set_lgm_func(fn) — store reference to lgm path function */
static int l_set_lgm_func(lua_State *L) {
    NaShieldStampCtx *ctx = getCtx(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (ctx->lgm_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ctx->lgm_ref);
    lua_pushvalue(L, 1);
    ctx->lgm_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

static void reset_ctx_state(lua_State *L, NaShieldStampCtx *ctx) {
    /* Tunables — back to the same defaults as the original file-static
     * initializers so a reconfigure starts from a known baseline. */
    ctx->t_building       = 1;
    ctx->t_halfbuild      = 2;
    memset(ctx->non_build, 0, sizeof(ctx->non_build));
    ctx->approach_offset  = 1.5f;
    ctx->blocker_min_dist = 1.0f;
    ctx->score_per_slot   = 10.0;
    ctx->built_bonus      = 3.0;
    ctx->neighbor_bonus   = 2.0;

    /* Working state — slate is filled per-scan; scan_buf is memset at
     * the top of every l_scan_c, so it's effectively self-clearing.
     * Zero them here too so a hot-reload starts from a clean slate
     * in case the reload happens between scans. */
    memset(ctx->slate,         0, sizeof(ctx->slate));
    memset(ctx->slate_ni_used, 0, sizeof(ctx->slate_ni_used));
    ctx->slate_ncands = 0;
    memset(ctx->scan_buf, 0, sizeof(ctx->scan_buf));

    /* Drop any registered lgm callback — the brain will re-register
     * via set_lgm_func after a fresh configure_scan. */
    if (ctx->lgm_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ctx->lgm_ref);
        ctx->lgm_ref = LUA_NOREF;
    }
}

/* na_shield.configure_scan(tbl) — set terrain/scoring constants */
static int l_configure_scan(lua_State *L) {
    NaShieldStampCtx *ctx = getCtx(L);
    luaL_checktype(L, 1, LUA_TTABLE);

    /* Reset every per-brain field before applying the new config so
     * second-and-later calls produce the same end state as a first call. */
    reset_ctx_state(L, ctx);

#define READ_INT(k, dst) \
    lua_getfield(L, 1, k); if (!lua_isnil(L,-1)) dst = (int)lua_tointeger(L,-1); lua_pop(L,1)
#define READ_FLT(k, dst) \
    lua_getfield(L, 1, k); if (!lua_isnil(L,-1)) dst = (float)lua_tonumber(L,-1); lua_pop(L,1)
#define READ_DBL(k, dst) \
    lua_getfield(L, 1, k); if (!lua_isnil(L,-1)) dst = (double)lua_tonumber(L,-1); lua_pop(L,1)

    READ_INT("T_BUILDING",  ctx->t_building);
    READ_INT("T_HALFBUILD", ctx->t_halfbuild);
    READ_FLT("APPROACH_OFFSET",   ctx->approach_offset);
    READ_FLT("BLOCKER_MIN_DIST",  ctx->blocker_min_dist);
    READ_DBL("SCORE_PER_SLOT",    ctx->score_per_slot);
    READ_DBL("BUILT_BONUS",       ctx->built_bonus);
    READ_DBL("NEIGHBOR_BONUS",    ctx->neighbor_bonus);

    /* NON_BUILDABLE: array of terrain type ints */
    lua_getfield(L, 1, "NON_BUILDABLE");
    if (lua_istable(L, -1)) {
        int len = (int)lua_rawlen(L, -1);
        for (int i = 1; i <= len; i++) {
            lua_rawgeti(L, -1, i);
            int tt = (int)lua_tointeger(L, -1);
            lua_pop(L, 1);
            if (tt >= 0 && tt < 16) ctx->non_build[tt] = 1;
        }
    }
    lua_pop(L, 1);
#undef READ_INT
#undef READ_FLT
#undef READ_DBL

    ctx->cfg_done = 1;
    return 0;
}

/* ── scan_c internals ─────────────────────────────────────────────── */

static int lgm_cache_idx(int dx, int dy) {
    int x = dx + 8, y = dy + 8;
    if (x < 0 || x > 16 || y < 0 || y > 16) return -1;
    return x * 17 + y;
}

/* Check LGM reachability with lazy cache (caller-owned).
 * smx/smy = LGM origin tile, tmx/tmy = target tile, pmx/pmy = pill tile.
 * lgm_reach[17*17] is the per-scan reachability cache; -1=unchecked,
 * 0=unreachable, 1=reachable. */
static int lgm_reachable(lua_State *L, const NaShieldStampCtx *ctx,
                         int8_t *lgm_reach,
                         int smx, int smy,
                         int tmx, int tmy, int pmx, int pmy) {
    /* Adjacent to origin: always reachable */
    if (abs(tmx - smx) + abs(tmy - smy) <= 1) return 1;

    int ci = lgm_cache_idx(tmx - pmx, tmy - pmy);
    if (ci >= 0 && lgm_reach[ci] != -1) return lgm_reach[ci];

    /* Call stored Lua function */
    int result = 1;  /* default: reachable if function not set */
    if (ctx->lgm_ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->lgm_ref);
        lua_pushinteger(L, smx);  lua_pushinteger(L, smy);
        lua_pushinteger(L, tmx);  lua_pushinteger(L, tmy);
        lua_pushinteger(L, 0);    lua_pushinteger(L, 0);   /* blessX, blessY */
        lua_pushinteger(L, 2000); lua_pushinteger(L, 150); /* maxTicks, stuckTicks */
        lua_call(L, 8, 1);
        result = (lua_tointeger(L, -1) != -1) ? 1 : 0;
        lua_pop(L, 1);
    }

    if (ci >= 0) lgm_reach[ci] = (int8_t)result;
    return result;
}

static int in_outgoing(const OutgoingSet *os, int8_t dx, int8_t dy) {
    for (int i = 0; i < os->n; i++)
        if (os->dx[i] == dx && os->dy[i] == dy) return 1;
    return 0;
}

/* Score one aim for one candidate using stamp tiles.
 * Returns 1 if this aim is usable (not blocked), 0 if blocked.
 * approach_mx/my = LGM dispatch origin for reachability check.
 * pmx/pmy = pill tile. pill_wx/wy = pill world position.
 * pill_map[17*17]: 0=none, 1=hostile/neutral, 2=friendly (centred on pill). */
static void score_one_aim(lua_State *L,
                          const NaShieldStampCtx *ctx,
                          int8_t *lgm_reach,
                          const ShieldShotPath *out_path,
                          const ShieldShotPath *ret_path,
                          int pmx, int pmy,
                          float standoff_cx, float standoff_cy,
                          int approach_mx, int approach_my,
                          const uint8_t *pill_map,
                          int target_is_pill,
                          int no_builder,
                          ScanAimResult *res) {
    res->blocked = 0;
    res->actual.n = 0;
    res->potential.n = 0;

    /* Walk outgoing path: detect wall/pill block, build outgoing set */
    OutgoingSet os = { .n = 0 };
    for (int i = 0; i < (int)out_path->n_tiles; i++) {
        int8_t dx = out_path->tiles[i].x;
        int8_t dy = out_path->tiles[i].y;
        if (dx == 0 && dy == 0) break;  /* reached pill tile */
        int tx = pmx + dx, ty = pmy + dy;
        int tt = naThreatRawTT(tx, ty);
        if (tt == ctx->t_building || tt == ctx->t_halfbuild) { res->blocked = 1; return; }
        /* Non-target pill in outgoing path blocks the aim */
        int ci = lgm_cache_idx(dx, dy);
        if (ci >= 0) {
            uint8_t pm = pill_map[ci];
            if (pm != 0 && !target_is_pill) { res->blocked = 1; return; }
        }
        if (os.n < STAMP_MAX_TILES) {
            os.dx[os.n] = dx; os.dy[os.n] = dy; os.n++;
        }
    }

    /* Distance gate: pill_to_standoff_d in tiles */
    float pdtx = standoff_cx - (pmx + 0.5f);
    float pdty = standoff_cy - (pmy + 0.5f);
    float pill_to_standoff_d = sqrtf(pdtx * pdtx + pdty * pdty);

    float scx = standoff_cx, scy = standoff_cy;

    /* Walk return-fire path: classify blockers */
    for (int i = 0; i < (int)ret_path->n_tiles; i++) {
        int8_t dx = ret_path->tiles[i].x;
        int8_t dy = ret_path->tiles[i].y;
        int tx = pmx + dx, ty = pmy + dy;
        /* Skip pill tile and standoff tile */
        if (dx == 0 && dy == 0) continue;
        if (tx == (int)scx && ty == (int)scy) continue;
        /* Skip tiles on the outgoing path */
        if (in_outgoing(&os, dx, dy)) continue;
        if (tx < 0 || tx > 255 || ty < 0 || ty > 255) continue;

        /* Distance gate */
        float pdx = tx + 0.5f - (pmx + 0.5f);
        float pdy = ty + 0.5f - (pmy + 0.5f);
        float pdist = sqrtf(pdx * pdx + pdy * pdy);
        if (pdist >= pill_to_standoff_d) continue;

        /* Minimum standoff distance */
        float sdx = tx + 0.5f - scx, sdy = ty + 0.5f - scy;
        float d_standoff = sqrtf(sdx * sdx + sdy * sdy);
        if (d_standoff < ctx->blocker_min_dist) continue;

        /* Classify: actual (friendly pill or wall) vs potential (buildable) */
        int ci = lgm_cache_idx(dx, dy);
        uint8_t pm_val = (ci >= 0) ? pill_map[ci] : 0;

        if (pm_val == 2) {
            /* Friendly pill → actual blocker */
            if (res->actual.n < SCAN_MAX_BLOCKERS) {
                res->actual.dx[res->actual.n] = dx;
                res->actual.dy[res->actual.n] = dy;
                res->actual.n++;
            }
            continue;
        }
        int tt = naThreatRawTT(tx, ty);
        if (tt == ctx->t_building || tt == ctx->t_halfbuild) {
            if (res->actual.n < SCAN_MAX_BLOCKERS) {
                res->actual.dx[res->actual.n] = dx;
                res->actual.dy[res->actual.n] = dy;
                res->actual.n++;
            }
        } else if (!no_builder && !ctx->non_build[tt & 0x0F]) {
            /* Potentially buildable — check LGM reachability */
            if (lgm_reachable(L, ctx, lgm_reach, approach_mx, approach_my,
                              tx, ty, pmx, pmy)) {
                if (res->potential.n < SCAN_MAX_BLOCKERS) {
                    res->potential.dx[res->potential.n] = dx;
                    res->potential.dy[res->potential.n] = dy;
                    res->potential.n++;
                }
            }
        }
    }

    /* Compute score */
    res->score = (float)(ctx->score_per_slot * (res->actual.n + res->potential.n)
                       + ctx->built_bonus    *  res->actual.n);
}

/* na_shield.scan_c(
 *   pill_mx, pill_my, pill_wx, pill_wy,
 *   standoff_deg, standoff_cx, standoff_cy, scan_radius,
 *   pill_table,      -- flat Lua array: {mx1,my1,owner_int1, ...}
 *                    -- owner_int: 0=hostile/neutral, 1=friendly
 *   approach_mx, approach_my,
 *   no_builder
 * ) -> flat result table or nil
 *
 * Result layout (all values 1-based in table):
 *  [1]  found (0/1)
 *  [2]  cx  [3] cy  [4] mx  [5] my  [6] deg
 *  [7]  score  [8] best_aim (1-based)  [9] nudge_wu
 *  [10] n_actual
 *  [11..20] act_dx1..5, act_dy1..5  (relative to pill tile)
 *  [21] n_potential
 *  [22..31] pot_dx1..5, pot_dy1..5
 *  [32] score_actual  [33] score_potential  [34] score_neighbor
 */
static int l_scan_c(lua_State *L) {
    if (!entries_ready()) { lua_pushnil(L); return 1; }
    NaShieldStampCtx *ctx = requireConfigured(L);

    int   pmx        = (int)luaL_checkinteger(L, 1);
    int   pmy        = (int)luaL_checkinteger(L, 2);
    int   pill_wx    = (int)luaL_checkinteger(L, 3);
    int   pill_wy    = (int)luaL_checkinteger(L, 4);
    float chosen_deg = (float)luaL_checknumber(L, 5);
    float standoff_cx = (float)luaL_checknumber(L, 6);
    float standoff_cy = (float)luaL_checknumber(L, 7);
    float R           = (float)luaL_checknumber(L, 8);
    /* arg 9 = pill_table */
    int   app_mx     = (int)luaL_checkinteger(L, 10);
    int   app_my     = (int)luaL_checkinteger(L, 11);
    int   no_builder = lua_toboolean(L, 12);

    /* Per-call HP-dependent neighbor-bonus tunables (args 13..19).
     * Stack-local so concurrent scans on different brains don't trample
     * each other's bonuses. */
    ScanCfg cfg = { 0, {0, 0}, {0.0, 0.0}, 0, 0.0 };
    if (lua_gettop(L) >= 19) {
        cfg.n_fav        = (int)luaL_checkinteger(L, 13);
        cfg.fav_size[0]  = (int)luaL_checkinteger(L, 14);
        cfg.fav_bon[0]   = luaL_checknumber(L, 15);
        cfg.fav_size[1]  = (int)luaL_checkinteger(L, 16);
        cfg.fav_bon[1]   = luaL_checknumber(L, 17);
        cfg.min_chain    = (int)luaL_checkinteger(L, 18);
        cfg.max_bonus    = luaL_checknumber(L, 19);
    }

    /* Stack-local 17×17 local pill map centred on pill tile */
    uint8_t pill_map[17 * 17];
    memset(pill_map, 0, sizeof(pill_map));
    if (lua_istable(L, 9)) {
        int len = (int)lua_rawlen(L, 9);
        for (int i = 1; i <= len - 2; i += 3) {
            lua_rawgeti(L, 9, i);   int tx  = (int)lua_tointeger(L, -1); lua_pop(L,1);
            lua_rawgeti(L, 9, i+1); int ty  = (int)lua_tointeger(L, -1); lua_pop(L,1);
            lua_rawgeti(L, 9, i+2); int own = (int)lua_tointeger(L, -1); lua_pop(L,1);
            int ci = lgm_cache_idx(tx - pmx, ty - pmy);
            if (ci >= 0)
                pill_map[ci] = (own == 1) ? 2 : 1;  /* 2=friendly, 1=hostile/neutral */
        }
    }

    /* Stack-local 17×17 LGM reachability cache for this scan only.
     * -1=unchecked, 0=unreachable, 1=reachable. */
    int8_t lgm_reach[17 * 17];
    memset(lgm_reach, -1, sizeof(lgm_reach));

    /* ── Generate candidates ──────────────────────────────────────── */
    int n_cands = 0;
    memset(ctx->scan_buf, 0, sizeof(ctx->scan_buf));

    /* Candidate 0: the chosen standoff */
    {
        ScanCand *c = &ctx->scan_buf[0].cand;
        c->cx  = standoff_cx; c->cy  = standoff_cy;
        c->mx  = (int)floorf(standoff_cx);
        c->my  = (int)floorf(standoff_cy);
        c->deg = chosen_deg;
        float deg_norm = (float)fmod((double)chosen_deg, 360.0);
        if (deg_norm < 0) deg_norm += 360.0f;
        c->ak  = clamp_angle_key((int)(deg_norm * 4.0f));
        c->valid = 1;
        n_cands++;
    }

    /* Candidates 1..NUM_CANDIDATES: ring around pill */
    float pcx = pmx + 0.5f, pcy = pmy + 0.5f;
    int NUM_CANDS = 28;  /* M.NUM_CANDIDATES */
    float STEP    = 0.5f; /* M.STEP_DEG */
    for (int i = 1; i <= NUM_CANDS && n_cands < SCAN_MAX_CANDS; i++) {
        float offset = (i - NUM_CANDS * 0.5f - 0.5f) * STEP;
        float deg    = chosen_deg + offset;
        float rad    = (float)(deg * (3.14159265358979323846 / 180.0));
        float cx     = pcx + sinf(rad) * R;
        float cy     = pcy - cosf(rad) * R;
        float deg_n  = (float)fmod((double)deg, 360.0);
        if (deg_n < 0) deg_n += 360.0f;
        ScanCand *c  = &ctx->scan_buf[n_cands].cand;
        c->cx  = cx; c->cy = cy;
        c->mx  = (int)floorf(cx); c->my = (int)floorf(cy);
        c->deg = deg;
        c->ak  = clamp_angle_key((int)(deg_n * 4.0f));
        c->valid = 1;
        n_cands++;
    }

    /* ── Score candidates ─────────────────────────────────────────── */
    /* Prepare slate */
    ctx->slate_ncands = n_cands;
    memset(ctx->slate,         0, sizeof(ctx->slate));
    memset(ctx->slate_ni_used, 0, sizeof(ctx->slate_ni_used));

    for (int ci = 0; ci < n_cands; ci++) {
        ScanCand *cand = &ctx->scan_buf[ci].cand;
        ScanResult *sr = &ctx->scan_buf[ci];
        sr->best_aim   = -1;
        sr->best_score = -1.0f;

        int ak = cand->ak;
        float cx = cand->cx, cy = cand->cy;

        /* Compute approach tile (LGM dispatch origin for this candidate) */
        float dx2p = cx - pcx, dy2p = cy - pcy;
        float dist = sqrtf(dx2p * dx2p + dy2p * dy2p);
        int   lgm_mx = app_mx, lgm_my = app_my;
        if (dist > 0.01f) {
            float ux = dx2p / dist, uy = dy2p / dist;
            float afx = cx + ux * ctx->approach_offset;
            float afy = cy + uy * ctx->approach_offset;
            int amx = (int)floorf(afx), amy = (int)floorf(afy);
            if (amx >= 0 && amx <= 255 && amy >= 0 && amy <= 255) {
                lgm_mx = amx; lgm_my = amy;
            }
        }

        /* Base scoring (nudge=0) */
        const ShieldStampEntry *e0 = &s_entries[ak * s_n_nudges + 0];
        for (int ai = 0; ai < SCAN_N_AIMS; ai++) {
            ScanAimResult *ar = &sr->aims[ai];
            ar->nudge_step = 0;
            score_one_aim(L, ctx, lgm_reach,
                          &e0->aim[ai], &e0->pill_to_tank,
                          pmx, pmy, cx, cy, lgm_mx, lgm_my,
                          pill_map, 1 /*target_is_pill*/, no_builder, ar);

            /* Check if base shot hits pill — if not, find nudge */
            if (!ar->blocked && !e0->aim[ai].has_pill) {
                int found_step = 0;
                for (int ns = 1; ns < s_n_nudges && !found_step; ns++) {
                    const ShieldStampEntry *en = &s_entries[ak * s_n_nudges + ns];
                    if (en->aim[ai].has_pill) {
                        found_step = ns;
                    }
                }
                if (found_step) {
                    /* Re-score at nudge position */
                    const ShieldStampEntry *en = &s_entries[ak * s_n_nudges + found_step];
                    /* Nudged candidate position from stamp source */
                    float ncx = (pill_wx + en->aim[ai].source.x) / 256.0f;
                    float ncy = (pill_wy + en->aim[ai].source.y) / 256.0f;
                    /* Recompute approach from nudged pos */
                    float ndx = ncx - pcx, ndy = ncy - pcy;
                    float nd  = sqrtf(ndx * ndx + ndy * ndy);
                    int nlgm_mx = lgm_mx, nlgm_my = lgm_my;
                    if (nd > 0.01f) {
                        float ux = ndx / nd, uy = ndy / nd;
                        int amx = (int)floorf(ncx + ux * ctx->approach_offset);
                        int amy = (int)floorf(ncy + uy * ctx->approach_offset);
                        if (amx >= 0 && amx <= 255 && amy >= 0 && amy <= 255) {
                            nlgm_mx = amx; nlgm_my = amy;
                        }
                    }
                    score_one_aim(L, ctx, lgm_reach,
                                  &en->aim[ai], &en->pill_to_tank,
                                  pmx, pmy, ncx, ncy,
                                  nlgm_mx, nlgm_my,
                                  pill_map, 1, no_builder, ar);
                    ar->nudge_step = found_step;
                } else {
                    /* No nudge works — mark blocked */
                    ar->blocked = 1;
                }
            }

            /* Populate slate */
            int ni_slate = ar->nudge_step;
            if (ni_slate >= SLATE_MAX_NUDGES) ni_slate = SLATE_MAX_NUDGES - 1;
            SlateEntry *se = &ctx->slate[ci][ni_slate][ai];
            se->blocked    = (uint8_t)ar->blocked;
            se->n_actual   = (uint8_t)(ar->actual.n < SLATE_MAX_BLOCKERS
                                        ? ar->actual.n : SLATE_MAX_BLOCKERS);
            se->n_potential= (uint8_t)(ar->potential.n < SLATE_MAX_BLOCKERS
                                        ? ar->potential.n : SLATE_MAX_BLOCKERS);
            for (int k = 0; k < SLATE_MAX_BLOCKERS; k++) {
                se->actual_dx[k]    = k < ar->actual.n    ? ar->actual.dx[k]    : 0;
                se->actual_dy[k]    = k < ar->actual.n    ? ar->actual.dy[k]    : 0;
                se->potential_dx[k] = k < ar->potential.n ? ar->potential.dx[k] : 0;
                se->potential_dy[k] = k < ar->potential.n ? ar->potential.dy[k] : 0;
            }
            ctx->slate_ni_used[ci][ai] = (uint8_t)ni_slate;

            if (!ar->blocked && ar->score > sr->best_score) {
                sr->best_score = ar->score;
                sr->best_aim   = ai;
            }
        }
    }

    /* ── Neighbor bonus ───────────────────────────────────────────── */
    NbResult nb_results[SCAN_MAX_CANDS];
    for (int ci = 0; ci < n_cands; ci++) {
        run_nb_for_cand(ctx, &cfg, ci, n_cands, SCAN_N_AIMS,
                        ctx->score_per_slot, ctx->built_bonus, ctx->neighbor_bonus,
                        &nb_results[ci]);
    }

    /* ── Find best candidate ──────────────────────────────────────── */
    int    best_ci    = -1;
    double best_total = -1.0;
    for (int ci = 0; ci < n_cands; ci++) {
        if (nb_results[ci].best_aim > 0 &&
            nb_results[ci].best_total > best_total) {
            best_total = nb_results[ci].best_total;
            best_ci    = ci;
        }
    }

    lua_createtable(L, 34, 0);
    if (best_ci < 0) {
        lua_pushinteger(L, 0); lua_rawseti(L, -2, 1);  /* found=0 */
        return 1;
    }

    const ScanCand   *bc  = &ctx->scan_buf[best_ci].cand;
    const NbResult   *nbr = &nb_results[best_ci];
    int best_ai_0 = nbr->best_aim - 1;  /* 0-based */
    int nudge_wu  = (best_ai_0 >= 0 && best_ai_0 < SCAN_N_AIMS)
                    ? ctx->scan_buf[best_ci].aims[best_ai_0].nudge_step * s_nudge_step
                    : 0;

    lua_pushinteger(L, 1);                           lua_rawseti(L,-2,  1);
    lua_pushnumber (L, (double)bc->cx);              lua_rawseti(L,-2,  2);
    lua_pushnumber (L, (double)bc->cy);              lua_rawseti(L,-2,  3);
    lua_pushinteger(L, bc->mx);                      lua_rawseti(L,-2,  4);
    lua_pushinteger(L, bc->my);                      lua_rawseti(L,-2,  5);
    lua_pushnumber (L, (double)bc->deg);             lua_rawseti(L,-2,  6);
    lua_pushnumber (L, nbr->best_total);             lua_rawseti(L,-2,  7);
    lua_pushinteger(L, nbr->best_aim);               lua_rawseti(L,-2,  8);
    lua_pushinteger(L, nudge_wu);                    lua_rawseti(L,-2,  9);
    lua_pushinteger(L, nbr->best_n_act);             lua_rawseti(L,-2, 10);
    for (int k = 0; k < SLATE_MAX_BLOCKERS; k++) {
        lua_pushinteger(L, nbr->best_adx[k]);        lua_rawseti(L,-2, 11 + k);
        lua_pushinteger(L, nbr->best_ady[k]);        lua_rawseti(L,-2, 16 + k);
    }
    lua_pushinteger(L, nbr->best_n_pot);             lua_rawseti(L,-2, 21);
    for (int k = 0; k < SLATE_MAX_BLOCKERS; k++) {
        lua_pushinteger(L, nbr->best_pdx[k]);        lua_rawseti(L,-2, 22 + k);
        lua_pushinteger(L, nbr->best_pdy[k]);        lua_rawseti(L,-2, 27 + k);
    }
    lua_pushnumber(L, nbr->best_score_act);          lua_rawseti(L,-2, 32);
    lua_pushnumber(L, nbr->best_score_pot);          lua_rawseti(L,-2, 33);
    lua_pushnumber(L, nbr->best_score_nb);           lua_rawseti(L,-2, 34);
    return 1;
}

/* ── Registration ─────────────────────────────────────────────────── */
static const luaL_Reg na_shield_lib[] = {
    { "load",                 l_load                },
    { "is_loaded",            l_is_loaded           },
    { "get_flat",             l_get_flat            },
    { "slate_clear",          l_slate_clear         },
    { "slate_set",            l_slate_set           },
    { "slate_set_nudge_used", l_slate_set_nudge_used},
    { "run_neighbor_bonus",   l_run_neighbor_bonus  },
    { "configure_scan",       l_configure_scan      },
    { "set_lgm_func",         l_set_lgm_func        },
    { "scan_c",               l_scan_c              },
    { NULL, NULL }
};

void naShieldStampRegister(lua_State *L) {
    /* Allocate the ctx as a Lua-managed userdata (the userdata storage
     * IS the ctx). Lua frees it on lua_State close; the __gc metatable
     * unrefs the held lgm_ref so its registry slot can be reused. */
    NaShieldStampCtx *ctx = (NaShieldStampCtx *)lua_newuserdata(L, sizeof(NaShieldStampCtx));
    memset(ctx, 0, sizeof(*ctx));
    ctx->lgm_ref = LUA_NOREF;

    if (luaL_newmetatable(L, "na_shield_stamp_ctx_mt")) {
        lua_pushcfunction(L, naShieldStampCtxGc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);

    lua_setfield(L, LUA_REGISTRYINDEX, "na_shield_stamp_ctx");

    luaL_newlib(L, na_shield_lib);
    lua_setglobal(L, "na_shield");
}
