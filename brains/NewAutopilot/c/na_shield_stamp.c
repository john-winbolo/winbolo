/*
 * na_shield_stamp.c — precomputed shield-scan lookup table
 *
 * Loads shield_stamp_cache.bin (written by generate_shield_stamps.lua)
 * and exposes fast O(1) Lua bindings for nudge pill-hit detection and
 * shot-path tile retrieval.
 *
 * Binary file layout:
 *   ShieldStampHeader  (56 bytes)
 *   ShieldStampEntry   [n_angles * n_nudges * n_aims]  (66 bytes each)
 *
 * Entry index: (angle_key * n_nudges + nudge_idx) * n_aims + aim_idx
 */

#include "na_shield_stamp.h"
#include <lauxlib.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Binary format constants ──────────────────────────────────────────── */
#define STAMP_MAGIC      0x444C4853u  /* "SHLD" in little-endian */
#define STAMP_VERSION    1u
#define STAMP_MAX_ANGLES 1440
#define STAMP_MAX_NUDGES 8            /* header value; actual varies */
#define STAMP_MAX_AIMS   8            /* header value; actual varies */
#define STAMP_MAX_TILES  32

#pragma pack(push, 1)
typedef struct {
    uint8_t n_tiles;
    uint8_t has_pill;
    int8_t  dx[STAMP_MAX_TILES];
    int8_t  dy[STAMP_MAX_TILES];
} ShieldStampEntry;  /* 2 + 64 = 66 bytes */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t n_angles;
    uint32_t n_nudges;
    uint32_t n_aims;
    uint32_t max_tiles;
    uint32_t nudge_wu[STAMP_MAX_NUDGES];
    float    standoff;
    float    aim_inset;
    float    step_deg;
} ShieldStampHeader;  /* 6*4 + 8*4 + 3*4 = 56 bytes */
#pragma pack(pop)

/* ── Module state ─────────────────────────────────────────────────────── */
static ShieldStampEntry *s_entries  = NULL;
static int               s_loaded   = 0;
static uint32_t          s_nudge_wu[STAMP_MAX_NUDGES];
static int               s_n_nudges = 0;
static int               s_n_aims   = 0;
static int               s_n_angles = 0;

/* ── Internal helpers ─────────────────────────────────────────────────── */
static int nudge_wu_to_idx(int wu) {
    for (int i = 0; i < s_n_nudges; i++) {
        if ((int)s_nudge_wu[i] == wu) return i;
    }
    return -1;
}

static int deg_to_angle_key(double deg) {
    deg = fmod(deg, 360.0);
    if (deg < 0.0) deg += 360.0;
    int ak = (int)(deg * 4.0);
    if (ak < 0)          ak = 0;
    if (ak >= s_n_angles) ak = s_n_angles - 1;
    return ak;
}

/* ── Lua bindings ─────────────────────────────────────────────────────── */

/* na_shield.load(path) -> true/false */
static int l_load(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    FILE *f = fopen(path, "rb");
    if (!f) { lua_pushboolean(L, 0); return 1; }

    ShieldStampHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f); lua_pushboolean(L, 0); return 1;
    }
    if (hdr.magic   != STAMP_MAGIC   ||
        hdr.version != STAMP_VERSION ||
        hdr.n_angles == 0 || hdr.n_nudges == 0 || hdr.n_aims == 0 ||
        hdr.n_angles > STAMP_MAX_ANGLES ||
        hdr.n_nudges > STAMP_MAX_NUDGES ||
        hdr.n_aims   > STAMP_MAX_AIMS   ||
        hdr.max_tiles != STAMP_MAX_TILES) {
        fclose(f); lua_pushboolean(L, 0); return 1;
    }

    size_t n = (size_t)hdr.n_angles * hdr.n_nudges * hdr.n_aims;
    ShieldStampEntry *buf = (ShieldStampEntry *)malloc(n * sizeof(ShieldStampEntry));
    if (!buf) { fclose(f); lua_pushboolean(L, 0); return 1; }

    if (fread(buf, sizeof(ShieldStampEntry), n, f) != n) {
        free(buf); fclose(f); lua_pushboolean(L, 0); return 1;
    }
    fclose(f);

    free(s_entries);
    s_entries  = buf;
    s_n_angles = (int)hdr.n_angles;
    s_n_nudges = (int)hdr.n_nudges;
    s_n_aims   = (int)hdr.n_aims;
    memcpy(s_nudge_wu, hdr.nudge_wu, s_n_nudges * sizeof(uint32_t));
    s_loaded   = 1;

    /* Build pill_hit flat sequence table: index (1-based) =
     * (angle_key * n_nudges + nudge_idx) * n_aims + aim_idx + 1
     * Lua reads it with pure integer arithmetic — no C call overhead. */
    lua_createtable(L, (int)n, 0);
    for (size_t i = 0; i < n; i++) {
        lua_pushboolean(L, buf[i].has_pill);
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
    /* Store as na_shield.pill_hit */
    lua_getglobal(L, "na_shield");  /* push na_shield table */
    lua_insert(L, -2);              /* swap: na_shield below pill_hit */
    lua_setfield(L, -2, "pill_hit");
    lua_pop(L, 1);                  /* pop na_shield */

    lua_pushboolean(L, 1);
    return 1;
}

/* na_shield.is_loaded() -> true/false */
static int l_is_loaded(lua_State *L) {
    lua_pushboolean(L, s_loaded);
    return 1;
}

/* na_shield.stamp_hit(deg, nudge_wu, aim_idx) -> true/false/nil
 * Returns nil when not loaded or nudge_wu is outside the stamp range.
 * Callers must fall back to cpf.simulate_shot / ensure_nudge_ring on nil. */
static int l_stamp_hit(lua_State *L) {
    if (!s_loaded) { lua_pushnil(L); return 1; }
    double   deg      = luaL_checknumber(L, 1);
    int      nudge_wu = (int)luaL_checkinteger(L, 2);
    int      aim_idx  = (int)luaL_checkinteger(L, 3);
    int ak = deg_to_angle_key(deg);
    int ni = nudge_wu_to_idx(nudge_wu);
    if (ni < 0 || aim_idx < 0 || aim_idx >= s_n_aims) {
        lua_pushnil(L); return 1;
    }
    int idx = (ak * s_n_nudges + ni) * s_n_aims + aim_idx;
    lua_pushboolean(L, s_entries[idx].has_pill);
    return 1;
}

/* na_shield.stamp_tiles(pmx, pmy, deg, nudge_wu, aim_idx) -> list/{mx,my} or nil */
static int l_stamp_tiles(lua_State *L) {
    if (!s_loaded) { lua_pushnil(L); return 1; }
    int      pmx      = (int)luaL_checkinteger(L, 1);
    int      pmy      = (int)luaL_checkinteger(L, 2);
    double   deg      = luaL_checknumber(L, 3);
    int      nudge_wu = (int)luaL_checkinteger(L, 4);
    int      aim_idx  = (int)luaL_checkinteger(L, 5);
    int ak = deg_to_angle_key(deg);
    int ni = nudge_wu_to_idx(nudge_wu);
    if (ni < 0 || aim_idx < 0 || aim_idx >= s_n_aims) {
        lua_pushnil(L); return 1;
    }
    int idx = (ak * s_n_nudges + ni) * s_n_aims + aim_idx;
    const ShieldStampEntry *e = &s_entries[idx];
    if (e->n_tiles == 0) { lua_pushnil(L); return 1; }
    lua_createtable(L, (int)e->n_tiles, 0);
    for (int i = 0; i < (int)e->n_tiles; i++) {
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, pmx + e->dx[i]);
        lua_setfield(L, -2, "mx");
        lua_pushinteger(L, pmy + e->dy[i]);
        lua_setfield(L, -2, "my");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* ── Blocker slate ────────────────────────────────────────────────────── */
/* Fixed-size C array storing blocker geometry per (candidate, nudge, aim).
 * Populated by Lua score_aim results via slate_set; read by run_neighbor_bonus.
 * Avoids all intermediate Lua table allocation in the neighbor chain walk. */

#define SLATE_MAX_CANDS    32
#define SLATE_MAX_NUDGES    5   /* slot 0=base, 1-4=nudge steps (wu 8,16,24,32) */
#define SLATE_MAX_AIMS      5   /* outgoing aims, 0-based (Lua ai-1) */
#define SLATE_MAX_BLOCKERS  3
#define RESULT_STRIDE      21   /* values returned per candidate by run_neighbor_bonus */

typedef struct {
    uint8_t n_actual;
    uint8_t n_potential;
    uint8_t blocked;
    uint8_t _pad;
    int8_t  actual_dx[SLATE_MAX_BLOCKERS];
    int8_t  actual_dy[SLATE_MAX_BLOCKERS];
    int8_t  potential_dx[SLATE_MAX_BLOCKERS];
    int8_t  potential_dy[SLATE_MAX_BLOCKERS];
} SlateEntry;  /* 16 bytes */

static SlateEntry s_slate[SLATE_MAX_CANDS][SLATE_MAX_NUDGES][SLATE_MAX_AIMS];
static uint8_t    s_slate_ni_used[SLATE_MAX_CANDS][SLATE_MAX_AIMS];
static int        s_slate_ncands = 0;

/* na_shield.slate_clear(n_cands) */
static int l_slate_clear(lua_State *L) {
    int n = (int)luaL_checkinteger(L, 1);
    if (n > SLATE_MAX_CANDS) n = SLATE_MAX_CANDS;
    s_slate_ncands = n;
    memset(s_slate,         0, sizeof(s_slate));
    memset(s_slate_ni_used, 0, sizeof(s_slate_ni_used));
    return 0;
}

/* na_shield.slate_set(ci, ni, ai, blocked,
 *   n_act, dx1,dy1,dx2,dy2,dx3,dy3,
 *   n_pot, pdx1,pdy1,pdx2,pdy2,pdx3,pdy3)
 * ci: 1-based; ni: 0-4 nudge slot; ai: 0-based outgoing aim (Lua ai-1) */
static int l_slate_set(lua_State *L) {
    int ci = (int)luaL_checkinteger(L, 1) - 1;
    int ni = (int)luaL_checkinteger(L, 2);
    int ai = (int)luaL_checkinteger(L, 3);
    if (ci < 0 || ci >= SLATE_MAX_CANDS ||
        ni < 0 || ni >= SLATE_MAX_NUDGES ||
        ai < 0 || ai >= SLATE_MAX_AIMS) return 0;
    SlateEntry *e    = &s_slate[ci][ni][ai];
    e->blocked       = (uint8_t)lua_toboolean(L, 4);
    e->n_actual      = (uint8_t)luaL_checkinteger(L, 5);
    e->actual_dx[0]  = (int8_t)luaL_checkinteger(L, 6);
    e->actual_dy[0]  = (int8_t)luaL_checkinteger(L, 7);
    e->actual_dx[1]  = (int8_t)luaL_checkinteger(L, 8);
    e->actual_dy[1]  = (int8_t)luaL_checkinteger(L, 9);
    e->actual_dx[2]  = (int8_t)luaL_checkinteger(L, 10);
    e->actual_dy[2]  = (int8_t)luaL_checkinteger(L, 11);
    e->n_potential      = (uint8_t)luaL_checkinteger(L, 12);
    e->potential_dx[0]  = (int8_t)luaL_checkinteger(L, 13);
    e->potential_dy[0]  = (int8_t)luaL_checkinteger(L, 14);
    e->potential_dx[1]  = (int8_t)luaL_checkinteger(L, 15);
    e->potential_dy[1]  = (int8_t)luaL_checkinteger(L, 16);
    e->potential_dx[2]  = (int8_t)luaL_checkinteger(L, 17);
    e->potential_dy[2]  = (int8_t)luaL_checkinteger(L, 18);
    return 0;
}

/* na_shield.slate_set_nudge_used(ci, ai, ni) — records which nudge slot was
 * used for final scoring so run_neighbor_bonus uses the right ring entry. */
static int l_slate_set_nudge_used(lua_State *L) {
    int ci = (int)luaL_checkinteger(L, 1) - 1;
    int ai = (int)luaL_checkinteger(L, 2);
    int ni = (int)luaL_checkinteger(L, 3);
    if (ci >= 0 && ci < SLATE_MAX_CANDS &&
        ai >= 0 && ai < SLATE_MAX_AIMS  &&
        ni >= 0 && ni < SLATE_MAX_NUDGES)
        s_slate_ni_used[ci][ai] = (uint8_t)ni;
    return 0;
}

/* Returns 1 if all tiles in subset (sub_dx/sub_dy, sub_n) appear in
 * candidate j's actual+potential blocker list at nudge slot ni, aim ai. */
static int covers_subset(int j, int ni, int ai,
                          const int8_t *sub_dx, const int8_t *sub_dy, int sub_n) {
    if (j < 0 || j >= s_slate_ncands) return 0;
    const SlateEntry *e = &s_slate[j][ni][ai];
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

/* na_shield.run_neighbor_bonus(n_cands, n_aims,
 *   score_per_slot, built_bonus, neighbor_bonus,
 *   n_fav, fav_size1, fav_bonus1, fav_size2, fav_bonus2,
 *   min_chain, max_bonus)
 * Returns flat sequence table: RESULT_STRIDE values per candidate. */
static int l_run_neighbor_bonus(lua_State *L) {
    int    n_cands        = (int)luaL_checkinteger(L, 1);
    int    n_aims         = (int)luaL_checkinteger(L, 2);
    double score_per_slot = luaL_checknumber(L, 3);
    double built_bonus    = luaL_checknumber(L, 4);
    double neighbor_bonus = luaL_checknumber(L, 5);
    int    n_fav          = (int)luaL_checkinteger(L, 6);
    int    fav_size[2]    = { (int)luaL_checkinteger(L, 7), (int)luaL_checkinteger(L, 9) };
    double fav_bon[2]     = { luaL_checknumber(L, 8), luaL_checknumber(L, 10) };
    int    min_chain      = (int)luaL_checkinteger(L, 11);
    double max_bonus      = luaL_checknumber(L, 12);

    if (n_cands > SLATE_MAX_CANDS) n_cands = SLATE_MAX_CANDS;
    if (n_aims  > SLATE_MAX_AIMS)  n_aims  = SLATE_MAX_AIMS;

    lua_createtable(L, n_cands * RESULT_STRIDE, 0);
    int out = 1;

    for (int i = 0; i < n_cands; i++) {
        double best_total      = -1.0;
        int    best_aim        = 0;   /* 1-based Lua aim; 0 = no winner */
        double best_score_nb   = 0.0, best_score_act = 0.0, best_score_pot = 0.0;
        int    best_chain      = 0, best_bcnt = 0, best_n_act = 0, best_n_pot = 0;
        int8_t best_adx[SLATE_MAX_BLOCKERS], best_ady[SLATE_MAX_BLOCKERS];
        int8_t best_pdx[SLATE_MAX_BLOCKERS], best_pdy[SLATE_MAX_BLOCKERS];
        memset(best_adx, 0, sizeof(best_adx)); memset(best_ady, 0, sizeof(best_ady));
        memset(best_pdx, 0, sizeof(best_pdx)); memset(best_pdy, 0, sizeof(best_pdy));

        for (int ai = 0; ai < n_aims; ai++) {
            int ni = (int)s_slate_ni_used[i][ai];
            const SlateEntry *e = &s_slate[i][ni][ai];
            if (e->blocked) continue;
            int n_tot = (int)e->n_actual + (int)e->n_potential;
            if (n_tot == 0) continue;

            /* Build combined blocker array: actual then potential */
            int8_t all_dx[SLATE_MAX_BLOCKERS * 2], all_dy[SLATE_MAX_BLOCKERS * 2];
            int    all_act[SLATE_MAX_BLOCKERS * 2];
            int n_all = 0;
            for (int k = 0; k < (int)e->n_actual && k < SLATE_MAX_BLOCKERS; k++) {
                all_dx[n_all] = e->actual_dx[k]; all_dy[n_all] = e->actual_dy[k];
                all_act[n_all++] = 1;
            }
            for (int k = 0; k < (int)e->n_potential && k < SLATE_MAX_BLOCKERS; k++) {
                all_dx[n_all] = e->potential_dx[k]; all_dy[n_all] = e->potential_dy[k];
                all_act[n_all++] = 0;
            }

            int n_iter   = n_all < 3 ? n_all : 3;
            int mask_max = (1 << n_iter) - 1;

            for (int mask = 1; mask <= mask_max; mask++) {
                int8_t sub_dx[3], sub_dy[3]; int sub_act[3];
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
                for (int j = i-1; j >= 0; j--) {
                    if (!covers_subset(j, ni, ai, sub_dx, sub_dy, sub_n)) break;
                    left++;
                }
                int right = 0;
                for (int j = i+1; j < n_cands; j++) {
                    if (!covers_subset(j, ni, ai, sub_dx, sub_dy, sub_n)) break;
                    right++;
                }
                int sym   = left < right ? left : right;
                int chain = sym * 2;

                double bias = 0.0;
                int    sz   = sub_na + sub_np;
                for (int fi = 0; fi < n_fav && fi < 2; fi++)
                    if (sz == fav_size[fi]) bias += fav_bon[fi];
                if (min_chain > 0 && chain < min_chain)
                    bias -= max_bonus * 100.0;

                double total = aim_score + chain * neighbor_bonus + bias;
                if (total > best_total) {
                    best_total    = total;
                    best_aim      = ai + 1;   /* Lua 1-based */
                    best_chain    = chain;
                    best_n_act    = sub_na; best_n_pot = sub_np;
                    best_bcnt     = sub_na + sub_np;
                    best_score_act = (score_per_slot + built_bonus) * sub_na;
                    best_score_pot =  score_per_slot * sub_np;
                    best_score_nb  = chain * neighbor_bonus;
                    int ao = 0, po = 0;
                    memset(best_adx,0,sizeof(best_adx)); memset(best_ady,0,sizeof(best_ady));
                    memset(best_pdx,0,sizeof(best_pdx)); memset(best_pdy,0,sizeof(best_pdy));
                    for (int k = 0; k < sub_n; k++) {
                        if (sub_act[k]) { best_adx[ao]=sub_dx[k]; best_ady[ao++]=sub_dy[k]; }
                        else            { best_pdx[po]=sub_dx[k]; best_pdy[po++]=sub_dy[k]; }
                    }
                }
            }
        }

        /* Emit RESULT_STRIDE values */
        lua_pushinteger(L, best_aim);                                lua_rawseti(L,-2,out++);
        lua_pushnumber (L, best_aim ? best_total : 0.0);            lua_rawseti(L,-2,out++);
        lua_pushnumber (L, best_score_nb);                          lua_rawseti(L,-2,out++);
        lua_pushinteger(L, best_chain);                             lua_rawseti(L,-2,out++);
        lua_pushnumber (L, best_score_act);                         lua_rawseti(L,-2,out++);
        lua_pushnumber (L, best_score_pot);                         lua_rawseti(L,-2,out++);
        lua_pushinteger(L, best_bcnt);                              lua_rawseti(L,-2,out++);
        lua_pushinteger(L, best_n_act);                             lua_rawseti(L,-2,out++);
        lua_pushinteger(L, best_n_pot);                             lua_rawseti(L,-2,out++);
        for (int k=0;k<SLATE_MAX_BLOCKERS;k++){lua_pushinteger(L,best_adx[k]);lua_rawseti(L,-2,out++);lua_pushinteger(L,best_ady[k]);lua_rawseti(L,-2,out++);}
        for (int k=0;k<SLATE_MAX_BLOCKERS;k++){lua_pushinteger(L,best_pdx[k]);lua_rawseti(L,-2,out++);lua_pushinteger(L,best_pdy[k]);lua_rawseti(L,-2,out++);}
    }
    return 1;
}

/* ── Registration ─────────────────────────────────────────────────────── */
static const luaL_Reg na_shield_lib[] = {
    { "load",                l_load                },
    { "is_loaded",           l_is_loaded           },
    { "stamp_hit",           l_stamp_hit           },
    { "stamp_tiles",         l_stamp_tiles         },
    { "slate_clear",         l_slate_clear         },
    { "slate_set",           l_slate_set           },
    { "slate_set_nudge_used",l_slate_set_nudge_used},
    { "run_neighbor_bonus",  l_run_neighbor_bonus  },
    { NULL, NULL }
};

void naShieldStampRegister(lua_State *L) {
    luaL_newlib(L, na_shield_lib);
    lua_setglobal(L, "na_shield");
}
