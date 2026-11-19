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

/*
 * gh_pt2.c -- pill take v2 evaluator in C (pilltake_v2.lua + shadow.lua).
 *
 * This is a line-by-line port of pilltake_v2.lua M.evaluate_lua and the
 * shadow.lua ray walks it uses. The Lua stays the reference: the parity
 * test (tests/unit/test_gh_pt2_parity.c) runs both on the same inputs and
 * compares every field of the result.
 *
 * Rules the port keeps, so the result is the same as the Lua's:
 *   - Every Lua number is a C double, and each expression keeps the Lua's
 *     order of operations. shadow.lua's f32() is a (float) cast here.
 *     sin, cos, atan, atan2, fmod and floor are the same C library calls
 *     LuaJIT makes. math.rad is x * 0.017453292519943295 (LuaJIT's).
 *   - bit.rshift(x, 8) is LuaJIT's tobit (round to int32) then a logical
 *     shift.
 *   - Every callback in inp and inp.env is called at the same point and in
 *     the same order as the Lua calls it, with two exceptions that give the
 *     same result: env.ttype and env.pill_on are called once per square per
 *     evaluate (the map does not change inside one evaluate) and the other
 *     reads use the answer; env.is_ally and env.cover_of_pill once per
 *     pill. env.count_reads(n), when given, gets the number of ttype reads
 *     taken from the cache (U.ttype's metrics counter).
 *   - env.base_stops is not called for a square with no entry in
 *     env.base_at (when given): U.base_tile_stops_tank_shell returns false
 *     there before it reads anything.
 *   - table.sort is LuaJIT's quicksort (lib_table.c auxsort), step for
 *     step, so equal keys end in the same order.
 *   - Tables the Lua shares between records (a ray reused from the record
 *     before, the T list, the lane, the clear-aims list, the blocker table)
 *     are one table in the result here too.
 */

#include "gh_pt2.h"

#include <lauxlib.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Engine constants (shadow.lua) ──────────────────────────────────── */
#define PT2_RADIANS_MAX   (2.0 * 3.14159)
#define PT2_SHELL_SPEED   32.0
#define PT2_START_ADD     5.0
#define PT2_SHELL_LIFE    8.0
#define PT2_PILL_FIRE_LEN 8.5
#define PT2_TANK_FIRE_LEN 7.0
#define PT2_TANK_HIT_R    112.0
#define PT2_LUA_RAD       0.017453292519943295
#define PT2_MAX_T         128   /* > life_ticks(8.5) = 64 */
#define PT2_NAIMS         5
#define PT2_MAP_SQ        65536

static const double s_aims[PT2_NAIMS][2] = {
    { 128, 128 }, { 24, 24 }, { 255 - 24, 24 }, { 24, 255 - 24 }, { 255 - 24, 255 - 24 },
};

/* Block kinds (shadow.lua). */
enum { BK_ALLY_PILL, BK_PLANNED, BK_WALL, BK_HALF_WALL, BK_BOAT, BK_FOREST };
static const char *s_bk_name[] = { "ally_pill", "planned", "wall", "half_wall", "boat", "forest" };

/* tank_shot why. */
enum { TS_OK, TS_SHORT, TS_WALL, TS_PILL, TS_BASE, TS_PLANNED };
static const char *s_ts_name[] = { "ok", "short", "wall", "pill", "base", "planned" };

/* Why codes for rejects and leeway points. */
enum { W_NONE, W_A_OUT, W_OUTGOING, W_OVERLAP, W_CHAIN, W_QMARK };

/* ── Arena (reset at each evaluate) ─────────────────────────────────── */
typedef struct Chunk {
    struct Chunk *next;
    size_t used, cap;
    double data[1];
} Chunk;

typedef struct {
    Chunk *head, *cur;
} Arena;

typedef struct {
    int    has_mx;
    double mx, my, health;
    int    in_tank, pos_unknown;
    int    ally;       /* -1 not asked yet */
    int    cover_set;
    double cover;
    int    skip;       /* chain_skip: -1 not asked yet */
} Pill;

typedef struct {
    Arena     ar;
    uint32_t  gen;
    uint32_t *tt_stamp;
    double   *tt_val;
    uint32_t *po_stamp;
    int      *po_val;
    Pill    **pills;      /* 1-based; each Pill is in the arena, so a
                             * Pill pointer stays valid while the list grows */
    int       npills, cap_pills;
} Pt2Ctx;

static void ar_reset(Arena *a) {
    a->cur = a->head;
    if (a->cur) a->cur->used = 0;
}

static void *ar_alloc(lua_State *L, Arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    for (;;) {
        Chunk *c = a->cur;
        if (c && c->used + n <= c->cap) {
            void *p = (unsigned char *)c->data + c->used;
            c->used += n;
            return p;
        }
        if (c && c->next) {
            a->cur = c->next;
            a->cur->used = 0;
            continue;
        }
        size_t cap = n > (size_t)(1u << 20) ? n : (size_t)(1u << 20);
        Chunk *nc = (Chunk *)malloc(sizeof(Chunk) + cap);
        if (!nc) { luaL_error(L, "gh_pt2: out of memory"); return NULL; }
        nc->next = NULL; nc->used = 0; nc->cap = cap;
        if (c) c->next = nc; else a->head = nc;
        a->cur = nc;
    }
}

/* ── Objects ────────────────────────────────────────────────────────── */
typedef struct { int n, cap; int *v; int memo, memo_ic; } PList;
typedef struct { int n; int64_t *v; int memo; } Lane;
typedef struct { int n; int v[PT2_NAIMS]; int memo; } Aims;
typedef struct { int n; int ai[PT2_NAIMS]; Lane *ln[PT2_NAIMS]; int memo; } Lanes;
typedef struct { int n, cap; int64_t *sq; double *shots; int memo; } Extra;
typedef struct { int64_t sq; double shots; int kind; double t; int pill; } Blk;
typedef struct {
    double  angle;
    int     tank;
    double  t;
    int     link;     /* pill index, 0 = none */
    double  link_t;
    double  cover, front;
    int     n_sq, n_block;
    int64_t *sq;
    double  *sq_t;
    Blk     *blk;
    double  end_x, end_y;
    int     X;
    double  bar;
    int     link_cover, ok;
    int     memo;
} Ray;
typedef struct { int n; Ray **v; int memo; } RayList;
typedef struct { int n; int *v; int memo; } IList;
typedef struct { int64_t sq; double mx, my, shots; int kind_pill; } PlanE;
typedef struct { int n, cap; PlanE *v; int memo; } Plan;
typedef struct { double x, y; int ok; int why; int why_arg; } LeePt;
typedef struct { double fx, fy, d; int safe; int why_pill; } Hold;

/* Optional record fields. */
#define RF_EXTRA_IN  0x0001
#define RF_OMXY      0x0002
#define RF_POS       0x0004
#define RF_BASE      0x0008
#define RF_PLAN      0x0010
#define RF_SCORED    0x0020
#define RF_UNSEARCH  0x0040
#define RF_BAND      0x0080
#define RF_LEEWAY    0x0100
#define RF_WHYUNSAFE 0x0200
#define RF_HOLD      0x0400

typedef struct {
    double  px, py, nudge_wu;
    PList  *T;
    RayList *rays;
    PList  *chain;
    int     chain_mode;
    int     aim_idx;
    Lane   *lane;
    Aims   *clear;
    Lanes  *lanes;
    double  aims_done;
    Extra  *extra_in;
    int     covered;
    IList  *uncovered;
    int     safe, max_ok;
    double  kills;
    int     ally_cover;
    double  need, have, depth;
    int     finish;
    unsigned flags;
    double  omx, omy;
    double  deg, cx, cy, mx, my, radius;
    double  base;
    Plan   *plan;
    Extra  *extra;
    double  n_built, blk, score;
    double  tier_min;
    int     in_band;
    double  leeway;
    LeePt  *lpts;
    Hold   *hold;
    int     memo;
} Rec;

typedef struct { double deg, cx, cy, mx, my; int why, why_arg; } Reject;
typedef struct { Rec *rec; Reject *rej; } AllE;

/* Stack slots of one evaluate. */
enum {
    S_INP = 1, S_V2, S_ENV, S_PILLS, S_PIDX, S_MEMO,
    S_TTYPE, S_PILL_ON, S_IS_ALLY, S_COVER, S_BASE_STOPS,
    S_IN_MAP, S_STAY_IN, S_STANDABLE, S_BUILDABLE, S_LGM_REACH, S_SCORE_FN,
    S_BANNED, S_ANGER_OF, S_CHAIN_SKIP, S_BASE_AT, S_PILL_NAME, S_COUNT_READS,
    S_TOP = S_COUNT_READS
};

typedef struct {
    lua_State *L;
    Pt2Ctx    *c;
    /* inp */
    int    A;
    double radius, angle_step, approach_offset, num_pill_blockers, pill_drop_shots;
    double wall_shots, build_max, build_cost, bar_single, bar_chain, bar_other;
    int    chains, link_covers, leeway_on, own_pill_rank, orth_only, aim_nudge, keep_all;
    double max_chain, leeway_ring_wu, leeway_top, search_top, pill_range, range_margin;
    double blocker_min_dist, nudge_step_wu, nudge_max_steps;
    int    has_leeway_min; double leeway_min;
    int    has_band; double band;
    int    has_fma; double finish_max_anger;
    double finish_min_armour, armour;
    /* env */
    double T_BUILDING, T_HALFBUILD, T_BOAT, T_FOREST;
    double e_wall_shots, e_half_wall_shots, e_boat_shots;
    int    forest_transparent;
    /* env.omx / env.omy */
    int    om_set, om_sent;
    double omx, omy, sent_omx, sent_omy;
    /* ctx */
    PList *hostile;
    int    finish;
    double tt_hits;
    /* reach_cache: sq -> 0 / 1 (2 = not asked) */
    int64_t *rc_sq; char *rc_val; int rc_n, rc_cap;
    int    memo_n;
} Ev;

static double NB[8][2];
static int    s_nb_ready = 0;

/* ── Helpers ────────────────────────────────────────────────────────── */
static double f32(double x) { return (double)(float)x; }

static double round_double(double x) {
    if (x >= 0) return floor(x + 0.5);
    return -floor(-x + 0.5);
}

/* LuaJIT lj_num2bit: round to int32 the way bit.* does. */
static int32_t lj_tobit(double n) {
    union { double d; uint64_t u; } o;
    o.d = n + 6755399441055744.0;
    return (int32_t)(uint32_t)o.u;
}

static double rsh8(double x) {
    return (double)(int32_t)(((uint32_t)lj_tobit(x)) >> 8);
}

static double calc_angle(double o1x, double o1y, double o2x, double o2y) {
    double ddx = o2x - o1x, ddy = o2y - o1y;
    double gx = ddx < 0 ? -ddx : ddx;
    double gy = ddy < 0 ? -ddy : ddy;
    double a = atan(gx / gy);
    a = (a / PT2_RADIANS_MAX) * 360.0;
    double r = f32((256.0 / 360.0) * a);
    if (ddx <= 0 && ddy <= 0) r = f32(256.0 - r);
    else if (ddx > 0 && ddy >= 0) r = f32(128.0 - r);
    else if (ddx <= 0 && ddy >= 0) r = f32(r + 128);
    return r;
}

static double tank_angle(double ox, double oy, double tx, double ty) {
    double dx = tx - ox, dy = ty - oy;
    if (dx == 0 && dy == 0) return 0;
    double a = f32(f32(atan2(dx, -dy)) * f32(128.0 / 3.14159265358979323846));
    a = fmod(fmod(a, 256.0) + 256.0, 256.0);
    return f32(a);
}

static double sincos_args(double angle) {
    double a = f32(angle - 64);
    if (a < 0) a = f32(a + 256.0);
    double db = (360.0 / 256.0) * a;
    return (db / 360.0) * PT2_RADIANS_MAX;
}

static void calc_distance(double angle, double speed, double *ox, double *oy) {
    double db = sincos_args(angle);
    *ox = round_double(speed * cos(db));
    *oy = round_double(speed * sin(db));
}

static void calc_distance_hp(double angle, double speed, double *ox, double *oy) {
    double db = sincos_args(angle);
    *ox = round_double(speed * cos(db) * 256.0);
    *oy = round_double(speed * sin(db) * 256.0);
}

static double life_ticks(double len) {
    double t = 1 + floor(PT2_SHELL_LIFE * len) - PT2_START_ADD;
    return t < 0 ? 0 : t;
}

/* ── Lua calls ──────────────────────────────────────────────────────── */
static int call_b2(Ev *e, int fn, double a, double b) {
    lua_State *L = e->L;
    lua_pushvalue(L, fn);
    lua_pushnumber(L, a);
    lua_pushnumber(L, b);
    lua_call(L, 2, 1);
    int r = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return r;
}

static int in_map(Ev *e, double mx, double my)    { return call_b2(e, S_IN_MAP, mx, my); }
static int stay_in(Ev *e, double mx, double my)   { return call_b2(e, S_STAY_IN, mx, my); }
static int standable(Ev *e, double mx, double my) { return call_b2(e, S_STANDABLE, mx, my); }
static int buildable(Ev *e, double mx, double my) { return call_b2(e, S_BUILDABLE, mx, my); }

static void set_om(Ev *e, double mx, double my) {
    e->omx = mx; e->omy = my; e->om_set = 1;
}

static void sync_om(Ev *e) {
    if (!e->om_set) return;
    if (e->om_sent && e->sent_omx == e->omx && e->sent_omy == e->omy) return;
    lua_State *L = e->L;
    lua_pushnumber(L, e->omx); lua_setfield(L, S_ENV, "omx");
    lua_pushnumber(L, e->omy); lua_setfield(L, S_ENV, "omy");
    e->om_sent = 1; e->sent_omx = e->omx; e->sent_omy = e->omy;
}

/* The pill index of the table at stack index idx (added when new). */
static int pill_index(Ev *e, int idx) {
    lua_State *L = e->L;
    Pt2Ctx *c = e->c;
    if (idx < 0) idx = lua_gettop(L) + idx + 1;
    lua_pushvalue(L, idx);
    lua_rawget(L, S_PIDX);
    if (lua_type(L, -1) == LUA_TNUMBER) {
        int k = (int)lua_tonumber(L, -1);
        lua_pop(L, 1);
        return k;
    }
    lua_pop(L, 1);
    if (c->npills + 1 >= c->cap_pills) {
        int nc = c->cap_pills ? c->cap_pills * 2 : 64;
        Pill **np = (Pill **)realloc(c->pills, sizeof(Pill *) * (size_t)nc);
        if (!np) { luaL_error(L, "gh_pt2: out of memory"); return 0; }
        c->pills = np; c->cap_pills = nc;
    }
    int k = ++c->npills;
    Pill *p = (Pill *)ar_alloc(L, &c->ar, sizeof(Pill));
    c->pills[k] = p;
    memset(p, 0, sizeof(*p));
    p->ally = -1; p->skip = -1;
    lua_getfield(L, idx, "mx");
    p->has_mx = !lua_isnil(L, -1);
    p->mx = lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "my");
    p->my = lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "health");
    p->health = lua_isnil(L, -1) ? 0 : lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "in_tank");
    p->in_tank = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "pos_unknown");
    p->pos_unknown = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_pushvalue(L, idx);
    lua_rawseti(L, S_PILLS, k);
    lua_pushvalue(L, idx);
    lua_pushinteger(L, k);
    lua_rawset(L, S_PIDX);
    return k;
}

static Pill *PI(Ev *e, int k) { return e->c->pills[k]; }

static void push_pill(Ev *e, int k) { lua_rawgeti(e->L, S_PILLS, k); }

static int alive(Ev *e, int k) {
    Pill *p = PI(e, k);
    return p->health > 0 && !p->in_tank && !p->pos_unknown && p->has_mx;
}

static int is_ally(Ev *e, int k) {
    Pill *p = PI(e, k);
    if (p->ally < 0) {
        lua_State *L = e->L;
        lua_pushvalue(L, S_IS_ALLY);
        push_pill(e, k);
        lua_call(L, 1, 1);
        p->ally = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    return p->ally;
}

static double cover_of_pill(Ev *e, int k) {
    Pill *p = PI(e, k);
    if (!p->cover_set) {
        lua_State *L = e->L;
        lua_pushvalue(L, S_COVER);
        push_pill(e, k);
        lua_call(L, 1, 1);
        p->cover = lua_tonumber(L, -1);
        p->cover_set = 1;
        lua_pop(L, 1);
    }
    return p->cover;
}

static int chain_skip(Ev *e, int k) {
    Pill *p = PI(e, k);
    if (p->skip < 0) {
        lua_State *L = e->L;
        if (lua_isnil(L, S_CHAIN_SKIP) || lua_toboolean(L, S_CHAIN_SKIP) == 0) {
            p->skip = 0;
        } else {
            push_pill(e, k);
            lua_gettable(L, S_CHAIN_SKIP);
            p->skip = lua_toboolean(L, -1);
            lua_pop(L, 1);
        }
    }
    return p->skip;
}

static double call_ttype(Ev *e, double mx, double my) {
    lua_State *L = e->L;
    lua_pushvalue(L, S_TTYPE);
    lua_pushnumber(L, mx);
    lua_pushnumber(L, my);
    lua_call(L, 2, 1);
    double v = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : NAN;
    lua_pop(L, 1);
    return v;
}

static double ttype(Ev *e, double mx, double my) {
    if (mx >= 0 && mx <= 255 && my >= 0 && my <= 255) {
        int k = (int)my * 256 + (int)mx;
        Pt2Ctx *c = e->c;
        if (c->tt_stamp[k] == c->gen) { e->tt_hits += 1; return c->tt_val[k]; }
        double v = call_ttype(e, mx, my);
        c->tt_stamp[k] = c->gen;
        c->tt_val[k] = v;
        return v;
    }
    return call_ttype(e, mx, my);
}

static int call_pill_on(Ev *e, double mx, double my) {
    lua_State *L = e->L;
    lua_pushvalue(L, S_PILL_ON);
    lua_pushnumber(L, mx);
    lua_pushnumber(L, my);
    lua_call(L, 2, 1);
    int k = 0;
    if (lua_istable(L, -1)) k = pill_index(e, -1);
    lua_pop(L, 1);
    return k;
}

static int pill_on(Ev *e, double mx, double my) {
    if (mx >= 0 && mx <= 255 && my >= 0 && my <= 255) {
        int k = (int)my * 256 + (int)mx;
        Pt2Ctx *c = e->c;
        if (c->po_stamp[k] == c->gen) return c->po_val[k];
        int v = call_pill_on(e, mx, my);
        c->po_stamp[k] = c->gen;
        c->po_val[k] = v;
        return v;
    }
    return call_pill_on(e, mx, my);
}

static int base_stops(Ev *e, double mx, double my) {
    lua_State *L = e->L;
    if (lua_isnil(L, S_BASE_STOPS) || !lua_toboolean(L, S_BASE_STOPS)) return 0;
    if (lua_istable(L, S_BASE_AT)) {
        lua_pushnumber(L, my * 256 + mx);
        lua_rawget(L, S_BASE_AT);
        int has = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (!has) return 0;
    }
    sync_om(e);
    return call_b2(e, S_BASE_STOPS, mx, my);
}

/* ── Objects ────────────────────────────────────────────────────────── */
#define NEW(e, T) ((T *)ar_alloc((e)->L, &(e)->c->ar, sizeof(T)))
#define NEWN(e, T, n) ((T *)ar_alloc((e)->L, &(e)->c->ar, sizeof(T) * (size_t)((n) > 0 ? (n) : 1)))

static PList *plist_new(Ev *e, int cap) {
    PList *l = NEW(e, PList);
    l->n = 0; l->cap = cap > 0 ? cap : 4; l->memo = 0; l->memo_ic = 0;
    l->v = NEWN(e, int, l->cap);
    return l;
}

static void plist_push(Ev *e, PList *l, int k) {
    if (l->n >= l->cap) {
        int nc = l->cap * 2;
        int *nv = NEWN(e, int, nc);
        memcpy(nv, l->v, sizeof(int) * (size_t)l->n);
        l->v = nv; l->cap = nc;
    }
    l->v[l->n++] = k;
}

/* in_chain[k]: the 1-based place of pill k in the chain, or 0. */
static int in_chain(PList *chain, int k) {
    if (!chain || !k) return 0;
    for (int i = 0; i < chain->n; i++) if (chain->v[i] == k) return i + 1;
    return 0;
}

static Extra *extra_new(Ev *e) {
    Extra *x = NEW(e, Extra);
    x->n = 0; x->cap = 4; x->memo = 0;
    x->sq = NEWN(e, int64_t, 4);
    x->shots = NEWN(e, double, 4);
    return x;
}

static int extra_find(const Extra *x, int64_t sq) {
    if (!x) return -1;
    for (int i = 0; i < x->n; i++) if (x->sq[i] == sq) return i;
    return -1;
}

static void extra_set(Ev *e, Extra *x, int64_t sq, double shots) {
    int i = extra_find(x, sq);
    if (i >= 0) { x->shots[i] = shots; return; }
    if (x->n >= x->cap) {
        int nc = x->cap * 2;
        int64_t *ns = NEWN(e, int64_t, nc);
        double *nsh = NEWN(e, double, nc);
        memcpy(ns, x->sq, sizeof(int64_t) * (size_t)x->n);
        memcpy(nsh, x->shots, sizeof(double) * (size_t)x->n);
        x->sq = ns; x->shots = nsh; x->cap = nc;
    }
    x->sq[x->n] = sq; x->shots[x->n] = shots; x->n++;
}

static void extra_del(Extra *x, int64_t sq) {
    int i = extra_find(x, sq);
    if (i < 0) return;
    for (int j = i + 1; j < x->n; j++) { x->sq[j - 1] = x->sq[j]; x->shots[j - 1] = x->shots[j]; }
    x->n--;
}

/* ── shadow.cast ────────────────────────────────────────────────────── */
/* keep = 0: the ray is scratch (hold_safe), only hit/link/cover are read. */
static Ray *cast(Ev *e, double sx, double sy, double tx, double ty, double len,
                 const Extra *extra, int keep, Ray *scratch) {
    int64_t sq_list[PT2_MAX_T];
    double  sq_t[PT2_MAX_T];
    Blk     block[PT2_MAX_T];
    Ray *res = keep ? NEW(e, Ray) : scratch;
    memset(res, 0, sizeof(*res));
    double angle = calc_angle(sx, sy, tx, ty);
    res->angle = angle;
    double xa, ya, xs, ys;
    calc_distance(angle, PT2_SHELL_SPEED, &xa, &ya);
    calc_distance_hp(angle, PT2_SHELL_SPEED, &xs, &ys);
    double x = sx + PT2_START_ADD * xa;
    double y = sy + PT2_START_ADD * ya;
    double xc = 0, yc = 0;
    double n = life_ticks(len);
    double r = PT2_TANK_HIT_R;
    double r2 = r * r;
    int nsq = 0, nblk = 0;
    double last_sq = -1;
    double cover = 0, front = 0;
    int link = 0; double link_t = 0;
    int tank = 0; double hit_t = n;
    for (double t = 1; t <= n; t++) {
        xc = xc + xs; yc = yc + ys;
        double xm = floor(xc / 256), ym = floor(yc / 256);
        xc = xc - xm * 256; yc = yc - ym * 256;
        x = x + xm; y = y + ym;
        double mx = rsh8(x), my = rsh8(y);
        double sq = my * 256 + mx;
        int new_sq = sq != last_sq;
        /* 1. a live pill on the square */
        if (new_sq) {
            int p = pill_on(e, mx, my);
            if (p) {
                if (is_ally(e, p)) {
                    double s = cover_of_pill(e, p);
                    cover = cover + s;
                    if (!link) front = front + s;
                    if (nblk < PT2_MAX_T) {
                        Blk *b = &block[nblk];
                        b->sq = (int64_t)sq; b->shots = s; b->kind = BK_ALLY_PILL; b->t = t; b->pill = p;
                    }
                    nblk++;
                } else if (!link) {
                    link = p; link_t = t;
                }
            }
        }
        /* 2. the tank circle */
        double dx = x - tx, dy = y - ty;
        if (dx < r && dx > -r && dy < r && dy > -r && dx * dx + dy * dy < r2) {
            tank = 1; hit_t = t;
            break;
        }
        /* 3. terrain, then planned blockers */
        if (new_sq) {
            if (nsq < PT2_MAX_T) { sq_list[nsq] = (int64_t)sq; sq_t[nsq] = t; }
            nsq++;
            double s = 0; int kind = -1;
            int ei = extra_find(extra, (int64_t)sq);
            if (ei >= 0) {
                s = extra->shots[ei]; kind = BK_PLANNED;
            } else {
                double tt = ttype(e, mx, my);
                if (tt == e->T_BUILDING) { s = e->e_wall_shots; kind = BK_WALL; }
                else if (tt == e->T_HALFBUILD) { s = e->e_half_wall_shots; kind = BK_HALF_WALL; }
                else if (tt == e->T_BOAT) { s = e->e_boat_shots; kind = BK_BOAT; }
                else if (tt == e->T_FOREST && !e->forest_transparent) { s = 1; kind = BK_FOREST; }
            }
            if (kind >= 0) {
                cover = cover + s;
                if (!link) front = front + s;
                if (nblk < PT2_MAX_T) {
                    Blk *b = &block[nblk];
                    b->sq = (int64_t)sq; b->shots = s; b->kind = kind; b->t = t; b->pill = 0;
                }
                nblk++;
            }
            last_sq = sq;
        }
    }
    if (nsq > PT2_MAX_T || nblk > PT2_MAX_T) luaL_error(e->L, "gh_pt2: ray too long");
    res->tank = tank; res->t = hit_t;
    res->link = link; res->link_t = link_t;
    res->cover = cover; res->front = front;
    res->n_sq = nsq; res->n_block = nblk;
    res->end_x = x; res->end_y = y;
    if (keep) {
        res->sq = NEWN(e, int64_t, nsq);
        res->sq_t = NEWN(e, double, nsq);
        res->blk = NEWN(e, Blk, nblk);
        memcpy(res->sq, sq_list, sizeof(int64_t) * (size_t)nsq);
        memcpy(res->sq_t, sq_t, sizeof(double) * (size_t)nsq);
        memcpy(res->blk, block, sizeof(Blk) * (size_t)nblk);
    } else {
        res->sq = NULL; res->sq_t = NULL; res->blk = NULL;
    }
    return res;
}

/* ── shadow.tank_shot ───────────────────────────────────────────────── */
/* Returns the why (TS_OK when the shell enters the target square). On
 * TS_OK *lane_out is the lane (a new object). */
static int tank_shot(Ev *e, double ox, double oy, double ax, double ay, double tmx, double tmy,
                     const Extra *extra, Lane **lane_out) {
    int64_t lane[PT2_MAX_T];
    double angle = tank_angle(ox, oy, ax, ay);
    double xa, ya, xs, ys;
    calc_distance(angle, PT2_SHELL_SPEED, &xa, &ya);
    calc_distance_hp(angle, PT2_SHELL_SPEED, &xs, &ys);
    double x = ox + PT2_START_ADD * xa;
    double y = oy + PT2_START_ADD * ya;
    double xc = 0, yc = 0;
    double n = life_ticks(PT2_TANK_FIRE_LEN);
    int nl = 0;
    double last_sq = -1;
    double omx = rsh8(ox), omy = rsh8(oy);
    int why = TS_SHORT;
    for (double t = 1; t <= n; t++) {
        xc = xc + xs; yc = yc + ys;
        double xm = floor(xc / 256), ym = floor(yc / 256);
        xc = xc - xm * 256; yc = yc - ym * 256;
        x = x + xm; y = y + ym;
        double mx = rsh8(x), my = rsh8(y);
        double sq = my * 256 + mx;
        if (sq != last_sq) {
            last_sq = sq;
            if (mx == tmx && my == tmy) { why = TS_OK; break; }
            if (!(mx == omx && my == omy)) {
                if (nl < PT2_MAX_T) lane[nl] = (int64_t)sq;
                nl++;
                if (pill_on(e, mx, my)) { why = TS_PILL; break; }
                if (extra_find(extra, (int64_t)sq) >= 0) { why = TS_PLANNED; break; }
                double tt = ttype(e, mx, my);
                if (tt == e->T_BUILDING || tt == e->T_HALFBUILD) { why = TS_WALL; break; }
                if (base_stops(e, mx, my)) { why = TS_BASE; break; }
            }
        }
    }
    if (why == TS_OK && lane_out) {
        if (nl > PT2_MAX_T) luaL_error(e->L, "gh_pt2: lane too long");
        Lane *l = NEW(e, Lane);
        l->n = nl; l->memo = 0;
        l->v = NEWN(e, int64_t, nl);
        memcpy(l->v, lane, sizeof(int64_t) * (size_t)nl);
        *lane_out = l;
    }
    return why;
}

/* ── One tank point ─────────────────────────────────────────────────── */
static PList *pills_in_range(Ev *e, double px, double py, int *hasA) {
    double pr = e->pill_range + e->range_margin;
    double pr2 = pr * pr;
    PList *T = plist_new(e, e->hostile->n);
    *hasA = 0;
    for (int i = 0; i < e->hostile->n; i++) {
        int k = e->hostile->v[i];
        Pill *p = PI(e, k);
        double cx = p->mx * 256 + 128, cy = p->my * 256 + 128;
        double dx = cx - px, dy = cy - py;
        if (dx * dx + dy * dy <= pr2) {
            plist_push(e, T, k);
            if (k == e->A) *hasA = 1;
        }
    }
    return T;
}

static int nudge_aim(Ev *e, double px, double py, const Extra *extra, const int *shorts, int nshort,
                     double *onx, double *ony, double *onw, int *oai, Lane **oln) {
    Pill *A = PI(e, e->A);
    double acx = A->mx * 256 + 128, acy = A->my * 256 + 128;
    double dx = acx - px, dy = acy - py;
    double d = sqrt(dx * dx + dy * dy);
    if (d < 1) return 0;
    int live[PT2_NAIMS];
    int nlive = nshort;
    for (int i = 0; i < nshort; i++) live[i] = shorts[i];
    double tx = A->mx * 256, ty = A->my * 256;
    for (double step = 1; step <= e->nudge_max_steps; step++) {
        double nw = step * e->nudge_step_wu;
        double frac = nw / d;
        double nx = floor(px + dx * frac + 0.5), ny = floor(py + dy * frac + 0.5);
        double mx = floor(nx / 256), my = floor(ny / 256);
        if (in_map(e, mx, my) && stay_in(e, mx, my) && standable(e, mx, my)) {
            set_om(e, mx, my);
            int n = 0;
            for (int i = 0; i < nlive; i++) {
                int ai = live[i];
                Lane *ln = NULL;
                int why = tank_shot(e, nx, ny, tx + s_aims[ai - 1][0], ty + s_aims[ai - 1][1],
                                    A->mx, A->my, extra, &ln);
                if (why == TS_OK) {
                    *onx = nx; *ony = ny; *onw = nw; *oai = ai; *oln = ln;
                    return 1;
                }
                if (why == TS_SHORT) live[n++] = ai;
            }
            nlive = n;
            if (n == 0) return 0;
        }
    }
    return 0;
}

static Rec *eval_point(Ev *e, double px, double py, Extra *extra, int aim_fixed,
                       Rec *prev, int64_t new_sq, int nudge, int *why, int *why_arg) {
    PList *T;
    int aim_idx = 0;
    Lane *lane = NULL;
    Aims *clear;
    Lanes *lanes;
    double nudge_wu = prev ? prev->nudge_wu : 0;
    Pill *A = PI(e, e->A);
    if (prev) {
        T = prev->T; aim_idx = prev->aim_idx; lane = prev->lane;
        clear = prev->clear; lanes = prev->lanes;
        set_om(e, floor(px / 256), floor(py / 256));
    } else {
        int hasA;
        T = pills_in_range(e, px, py, &hasA);
        if (!hasA) { *why = W_A_OUT; *why_arg = 0; return NULL; }
        set_om(e, floor(px / 256), floor(py / 256));
        double amx = A->mx, amy = A->my;
        int a1 = 1, a2 = PT2_NAIMS;
        if (aim_fixed) { a1 = aim_fixed; a2 = aim_fixed; }
        int why_out = TS_SHORT;
        int shorts[PT2_NAIMS], ns = 0;
        for (int ai = a1; ai <= a2; ai++) {
            Lane *ln = NULL;
            int w = tank_shot(e, px, py, amx * 256 + s_aims[ai - 1][0], amy * 256 + s_aims[ai - 1][1],
                              amx, amy, extra, &ln);
            if (w == TS_OK) { aim_idx = ai; lane = ln; break; }
            why_out = w;
            if (w == TS_SHORT) shorts[ns++] = ai;
        }
        if (!aim_idx && nudge && ns > 0 && e->aim_nudge && !aim_fixed) {
            double nx, ny, nw; int nai; Lane *nln;
            if (nudge_aim(e, px, py, extra, shorts, ns, &nx, &ny, &nw, &nai, &nln)) {
                px = nx; py = ny; nudge_wu = nw; aim_idx = nai; lane = nln;
                int dummy;
                T = pills_in_range(e, px, py, &dummy);
                set_om(e, floor(px / 256), floor(py / 256));
            }
        }
        if (!aim_idx) { *why = W_OUTGOING; *why_arg = why_out; return NULL; }
        clear = NEW(e, Aims);
        clear->n = 1; clear->v[0] = aim_idx; clear->memo = 0;
        lanes = NEW(e, Lanes);
        lanes->n = 1; lanes->ai[0] = aim_idx; lanes->ln[0] = lane; lanes->memo = 0;
    }
    /* rays */
    RayList *rays = NEW(e, RayList);
    rays->n = T->n; rays->memo = 0;
    rays->v = NEWN(e, Ray *, T->n);
    for (int i = 0; i < T->n; i++) {
        Ray *pr = (prev && i < prev->rays->n) ? prev->rays->v[i] : NULL;
        int same = 0;
        if (pr) {
            same = 1;
            for (int k = 0; k < pr->n_sq; k++) {
                if (pr->sq[k] == new_sq) { same = 0; break; }
            }
        }
        if (same) {
            rays->v[i] = pr;
        } else {
            Pill *X = PI(e, T->v[i]);
            double sx = X->mx * 256 + 128, sy = X->my * 256 + 128;
            Ray *r = cast(e, sx, sy, px, py, PT2_PILL_FIRE_LEN, extra, 1, NULL);
            r->X = T->v[i];
            rays->v[i] = r;
        }
    }
    /* chain */
    PList *chain = plist_new(e, 4);
    plist_push(e, chain, e->A);
    int chain_mode = 0;
    if (e->chains) {
        int cur = e->A;
        for (int it = 0; it < 8; it++) {
            Ray *r = NULL;
            for (int i = 0; i < T->n; i++) if (T->v[i] == cur) r = rays->v[i];
            int nxt = r ? r->link : 0;
            if (!nxt || in_chain(chain, nxt) || chain_skip(e, nxt)) break;
            plist_push(e, chain, nxt);
            chain_mode = 1;
            cur = nxt;
        }
    }
    /* cover per ray */
    int covered_all = 1;
    IList *unc = NEW(e, IList);
    unc->n = 0; unc->memo = 0;
    unc->v = NEWN(e, int, T->n);
    double need_A = 0, have_A = 0;
    int ally_cover = 0;
    for (int i = 0; i < T->n; i++) {
        Ray *r = rays->v[i];
        int X = T->v[i];
        int ci = in_chain(chain, X);
        double bar;
        if (X == e->A && e->finish) bar = 0;
        else if (X == e->A && !chain_mode) bar = e->bar_single;
        else if (ci) bar = e->bar_chain;
        else bar = e->bar_other;
        r->bar = bar;
        r->link_cover = (e->link_covers && ci && r->link != 0 && ci < chain->n
                         && r->link == chain->v[ci]) ? 1 : 0;
        int ok = (!r->tank) || r->link_cover || (r->cover >= bar);
        r->ok = ok;
        if (X == e->A) { need_A = bar; have_A = (!r->tank) ? bar : r->cover; }
        if (!ok) {
            covered_all = 0;
            unc->v[unc->n++] = i + 1;
        }
        if (ok && r->tank) {
            for (int k = 0; k < r->n_block; k++)
                if (r->blk[k].kind == BK_ALLY_PILL) ally_cover = 1;
        }
    }
    int max_ok = (double)chain->n <= e->max_chain;
    int last = chain->v[chain->n - 1];
    int have_depth = 0;
    double depth = 0;
    for (int i = 0; i < T->n; i++) {
        if (T->v[i] == last) {
            Ray *r = rays->v[i];
            depth = (!r->tank) ? (r->cover > r->bar ? r->cover : r->bar) : r->cover;
            have_depth = 1;
        }
    }
    if (!have_depth) depth = chain_mode ? e->bar_chain : e->bar_single;
    Rec *rec = NEW(e, Rec);
    memset(rec, 0, sizeof(*rec));
    rec->px = px; rec->py = py; rec->nudge_wu = nudge_wu;
    rec->T = T; rec->rays = rays; rec->chain = chain; rec->chain_mode = chain_mode;
    rec->aim_idx = aim_idx; rec->lane = lane; rec->clear = clear; rec->lanes = lanes;
    rec->aims_done = aim_fixed ? aim_fixed : (prev ? prev->aims_done : aim_idx);
    if (extra) { rec->extra_in = extra; rec->flags |= RF_EXTRA_IN; }
    rec->covered = covered_all; rec->uncovered = unc;
    rec->safe = covered_all && max_ok; rec->max_ok = max_ok;
    rec->kills = (chain_mode && max_ok) ? (double)chain->n : 1;
    rec->ally_cover = ally_cover; rec->need = need_A; rec->have = have_A; rec->depth = depth;
    rec->finish = e->finish;
    return rec;
}

static Aims *clear_aims_of(Ev *e, Rec *rec) {
    if (rec->aims_done < PT2_NAIMS) {
        Pill *A = PI(e, e->A);
        set_om(e, floor(rec->px / 256), floor(rec->py / 256));
        for (double ai = rec->aims_done + 1; ai <= PT2_NAIMS; ai++) {
            int a = (int)ai;
            Lane *ln = NULL;
            int w = tank_shot(e, rec->px, rec->py, A->mx * 256 + s_aims[a - 1][0],
                              A->my * 256 + s_aims[a - 1][1], A->mx, A->my,
                              (rec->flags & RF_EXTRA_IN) ? rec->extra_in : NULL, &ln);
            if (w == TS_OK) {
                if (rec->clear->n >= PT2_NAIMS) luaL_error(e->L, "gh_pt2: too many aims");
                rec->clear->v[rec->clear->n++] = a;
                Lanes *ls = rec->lanes;
                int j;
                for (j = 0; j < ls->n; j++) if (ls->ai[j] == a) break;
                if (j == ls->n) { ls->ai[ls->n] = a; ls->n++; }
                ls->ln[j] = ln;
            }
        }
        rec->aims_done = PT2_NAIMS;
    }
    return rec->clear;
}

static Lane *lanes_get(const Lanes *ls, int ai) {
    for (int j = 0; j < ls->n; j++) if (ls->ai[j] == ai) return ls->ln[j];
    return NULL;
}

static Rec *with_aim(Ev *e, Rec *rec, int ai) {
    Rec *o = NEW(e, Rec);
    *o = *rec;
    o->memo = 0;
    o->aim_idx = ai; o->lane = lanes_get(rec->lanes, ai);
    o->clear = NEW(e, Aims);
    o->clear->n = 1; o->clear->v[0] = ai; o->clear->memo = 0;
    o->lanes = NEW(e, Lanes);
    o->lanes->n = 1; o->lanes->ai[0] = ai; o->lanes->ln[0] = o->lane; o->lanes->memo = 0;
    o->aims_done = ai;
    return o;
}

/* ── Blocker search ─────────────────────────────────────────────────── */
static int lane_has(const Lane *l, int64_t sq) {
    if (!l) return 0;
    for (int i = 0; i < l->n; i++) if (l->v[i] == sq) return 1;
    return 0;
}

static int slot_ok(Ev *e, Rec *rec, Ray *r, int k, const Extra *extra_tmp) {
    int64_t sq = r->sq[k];
    double sqd = (double)sq;
    double mx = sqd - floor(sqd / 256) * 256, my = floor(sqd / 256);
    Pill *X = PI(e, r->X);
    if (lane_has(rec->lane, sq)) return 0;
    if (extra_tmp && extra_find(extra_tmp, sq) >= 0) return 0;
    if (r->link && r->sq_t[k] <= r->link_t) return 0;
    if (e->orth_only && r->X == e->A && fabs(mx - X->mx) + fabs(my - X->my) != 1) return 0;
    if (!in_map(e, mx, my) || !stay_in(e, mx, my)) return 0;
    double cx = mx * 256 + 128, cy = my * 256 + 128;
    double dxp = cx - rec->px, dyp = cy - rec->py;
    double md = e->blocker_min_dist * 256;
    if (dxp * dxp + dyp * dyp < md * md) return 0;
    double sx = X->mx * 256 + 128, sy = X->my * 256 + 128;
    double d1 = (cx - sx) * (cx - sx) + (cy - sy) * (cy - sy);
    double d2 = (rec->px - sx) * (rec->px - sx) + (rec->py - sy) * (rec->py - sy);
    if (d1 >= d2) return 0;
    if (pill_on(e, mx, my)) return 0;
    double tt = ttype(e, mx, my);
    if (tt == e->T_BUILDING || tt == e->T_HALFBUILD) return 0;
    if (!buildable(e, mx, my)) return 0;
    for (int i = 0; i < e->rc_n; i++) {
        if (e->rc_sq[i] == sq) return e->rc_val[i];
    }
    lua_State *L = e->L;
    lua_pushvalue(L, S_LGM_REACH);
    lua_pushnumber(L, rec->omx);
    lua_pushnumber(L, rec->omy);
    lua_pushnumber(L, mx);
    lua_pushnumber(L, my);
    lua_call(L, 4, 1);
    int rc = lua_toboolean(L, -1) ? 1 : 0;
    lua_pop(L, 1);
    if (e->rc_n >= e->rc_cap) {
        int nc = e->rc_cap ? e->rc_cap * 2 : 64;
        int64_t *ns = NEWN(e, int64_t, nc);
        char *nv = NEWN(e, char, nc);
        if (e->rc_n) {
            memcpy(ns, e->rc_sq, sizeof(int64_t) * (size_t)e->rc_n);
            memcpy(nv, e->rc_val, (size_t)e->rc_n);
        }
        e->rc_sq = ns; e->rc_val = nv; e->rc_cap = nc;
    }
    e->rc_sq[e->rc_n] = sq; e->rc_val[e->rc_n] = (char)rc; e->rc_n++;
    return rc;
}

static Rec *search_one(Ev *e, Rec *rec) {
    Extra *extra = extra_new(e);
    Plan *plan = NEW(e, Plan);
    plan->n = 0; plan->cap = 4; plan->memo = 0;
    plan->v = NEWN(e, PlanE, 4);
    double pills_left = e->num_pill_blockers;
    /* tried: squares picked before (few) */
    int64_t tried[64]; int ntried = 0;
    Rec *cur = rec;
    int A_link0 = rec->chain->n >= 2 ? rec->chain->v[1] : 0;
    for (double it = 1; it <= e->build_max; it++) {
        if (cur->covered) break;
        int64_t picked = -1; int have_pick = 0;
        for (int u = 0; u < cur->uncovered->n && !have_pick; u++) {
            Ray *r = cur->rays->v[cur->uncovered->v[u] - 1];
            for (int k = 0; k < r->n_sq; k++) {
                int64_t sq = r->sq[k];
                int was = 0;
                for (int j = 0; j < ntried; j++) if (tried[j] == sq) { was = 1; break; }
                if (!was) {
                    if (slot_ok(e, cur, r, k, extra)) { picked = sq; have_pick = 1; break; }
                }
            }
        }
        if (!have_pick) break;
        if (ntried < 64) tried[ntried++] = picked;
        else luaL_error(e->L, "gh_pt2: too many blockers");
        double shots; int kind_pill;
        if (pills_left > 0) {
            shots = e->pill_drop_shots; kind_pill = 1;
            pills_left = pills_left - 1;
        } else {
            shots = e->wall_shots; kind_pill = 0;
        }
        extra_set(e, extra, picked, shots);
        int w, wa;
        Rec *nrec = eval_point(e, rec->px, rec->py, extra, rec->aim_idx, cur, picked, 0, &w, &wa);
        int link1 = (nrec && nrec->chain->n >= 2) ? nrec->chain->v[1] : 0;
        if (!nrec || link1 != A_link0) {
            extra_del(extra, picked);
            if (kind_pill) pills_left = pills_left + 1;
        } else {
            if (plan->n >= plan->cap) {
                int nc = plan->cap * 2;
                PlanE *nv = NEWN(e, PlanE, nc);
                memcpy(nv, plan->v, sizeof(PlanE) * (size_t)plan->n);
                plan->v = nv; plan->cap = nc;
            }
            PlanE *pe = &plan->v[plan->n++];
            double pd = (double)picked;
            pe->sq = picked; pe->mx = pd - floor(pd / 256) * 256; pe->my = floor(pd / 256);
            pe->shots = shots; pe->kind_pill = kind_pill;
            nrec->omx = rec->omx; nrec->omy = rec->omy;
            nrec->flags |= (rec->flags & RF_OMXY) ? RF_OMXY : 0;
            cur = nrec;
        }
    }
    cur->plan = plan; cur->extra = extra; cur->flags |= RF_PLAN;
    return cur;
}

static Rec *search_blockers(Ev *e, Rec *rec) {
    Rec *best = NULL;
    Aims *ca = clear_aims_of(e, rec);
    int list[PT2_NAIMS], nl = ca->n;
    for (int i = 0; i < nl; i++) list[i] = ca->v[i];
    for (int i = 0; i < nl; i++) {
        int ai = list[i];
        Rec *base = rec;
        if (ai != rec->aim_idx) base = with_aim(e, rec, ai);
        Rec *r = search_one(e, base);
        if (!best
            || (r->covered && !best->covered)
            || (r->covered == best->covered && (r->plan->n < best->plan->n
                || (r->plan->n == best->plan->n && r->have > best->have)))) {
            best = r;
        }
        if (best->covered && best->plan->n == 0) break;
    }
    return best ? best : search_one(e, rec);
}

static int leeway(Ev *e, Rec *rec, double ring_wu, LeePt **out) {
    int n = 0;
    LeePt *res = NEWN(e, LeePt, 8);
    Extra *ex = (rec->flags & RF_PLAN) ? rec->extra : NULL;
    for (int k = 0; k < 8; k++) {
        double qx = floor(rec->px + NB[k][0] * ring_wu + 0.5);
        double qy = floor(rec->py + NB[k][1] * ring_wu + 0.5);
        int why = W_NONE, why_arg = 0;
        Rec *q = eval_point(e, qx, qy, ex, 0, NULL, -1, 0, &why, &why_arg);
        int ok = q && q->covered && q->max_ok && (q->chain->n == rec->chain->n);
        if (ok) n++;
        if (!ok && q) {
            why = W_NONE;
            if (q->uncovered->n > 0) {
                why = W_OVERLAP;
                why_arg = q->rays->v[q->uncovered->v[0] - 1]->X;
            }
            if (why == W_NONE && q->chain->n != rec->chain->n) why = W_CHAIN;
        }
        res[k].x = qx; res[k].y = qy; res[k].ok = ok;
        res[k].why = ok ? W_NONE : (why == W_NONE ? W_QMARK : why);
        res[k].why_arg = why_arg;
    }
    *out = res;
    return n;
}

/* ── Hold point ─────────────────────────────────────────────────────── */
static int hold_safe(Ev *e, Rec *b, double hx, double hy, int *why_pill) {
    double pr = e->pill_range + e->range_margin;
    Ray scratch;
    for (int i = 0; i < e->hostile->n; i++) {
        int p = e->hostile->v[i];
        Pill *P = PI(e, p);
        double sx = P->mx * 256 + 128, sy = P->my * 256 + 128;
        double dx = sx - hx, dy = sy - hy;
        if (dx * dx + dy * dy <= pr * pr) {
            int ci = in_chain(b->chain, p);
            double bar;
            if (p == e->A && e->finish) bar = 0;
            else if (p == e->A && !b->chain_mode) bar = e->bar_single;
            else if (ci) bar = e->bar_chain;
            else bar = e->bar_other;
            Ray *r = cast(e, sx, sy, hx, hy, PT2_PILL_FIRE_LEN, NULL, 0, &scratch);
            int linked = e->link_covers && ci && r->link != 0 && ci < b->chain->n
                         && r->link == b->chain->v[ci];
            if (r->tank && !linked && r->cover < bar) { *why_pill = p; return 0; }
        }
    }
    return 1;
}

static Hold *hold_point(Ev *e, Rec *b) {
    double ux = sin(b->deg * PT2_LUA_RAD), uy = -cos(b->deg * PT2_LUA_RAD);
    double d0 = e->approach_offset;
    double dmax = d0 + (e->pill_range + e->range_margin) / 256;
    int first_why = 0;
    double d = d0;
    Hold *h = NEW(e, Hold);
    while (d <= dmax + 1e-6) {
        double fx = b->cx + ux * d, fy = b->cy + uy * d;
        double mx = floor(fx), my = floor(fy);
        if (in_map(e, mx, my) && stay_in(e, mx, my) && standable(e, mx, my)) {
            int wp = 0;
            if (hold_safe(e, b, floor(fx * 256), floor(fy * 256), &wp)) {
                h->fx = fx; h->fy = fy; h->d = d; h->safe = 1; h->why_pill = 0;
                return h;
            }
            if (!first_why) first_why = wp;
        }
        d = d + 64.0 / 256.0;
    }
    h->fx = b->cx + ux * d0; h->fy = b->cy + uy * d0; h->d = d0; h->safe = 0;
    h->why_pill = first_why;   /* 0: "no_square" */
    return h;
}

/* ── Rank ───────────────────────────────────────────────────────────── */
static double dmin(double a, double b) { return a < b ? a : b; }

/* M.better(a, b, cap, band) with this evaluate's cap and band. */
static int better(Ev *e, const Rec *a, const Rec *b) {
    if (a->safe != b->safe) return a->safe;
    if (a->kills != b->kills) return a->kills > b->kills;
    if (e->has_band && e->band > 0) {
        double sa = a->score, sb = b->score;
        double ta = (a->flags & RF_BAND) ? a->tier_min : sa;
        double tb = (b->flags & RF_BAND) ? b->tier_min : sb;
        double m = sa;
        /* math.min(sa, sb, ta, tb) */
        if (sb < m) m = sb;
        if (ta < m) m = ta;
        if (tb < m) m = tb;
        int ia = sa <= m + e->band, ib = sb <= m + e->band;
        if (ia != ib) return ia;
        if (!ia && sa != sb) return sa < sb;
    }
    if (a->kills >= 2 && a->depth != b->depth) return a->depth > b->depth;
    if (a->blk != b->blk) return a->blk > b->blk;
    if (a->kills >= 2 && a->score != b->score) return a->score < b->score;
    double cap = e->has_leeway_min ? e->leeway_min : 8;
    double la = dmin((a->flags & RF_LEEWAY) ? a->leeway : -1, cap);
    double lb = dmin((b->flags & RF_LEEWAY) ? b->leeway : -1, cap);
    if (la != lb) return la > lb;
    return a->score < b->score;
}

static int prerank_lt(Ev *e, const Rec *a, const Rec *b) {
    (void)e;
    double ka = (a->chain_mode && a->max_ok) ? (double)a->chain->n : 1;
    double kb = (b->chain_mode && b->max_ok) ? (double)b->chain->n : 1;
    if (ka != kb) return ka > kb;
    if (a->covered != b->covered) return a->covered;
    return a->base < b->base;
}

typedef int (*RecLt)(Ev *, const Rec *, const Rec *);

/* LuaJIT lib_table.c auxsort over a[1..n], comparison for comparison. */
static void auxsort(Ev *e, Rec **a, int l, int u, RecLt lt) {
    while (l < u) {
        int i, j;
        Rec *t;
        if (lt(e, a[u], a[l])) { t = a[l]; a[l] = a[u]; a[u] = t; }
        if (u - l == 1) break;
        i = (l + u) / 2;
        if (lt(e, a[i], a[l])) {
            t = a[i]; a[i] = a[l]; a[l] = t;
        } else {
            if (lt(e, a[u], a[i])) { t = a[i]; a[i] = a[u]; a[u] = t; }
        }
        if (u - l == 2) break;
        Rec *P = a[i];
        t = a[i]; a[i] = a[u - 1]; a[u - 1] = t;
        i = l; j = u - 1;
        for (;;) {
            while (lt(e, a[++i], P)) {
                if (i >= u) luaL_error(e->L, "invalid order function for sorting");
            }
            while (lt(e, P, a[--j])) {
                if (j <= l) luaL_error(e->L, "invalid order function for sorting");
            }
            if (j < i) break;
            t = a[i]; a[i] = a[j]; a[j] = t;
        }
        t = a[u - 1]; a[u - 1] = a[i]; a[i] = t;
        if (i - l < u - i) {
            j = l; i = i - 1; l = i + 2;
        } else {
            j = i + 1; i = u; u = j - 2;
        }
        auxsort(e, a, j, i, lt);
    }
}

static void set_band(Ev *e, Rec **sc, int n) {
    if (!e->has_band || !(e->band > 0)) return;
    /* key (safe, kills) -> min score, first seen order */
    double kmin[64]; int ksafe[64]; double kkills[64]; int nk = 0;
    for (int i = 1; i <= n; i++) {
        Rec *r = sc[i];
        int j;
        for (j = 0; j < nk; j++) if (ksafe[j] == r->safe && kkills[j] == r->kills) break;
        if (j == nk) {
            if (nk >= 64) luaL_error(e->L, "gh_pt2: too many tiers");
            ksafe[nk] = r->safe; kkills[nk] = r->kills; kmin[nk] = r->score; nk++;
        } else if (r->score < kmin[j]) {
            kmin[j] = r->score;
        }
    }
    for (int i = 1; i <= n; i++) {
        Rec *r = sc[i];
        for (int j = 0; j < nk; j++) {
            if (ksafe[j] == r->safe && kkills[j] == r->kills) {
                r->tier_min = kmin[j];
                r->in_band = r->score <= r->tier_min + e->band;
                r->flags |= RF_BAND;
                break;
            }
        }
    }
}

/* ── Output ─────────────────────────────────────────────────────────── */
static int memo_get(Ev *e, int memo) {
    if (!memo) return 0;
    lua_rawgeti(e->L, S_MEMO, memo);
    return 1;
}

static int memo_put(Ev *e) {
    /* table on top stays on top */
    lua_State *L = e->L;
    int k = ++e->memo_n;
    lua_pushvalue(L, -1);
    lua_rawseti(L, S_MEMO, k);
    return k;
}

static void setnum(lua_State *L, const char *k, double v) { lua_pushnumber(L, v); lua_setfield(L, -2, k); }
static void setbool(lua_State *L, const char *k, int v) { lua_pushboolean(L, v); lua_setfield(L, -2, k); }

static void push_plist(Ev *e, PList *l) {
    lua_State *L = e->L;
    if (memo_get(e, l->memo)) return;
    lua_createtable(L, l->n, 0);
    for (int i = 0; i < l->n; i++) { push_pill(e, l->v[i]); lua_rawseti(L, -2, i + 1); }
    l->memo = memo_put(e);
}

static void push_in_chain(Ev *e, PList *chain) {
    lua_State *L = e->L;
    if (memo_get(e, chain->memo_ic)) return;
    lua_createtable(L, 0, chain->n);
    for (int i = 0; i < chain->n; i++) {
        push_pill(e, chain->v[i]);
        lua_pushinteger(L, i + 1);
        lua_rawset(L, -3);
    }
    chain->memo_ic = memo_put(e);
}

static void push_lane(Ev *e, Lane *l) {
    lua_State *L = e->L;
    if (!l) { lua_pushnil(L); return; }
    if (memo_get(e, l->memo)) return;
    lua_createtable(L, l->n, 0);
    for (int i = 0; i < l->n; i++) { lua_pushnumber(L, (double)l->v[i]); lua_rawseti(L, -2, i + 1); }
    l->memo = memo_put(e);
}

static void push_aims(Ev *e, Aims *a) {
    lua_State *L = e->L;
    if (memo_get(e, a->memo)) return;
    lua_createtable(L, a->n, 0);
    for (int i = 0; i < a->n; i++) { lua_pushinteger(L, a->v[i]); lua_rawseti(L, -2, i + 1); }
    a->memo = memo_put(e);
}

static void push_lanes(Ev *e, Lanes *ls) {
    lua_State *L = e->L;
    if (memo_get(e, ls->memo)) return;
    lua_createtable(L, 0, ls->n);
    for (int i = 0; i < ls->n; i++) {
        if (!ls->ln[i]) continue;
        push_lane(e, ls->ln[i]);
        lua_rawseti(L, -2, ls->ai[i]);
    }
    ls->memo = memo_put(e);
}

static void push_extra(Ev *e, Extra *x) {
    lua_State *L = e->L;
    if (memo_get(e, x->memo)) return;
    lua_createtable(L, 0, x->n);
    for (int i = 0; i < x->n; i++) {
        lua_pushnumber(L, (double)x->sq[i]);
        lua_pushnumber(L, x->shots[i]);
        lua_rawset(L, -3);
    }
    x->memo = memo_put(e);
}

static void push_ray(Ev *e, Ray *r) {
    lua_State *L = e->L;
    if (memo_get(e, r->memo)) return;
    lua_createtable(L, 0, 20);
    r->memo = memo_put(e);
    setnum(L, "angle", r->angle);
    lua_pushstring(L, r->tank ? "tank" : "reach"); lua_setfield(L, -2, "hit");
    setnum(L, "t", r->t);
    if (r->link) {
        push_pill(e, r->link); lua_setfield(L, -2, "link");
        setnum(L, "link_t", r->link_t);
    }
    setnum(L, "cover", r->cover);
    setnum(L, "front", r->front);
    lua_createtable(L, r->n_sq, 0);
    for (int i = 0; i < r->n_sq; i++) { lua_pushnumber(L, (double)r->sq[i]); lua_rawseti(L, -2, i + 1); }
    lua_setfield(L, -2, "sq");
    lua_createtable(L, r->n_sq, 0);
    for (int i = 0; i < r->n_sq; i++) { lua_pushnumber(L, r->sq_t[i]); lua_rawseti(L, -2, i + 1); }
    lua_setfield(L, -2, "sq_t");
    lua_createtable(L, r->n_block, 0);
    for (int i = 0; i < r->n_block; i++) {
        Blk *b = &r->blk[i];
        lua_createtable(L, 0, 5);
        setnum(L, "sq", (double)b->sq);
        setnum(L, "shots", b->shots);
        lua_pushstring(L, s_bk_name[b->kind]); lua_setfield(L, -2, "kind");
        setnum(L, "t", b->t);
        if (b->pill) { push_pill(e, b->pill); lua_setfield(L, -2, "pill"); }
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "block");
    setnum(L, "n_sq", r->n_sq);
    setnum(L, "n_block", r->n_block);
    setnum(L, "end_x", r->end_x);
    setnum(L, "end_y", r->end_y);
    if (r->X) { push_pill(e, r->X); lua_setfield(L, -2, "X"); }
    setnum(L, "bar", r->bar);
    setbool(L, "link_cover", r->link_cover);
    setbool(L, "ok", r->ok);
}

static void push_raylist(Ev *e, RayList *rl) {
    lua_State *L = e->L;
    if (memo_get(e, rl->memo)) return;
    lua_createtable(L, rl->n, 0);
    rl->memo = memo_put(e);
    for (int i = 0; i < rl->n; i++) { push_ray(e, rl->v[i]); lua_rawseti(L, -2, i + 1); }
}

static void push_pill_name(Ev *e, int p) {
    lua_State *L = e->L;
    lua_pushvalue(L, S_PILL_NAME);
    lua_pushvalue(L, S_INP);
    push_pill(e, p);
    lua_call(L, 2, 1);
}

static void push_why(Ev *e, int why, int arg) {
    lua_State *L = e->L;
    switch (why) {
    case W_A_OUT: lua_pushstring(L, "A_OUT_OF_RANGE"); break;
    case W_OUTGOING: lua_pushfstring(L, "OUTGOING:%s", s_ts_name[arg]); break;
    case W_OVERLAP:
        lua_pushstring(L, "OVERLAP:");
        push_pill_name(e, arg);
        lua_concat(L, 2);
        break;
    case W_CHAIN: lua_pushstring(L, "CHAIN"); break;
    case W_QMARK: lua_pushstring(L, "?"); break;
    default: lua_pushnil(L); break;
    }
}

static void push_rec(Ev *e, Rec *r) {
    lua_State *L = e->L;
    if (memo_get(e, r->memo)) return;
    lua_createtable(L, 0, 48);
    r->memo = memo_put(e);
    setnum(L, "px", r->px); setnum(L, "py", r->py); setnum(L, "nudge_wu", r->nudge_wu);
    push_plist(e, r->T); lua_setfield(L, -2, "T");
    push_raylist(e, r->rays); lua_setfield(L, -2, "rays");
    push_plist(e, r->chain); lua_setfield(L, -2, "chain");
    push_in_chain(e, r->chain); lua_setfield(L, -2, "in_chain");
    setbool(L, "chain_mode", r->chain_mode);
    setnum(L, "aim_idx", r->aim_idx);
    push_lane(e, r->lane); lua_setfield(L, -2, "lane");
    push_aims(e, r->clear); lua_setfield(L, -2, "clear_aims");
    push_lanes(e, r->lanes); lua_setfield(L, -2, "lanes");
    setnum(L, "aims_done", r->aims_done);
    if (r->flags & RF_EXTRA_IN) { push_extra(e, r->extra_in); lua_setfield(L, -2, "extra_in"); }
    setbool(L, "covered", r->covered);
    if (!memo_get(e, r->uncovered->memo)) {
        lua_createtable(L, r->uncovered->n, 0);
        for (int i = 0; i < r->uncovered->n; i++) { lua_pushinteger(L, r->uncovered->v[i]); lua_rawseti(L, -2, i + 1); }
        r->uncovered->memo = memo_put(e);
    }
    lua_setfield(L, -2, "uncovered");
    setbool(L, "safe", r->safe); setbool(L, "max_ok", r->max_ok);
    setnum(L, "kills", r->kills);
    setbool(L, "ally_cover", r->ally_cover);
    setnum(L, "need", r->need); setnum(L, "have", r->have); setnum(L, "depth", r->depth);
    setbool(L, "finish", r->finish);
    if (r->flags & RF_OMXY) { setnum(L, "omx", r->omx); setnum(L, "omy", r->omy); }
    if (r->flags & RF_POS) {
        setnum(L, "deg", r->deg); setnum(L, "cx", r->cx); setnum(L, "cy", r->cy);
        setnum(L, "mx", r->mx); setnum(L, "my", r->my); setnum(L, "radius", r->radius);
    }
    if (r->flags & RF_BASE) setnum(L, "base", r->base);
    if (r->flags & RF_PLAN) {
        Plan *p = r->plan;
        if (memo_get(e, p->memo)) {
            lua_setfield(L, -2, "plan");
        } else {
            lua_createtable(L, p->n, 0);
            p->memo = memo_put(e);
            for (int i = 0; i < p->n; i++) {
                PlanE *pe = &p->v[i];
                lua_createtable(L, 0, 5);
                setnum(L, "sq", (double)pe->sq); setnum(L, "mx", pe->mx); setnum(L, "my", pe->my);
                setnum(L, "shots", pe->shots);
                lua_pushstring(L, pe->kind_pill ? "pill" : "wall"); lua_setfield(L, -2, "kind");
                lua_rawseti(L, -2, i + 1);
            }
            lua_setfield(L, -2, "plan");
        }
        push_extra(e, r->extra); lua_setfield(L, -2, "extra");
    }
    if (r->flags & RF_SCORED) {
        setnum(L, "n_built", r->n_built); setnum(L, "blk", r->blk); setnum(L, "score", r->score);
    }
    if (r->flags & RF_UNSEARCH) setbool(L, "unsearched", 1);
    if (r->flags & RF_BAND) { setnum(L, "tier_min", r->tier_min); setbool(L, "in_band", r->in_band); }
    if (r->flags & RF_LEEWAY) {
        setnum(L, "leeway", r->leeway);
        lua_createtable(L, 8, 0);
        for (int k = 0; k < 8; k++) {
            LeePt *q = &r->lpts[k];
            lua_createtable(L, 0, 4);
            setnum(L, "x", q->x); setnum(L, "y", q->y); setbool(L, "ok", q->ok);
            if (!q->ok) { push_why(e, q->why, q->why_arg); lua_setfield(L, -2, "why"); }
            lua_rawseti(L, -2, k + 1);
        }
        lua_setfield(L, -2, "leeway_pts");
    }
    if (r->flags & RF_WHYUNSAFE) { lua_pushstring(L, "LEEWAY"); lua_setfield(L, -2, "why_unsafe"); }
    if (r->flags & RF_HOLD) {
        Hold *h = r->hold;
        lua_createtable(L, 0, 5);
        setnum(L, "fx", h->fx); setnum(L, "fy", h->fy); setnum(L, "d", h->d);
        setbool(L, "safe", h->safe);
        if (!h->safe) {
            if (h->why_pill) push_pill_name(e, h->why_pill); else lua_pushstring(L, "no_square");
            lua_setfield(L, -2, "why");
        }
        lua_setfield(L, -2, "hold");
    }
}

/* ── evaluate ───────────────────────────────────────────────────────── */
static double getnum(lua_State *L, int t, const char *k, double def, int *has) {
    lua_getfield(L, t, k);
    int h = lua_type(L, -1) == LUA_TNUMBER;
    double v = h ? lua_tonumber(L, -1) : def;
    lua_pop(L, 1);
    if (has) *has = h;
    return v;
}

static int getbool(lua_State *L, int t, const char *k) {
    lua_getfield(L, t, k);
    int v = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return v;
}

static Pt2Ctx *get_ctx(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "gh_pt2_ctx");
    Pt2Ctx *c = (Pt2Ctx *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!c) luaL_error(L, "gh_pt2: no ctx");
    return c;
}

typedef struct { Rec **v; int n, cap; } RecVec;   /* 1-based */

static void rv_push(Ev *e, RecVec *rv, Rec *r) {
    if (rv->n + 1 >= rv->cap) {
        int nc = rv->cap ? rv->cap * 2 : 256;
        Rec **nv = NEWN(e, Rec *, nc);
        if (rv->n) memcpy(nv, rv->v, sizeof(Rec *) * (size_t)(rv->n + 1));
        rv->v = nv; rv->cap = nc;
    }
    rv->v[++rv->n] = r;
}

static int l_evaluate(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_settop(L, 2);
    Pt2Ctx *c = get_ctx(L);
    if (!c->tt_stamp) {
        c->tt_stamp = (uint32_t *)calloc(PT2_MAP_SQ, sizeof(uint32_t));
        c->tt_val = (double *)calloc(PT2_MAP_SQ, sizeof(double));
        c->po_stamp = (uint32_t *)calloc(PT2_MAP_SQ, sizeof(uint32_t));
        c->po_val = (int *)calloc(PT2_MAP_SQ, sizeof(int));
        if (!c->tt_stamp || !c->tt_val || !c->po_stamp || !c->po_val)
            return luaL_error(L, "gh_pt2: out of memory");
    }
    c->gen++;
    if (c->gen == 0) {
        memset(c->tt_stamp, 0, sizeof(uint32_t) * PT2_MAP_SQ);
        memset(c->po_stamp, 0, sizeof(uint32_t) * PT2_MAP_SQ);
        c->gen = 1;
    }
    ar_reset(&c->ar);
    c->npills = 0;
    if (!s_nb_ready) {
        for (int k = 0; k <= 7; k++) {
            double a = k * 3.141592653589793 / 4;
            NB[k][0] = cos(a); NB[k][1] = sin(a);
        }
        s_nb_ready = 1;
    }

    Ev ev;
    memset(&ev, 0, sizeof(ev));
    Ev *e = &ev;
    e->L = L; e->c = c;

    lua_getfield(L, S_INP, "env");                 /* S_ENV */
    luaL_checktype(L, S_ENV, LUA_TTABLE);
    lua_newtable(L);                               /* S_PILLS */
    lua_newtable(L);                               /* S_PIDX */
    lua_newtable(L);                               /* S_MEMO */
    lua_getfield(L, S_ENV, "ttype");
    lua_getfield(L, S_ENV, "pill_on");
    lua_getfield(L, S_ENV, "is_ally");
    lua_getfield(L, S_ENV, "cover_of_pill");
    lua_getfield(L, S_ENV, "base_stops");
    lua_getfield(L, S_INP, "in_map");
    lua_getfield(L, S_INP, "stay_in");
    lua_getfield(L, S_INP, "standable");
    lua_getfield(L, S_INP, "buildable");
    lua_getfield(L, S_INP, "lgm_reach");
    lua_getfield(L, S_INP, "score_fn");
    lua_getfield(L, S_INP, "banned");
    lua_getfield(L, S_INP, "anger_of");
    lua_getfield(L, S_INP, "chain_skip");
    lua_getfield(L, S_ENV, "base_at");
    lua_getfield(L, S_V2, "pill_name");
    lua_getfield(L, S_ENV, "count_reads");
    if (lua_gettop(L) != S_TOP) return luaL_error(L, "gh_pt2: stack");

    /* inp numbers */
    e->radius = getnum(L, S_INP, "radius", 0, NULL);
    e->angle_step = getnum(L, S_INP, "angle_step", 2, NULL);
    e->approach_offset = getnum(L, S_INP, "approach_offset", 0, NULL);
    e->num_pill_blockers = getnum(L, S_INP, "num_pill_blockers", 0, NULL);
    e->pill_drop_shots = getnum(L, S_INP, "pill_drop_shots", 0, NULL);
    e->wall_shots = getnum(L, S_INP, "wall_shots", 0, NULL);
    e->build_max = getnum(L, S_INP, "build_max", 0, NULL);
    e->build_cost = getnum(L, S_INP, "build_cost", 0, NULL);
    e->bar_single = getnum(L, S_INP, "bar_single", 0, NULL);
    e->bar_chain = getnum(L, S_INP, "bar_chain", 0, NULL);
    {
        int h;
        double bo = getnum(L, S_INP, "bar_other", 0, &h);
        e->bar_other = h ? bo : e->wall_shots;
    }
    e->chains = getbool(L, S_INP, "chains");
    e->link_covers = getbool(L, S_INP, "link_covers");
    e->leeway_on = getbool(L, S_INP, "leeway_on");
    e->own_pill_rank = getbool(L, S_INP, "own_pill_rank");
    e->orth_only = getbool(L, S_INP, "orth_only");
    e->aim_nudge = getbool(L, S_INP, "aim_nudge");
    e->keep_all = getbool(L, S_INP, "keep_all");
    e->max_chain = getnum(L, S_INP, "max_chain", 0, NULL);
    e->leeway_ring_wu = getnum(L, S_INP, "leeway_ring_wu", 0, NULL);
    e->leeway_top = getnum(L, S_INP, "leeway_top", 0, NULL);
    e->search_top = getnum(L, S_INP, "search_top", 0, NULL);
    e->pill_range = getnum(L, S_INP, "pill_range", 0, NULL);
    e->range_margin = getnum(L, S_INP, "range_margin", 0, NULL);
    e->blocker_min_dist = getnum(L, S_INP, "blocker_min_dist", 0, NULL);
    e->nudge_step_wu = getnum(L, S_INP, "nudge_step_wu", 0, NULL);
    e->nudge_max_steps = getnum(L, S_INP, "nudge_max_steps", 0, NULL);
    e->leeway_min = getnum(L, S_INP, "leeway_min", 0, &e->has_leeway_min);
    e->band = getnum(L, S_INP, "travel_band", 0, &e->has_band);
    e->finish_max_anger = getnum(L, S_INP, "finish_max_anger", 0, &e->has_fma);
    e->finish_min_armour = getnum(L, S_INP, "finish_min_armour", 5, NULL);
    e->armour = getnum(L, S_INP, "armour", 0, NULL);
    /* env */
    e->T_BUILDING = getnum(L, S_ENV, "T_BUILDING", NAN, NULL);
    e->T_HALFBUILD = getnum(L, S_ENV, "T_HALFBUILD", NAN, NULL);
    e->T_BOAT = getnum(L, S_ENV, "T_BOAT", NAN, NULL);
    e->T_FOREST = getnum(L, S_ENV, "T_FOREST", NAN, NULL);
    e->e_wall_shots = getnum(L, S_ENV, "wall_shots", 0, NULL);
    e->e_half_wall_shots = getnum(L, S_ENV, "half_wall_shots", 0, NULL);
    e->e_boat_shots = getnum(L, S_ENV, "boat_shots", 0, NULL);
    e->forest_transparent = getbool(L, S_ENV, "forest_transparent");

    /* A */
    lua_getfield(L, S_INP, "A");
    if (!lua_istable(L, -1)) return luaL_error(L, "gh_pt2.evaluate: inp.A is not a table");
    e->A = pill_index(e, -1);
    lua_pop(L, 1);

    /* new_ctx: hostile */
    e->hostile = plist_new(e, 16);
    lua_getfield(L, S_INP, "pills");
    {
        int pl = lua_gettop(L);
        for (int i = 1;; i++) {
            lua_rawgeti(L, pl, i);
            if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
            int k = lua_istable(L, -1) ? pill_index(e, -1) : 0;
            lua_pop(L, 1);
            if (k && alive(e, k) && !is_ally(e, k)) plist_push(e, e->hostile, k);
        }
    }
    lua_pop(L, 1);
    /* finish_ok(inp, A) */
    {
        Pill *A = PI(e, e->A);
        int fin = 0;
        if (e->has_fma && !(e->finish_max_anger < 0) && A->health == 1
            && !(e->armour < e->finish_min_armour)) {
            double anger = 0;
            int got = 0;
            if (lua_toboolean(L, S_ANGER_OF)) {
                lua_pushvalue(L, S_ANGER_OF);
                push_pill(e, e->A);
                lua_call(L, 1, 1);
                if (lua_toboolean(L, -1)) { anger = lua_tonumber(L, -1); got = 1; }
                lua_pop(L, 1);
            }
            if (!got) {
                push_pill(e, e->A);
                lua_getfield(L, -1, "anger");
                anger = lua_toboolean(L, -1) ? lua_tonumber(L, -1) : 0;
                lua_pop(L, 2);
            }
            fin = anger <= e->finish_max_anger;
        }
        e->finish = fin;
    }

    Pill *A = PI(e, e->A);
    double pcx = A->mx + 0.5, pcy = A->my + 0.5;
    AllE *all = NULL; int nall = 0, capall = 0;
    Rec *best = NULL;
    double n_eval = 0, n_reject = 0;

    lua_getfield(L, S_INP, "radii_offsets");
    int ro_idx = lua_gettop(L);
    for (int ri = 1;; ri++) {
        lua_rawgeti(L, ro_idx, ri);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        double roff = lua_tonumber(L, -1);
        lua_pop(L, 1);
        double R = e->radius + roff;
        RecVec cands = { NULL, 0, 0 };
        double nsteps = floor(360 / e->angle_step + 0.5);
        for (double s = 0; s <= nsteps - 1; s++) {
            double deg = s * e->angle_step;
            int ban = 0;
            if (lua_toboolean(L, S_BANNED)) {
                lua_pushvalue(L, S_BANNED);
                lua_pushnumber(L, deg);
                lua_call(L, 1, 1);
                ban = lua_toboolean(L, -1);
                lua_pop(L, 1);
            }
            if (ban) continue;
            double cx = pcx + sin(deg * PT2_LUA_RAD) * R;
            double cy = pcy - cos(deg * PT2_LUA_RAD) * R;
            double mx = floor(cx), my = floor(cy);
            if (in_map(e, mx, my) && stay_in(e, mx, my) && standable(e, mx, my)) {
                n_eval = n_eval + 1;
                double px = floor(cx * 256), py = floor(cy * 256);
                int why = 0, why_arg = 0;
                Rec *rec = eval_point(e, px, py, NULL, 0, NULL, -1, 1, &why, &why_arg);
                if (rec) {
                    if (rec->nudge_wu > 0) {
                        cx = rec->px / 256; cy = rec->py / 256;
                        mx = floor(cx); my = floor(cy);
                    }
                    rec->deg = deg; rec->cx = cx; rec->cy = cy; rec->mx = mx; rec->my = my;
                    rec->radius = R; rec->flags |= RF_POS;
                    lua_pushvalue(L, S_SCORE_FN);
                    lua_pushnumber(L, deg); lua_pushnumber(L, mx); lua_pushnumber(L, my);
                    lua_pushnumber(L, cx); lua_pushnumber(L, cy);
                    lua_call(L, 5, 1);
                    rec->base = lua_tonumber(L, -1);
                    lua_pop(L, 1);
                    rec->flags |= RF_BASE;
                    double ax = cx + sin(deg * PT2_LUA_RAD) * e->approach_offset;
                    double ay = cy - cos(deg * PT2_LUA_RAD) * e->approach_offset;
                    rec->omx = floor(ax); rec->omy = floor(ay); rec->flags |= RF_OMXY;
                    rv_push(e, &cands, rec);
                } else {
                    n_reject = n_reject + 1;
                    if (e->keep_all) {
                        Reject *rj = NEW(e, Reject);
                        rj->deg = deg; rj->cx = cx; rj->cy = cy; rj->mx = mx; rj->my = my;
                        rj->why = why; rj->why_arg = why_arg;
                        if (nall >= capall) {
                            int nc = capall ? capall * 2 : 256;
                            AllE *na = NEWN(e, AllE, nc);
                            if (nall) memcpy(na, all, sizeof(AllE) * (size_t)nall);
                            all = na; capall = nc;
                        }
                        all[nall].rec = NULL; all[nall].rej = rj; nall++;
                    }
                }
            }
        }
        if (cands.n > 1) auxsort(e, cands.v, 1, cands.n, prerank_lt);
        RecVec scored = { NULL, 0, 0 };
        for (int i = 1; i <= cands.n; i++) {
            Rec *rec = cands.v[i];
            Rec *fin = rec;
            if ((double)i <= e->search_top && !rec->covered) {
                fin = search_blockers(e, rec);
                fin->deg = rec->deg; fin->cx = rec->cx; fin->cy = rec->cy;
                fin->mx = rec->mx; fin->my = rec->my; fin->radius = rec->radius;
                fin->flags |= RF_POS;
                fin->base = rec->base; fin->flags |= RF_BASE;
            } else {
                fin->plan = NEW(e, Plan);
                fin->plan->n = 0; fin->plan->cap = 0; fin->plan->v = NULL; fin->plan->memo = 0;
                fin->extra = extra_new(e);
                fin->flags |= RF_PLAN;
            }
            if ((double)i > e->search_top && !rec->covered) fin->flags |= RF_UNSEARCH;
            fin->n_built = fin->plan->n;
            fin->blk = (e->own_pill_rank && fin->ally_cover) ? 2 : 1;
            fin->score = fin->base + fin->n_built * e->build_cost;
            fin->flags |= RF_SCORED;
            rv_push(e, &scored, fin);
        }
        set_band(e, scored.v, scored.n);
        if (scored.n > 1) auxsort(e, scored.v, 1, scored.n, better);
        if (e->leeway_on) {
            double top = e->leeway_top < scored.n ? e->leeway_top : (double)scored.n;
            for (int i = 1; (double)i <= top; i++) {
                Rec *rec = scored.v[i];
                LeePt *pts;
                rec->leeway = leeway(e, rec, e->leeway_ring_wu, &pts);
                rec->lpts = pts;
                rec->flags |= RF_LEEWAY;
                if (rec->safe && rec->kills >= 2 && rec->leeway < e->leeway_min) {
                    rec->safe = 0; rec->flags |= RF_WHYUNSAFE;
                }
            }
            set_band(e, scored.v, scored.n);
            if (scored.n > 1) auxsort(e, scored.v, 1, scored.n, better);
        }
        for (int i = 1; i <= scored.n; i++) {
            if (nall >= capall) {
                int nc = capall ? capall * 2 : 256;
                AllE *na = NEWN(e, AllE, nc);
                if (nall) memcpy(na, all, sizeof(AllE) * (size_t)nall);
                all = na; capall = nc;
            }
            all[nall].rec = scored.v[i]; all[nall].rej = NULL; nall++;
        }
        if (scored.n >= 1 && (!best || better(e, scored.v[1], best))) best = scored.v[1];
        if (best && best->safe) break;
    }
    lua_pop(L, 1);   /* radii_offsets */
    if (best && best->safe && (best->flags & RF_PLAN) && best->plan->n > 0) {
        best->hold = hold_point(e, best);
        best->flags |= RF_HOLD;
    }

    /* env.omx / env.omy: the last values the Lua would have left there */
    sync_om(e);
    if (e->tt_hits > 0 && lua_toboolean(L, S_COUNT_READS)) {
        lua_pushvalue(L, S_COUNT_READS);
        lua_pushnumber(L, e->tt_hits);
        lua_call(L, 1, 0);
    }

    /* result */
    lua_createtable(L, 0, 6);
    int res = lua_gettop(L);
    if (best) { push_rec(e, best); lua_setfield(L, res, "best"); }
    if (e->keep_all) {
        lua_createtable(L, nall, 0);
        for (int i = 0; i < nall; i++) {
            if (all[i].rec) {
                push_rec(e, all[i].rec);
            } else {
                Reject *rj = all[i].rej;
                lua_createtable(L, 0, 6);
                setnum(L, "deg", rj->deg); setnum(L, "cx", rj->cx); setnum(L, "cy", rj->cy);
                setnum(L, "mx", rj->mx); setnum(L, "my", rj->my);
                push_why(e, rj->why, rj->why_arg); lua_setfield(L, -2, "rejected");
            }
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, res, "all");
    }
    setnum(L, "n_eval", n_eval);
    setnum(L, "n_reject", n_reject);
    push_plist(e, e->hostile); lua_setfield(L, res, "hostile");
    lua_pushvalue(L, S_INP); lua_setfield(L, res, "inp");
    return 1;
}

/* ── Registration ───────────────────────────────────────────────────── */
static int pt2_ctx_gc(lua_State *L) {
    Pt2Ctx *c = (Pt2Ctx *)lua_touserdata(L, 1);
    if (!c) return 0;
    Chunk *k = c->ar.head;
    while (k) { Chunk *n = k->next; free(k); k = n; }
    c->ar.head = c->ar.cur = NULL;
    free(c->tt_stamp); free(c->tt_val); free(c->po_stamp); free(c->po_val); free(c->pills);
    c->tt_stamp = NULL; c->tt_val = NULL; c->po_stamp = NULL; c->po_val = NULL; c->pills = NULL;
    return 0;
}

static const luaL_Reg gh_pt2_lib[] = {
    { "evaluate", l_evaluate },
    { NULL, NULL }
};

void naPt2Register(lua_State *L) {
    Pt2Ctx *c = (Pt2Ctx *)lua_newuserdata(L, sizeof(Pt2Ctx));
    memset(c, 0, sizeof(*c));
    if (luaL_newmetatable(L, "gh_pt2_ctx_mt")) {
        lua_pushcfunction(L, pt2_ctx_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, "gh_pt2_ctx");
    luaL_newlib(L, gh_pt2_lib);
    lua_setglobal(L, "gh_pt2");
}
