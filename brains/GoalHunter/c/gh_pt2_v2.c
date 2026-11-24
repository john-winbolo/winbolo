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
 * gh_pt2_v2.c -- the pill take evaluator v2 in C (pilltake_v2.lua,
 * pt2_start.lua and the shadow.lua ray walks it uses).
 *
 * This is a line-by-line port of pilltake_v2.lua M.evaluate_lua. The Lua
 * stays the reference: the parity test (tests/unit/test_gh_pt2_v2_parity.c)
 * runs both on the same inputs and compares a digest of every field of
 * the result and of the map reads.
 *
 * Rules the port keeps, so the result is the same as the Lua's:
 *   - Every Lua number is a C double, and each expression keeps the Lua's
 *     order of operations. shadow.lua's f32() is a (float) cast here.
 *     sin, cos, atan, atan2, fmod and floor are the same C library calls
 *     LuaJIT makes. math.rad is x * 0.017453292519943295 (LuaJIT's). The
 *     Lua's x ^ 2 is x * x here. tostring of a number is "%.14g".
 *   - Every callback in inp and inp.env is called with the same arguments
 *     as the Lua calls it. env.ttype and env.pill_on are called once per
 *     square per step (the map does not change inside one step) and the
 *     other reads use the answer; env.count_reads(n), when given, gets the
 *     number of ttype reads taken from that cache. env.is_ally and
 *     env.cover_of_pill are called once per pill per step.
 *   - env.base_stops is not called for a square with no entry in
 *     env.base_at (when given): U.base_tile_stops_tank_shell returns false
 *     there before it reads anything.
 *   - Every table.sort with an order function is LuaJIT's quicksort
 *     (lib_table.c auxsort), step for step, so equal keys end in the same
 *     order.
 *   - Tables the Lua shares (a ray view and its base ray's square lists,
 *     the lane of the chosen aim, the plan's extra table, the wall
 *     search's paths) are one table in the result here too.
 *   - The job cache (rays, square reads, square facts) lives as long as
 *     the Lua's job.cache: one group run, or one lone evaluate.
 *   - The evaluate stops at the same points as the Lua's slicer (unit()
 *     in evaluate_lua) and goes on from there at the next step.
 */

#include "gh_pt2_v2.h"

#include <lauxlib.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Engine constants (shadow.lua) ──────────────────────────────────── */
#define PT2_RADIANS_MAX   (2.0 * 3.14159)
#define PT2_SHELL_SPEED   32.0
#define PT2_START_ADD     5.0
#define PT2_SHELL_LIFE    8.0
#define PT2_PILL_FIRE_LEN 8.5
#define PT2_TANK_FIRE_LEN 7.0
#define PT2_TS_MAX_TANKS 64
#define PT2_TANK_HIT_R    112.0
#define PT2_LUA_RAD       0.017453292519943295
#define PT2_PI            3.141592653589793
#define PT2_TWO_PI        (2 * 3.141592653589793)
#define PT2_MAX_T         128   /* > life_ticks(8.5) = 64 */
#define PT2_MAXAIMS       9
#define PT2_MAP_SQ        65536
#define PT2_TT_N          64    /* inp.c_std.cost keys 0 .. 63 */
#define PT2_STD_LIST      8     /* inp.c_std.water / nobuild: at most 8 */
#define PT2_STAY_MAX      32    /* inp.c_std.rects: at most 32 */
#define PT2_MAXRADII      16
#define WS_MAXK           8     /* the wall search: at most 8 blockers */

/* Wedge edge margins (pilltake_v2.lua). */
#define EDGE_IN 24
#define SLACK   4
#define NBIN    128
enum { K_NONE, K_WALL, K_HALF, K_BOAT, K_FOREST, K_ALLY, K_HOST };

/* The slicer's unit weights (P1b, Andrew approved Nov 23). */
#define SLICE_W_CAST  2
#define SLICE_W_SHOT  2
#define SLICE_W_NODE  2
#define SLICE_W_POINT 3
#define SLICE_PER_SURVIVOR 52
#define S6_TRIES 32

/* pt2_start.lua */
#define LGM_BUILD_TIME   20
#define LGM_TICKS_PER_SQ 16
#define SHELL_ARMOUR     5
#define HEAT_MAX_HITS    4

static const double s_aims9[PT2_MAXAIMS][2] = {
    { 128, 128 }, { 24, 24 }, { 255 - 24, 24 }, { 24, 255 - 24 }, { 255 - 24, 255 - 24 },
    { 128, 24 }, { 231, 128 }, { 128, 231 }, { 24, 128 },
};

/* Block kinds (shadow.lua). */
enum { BK_ALLY_PILL, BK_PLANNED, BK_WALL, BK_HALF_WALL, BK_BOAT, BK_FOREST };
static const char *s_bk_name[] = { "ally_pill", "planned", "wall", "half_wall", "boat", "forest" };

/* tank_shot why. */
enum { TS_OK, TS_SHORT, TS_WALL, TS_PILL, TS_BASE, TS_PLANNED, TS_BOAT, TS_TANK };
static const char *s_ts_name[] = { "ok", "short", "wall", "pill", "base", "planned", "boat", "tank" };

/* Why codes for rejects and leeway points. */
enum { W_NONE, W_A_OUT, W_OUTGOING, W_OVERLAP, W_CHAIN, W_QMARK, W_CANT };

/* Square facts (slot_ok's ctx.sqf). */
enum { SF_NIL, SF_TRUE, SF_AREA, SF_PILL, SF_WALL, SF_NOBUILD };
static const char *s_sf_name[] = { "", "", "area", "pill", "wall", "not_buildable" };

/* The profile macros of gh_pt2_old.c: no profile here. */
#define PCALL(e, L, na, nr, cat) lua_call((L), (na), (nr))
#define PCOUNT(e, cat) do { } while (0)
#define PTIME(e, cat, stmt) do { stmt; } while (0)

/* ── Arena ──────────────────────────────────────────────────────────── */
typedef struct Chunk {
    struct Chunk *next;
    size_t used, cap;
    double data[1];
} Chunk;

typedef struct {
    Chunk *head, *cur;
} Arena;

static void ar_free(Arena *a) {
    Chunk *c = a->head;
    while (c) { Chunk *n = c->next; free(c); c = n; }
    a->head = a->cur = NULL;
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
        if (!nc) { luaL_error(L, "gh_pt2_v2: out of memory"); return NULL; }
        nc->next = NULL; nc->used = 0; nc->cap = cap;
        if (c) c->next = nc; else a->head = nc;
        a->cur = nc;
    }
}

/* ── Hash map: two 64-bit key words to one value ───────────────────── */
typedef union { double d; void *p; int64_t i; } HVal;
typedef struct {
    uint64_t *k1, *k2;
    HVal     *v;
    uint8_t  *u;
    int       cap, n;
} HMap;

static uint64_t hm_hash(uint64_t a, uint64_t b) {
    uint64_t h = a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull + (a << 6) + (a >> 2));
    h ^= h >> 31; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 29;
    return h;
}

static void hm_free(HMap *m) {
    free(m->k1); free(m->k2); free(m->v); free(m->u);
    memset(m, 0, sizeof(*m));
}

static void hm_clear(HMap *m) {
    if (m->u) memset(m->u, 0, (size_t)m->cap);
    m->n = 0;
}

static HVal *hm_find(HMap *m, uint64_t a, uint64_t b) {
    if (!m->cap) return NULL;
    uint64_t mask = (uint64_t)m->cap - 1;
    uint64_t i = hm_hash(a, b) & mask;
    while (m->u[i]) {
        if (m->k1[i] == a && m->k2[i] == b) return &m->v[i];
        i = (i + 1) & mask;
    }
    return NULL;
}

static void hm_grow(lua_State *L, HMap *m) {
    int oc = m->cap, nc = oc ? oc * 2 : 256;
    uint64_t *k1 = (uint64_t *)malloc(sizeof(uint64_t) * (size_t)nc);
    uint64_t *k2 = (uint64_t *)malloc(sizeof(uint64_t) * (size_t)nc);
    HVal *v = (HVal *)malloc(sizeof(HVal) * (size_t)nc);
    uint8_t *u = (uint8_t *)calloc((size_t)nc, 1);
    if (!k1 || !k2 || !v || !u) {
        free(k1); free(k2); free(v); free(u);
        luaL_error(L, "gh_pt2_v2: out of memory");
        return;
    }
    uint64_t mask = (uint64_t)nc - 1;
    for (int j = 0; j < oc; j++) {
        if (!m->u[j]) continue;
        uint64_t i = hm_hash(m->k1[j], m->k2[j]) & mask;
        while (u[i]) i = (i + 1) & mask;
        u[i] = 1; k1[i] = m->k1[j]; k2[i] = m->k2[j]; v[i] = m->v[j];
    }
    free(m->k1); free(m->k2); free(m->v); free(m->u);
    m->k1 = k1; m->k2 = k2; m->v = v; m->u = u; m->cap = nc;
}

/* The slot of key (a, b), made when new (*isnew = 1). */
static HVal *hm_put(lua_State *L, HMap *m, uint64_t a, uint64_t b, int *isnew) {
    HVal *f = hm_find(m, a, b);
    if (f) { *isnew = 0; return f; }
    if ((m->n + 1) * 4 >= m->cap * 3) hm_grow(L, m);
    uint64_t mask = (uint64_t)m->cap - 1;
    uint64_t i = hm_hash(a, b) & mask;
    while (m->u[i]) i = (i + 1) & mask;
    m->u[i] = 1; m->k1[i] = a; m->k2[i] = b; m->v[i].i = 0;
    m->n++;
    *isnew = 1;
    return &m->v[i];
}

static uint64_t dbits(double d) {
    union { double d; uint64_t u; } o;
    if (d == 0) d = 0;   /* -0 and 0 are one Lua key */
    o.d = d;
    return o.u;
}

/* ── Pills, shared tables ──────────────────────────────────────────── */
typedef struct {
    int    has_mx;
    double mx, my, health;
    int    in_tank, pos_unknown;
    int    ally;       /* -1 not asked yet (this step) */
    int    cover_set;
    double cover;
    int    skip;       /* chain_skip: -1 not asked yet */
    uint32_t gen;      /* the step the asks above belong to */
} Pill;

/* Output memo: an object is pushed once per output (mg == the output's
 * generation), later places get the same Lua table. */
typedef struct { int memo; uint32_t mg; } Memo;

/* One entry of the shell ray table (gh_pt2_old.c "Shell ray table"). */
#define CG_MAXS   32
#define CG_SLOTS  16384   /* cast(); a power of 2 */
#define TG_SLOTS  8192    /* tank_shot(); a power of 2 */
typedef struct {
    int32_t k1, k2, k3;
    uint8_t used;
    uint8_t n;
    uint8_t flag;
    int8_t  minx, maxx, miny, maxy;
    int16_t hit_t;
    int32_t ex, ey;
    double  angle;
    int8_t  ox[CG_MAXS], oy[CG_MAXS];
    uint8_t t[CG_MAXS];
} CGeom;

/* offsets(reach): kept per floor(reach), as the Lua's OFFS. */
typedef struct { int key; int n; double *o; } Offs;

typedef struct {
    uint32_t  gen;          /* one per step */
    uint32_t *tt_stamp;
    double   *tt_val;
    uint32_t *po_stamp;
    int      *po_val;
    uint32_t *ba_stamp;
    CGeom    *cg, *tg;
    int       cg_n, tg_n;
    double    cg_par[5];
    int       cg_par_set;
    int       cg_off, cg_check;
    Offs     *offs; int n_offs, cap_offs;
    uint32_t  mgen;         /* output generation */
} Pt2Ctx;

typedef struct { int64_t sq; double shots; int kind; double t; int pill; Memo m; } Blk;

/* A ray: a base ray (src = NULL: shadow.cast's own table) or a view of
 * one (ray_view: it shares the base's sq and sq_t tables, and its block
 * table when no planned blocker is on the ray). */
typedef struct Ray {
    double  angle;
    int     tank;
    double  t;
    int     link;     /* pill index, 0 = none */
    double  link_t;
    double  cover, front;
    int     n_sq, n_block;
    int64_t *sq;
    double  *sq_t;
    Blk    **blk;
    double  end_x, end_y;
    int     X;
    struct Ray *src;
    int     own_blk;
    int     cut_blk, cut_front, cut;
    int     has_bar;
    double  bar;
    int     link_cover, ok;
    Memo    m, m_sq, m_sqt, m_bl;
} Ray;

typedef struct { int n, cap; int *v; Memo m, m_ic; } PList;
typedef struct { int n; int64_t *v; Memo m; } Lane;
typedef struct { int n; int v[PT2_MAXAIMS]; Memo m; } Aims;
typedef struct { int n; int ai[PT2_MAXAIMS]; Lane *ln[PT2_MAXAIMS]; Memo m; } Lanes;
typedef struct { int n, cap; int64_t *sq; double *shots; Memo m; } Extra;
typedef struct { int n; Ray **v; Memo m; } RayList;
typedef struct { int n; int *v; Memo m; } IList;
typedef struct { int64_t sq; double mx, my, shots; int kind_pill; } PlanE;
typedef struct { int n, cap; PlanE *v; Memo m; } Plan;
typedef struct { double x, y; int ok; int why; int why_arg; } LeePt;
typedef struct { double fx, fy, d; int safe; int why_pill; } Hold;
typedef struct { double mx, my, shots; } PlanSq;
typedef struct { double mx, my, cover_now; int np; PlanSq *pl; } StartPill;
typedef struct {
    double mx, my, cover_now;
    int has_to; double to;
    int np; PlanSq *pl;      /* np < 0: no planned */
    int has_count, count;
} StartLink;
typedef struct { double sq, mx, my, shots; } XWall;
/* rec.wall_base */
typedef struct { Ray *r; double need; int n_on; int64_t *on_sq; double *on_v; } UBase;
typedef struct { int nf; int64_t *forbid; int nU; UBase *U; } WallBase;
/* rec.slot_memo */
typedef struct { int n, cap; int64_t *sq; uint8_t *v; } SlotMemo;

/* Optional record fields. */
#define RF_EXTRA_IN  0x00001
#define RF_OMXY      0x00002
#define RF_POS       0x00004
#define RF_BASE      0x00008
#define RF_PLAN      0x00010
#define RF_SCORED    0x00020
#define RF_UNSEARCH  0x00040
#define RF_BAND      0x00080
#define RF_LEEWAY    0x00100
#define RF_WHYUNSAFE 0x00200
#define RF_HOLD      0x00400
#define RF_START     0x00800
#define RF_KMAX      0x01000
#define RF_XCOVER    0x02000
#define RF_LAZY      0x04000
#define RF_ANEED     0x08000
#define RF_LB        0x10000
#define RF_KMIN      0x20000
#define RF_AIMNB     0x40000

typedef struct Rec {
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
    /* start_check */
    int     start_now;
    double  exposure, shells_through;
    int     start_why;          /* 0 nil, 1 bystander, 2 armour, 3 unreachable */
    int     n_sp; StartPill *sp; int has_sp;
    int     n_sc; StartLink *sc; int has_sc;
    double  kmax;
    int     n_xc; XWall *xc;
    WallBase *wb;
    SlotMemo *sm;
    /* the lazy stub */
    double  a_need, a_bar, lb, kmin;
    int     aimnb;              /* RF_AIMNB: PT2_AIM_AVOID_CHAIN_NEIGHBOUR */
    struct Rec *done;           /* the lone pass: the plan's record */
    Memo    m, m_ri;
} Rec;

static const char *s_start_why[] = { NULL, "bystander", "armour", "unreachable" };

typedef struct { double deg, cx, cy, mx, my; int why, why_arg; } Reject;
typedef struct { Rec *rec; Reject *rej; } AllE;

/* A wedge (make_wedge) and a wedge set (wedge_set). */
typedef struct {
    double sq, mx, my;
    int    kind;
    double shots;
    int    pill;
    double c, lo_o, hi_o, lo_i, hi_i, near, far;
} Wedge;
typedef struct {
    int     n;
    Wedge  *list;
    int     bin_n[NBIN];
    Wedge **bin[NBIN];
    double  ax, ay;
    int     X;
} WSet;

/* A wall search solution. */
typedef struct {
    int      n;
    int64_t  sq[WS_MAXK];
    uint8_t  pill[WS_MAXK];
    uint32_t mask;
    int      keys;
    double   depth;
    int      two;
    double   trip;
    int      order[WS_MAXK];   /* 1-based places in sq */
    char     key[80];
} Sol;
typedef struct { int n, cap; Sol **v; } SolList;
/* search_walls_all's out: per aim place i (aims[i]) */
typedef struct { int has; double k; SolList sols; int best; } AimOut;
typedef struct { int nA; AimOut a[PT2_MAXAIMS]; } WSOut;

/* The job: the cache one group run keeps (pilltake_v2.lua new_cache): the
 * plain rays, the square reads and the square facts. The pills it has
 * seen are in its uservalue table ({ pills, pidx }). */
typedef struct { int kind; double shots; int pill; } SqV;
typedef struct {
    Arena  ar;
    HMap   rays;      /* (X, ray key) -> Ray * */
    HMap   sqm;       /* key -> SqV * */
    HMap   sqf;       /* sq -> SF_* */
    int64_t *sqf_new; int n_sqf_new, cap_sqf_new;   /* sqf keys not yet in the Lua cache */
    Pill **pills;     /* 1-based */
    int    npills, cap_pills;
    int    freed;
} Job;

/* ── The Lua stack of one step ─────────────────────────────────────── */
/* step(st, paused): 1 = the state, 2 = paused; then the slots below, in
 * this order, from the state's uservalue table. */
enum {
    S_ST = 1, S_PAUSED,
    S_INP, S_M, S_OLD, S_ENV, S_PILLS, S_PIDX, S_MEMO,
    S_TTYPE, S_PILL_ON, S_IS_ALLY, S_COVER, S_BASE_STOPS,
    S_IN_MAP, S_STAY_IN, S_STANDABLE, S_BUILDABLE, S_LGM_REACH,
    S_SCORE_FN, S_BANNED, S_ANGER_OF, S_CHAIN_SKIP, S_BASE_AT,
    S_PILL_NAME, S_COUNT_READS, S_LGM_TICKS, S_PILL_INTERVAL, S_WKEY,
    S_WMT, S_WM, S_SLICE, S_CLK, S_JOB, S_SQF,
    S_TOP = S_SQF
};
#define N_SLOTS (S_TOP - S_INP + 1)

/* The wall search's solution memo entry (PT2_WALL_MEMO), kept as a
 * userdata in the goal's memo table. */
typedef struct {
    double slow, nodes;
    int    nA, nsol;
    /* then nA x WMAim, then nsol x Sol (aim by aim) */
} WMBlob;
typedef struct { int has; double k; int nsol; int best; } WMAim;

struct St;
typedef struct St Ev;

/* ── The evaluate state (one evaluate; a userdata) ─────────────────── */
struct St {
    lua_State *L;
    Pt2Ctx    *c;
    Job       *job;
    Arena      ar;          /* records, views, plans: this evaluate */
    int        done;        /* the result was made */
    int        pc;          /* where step goes on */
    /* inp */
    int    A;
    double radius, angle_step, approach_offset, num_pill_blockers, pill_drop_shots;
    double wall_shots, build_max, build_cost, bar_single, bar_chain, bar_other;
    int    chains, link_covers, leeway_on, own_pill_rank, orth_only, aim_nudge, keep_all;
    int    link_clear_front;
    double link_cut_min_shots;
    double max_chain, leeway_ring_wu, leeway_top, pill_range, range_margin;
    double blocker_min_dist, nudge_step_wu, nudge_max_steps;
    int    has_leeway_min; double leeway_min;
    int    has_band; double band;
    int    has_fma; double finish_max_anger;
    double finish_min_armour, armour;
    int    has_armour;
    int    pec, swb, rise, chains_only, xcover_later, has_cache;
    int    aim_nb_on;
    double extra_cover_walls, anger_bump;
    int    has_pat, has_pamt, has_pad, has_pct, has_trl;
    double pill_attack_ticks, pill_attack_min_ticks, pill_angry_divisor, pill_cooldown_ticks, tank_reload;
    int    has_env_speed; double env_speed;
    int    aim9; int naims;
    int    wall_memo; double wall_memo_max; int wkey;
    int    has_clk;
    int    has_score_fn;
    /* env */
    double T_BUILDING, T_HALFBUILD, T_BOAT, T_FOREST;
    double e_wall_shots, e_half_wall_shots, e_boat_shots;
    int    forest_transparent;
    double sh_speed, sh_add, sh_life, sh_hit_r, pill_len;
    int    cg_on;
    int    ts_boats, ts_nt;
    double ts_t[2 * PT2_TS_MAX_TANKS];
    int    om_set, om_sent;
    double omx, omy, sent_omx, sent_omy;
    /* ctx */
    PList *hostile;
    int    finish;
    double tt_hits;
    int    bs_on, ba_tab;
    HMap   reach;           /* reach_cache */
    HMap   lgt;             /* lgm_ticks memo: value 0 = false, else t + 1 */
    HMap   lgt_v;
    /* c_std */
    int    n_map, n_stay, n_std, n_bld;
    unsigned char std_ok[PT2_TT_N];
    double nb[PT2_STD_LIST]; int nb_n;
    int    no_builder;
    double st_r[4 * PT2_STAY_MAX]; int st_n;
    /* shells */
    double ts_s1, ts_slast, ts_step, ts_hit;
    double ps_s1, ps_slast, ps_step, ps_hit;
    WSet  *asets;
    WSet **cws; int n_cws;      /* centre wedges by pill index (this evaluate) */
    HMap   cwm;                 /* centre wedges: X -> WSet * */
    /* stat */
    double sq_reads, casts, casts_confirm, ray_reuse, nudged, planned, confirm_fail, wall_nodes;
    double cant, survive, lone, plan_skipped, lazy_n, plan_cut, must_cut, must_cut_wrong;
    double co_stop, co_single_eval, wall_slow, confirm_retry, confirm_rescued, reach_calls;
    double aimnb_moved;
    double leeway_more, leeway_more_fail, leeway_single;
    double n_shot, memo_nodes;
    /* the slicer */
    int    sl_on;
    double paused;
    /* the evaluate's own state (evaluate_lua's locals) */
    double T0;
    double surv1; int has_surv1;
    double n_eval, n_reject;
    double pcx, pcy;
    int    lazy, co;
    int    no_chain;            /* 0 nil, 1 "single" */
    Rec   *best;
    AllE  *all; int nall, capall;
    int    nradii; double radii[PT2_MAXRADII];
    int    ri;                  /* ring index */
    double R;
    double nsteps;
    /* the ring's points */
    int    np, cap_pp;
    double *pp_deg, *pp_cx, *pp_cy, *pp_mx, *pp_my, *pp_px, *pp_py;
    int    *pp_hasA, *pp_cant;
    int    q;                   /* point index */
    Rec  **cands; int ncands, cap_cands;      /* 1-based */
    Rec  **scored; int nscored, cap_scored;   /* 1-based */
    Rec  **singles; int nsingles;             /* 1-based */
    Rec  **order; int norder;                 /* 1-based */
    Rec  **top; int ntop;
    int    has_singles_co;
    int    lone_r;
    double m; int has_m;
    int    ci;                  /* loop index */
    Rec   *X, *Lr;
    int    again;
    int    any;
    double sb;                  /* kmin_of's shots per blocker */
    /* the slicer (M.slicer) */
    double sl_ticks, sl_budget; int sl_has_budget;
    double sl_slices, sl_start, sl_last, sl_over;
    double sl_wu[64];
    /* the wall search's scratch */
    HMap   wsmask, wslane, wsseen;
    double co_bc;
};

/* PT2_LINK_CLEAR_FRONT with PT2_LINK_CUT_MIN_SHOTS (OLD.link_cut). */
static int link_cut(const Ev *e, double front) {
    return front > 0 && front >= e->link_cut_min_shots;
}

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

/* Lua's a % b. */
static double luamod(double a, double b) { return a - floor(a / b) * b; }

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

static double life_ticks(const Ev *e, double len) {
    double t = 1 + floor(f32(e->sh_life * f32(len))) - e->sh_add;
    return t < 0 ? 0 : t;
}

static void calc_distance_both(double angle, double speed, double *ox, double *oy,
                               double *hx, double *hy) {
    double db = sincos_args(angle);
    double c = cos(db), s = sin(db);
    *ox = round_double(speed * c);
    *oy = round_double(speed * s);
    *hx = round_double(speed * c * 256.0);
    *hy = round_double(speed * s * 256.0);
}

typedef struct {
    int     fast;
    int32_t xi, yi, xci, yci, xsi, ysi;
    double  x, y, xc, yc, xs, ys;
} Walk;
typedef char pt2_arith_shift_ok[((-256 >> 8) == -1) ? 1 : -1];

static int whole_in(double v, double lim) { return v == floor(v) && v > -lim && v < lim; }

static void walk_init(Walk *w, double x, double y, double xs, double ys) {
    w->x = x; w->y = y; w->xc = 0; w->yc = 0; w->xs = xs; w->ys = ys;
    w->fast = whole_in(x, 1073741824.0) && whole_in(y, 1073741824.0)
              && whole_in(xs, 1048576.0) && whole_in(ys, 1048576.0);
    if (w->fast) {
        w->xi = (int32_t)x; w->yi = (int32_t)y; w->xci = 0; w->yci = 0;
        w->xsi = (int32_t)xs; w->ysi = (int32_t)ys;
    }
}

static void walk_step(Walk *w, double *mx, double *my, double *sq) {
    if (w->fast) {
        w->xci += w->xsi; w->yci += w->ysi;
        int32_t xm = w->xci >> 8, ym = w->yci >> 8;
        w->xci -= xm * 256; w->yci -= ym * 256;
        w->xi += xm; w->yi += ym;
        int32_t mxi = (int32_t)((uint32_t)w->xi >> 8), myi = (int32_t)((uint32_t)w->yi >> 8);
        w->x = (double)w->xi; w->y = (double)w->yi;
        *mx = (double)mxi; *my = (double)myi;
        *sq = (double)((int64_t)myi * 256 + mxi);
    } else {
        w->xc = w->xc + w->xs; w->yc = w->yc + w->ys;
        double xm = floor(w->xc / 256), ym = floor(w->yc / 256);
        w->xc = w->xc - xm * 256; w->yc = w->yc - ym * 256;
        w->x = w->x + xm; w->y = w->y + ym;
        *mx = rsh8(w->x); *my = rsh8(w->y);
        *sq = *my * 256 + *mx;
    }
}

/* tostring(n) / n .. "" of a Lua number: LuaJIT's "%.14g", with its
 * names for the special values. */
static void fmt_num(char *buf, size_t sz, double v) {
    if (v != v) { snprintf(buf, sz, "nan"); return; }
    if (v == HUGE_VAL) { snprintf(buf, sz, "inf"); return; }
    if (v == -HUGE_VAL) { snprintf(buf, sz, "-inf"); return; }
    snprintf(buf, sz, "%.14g", v);
}

/* A growing byte string. */
typedef struct { char *p; size_t n, cap; lua_State *L; } SBuf;
static void sb_init(SBuf *b, lua_State *L) { b->p = NULL; b->n = 0; b->cap = 0; b->L = L; }
static void sb_free(SBuf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }
static void sb_add(SBuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->n + n + 1) nc *= 2;
        char *np = (char *)realloc(b->p, nc);
        if (!np) { luaL_error(b->L, "gh_pt2_v2: out of memory"); return; }
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void sb_str(SBuf *b, const char *s) { sb_add(b, s, strlen(s)); }
static void sb_num(SBuf *b, double v) { char t[64]; fmt_num(t, sizeof(t), v); sb_str(b, t); }

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

static double ttype(Ev *e, double mx, double my);
static int pill_on(Ev *e, double mx, double my);

static int in_map(Ev *e, double mx, double my) {
    if (!e->n_map) return call_b2(e, S_IN_MAP, mx, my);
    return mx >= 0 && mx <= 255 && my >= 0 && my <= 255;
}

static int stay_in(Ev *e, double mx, double my) {
    if (!e->n_stay) return call_b2(e, S_STAY_IN, mx, my);
    if (e->st_n == 0) return 1;
    for (int i = 0; i < e->st_n; i++) {
        const double *r = e->st_r + 4 * i;
        if (mx >= r[0] && mx <= r[2] && my >= r[1] && my <= r[3]) return 1;
    }
    return 0;
}

static int standable(Ev *e, double mx, double my) {
    if (!e->n_std) return call_b2(e, S_STANDABLE, mx, my);
    double tt = ttype(e, mx, my);
    if (!(tt >= 0 && tt < PT2_TT_N && tt == floor(tt)) || !e->std_ok[(int)tt]) return 0;
    return pill_on(e, mx, my) == 0;
}

static int buildable(Ev *e, double mx, double my) {
    if (!e->n_bld) return call_b2(e, S_BUILDABLE, mx, my);
    if (e->no_builder) return 0;
    double tt = ttype(e, mx, my);
    for (int i = 0; i < e->nb_n; i++) if (tt == e->nb[i]) return 0;
    return 1;
}

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

#define NEW(e, T) ((T *)ar_alloc((e)->L, &(e)->ar, sizeof(T)))
#define NEWN(e, T, n) ((T *)ar_alloc((e)->L, &(e)->ar, sizeof(T) * (size_t)((n) > 0 ? (n) : 1)))
#define JNEW(e, T) ((T *)ar_alloc((e)->L, &(e)->job->ar, sizeof(T)))
#define JNEWN(e, T, n) ((T *)ar_alloc((e)->L, &(e)->job->ar, sizeof(T) * (size_t)((n) > 0 ? (n) : 1)))

static void pill_read(lua_State *L, int idx, Pill *p) {
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
    p->ally = -1; p->skip = -1; p->cover_set = 0;
}

/* The pill index of the table at stack index idx (added when new). The
 * list belongs to the job, so an index stays the same for its life. */
static int pill_index(Ev *e, int idx) {
    lua_State *L = e->L;
    Job *j = e->job;
    if (idx < 0) idx = lua_gettop(L) + idx + 1;
    lua_pushvalue(L, idx);
    lua_rawget(L, S_PIDX);
    if (lua_type(L, -1) == LUA_TNUMBER) {
        int k = (int)lua_tonumber(L, -1);
        lua_pop(L, 1);
        return k;
    }
    lua_pop(L, 1);
    if (j->npills + 1 >= j->cap_pills) {
        int nc = j->cap_pills ? j->cap_pills * 2 : 64;
        Pill **np = (Pill **)realloc(j->pills, sizeof(Pill *) * (size_t)nc);
        if (!np) { luaL_error(L, "gh_pt2_v2: out of memory"); return 0; }
        j->pills = np; j->cap_pills = nc;
    }
    int k = ++j->npills;
    Pill *p = (Pill *)ar_alloc(L, &j->ar, sizeof(Pill));
    j->pills[k] = p;
    memset(p, 0, sizeof(*p));
    pill_read(L, idx, p);
    lua_pushvalue(L, idx);
    lua_rawseti(L, S_PILLS, k);
    lua_pushvalue(L, idx);
    lua_pushinteger(L, k);
    lua_rawset(L, S_PIDX);
    return k;
}

static Pill *PI(Ev *e, int k) {
    Pill *p = e->job->pills[k];
    if (p->gen != e->c->gen) { p->gen = e->c->gen; p->ally = -1; p->skip = -1; p->cover_set = 0; }
    return p;
}

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
        if (!lua_toboolean(L, S_CHAIN_SKIP)) {
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
    if (!e->bs_on) return 0;
    if (e->ba_tab) {
        double key = my * 256 + mx;
        int has;
        if (key >= 0 && key < PT2_MAP_SQ && key == floor(key)) {
            has = e->c->ba_stamp[(int)key] == e->c->gen;
        } else {
            lua_pushnumber(L, key);
            lua_rawget(L, S_BASE_AT);
            has = !lua_isnil(L, -1);
            lua_pop(L, 1);
        }
        if (!has) return 0;
    }
    sync_om(e);
    return call_b2(e, S_BASE_STOPS, mx, my);
}

/* ── Lists ──────────────────────────────────────────────────────────── */
static PList *plist_new(Ev *e, int cap) {
    PList *l = NEW(e, PList);
    memset(l, 0, sizeof(*l));
    l->cap = cap > 0 ? cap : 4;
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
static int in_chain(const PList *chain, int k) {
    if (!chain || !k) return 0;
    for (int i = 0; i < chain->n; i++) if (chain->v[i] == k) return i + 1;
    return 0;
}

static Extra *extra_new(Ev *e) {
    Extra *x = NEW(e, Extra);
    memset(x, 0, sizeof(*x));
    x->cap = 4;
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

/* ── shadow.cast ────────────────────────────────────────────────────── */
/* The ray's lists from the walk's buffers, in arena a (the job's for a
 * plain ray, the evaluate's for the others). */
static void ray_keep(Ev *e, Arena *a, Ray *res, const int64_t *sq_list, const double *sq_t,
                     const Blk *block, int nsq, int nblk) {
    lua_State *L = e->L;
    res->sq = (int64_t *)ar_alloc(L, a, sizeof(int64_t) * (size_t)(nsq > 0 ? nsq : 1));
    res->sq_t = (double *)ar_alloc(L, a, sizeof(double) * (size_t)(nsq > 0 ? nsq : 1));
    res->blk = (Blk **)ar_alloc(L, a, sizeof(Blk *) * (size_t)(nblk > 0 ? nblk : 1));
    memcpy(res->sq, sq_list, sizeof(int64_t) * (size_t)nsq);
    memcpy(res->sq_t, sq_t, sizeof(double) * (size_t)nsq);
    Blk *bs = (Blk *)ar_alloc(L, a, sizeof(Blk) * (size_t)(nblk > 0 ? nblk : 1));
    for (int i = 0; i < nblk; i++) {
        bs[i] = block[i];
        bs[i].m.memo = 0; bs[i].m.mg = 0;
        res->blk[i] = &bs[i];
    }
}

/* keep = 0: scratch (hold_safe), only hit / link / cover are read. */
static Ray *cast_walk(Ev *e, Arena *a, double sx, double sy, double tx, double ty, double len,
                      const Extra *extra, int keep, Ray *scratch) {
    int64_t sq_list[PT2_MAX_T];
    double  sq_t[PT2_MAX_T];
    Blk     block[PT2_MAX_T];
    Ray *res = keep ? (Ray *)ar_alloc(e->L, a, sizeof(Ray)) : scratch;
    memset(res, 0, sizeof(*res));
    double angle = calc_angle(sx, sy, tx, ty);
    res->angle = angle;
    double xa, ya, xs, ys;
    calc_distance_both(angle, e->sh_speed, &xa, &ya, &xs, &ys);
    Walk w;
    walk_init(&w, sx + e->sh_add * xa, sy + e->sh_add * ya, xs, ys);
    double n = life_ticks(e, len);
    double r = e->sh_hit_r;
    double r2 = r * r;
    int nsq = 0, nblk = 0;
    double last_sq = -1;
    double cover = 0, front = 0;
    int link = 0; double link_t = 0;
    int tank = 0; double hit_t = n;
    for (double t = 1; t <= n; t++) {
        double mx, my, sq;
        walk_step(&w, &mx, &my, &sq);
        int new_sq = sq != last_sq;
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
        double dx = w.x - tx, dy = w.y - ty;
        if (dx < r && dx > -r && dy < r && dy > -r && dx * dx + dy * dy < r2) {
            tank = 1; hit_t = t;
            break;
        }
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
    if (nsq > PT2_MAX_T || nblk > PT2_MAX_T) luaL_error(e->L, "gh_pt2_v2: ray too long");
    res->tank = tank; res->t = hit_t;
    res->link = link; res->link_t = link_t;
    res->cover = cover; res->front = front;
    res->n_sq = nsq; res->n_block = nblk;
    res->end_x = w.x; res->end_y = w.y;
    if (keep) ray_keep(e, a, res, sq_list, sq_t, block, nsq, nblk);
    return res;
}

/* ── shadow.tank_shot ───────────────────────────────────────────────── */
static int ts_tank_hit(const Ev *e, double x, double y) {
    double r = e->sh_hit_r, r2 = r * r;
    for (int i = 0; i < e->ts_nt; i++) {
        double dx = x - e->ts_t[2 * i], dy = y - e->ts_t[2 * i + 1];
        if (dx < r && dx > -r && dy < r && dy > -r && dx * dx + dy * dy < r2) return 1;
    }
    return 0;
}

static int ts_tank_near(const Ev *e, double ox, double oy, double ax, double ay) {
    double dx = ax - ox, dy = ay - oy;
    double d = sqrt(dx * dx + dy * dy);
    if (!(d > 0)) return 1;
    double ux = dx / d, uy = dy / d;
    double len = (e->sh_add + life_ticks(e, PT2_TANK_FIRE_LEN)) * e->sh_speed + 2;
    double m = e->sh_hit_r + 8 + e->sh_add;
    for (int i = 0; i < e->ts_nt; i++) {
        double px = e->ts_t[2 * i] - ox, py = e->ts_t[2 * i + 1] - oy;
        double s = px * ux + py * uy;
        if (s < 0) s = 0;
        if (s > len) s = len;
        double qx = px - s * ux, qy = py - s * uy;
        if (qx * qx + qy * qy < m * m) return 1;
    }
    return 0;
}

static Lane *lane_make(Ev *e, const int64_t *lane, int nl) {
    Lane *l = NEW(e, Lane);
    memset(l, 0, sizeof(*l));
    l->n = nl;
    l->v = NEWN(e, int64_t, nl);
    memcpy(l->v, lane, sizeof(int64_t) * (size_t)nl);
    return l;
}

static int tank_shot_walk(Ev *e, double ox, double oy, double ax, double ay, double tmx, double tmy,
                          const Extra *extra, Lane **lane_out) {
    int64_t lane[PT2_MAX_T];
    double angle = tank_angle(ox, oy, ax, ay);
    double xa, ya, xs, ys;
    calc_distance_both(angle, e->sh_speed, &xa, &ya, &xs, &ys);
    Walk w;
    walk_init(&w, ox + e->sh_add * xa, oy + e->sh_add * ya, xs, ys);
    double n = life_ticks(e, PT2_TANK_FIRE_LEN);
    int nl = 0;
    double last_sq = -1;
    double omx = rsh8(ox), omy = rsh8(oy);
    int why = TS_SHORT;
    for (double t = 1; t <= n; t++) {
        double mx, my, sq;
        walk_step(&w, &mx, &my, &sq);
        int new_sq = 0;
        if (sq != last_sq) {
            last_sq = sq;
            if (mx == tmx && my == tmy) { why = TS_OK; break; }
            if (!(mx == omx && my == omy)) {
                new_sq = 1;
                if (nl < PT2_MAX_T) lane[nl] = (int64_t)sq;
                nl++;
                if (pill_on(e, mx, my)) { why = TS_PILL; break; }
            }
        }
        if (e->ts_nt > 0 && ts_tank_hit(e, w.x, w.y)) { why = TS_TANK; break; }
        if (new_sq) {
            if (extra_find(extra, (int64_t)sq) >= 0) { why = TS_PLANNED; break; }
            double tt = ttype(e, mx, my);
            if (tt == e->T_BUILDING || tt == e->T_HALFBUILD) { why = TS_WALL; break; }
            if (e->ts_boats && tt == e->T_BOAT) { why = TS_BOAT; break; }
            if (base_stops(e, mx, my)) { why = TS_BASE; break; }
        }
    }
    if (why == TS_OK && lane_out) {
        if (nl > PT2_MAX_T) luaL_error(e->L, "gh_pt2_v2: lane too long");
        *lane_out = lane_make(e, lane, nl);
    }
    return why;
}
static uint32_t cg_hash(int32_t a, int32_t b, int32_t c) {
    uint32_t h = (uint32_t)a * 0x9E3779B1u;
    h ^= (uint32_t)b * 0x85EBCA77u;
    h ^= (uint32_t)c * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

/* The entry of key (k1, k2, k3) (*found = 1) or the free slot for it. */
static CGeom *cg_slot(CGeom *tab, int nslots, int32_t k1, int32_t k2, int32_t k3, int *found) {
    uint32_t m = (uint32_t)nslots - 1;
    uint32_t i = cg_hash(k1, k2, k3) & m;
    for (;;) {
        CGeom *g = &tab[i];
        if (!g->used) { *found = 0; return g; }
        if (g->k1 == k1 && g->k2 == k2 && g->k3 == k3) { *found = 1; return g; }
        i = (i + 1) & m;
    }
}

/* Make the tables (first use) and clear them when the shell numbers
 * changed. Returns 1 when this evaluate uses them. */
static int cg_ready(Ev *e) {
    Pt2Ctx *c = e->c;
    if (c->cg_off) return 0;
    if (!c->cg) {
        c->cg = (CGeom *)calloc(CG_SLOTS, sizeof(CGeom));
        c->tg = (CGeom *)calloc(TG_SLOTS, sizeof(CGeom));
        if (!c->cg || !c->tg) {
            free(c->cg); free(c->tg); c->cg = NULL; c->tg = NULL;
            return 0;
        }
        c->cg_n = 0; c->tg_n = 0; c->cg_par_set = 0;
    }
    double par[5] = { e->sh_speed, e->sh_add, e->sh_life, e->sh_hit_r, e->pill_len };
    if (!c->cg_par_set || memcmp(par, c->cg_par, sizeof(par)) != 0) {
        memset(c->cg, 0, sizeof(CGeom) * CG_SLOTS);
        memset(c->tg, 0, sizeof(CGeom) * TG_SLOTS);
        c->cg_n = 0; c->tg_n = 0;
        memcpy(c->cg_par, par, sizeof(par));
        c->cg_par_set = 1;
    }
    return 1;
}

/* Add square (mx, my) at step t to g, as an offset from (bx, by). Returns 0
 * when it does not fit (off the map, too far, too many squares). */
static int cg_add(CGeom *g, double mx, double my, int bx, int by, double t) {
    if (mx < 0 || mx > 255 || my < 0 || my > 255 || g->n >= CG_MAXS || t > 255) return 0;
    int ox = (int)mx - bx, oy = (int)my - by;
    if (ox < -127 || ox > 127 || oy < -127 || oy > 127) return 0;
    int k = g->n++;
    g->ox[k] = (int8_t)ox; g->oy[k] = (int8_t)oy; g->t[k] = (uint8_t)t;
    if (k == 0 || ox < g->minx) g->minx = (int8_t)ox;
    if (k == 0 || ox > g->maxx) g->maxx = (int8_t)ox;
    if (k == 0 || oy < g->miny) g->miny = (int8_t)oy;
    if (k == 0 || oy > g->maxy) g->maxy = (int8_t)oy;
    return 1;
}

static int cg_fits(const CGeom *g, int bx, int by) {
    if (g->used != 1) return 0;
    if (g->n == 0) return 1;
    return bx + g->minx >= 0 && bx + g->maxx <= 255 && by + g->miny >= 0 && by + g->maxy <= 255;
}

/* The walk of cast_walk with no map reads: the squares up to the tank hit
 * (the square entered on the tank-hit step is kept: its pill is tested
 * before the tank) or the end of life. */
static int cg_build_cast(const Ev *e, double sx, double sy, double tx, double ty, double len,
                         int bx, int by, CGeom *g) {
    double angle = calc_angle(sx, sy, tx, ty);
    double xa, ya, xs, ys;
    calc_distance_both(angle, e->sh_speed, &xa, &ya, &xs, &ys);
    Walk w;
    walk_init(&w, sx + e->sh_add * xa, sy + e->sh_add * ya, xs, ys);
    if (!w.fast) return 0;   /* whole steps only: then a move of the start moves every point */
    double n = life_ticks(e, len);
    if (n > 255) return 0;
    double r = e->sh_hit_r;
    double r2 = r * r;
    double last_sq = -1;
    int tank = 0; double hit_t = 0;
    g->n = 0;
    for (double t = 1; t <= n; t++) {
        double mx, my, sq;
        walk_step(&w, &mx, &my, &sq);
        int new_sq = sq != last_sq;
        if (new_sq && !cg_add(g, mx, my, bx, by, t)) return 0;
        double dx = w.x - tx, dy = w.y - ty;
        if (dx < r && dx > -r && dy < r && dy > -r && dx * dx + dy * dy < r2) {
            tank = 1; hit_t = t;
            break;
        }
        if (new_sq) last_sq = sq;
    }
    double ex = w.x - sx, ey = w.y - sy;
    if (!whole_in(ex, 1073741824.0) || !whole_in(ey, 1073741824.0)) return 0;
    g->flag = (uint8_t)tank; g->hit_t = (int16_t)hit_t;
    g->ex = (int32_t)ex; g->ey = (int32_t)ey;
    g->angle = angle;
    return 1;
}

/* cast_walk's result from the table entry g (start square bx, by). */
static Ray *cg_cast(Ev *e, Arena *a, const CGeom *g, int bx, int by, double sx, double sy, double len,
                    const Extra *extra) {
    int64_t sq_list[CG_MAXS];
    double  sq_t[CG_MAXS];
    Blk     block[2 * CG_MAXS];
    Ray *res = (Ray *)ar_alloc(e->L, a, sizeof(Ray));
    memset(res, 0, sizeof(*res));
    res->angle = g->angle;
    double n = life_ticks(e, len);
    int nsq = 0, nblk = 0;
    double cover = 0, front = 0;
    int link = 0; double link_t = 0;
    for (int k = 0; k < g->n; k++) {
        double t = g->t[k];
        double mx = bx + g->ox[k], my = by + g->oy[k];
        double sq = my * 256 + mx;
        /* 1. a live pill on the square */
        int p = pill_on(e, mx, my);
        if (p) {
            if (is_ally(e, p)) {
                double s = cover_of_pill(e, p);
                cover = cover + s;
                if (!link) front = front + s;
                Blk *b = &block[nblk++];
                b->sq = (int64_t)sq; b->shots = s; b->kind = BK_ALLY_PILL; b->t = t; b->pill = p;
            } else if (!link) {
                link = p; link_t = t;
            }
        }
        /* 2. the tank circle: this square's step hit the tank */
        if (g->flag && t == g->hit_t) break;
        /* 3. terrain, then planned blockers */
        sq_list[nsq] = (int64_t)sq; sq_t[nsq] = t;
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
            Blk *b = &block[nblk++];
            b->sq = (int64_t)sq; b->shots = s; b->kind = kind; b->t = t; b->pill = 0;
        }
    }
    res->tank = g->flag; res->t = g->flag ? (double)g->hit_t : n;
    res->link = link; res->link_t = link_t;
    res->cover = cover; res->front = front;
    res->n_sq = nsq; res->n_block = nblk;
    res->end_x = sx + g->ex; res->end_y = sy + g->ey;
    ray_keep(e, a, res, sq_list, sq_t, block, nsq, nblk);
    return res;
}

static Ray *cast(Ev *e, Arena *a, double sx, double sy, double tx, double ty, double len,
                 const Extra *extra, int keep, Ray *scratch) {
    if (!keep || !e->cg_on || len != e->pill_len)
        return cast_walk(e, a, sx, sy, tx, ty, len, extra, keep, scratch);
    double bxd = (sx - 128) / 256, byd = (sy - 128) / 256;
    double dx = tx - sx, dy = ty - sy;
    if (!(bxd == floor(bxd) && bxd >= 0 && bxd <= 255 && byd == floor(byd) && byd >= 0 && byd <= 255
          && whole_in(dx, 1048576.0) && whole_in(dy, 1048576.0)))
        return cast_walk(e, a, sx, sy, tx, ty, len, extra, keep, scratch);
    Pt2Ctx *c = e->c;
    int bx = (int)bxd, by = (int)byd;
    if (c->cg_n >= CG_SLOTS / 4 * 3) { memset(c->cg, 0, sizeof(CGeom) * CG_SLOTS); c->cg_n = 0; }
    int found;
    CGeom *g = cg_slot(c->cg, CG_SLOTS, (int32_t)dx, (int32_t)dy, 0, &found);
    if (!found) {
        c->cg_n++;
        g->k1 = (int32_t)dx; g->k2 = (int32_t)dy; g->k3 = 0;
        g->used = cg_build_cast(e, sx, sy, tx, ty, len, bx, by, g) ? 1 : 2;
    }
    if (!cg_fits(g, bx, by)) return cast_walk(e, a, sx, sy, tx, ty, len, extra, keep, scratch);
    Ray *r = cg_cast(e, a, g, bx, by, sx, sy, len, extra);
    return r;
}

/* The walk of tank_shot_walk with no map reads: every new square (offset
 * from the start square) up to the target square or the end of life. */
static int cg_build_tshot(const Ev *e, double ox, double oy, double ax, double ay,
                          double tmx, double tmy, int bx, int by, CGeom *g) {
    double angle = tank_angle(ox, oy, ax, ay);
    double xa, ya, xs, ys;
    calc_distance_both(angle, e->sh_speed, &xa, &ya, &xs, &ys);
    Walk w;
    walk_init(&w, ox + e->sh_add * xa, oy + e->sh_add * ya, xs, ys);
    if (!w.fast) return 0;
    double n = life_ticks(e, PT2_TANK_FIRE_LEN);
    if (n > 255) return 0;
    double last_sq = -1;
    int tgt = 0;
    g->n = 0;
    for (double t = 1; t <= n; t++) {
        double mx, my, sq;
        walk_step(&w, &mx, &my, &sq);
        if (sq != last_sq) {
            last_sq = sq;
            if (!cg_add(g, mx, my, bx, by, t)) return 0;
            if (mx == tmx && my == tmy) { tgt = 1; break; }
        }
    }
    g->flag = (uint8_t)tgt; g->hit_t = 0; g->ex = 0; g->ey = 0; g->angle = angle;
    return 1;
}

/* tank_shot_walk's answer from the table entry g (start square bx, by). */
static int cg_tshot(Ev *e, const CGeom *g, int bx, int by, const Extra *extra, Lane **lane_out) {
    int64_t lane[CG_MAXS];
    int nl = 0;
    int why = TS_SHORT;
    for (int k = 0; k < g->n; k++) {
        if (g->flag && k == g->n - 1) { why = TS_OK; break; }
        double mx = bx + g->ox[k], my = by + g->oy[k];
        if (g->ox[k] == 0 && g->oy[k] == 0) continue;
        double sq = my * 256 + mx;
        lane[nl++] = (int64_t)sq;
        if (pill_on(e, mx, my)) { why = TS_PILL; break; }
        if (extra_find(extra, (int64_t)sq) >= 0) { why = TS_PLANNED; break; }
        double tt = ttype(e, mx, my);
        if (tt == e->T_BUILDING || tt == e->T_HALFBUILD) { why = TS_WALL; break; }
        if (e->ts_boats && tt == e->T_BOAT) { why = TS_BOAT; break; }
        if (base_stops(e, mx, my)) { why = TS_BASE; break; }
    }
    if (why == TS_OK && lane_out) {
        *lane_out = lane_make(e, lane, nl);
    }
    return why;
}

/* shadow.tank_shot: the table when the shot fits it, else the walk. */
static int tank_shot(Ev *e, double ox, double oy, double ax, double ay, double tmx, double tmy,
                     const Extra *extra, Lane **lane_out) {
    if (!e->cg_on) return tank_shot_walk(e, ox, oy, ax, ay, tmx, tmy, extra, lane_out);
    if (e->ts_nt > 0 && ts_tank_near(e, ox, oy, ax, ay))
        return tank_shot_walk(e, ox, oy, ax, ay, tmx, tmy, extra, lane_out);
    double dx = ax - ox, dy = ay - oy;
    if (!(whole_in(ox, 65536.0) && ox >= 0 && whole_in(oy, 65536.0) && oy >= 0
          && whole_in(dx, 1048576.0) && whole_in(dy, 1048576.0)
          && tmx == floor(tmx) && tmy == floor(tmy)))
        return tank_shot_walk(e, ox, oy, ax, ay, tmx, tmy, extra, lane_out);
    int oxi = (int)ox, oyi = (int)oy;
    int bx = oxi >> 8, by = oyi >> 8;
    double tdx = tmx - bx, tdy = tmy - by;
    if (tdx < -127 || tdx > 127 || tdy < -127 || tdy > 127)
        return tank_shot_walk(e, ox, oy, ax, ay, tmx, tmy, extra, lane_out);
    int32_t k3 = (int32_t)((uint32_t)(oxi & 255) | ((uint32_t)(oyi & 255) << 8)
                           | ((uint32_t)(uint8_t)(int8_t)tdx << 16)
                           | ((uint32_t)(uint8_t)(int8_t)tdy << 24));
    Pt2Ctx *c = e->c;
    if (c->tg_n >= TG_SLOTS / 4 * 3) { memset(c->tg, 0, sizeof(CGeom) * TG_SLOTS); c->tg_n = 0; }
    int found;
    CGeom *g = cg_slot(c->tg, TG_SLOTS, (int32_t)dx, (int32_t)dy, k3, &found);
    if (!found) {
        c->tg_n++;
        g->k1 = (int32_t)dx; g->k2 = (int32_t)dy; g->k3 = k3;
        g->used = cg_build_tshot(e, ox, oy, ax, ay, tmx, tmy, bx, by, g) ? 1 : 2;
    }
    if (!cg_fits(g, bx, by)) return tank_shot_walk(e, ox, oy, ax, ay, tmx, tmy, extra, lane_out);
    int why = cg_tshot(e, g, bx, by, extra, lane_out);
    return why;
}

/* ── table.sort ─────────────────────────────────────────────────────── */
/* LuaJIT lib_table.c auxsort over a[l..u] (1-based, as the Lua list),
 * comparison for comparison, so equal keys end in the same order. */
typedef int (*LtFn)(void *cx, void *a, void *b);

static void auxsort(lua_State *L, void **a, int l, int u, LtFn lt, void *cx) {
    while (l < u) {
        int i, j; void *t;
        if (lt(cx, a[u], a[l])) { t = a[l]; a[l] = a[u]; a[u] = t; }
        if (u - l == 1) break;
        i = (l + u) / 2;
        if (lt(cx, a[i], a[l])) { t = a[i]; a[i] = a[l]; a[l] = t; }
        else { if (lt(cx, a[u], a[i])) { t = a[i]; a[i] = a[u]; a[u] = t; } }
        if (u - l == 2) break;
        void *P = a[i]; t = a[i]; a[i] = a[u - 1]; a[u - 1] = t;
        i = l; j = u - 1;
        for (;;) {
            while (lt(cx, a[++i], P)) { if (i >= u) luaL_error(L, "invalid order function for sorting"); }
            while (lt(cx, P, a[--j])) { if (j <= l) luaL_error(L, "invalid order function for sorting"); }
            if (j < i) break;
            t = a[i]; a[i] = a[j]; a[j] = t;
        }
        t = a[u - 1]; a[u - 1] = a[i]; a[i] = t;
        if (i - l < u - i) { j = l; i = i - 1; l = i + 2; } else { j = i + 1; i = u; u = j - 2; }
        auxsort(L, a, j, i, lt, cx);
    }
}
/* table.sort(list) of n entries at a[1..n] */
static void lsort(lua_State *L, void **a, int n, LtFn lt, void *cx) { auxsort(L, a, 1, n, lt, cx); }

/* ── Wedges ─────────────────────────────────────────────────────────── */
static double bearing(double dx, double dy) { return atan2(dx, -dy); }
static double wrap(double a) {
    if (a > PT2_PI) a = a - PT2_TWO_PI; else if (a <= -PT2_PI) a = a + PT2_TWO_PI;
    if (a > PT2_PI) a = a - PT2_TWO_PI; else if (a <= -PT2_PI) a = a + PT2_TWO_PI;
    return a;
}
static int bin_of(double th) {
    double b = floor((th + PT2_PI) / PT2_TWO_PI * NBIN);
    if (b >= NBIN) b = NBIN - 1; else if (b < 0) b = 0;
    return (int)b;
}
static double min4(double a, double b, double c, double d) {
    double m = a; if (b < m) m = b; if (c < m) m = c; if (d < m) m = d; return m;
}
static double max4(double a, double b, double c, double d) {
    double m = a; if (b > m) m = b; if (c > m) m = c; if (d > m) m = d; return m;
}
static double max3(double a, double b, double c) {
    double m = a; if (b > m) m = b; if (c > m) m = c; return m;
}
static double dmin(double a, double b) { return a < b ? a : b; }
static double dmax(double a, double b) { return a > b ? a : b; }

static int span(double ax, double ay, double x0, double y0, double x1, double y1,
                double *c, double *lo, double *hi) {
    if (ax >= x0 && ax <= x1 && ay >= y0 && ay <= y1) return 0;
    double cc = bearing((x0 + x1) * 0.5 - ax, (y0 + y1) * 0.5 - ay);
    double r1 = wrap(bearing(x0 - ax, y0 - ay) - cc);
    double r2 = wrap(bearing(x1 - ax, y0 - ay) - cc);
    double r3 = wrap(bearing(x0 - ax, y1 - ay) - cc);
    double r4 = wrap(bearing(x1 - ax, y1 - ay) - cc);
    *c = cc; *lo = min4(r1, r2, r3, r4); *hi = max4(r1, r2, r3, r4);
    return 1;
}

static double box_near(double ax, double ay, double x0, double y0, double x1, double y1) {
    double dx = max3(x0 - ax, 0, ax - x1);
    double dy = max3(y0 - ay, 0, ay - y1);
    return sqrt(dx * dx + dy * dy);
}
static double box_far(double ax, double ay, double x0, double y0, double x1, double y1) {
    double dx = dmax(fabs(x0 - ax), fabs(x1 - ax));
    double dy = dmax(fabs(y0 - ay), fabs(y1 - ay));
    return sqrt(dx * dx + dy * dy);
}

static int make_wedge(double ax, double ay, double mx, double my, int kind, double shots, int pill, Wedge *w) {
    double x0 = mx * 256, y0 = my * 256;
    double x1 = x0 + 255, y1 = y0 + 255;
    double c, lo_o, hi_o;
    if (!span(ax, ay, x0 - EDGE_IN, y0 - EDGE_IN, x1 + EDGE_IN, y1 + EDGE_IN, &c, &lo_o, &hi_o)) return 0;
    double ci, lo_i, hi_i;
    int has_i = span(ax, ay, x0 + EDGE_IN, y0 + EDGE_IN, x1 - EDGE_IN, y1 - EDGE_IN, &ci, &lo_i, &hi_i);
    w->sq = my * 256 + mx; w->mx = mx; w->my = my; w->kind = kind; w->shots = shots; w->pill = pill;
    w->c = c; w->lo_o = lo_o; w->hi_o = hi_o;
    w->near = box_near(ax, ay, x0, y0, x1, y1); w->far = box_far(ax, ay, x0, y0, x1, y1);
    if (has_i) { w->lo_i = wrap(ci + lo_i - c); w->hi_i = wrap(ci + hi_i - c); }
    else { w->lo_i = 1; w->hi_i = -1; }
    return 1;
}

static int wedge_lt(void *cx, void *pa, void *pb) {
    (void)cx;
    const Wedge *a = (const Wedge *)pa, *b = (const Wedge *)pb;
    if (a->near != b->near) return a->near < b->near;
    return a->sq < b->sq;
}

/* wedge_set(list): the list sorted (nearest first) and the angle bins.
 * ws[1..n] holds the wedges (1-based). */
static WSet *wedge_set(Ev *e, Wedge **ws, int n) {
    lsort(e->L, (void **)ws, n, wedge_lt, NULL);
    WSet *s = NEW(e, WSet);
    memset(s, 0, sizeof(*s));
    s->n = n;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            for (int b = 0; b < NBIN; b++) {
                s->bin[b] = NEWN(e, Wedge *, s->bin_n[b]);
                s->bin_n[b] = 0;
            }
        }
        for (int i = 1; i <= n; i++) {
            Wedge *w = ws[i];
            double a0 = w->c + w->lo_o, a1 = w->c + w->hi_o;
            double b0 = floor((a0 + PT2_PI) / PT2_TWO_PI * NBIN);
            double b1 = floor((a1 + PT2_PI) / PT2_TWO_PI * NBIN);
            for (double b = b0; b <= b1; b++) {
                int bb = (int)luamod(b, NBIN);
                if (pass == 0) s->bin_n[bb]++;
                else s->bin[bb][s->bin_n[bb]++] = w;
            }
        }
    }
    return s;
}

/* offsets(reach): square offsets by near distance, kept by floor(reach)
 * (the first reach of a key wins, as the Lua's OFFS). */
typedef struct { double dx, dy, d; } Off;
static int off_lt(void *cx, void *pa, void *pb) {
    (void)cx;
    const Off *a = (const Off *)pa, *b = (const Off *)pb;
    if (a->d != b->d) return a->d < b->d;
    if (a->dy != b->dy) return a->dy < b->dy;
    return a->dx < b->dx;
}
static Offs *offsets(Ev *e, double reach) {
    Pt2Ctx *c = e->c;
    double key = floor(reach);
    for (int i = 0; i < c->n_offs; i++) if (c->offs[i].key == key) return &c->offs[i];
    if (c->n_offs >= c->cap_offs) {
        int nc = c->cap_offs ? c->cap_offs * 2 : 8;
        Offs *no = (Offs *)realloc(c->offs, sizeof(Offs) * (size_t)nc);
        if (!no) luaL_error(e->L, "gh_pt2_v2: out of memory");
        c->offs = no; c->cap_offs = nc;
    }
    double R = ceil(reach / 256) + 1;
    int side = (int)(2 * R + 1);
    Off *tmp = (Off *)malloc(sizeof(Off) * (size_t)side * (size_t)side);
    void **pt = (void **)malloc(sizeof(void *) * ((size_t)side * (size_t)side + 1));
    if (!tmp || !pt) { free(tmp); free(pt); luaL_error(e->L, "gh_pt2_v2: out of memory"); }
    int n = 0;
    for (double dy = -R; dy <= R; dy++) {
        for (double dx = -R; dx <= R; dx++) {
            if (dx != 0 || dy != 0) {
                double nx = dmax(0, fabs(dx) * 256 - 128);
                double ny = dmax(0, fabs(dy) * 256 - 128);
                double d = sqrt(nx * nx + ny * ny);
                if (d <= reach) { tmp[n].dx = dx; tmp[n].dy = dy; tmp[n].d = d; n++; pt[n] = &tmp[n - 1]; }
            }
        }
    }
    lsort(e->L, pt, n, off_lt, NULL);
    Offs *o = &c->offs[c->n_offs];
    o->key = key; o->n = n;
    o->o = (double *)malloc(sizeof(double) * 2 * (size_t)(n > 0 ? n : 1));
    if (!o->o) { free(tmp); free(pt); luaL_error(e->L, "gh_pt2_v2: out of memory"); }
    for (int i = 0; i < n; i++) { Off *f = (Off *)pt[i + 1]; o->o[2 * i] = f->dx; o->o[2 * i + 1] = f->dy; }
    c->n_offs++;
    free(tmp); free(pt);
    return o;
}

/* sq_read: what square (mx, my) holds, read once per job. */
static SqV *sq_read(Ev *e, double mx, double my) {
    double key = my * 256 + mx;
    int isnew;
    HVal *hv = hm_put(e->L, &e->job->sqm, dbits(key), 0, &isnew);
    if (!isnew) return (SqV *)hv->p;
    hv->p = NULL;
    SqV *v = JNEW(e, SqV);
    v->kind = K_NONE; v->shots = 0; v->pill = 0;
    if (in_map(e, mx, my)) {
        int p = pill_on(e, mx, my);
        if (p) {
            if (is_ally(e, p)) { v->kind = K_ALLY; v->shots = cover_of_pill(e, p); v->pill = p; }
            else { v->kind = K_HOST; v->shots = 0; v->pill = p; }
        } else {
            double tt = ttype(e, mx, my);
            if (tt == e->T_BUILDING) { v->kind = K_WALL; v->shots = e->e_wall_shots; }
            else if (tt == e->T_HALFBUILD) { v->kind = K_HALF; v->shots = e->e_half_wall_shots; }
            else if (tt == e->T_BOAT) { v->kind = K_BOAT; v->shots = e->e_boat_shots; }
            else if (tt == e->T_FOREST && !e->forest_transparent) { v->kind = K_FOREST; v->shots = 1; }
        }
    }
    /* the slot may have moved while the callbacks ran (a new pill does
     * not touch sqm, but keep to the rule: look it up again) */
    hv = hm_find(&e->job->sqm, dbits(key), 0);
    hv->p = v;
    e->sq_reads += 1;
    return v;
}

/* The centre wedges of hostile pill X (ctx.cw: once per evaluate). */
static WSet *centre_wedges(Ev *e, int X) {
    int isnew;
    HVal *hv = hm_put(e->L, &e->cwm, (uint64_t)X, 0, &isnew);
    if (!isnew) return (WSet *)hv->p;
    hv->p = NULL;
    Pill *P = PI(e, X);
    double reach = e->pill_range + e->range_margin;
    Offs *o = offsets(e, reach);
    Wedge **ws = NEWN(e, Wedge *, o->n + 1);
    int n = 0;
    double sx = P->mx * 256 + 128, sy = P->my * 256 + 128;
    double xmx = P->mx, xmy = P->my;
    double *fmx = NEWN(e, double, o->n), *fmy = NEWN(e, double, o->n);
    SqV **fv = NEWN(e, SqV *, o->n);
    int nf = 0;
    for (int i = 0; i < o->n; i++) {
        double mx = xmx + o->o[2 * i], my = xmy + o->o[2 * i + 1];
        SqV *v = sq_read(e, mx, my);
        if (v->kind != K_NONE) { fmx[nf] = mx; fmy[nf] = my; fv[nf] = v; nf++; }
    }
    for (int i = 0; i < nf; i++) {
        Wedge *w = NEW(e, Wedge);
        if (make_wedge(sx, sy, fmx[i], fmy[i], fv[i]->kind, fv[i]->shots, fv[i]->pill, w)) ws[++n] = w;
    }
    WSet *set = wedge_set(e, ws, n);
    set->X = X;
    hv = hm_find(&e->cwm, (uint64_t)X, 0);
    hv->p = set;
    return set;
}

/* The aim wedges of start pill A: one set per aim point. */
static WSet *aim_wedges(Ev *e, int A) {
    Pill *P = PI(e, A);
    double reach = e->ts_slast + 400;
    Offs *o = offsets(e, reach);
    double *fmx = NEWN(e, double, o->n), *fmy = NEWN(e, double, o->n);
    SqV **fv = NEWN(e, SqV *, o->n);
    int nf = 0;
    double amx = P->mx, amy = P->my;
    for (int i = 0; i < o->n; i++) {
        double mx = amx + o->o[2 * i], my = amy + o->o[2 * i + 1];
        SqV *v = sq_read(e, mx, my);
        int k = v->kind;
        if (k == K_WALL || k == K_HALF || k == K_ALLY || k == K_HOST || (e->ts_boats && k == K_BOAT)) {
            fmx[nf] = mx; fmy[nf] = my; fv[nf] = v; nf++;
        }
    }
    WSet *sets = NEWN(e, WSet, e->naims);
    for (int ai = 0; ai < e->naims; ai++) {
        double ax = PI(e, A)->mx * 256 + s_aims9[ai][0], ay = PI(e, A)->my * 256 + s_aims9[ai][1];
        Wedge **ws = NEWN(e, Wedge *, nf + 1);
        int n = 0;
        for (int i = 0; i < nf; i++) {
            Wedge *w = NEW(e, Wedge);
            if (make_wedge(ax, ay, fmx[i], fmy[i], fv[i]->kind, fv[i]->shots, fv[i]->pill, w)) ws[++n] = w;
        }
        WSet *s = wedge_set(e, ws, n);
        sets[ai] = *s;
        sets[ai].ax = ax; sets[ai].ay = ay;
    }
    return sets;
}

static int wedge_state(const Wedge *w, double th, double dmn, double dmx) {
    double rel = wrap(th - w->c);
    if (rel < w->lo_o || rel > w->hi_o) return 0;
    if (w->near > dmx + SLACK || w->far < dmn - SLACK) return 0;
    dmn = dmn + SLACK; dmx = dmx - SLACK;
    if (rel >= w->lo_i && rel <= w->hi_i && w->near >= dmn && w->far <= dmx) return 2;
    return 1;
}

/* CANT(A) at (px, py). */
static int cant(Ev *e, WSet *sets, double px, double py) {
    double own = floor(py / 256) * 256 + floor(px / 256);
    for (int ai = 0; ai < e->naims; ai++) {
        WSet *set = &sets[ai];
        double dx = px - set->ax, dy = py - set->ay;
        double d = sqrt(dx * dx + dy * dy);
        double th = bearing(dx, dy);
        double dmx = d - e->ts_s1, dmn = d - e->ts_slast;
        int blocked = 0;
        if (dmx > SLACK) {
            int b = bin_of(th);
            for (int i = 0; i < set->bin_n[b]; i++) {
                Wedge *w = set->bin[b][i];
                if (w->sq != own && wedge_state(w, th, dmn, dmx) == 2) { blocked = 1; break; }
            }
        }
        if (!blocked) return 0;
    }
    return 1;
}

/* predict: what X's shell meets at (px, py), from X's centre wedges. */
typedef struct { int sure; int link; int used; double cover, front; int reach; } Pred;
static void predict(Ev *e, WSet *set, double px, double py, Pred *out) {
    Pill *X = PI(e, set->X);
    double sx = X->mx * 256 + 128, sy = X->my * 256 + 128;
    double dx = px - sx, dy = py - sy;
    double d = sqrt(dx * dx + dy * dy);
    double th = bearing(dx, dy);
    out->sure = 1; out->link = 0; out->used = 0; out->cover = 0; out->front = 0; out->reach = 0;
    double hit_at = d - e->ps_hit;
    if (e->ps_slast < hit_at - e->ps_step - SLACK) { out->reach = 1; hit_at = e->ps_slast; }
    else if (e->ps_slast < hit_at + e->ps_step + SLACK) out->sure = 0;
    double dmax_in = hit_at;
    double dmax_out = hit_at + e->ps_step;
    int b = bin_of(th);
    int nb = set->bin_n[b];
    Wedge **ins = NEWN(e, Wedge *, nb), **maybe = NEWN(e, Wedge *, nb);
    int nin = 0, nmb = 0;
    for (int i = 0; i < nb; i++) {
        Wedge *w = set->bin[b][i];
        int s = wedge_state(w, th, e->ps_s1, dmax_out);
        if (s != 0) {
            if (s == 2 && wedge_state(w, th, e->ps_s1, dmax_in) == 2) ins[nin++] = w;
            else maybe[nmb++] = w;
        }
    }
    Wedge *link = NULL;
    for (int i = 0; i < nin; i++) {
        Wedge *w = ins[i];
        if (w->kind == K_HOST && (!link || w->near < link->near)) link = w;
    }
    for (int i = 0; i < nmb; i++) {
        Wedge *w = maybe[i];
        if (w->kind == K_HOST) {
            if (!link || w->near <= link->far) out->sure = 0;
        } else {
            out->sure = 0;
        }
    }
    double cover = 0, front = 0;
    int infront = 0;
    for (int i = 0; i < nin; i++) {
        Wedge *w = ins[i];
        if (w->kind != K_HOST) {
            cover = cover + w->shots;
            if (link) {
                if (w->far < link->near) {
                    front = front + w->shots;
                    if (!e->link_clear_front) infront = 1;
                } else if (w->near <= link->far) out->sure = 0;
            } else {
                front = front + w->shots;
            }
        }
    }
    if (link && e->link_clear_front && link_cut(e, front)) infront = 1;
    out->cover = cover; out->front = front;
    if (link) { out->link = link->pill; out->used = !infront; }
}

/* ── Exact rays ─────────────────────────────────────────────────────── */
static Ray *base_ray(Ev *e, int X, double px, double py) {
    double k = (py + 4096) * 131072 + (px + 4096);
    HVal *hv = hm_find(&e->job->rays, (uint64_t)X, dbits(k));
    if (hv) { e->ray_reuse += 1; return (Ray *)hv->p; }
    Pill *P = PI(e, X);
    double sx = P->mx * 256 + 128, sy = P->my * 256 + 128;
    Ray *r = cast(e, &e->job->ar, sx, sy, px, py, e->pill_len, NULL, 1, NULL);
    r->X = X;
    int isnew;
    hv = hm_put(e->L, &e->job->rays, (uint64_t)X, dbits(k), &isnew);
    hv->p = r;
    e->casts += 1;
    return r;
}

static Ray *ray_view(Ev *e, Ray *b, const Extra *extra) {
    Ray *v = NEW(e, Ray);
    memset(v, 0, sizeof(*v));
    v->X = b->X; v->angle = b->angle; v->tank = b->tank; v->t = b->t; v->link = b->link; v->link_t = b->link_t;
    v->sq = b->sq; v->sq_t = b->sq_t; v->n_sq = b->n_sq; v->end_x = b->end_x; v->end_y = b->end_y;
    v->cover = b->cover; v->front = b->front; v->blk = b->blk; v->n_block = b->n_block;
    v->src = b; v->own_blk = 0;
    if (extra && extra->n > 0) {
        int any = 0;
        const int64_t *sql = b->sq;
        for (int k = 0; k < b->n_sq; k++) if (extra_find(extra, sql[k]) >= 0) { any = 1; break; }
        if (any) {
            Blk **blk = NEWN(e, Blk *, b->n_block + b->n_sq);
            int n = 0, bi = 0;
            double cover = 0;
            for (int k = 0; k < b->n_sq; k++) {
                int64_t s = sql[k]; double t = b->sq_t[k];
                while (bi < b->n_block && b->blk[bi]->t <= t) {
                    Blk *be = b->blk[bi];
                    if (be->kind == BK_ALLY_PILL || !(be->sq == s && extra_find(extra, s) >= 0)) {
                        blk[n++] = be; cover = cover + be->shots;
                    }
                    bi++;
                }
                int xi = extra_find(extra, s);
                if (xi >= 0) {
                    Blk *nb = NEW(e, Blk);
                    memset(nb, 0, sizeof(*nb));
                    nb->sq = s; nb->shots = extra->shots[xi]; nb->kind = BK_PLANNED; nb->t = t; nb->pill = 0;
                    blk[n++] = nb; cover = cover + extra->shots[xi];
                }
            }
            while (bi < b->n_block) {
                Blk *be = b->blk[bi];
                blk[n++] = be; cover = cover + be->shots;
                bi++;
            }
            double front = 0;
            if (!b->link) front = cover;
            if (b->link) {
                front = 0;
                for (int i = 0; i < n; i++) {
                    Blk *be = blk[i];
                    if (be->t < b->link_t || (be->t == b->link_t && be->kind == BK_ALLY_PILL)) front = front + be->shots;
                }
            }
            v->blk = blk; v->n_block = n; v->cover = cover; v->front = front; v->own_blk = 1;
        }
    }
    int cut = 0;
    if (v->link) {
        for (int i = 0; i < v->n_block; i++) if (v->blk[i]->t < v->link_t) { cut = 1; break; }
    }
    v->cut_blk = cut;
    v->cut_front = v->link != 0 && v->front > 0;
    v->cut = cut;
    return v;
}

/* ── One tank point ─────────────────────────────────────────────────── */
static int A_in_range(Ev *e, double px, double py) {
    double pr = e->pill_range + e->range_margin;
    PList *h = e->hostile;
    for (int i = 0; i < h->n; i++) {
        if (h->v[i] == e->A) {
            Pill *P = PI(e, e->A);
            double cx = P->mx * 256 + 128, cy = P->my * 256 + 128;
            double dx = cx - px, dy = cy - py;
            return dx * dx + dy * dy <= pr * pr;
        }
    }
    return 0;
}

static PList *pills_in_range(Ev *e, double px, double py) {
    double pr = e->pill_range + e->range_margin;
    double pr2 = pr * pr;
    PList *T = plist_new(e, 8);
    for (int i = 0; i < e->hostile->n; i++) {
        int p = e->hostile->v[i];
        Pill *P = PI(e, p);
        double cx = P->mx * 256 + 128, cy = P->my * 256 + 128;
        double dx = cx - px, dy = cy - py;
        if (dx * dx + dy * dy <= pr2) plist_push(e, T, p);
    }
    return T;
}

static int nudge_aim(Ev *e, double px, double py, const Extra *extra, const int *shorts, int nshorts,
                     double *onx, double *ony, double *onw, int *oai, Lane **oln) {
    Pill *A = PI(e, e->A);
    double acx = A->mx * 256 + 128, acy = A->my * 256 + 128;
    double dx = acx - px, dy = acy - py;
    double d = sqrt(dx * dx + dy * dy);
    if (d < 1) return 0;
    int live[PT2_MAXAIMS];
    int nlive = nshorts;
    for (int i = 0; i < nshorts; i++) live[i] = shorts[i];
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
                A = PI(e, e->A);
                int why = tank_shot(e, nx, ny, tx + s_aims9[ai - 1][0], ty + s_aims9[ai - 1][1], A->mx, A->my,
                                    extra, &ln);
                e->n_shot += 1;
                if (why == TS_OK) { *onx = nx; *ony = ny; *onw = nw; *oai = ai; *oln = ln; return 1; }
                if (why == TS_SHORT) live[n++] = ai;
            }
            nlive = n;
            if (n == 0) return 0;
        }
    }
    return 0;
}

static Rec *rec_new(Ev *e) {
    Rec *r = NEW(e, Rec);
    memset(r, 0, sizeof(*r));
    return r;
}

/* ri[X]: the 1-based place of X in T, or 0. */
static int ri_of(const Rec *r, int X) {
    for (int i = 0; i < r->T->n; i++) if (r->T->v[i] == X) return i + 1;
    return 0;
}

static Rec *eval_point(Ev *e, double px, double py, Extra *extra, int aim_fixed, int nudge, int exact,
                       int lazy, int *why, int *why_arg) {
    *why = W_NONE; *why_arg = 0;
    int hasA = A_in_range(e, px, py);
    if (!hasA) { *why = W_A_OUT; return NULL; }
    set_om(e, floor(px / 256), floor(py / 256));
    int aim_idx = 0;
    Lane *lane = NULL;
    int a1 = 1, a2 = e->naims;
    if (aim_fixed) { a1 = aim_fixed; a2 = aim_fixed; }
    int why_out = TS_SHORT;
    int shorts[PT2_MAXAIMS]; int nshorts = 0;
    for (int ai = a1; ai <= a2; ai++) {
        Pill *A = PI(e, e->A);
        Lane *ln = NULL;
        int w = tank_shot(e, px, py, A->mx * 256 + s_aims9[ai - 1][0], A->my * 256 + s_aims9[ai - 1][1],
                          A->mx, A->my, extra, &ln);
        e->n_shot += 1;
        if (w == TS_OK) { aim_idx = ai; lane = ln; break; }
        why_out = w;
        if (w == TS_SHORT) shorts[nshorts++] = ai;
    }
    double nudge_wu = 0;
    if (!aim_idx && nudge && nshorts > 0 && e->aim_nudge && !aim_fixed) {
        double nx, ny, nw; int nai; Lane *nln;
        if (nudge_aim(e, px, py, extra, shorts, nshorts, &nx, &ny, &nw, &nai, &nln)) {
            if (A_in_range(e, nx, ny)) {
                px = nx; py = ny; nudge_wu = nw; aim_idx = nai; lane = nln;
                e->nudged += 1;
            }
            set_om(e, floor(px / 256), floor(py / 256));
        }
    }
    if (!aim_idx) { *why = W_OUTGOING; *why_arg = why_out; return NULL; }
    PList *T = pills_in_range(e, px, py);
    int A = e->A;
    if (lazy) {
        double bar = e->finish ? 0 : e->bar_single;
        if (e->co) {
            Pred pr;
            predict(e, centre_wedges(e, A), px, py, &pr);
            int Y = pr.link;
            if (pr.sure && !(Y && pr.used && Y != A && !chain_skip(e, Y))) {
                e->must_cut += 1;
                Rec *s = rec_new(e);
                s->flags = RF_LAZY;
                s->px = px; s->py = py; s->nudge_wu = nudge_wu; s->T = T; s->aim_idx = aim_idx;
                s->chain = plist_new(e, 1); plist_push(e, s->chain, A); s->kills = 1;
                s->a_bar = bar;
                return s;
            }
        }
        Ray *vA = ray_view(e, base_ray(e, A, px, py), extra);
        if (e->link_clear_front) vA->cut = vA->link != 0 && link_cut(e, vA->front);
        int nxt = (e->chains && e->max_chain >= 2 && !vA->cut) ? vA->link : 0;
        if (!nxt || nxt == A || chain_skip(e, nxt)) {
            e->lazy_n += 1;
            Rec *s = rec_new(e);
            s->flags = RF_LAZY | RF_ANEED;
            s->px = px; s->py = py; s->nudge_wu = nudge_wu; s->T = T; s->aim_idx = aim_idx;
            s->chain = plist_new(e, 1); plist_push(e, s->chain, A); s->kills = 1;
            s->a_need = bar - vA->cover;
            return s;
        }
    }
    RayList *rays = NEW(e, RayList);
    memset(rays, 0, sizeof(*rays));
    rays->n = T->n;
    rays->v = NEWN(e, Ray *, T->n);
    for (int i = 0; i < T->n; i++) {
        int X = T->v[i];
        Ray *v;
        if (exact) {
            Pill *P = PI(e, X);
            double sx = P->mx * 256 + 128, sy = P->my * 256 + 128;
            Ray *r = cast(e, &e->ar, sx, sy, px, py, e->pill_len, extra, 1, NULL);
            r->X = X;
            e->casts_confirm += 1;
            v = ray_view(e, r, NULL);
        } else {
            v = ray_view(e, base_ray(e, X, px, py), extra);
        }
        if (e->link_clear_front) v->cut = v->link != 0 && link_cut(e, v->front);
        rays->v[i] = v;
    }
    Rec *out = rec_new(e);
    out->T = T; out->rays = rays;
    PList *chain = plist_new(e, 4);
    plist_push(e, chain, A);
    int chain_mode = 0;
    if (e->chains) {
        int cur = A;
        while (chain->n < e->max_chain) {
            int i = ri_of(out, cur);
            Ray *r = i ? rays->v[i - 1] : NULL;
            int nxt = (r && !r->cut) ? r->link : 0;
            if (!nxt || in_chain(chain, nxt) || chain_skip(e, nxt)) break;
            plist_push(e, chain, nxt);
            chain_mode = 1;
            cur = nxt;
        }
    }
    int covered_all = 1;
    IList *unc = NEW(e, IList);
    memset(unc, 0, sizeof(*unc));
    unc->v = NEWN(e, int, T->n);
    double need_A = 0, have_A = 0;
    int ally_cover = 0;
    for (int i = 0; i < T->n; i++) {
        int X = T->v[i];
        Ray *r = rays->v[i];
        double bar;
        int ci = in_chain(chain, X);
        if (X == A && e->finish) bar = 0;
        else if (X == A && !chain_mode) bar = e->bar_single;
        else if (ci) bar = e->bar_chain;
        else bar = e->bar_other;
        r->bar = bar;
        r->has_bar = 1;
        r->link_cover = e->link_covers && ci && !r->cut && r->link != 0 && ci < chain->n
                        && r->link == chain->v[ci];
        int ok;
        if (ci) ok = r->link_cover || r->cover >= bar;
        else ok = !r->tank || r->cover >= bar;
        r->ok = ok;
        if (X == A) { need_A = bar; have_A = r->cover; }
        if (!ok) { covered_all = 0; unc->v[unc->n++] = i + 1; }
        if (ok && (r->tank || ci)) {
            for (int k = 0; k < r->n_block; k++) if (r->blk[k]->kind == BK_ALLY_PILL) ally_cover = 1;
        }
    }
    int last = chain->v[chain->n - 1];
    int li = ri_of(out, last);
    double depth = li ? rays->v[li - 1]->cover : 0;
    out->px = px; out->py = py; out->nudge_wu = nudge_wu;
    out->chain = chain; out->chain_mode = chain_mode; out->aim_idx = aim_idx; out->lane = lane;
    Aims *ca = NEW(e, Aims); memset(ca, 0, sizeof(*ca));
    ca->n = 1; ca->v[0] = aim_idx;
    Lanes *ls = NEW(e, Lanes); memset(ls, 0, sizeof(*ls));
    ls->n = 1; ls->ai[0] = aim_idx; ls->ln[0] = lane;
    out->clear = ca; out->lanes = ls;
    out->aims_done = aim_fixed ? aim_fixed : aim_idx;
    if (extra) { out->extra_in = extra; out->flags |= RF_EXTRA_IN; }
    out->covered = covered_all; out->uncovered = unc;
    out->safe = covered_all; out->max_ok = 1;
    out->kills = chain_mode ? chain->n : 1;
    out->ally_cover = ally_cover; out->need = need_A; out->have = have_A; out->depth = depth;
    out->finish = e->finish;
    return out;
}

/* Every clear aim of a record (clear_aims_of). */
static Aims *clear_aims_of(Ev *e, Rec *rec) {
    if (rec->aims_done < e->naims) {
        set_om(e, floor(rec->px / 256), floor(rec->py / 256));
        for (int ai = (int)rec->aims_done + 1; ai <= e->naims; ai++) {
            Pill *A = PI(e, e->A);
            Lane *ln = NULL;
            int w = tank_shot(e, rec->px, rec->py, A->mx * 256 + s_aims9[ai - 1][0],
                              A->my * 256 + s_aims9[ai - 1][1], A->mx, A->my,
                              (rec->flags & RF_EXTRA_IN) ? rec->extra_in : NULL, &ln);
            e->n_shot += 1;
            if (w == TS_OK) {
                rec->clear->v[rec->clear->n++] = ai;
                rec->lanes->ai[rec->lanes->n] = ai; rec->lanes->ln[rec->lanes->n] = ln; rec->lanes->n++;
            }
        }
        rec->aims_done = e->naims;
    }
    return rec->clear;
}

/* sq % 256 and floor(sq / 256), as the Lua takes them */
static double sq_mx(int64_t sq) { return luamod((double)sq, 256); }
static double sq_my(int64_t sq) { return floor((double)sq / 256); }

/* ── Wall search ────────────────────────────────────────────────────── */
/* The square's own facts (ctx.sqf, the job's table): read once per job.
 * The Lua cache table (inp._v2_cache.sqf) gets each new fact too, so a
 * Lua run on the same job (job_done's extra_cover) reads it there. */
static int sqf_get(Ev *e, int64_t sq, double mx, double my) {
    lua_State *L = e->L;
    HVal *hv = hm_find(&e->job->sqf, (uint64_t)sq, 0);
    if (hv) return (int)hv->i;
    int f = SF_NIL;
    int lt = lua_istable(L, S_SQF);
    if (lt) {
        lua_pushnumber(L, (double)sq);
        lua_rawget(L, S_SQF);
        if (lua_type(L, -1) == LUA_TSTRING) {
            const char *v = lua_tostring(L, -1);
            if (!strcmp(v, "area")) f = SF_AREA;
            else if (!strcmp(v, "pill")) f = SF_PILL;
            else if (!strcmp(v, "wall")) f = SF_WALL;
            else f = SF_NOBUILD;
        } else if (lua_toboolean(L, -1)) f = SF_TRUE;
        lua_pop(L, 1);
    }
    if (f == SF_NIL) {
        if (!in_map(e, mx, my) || !stay_in(e, mx, my)) f = SF_AREA;
        else if (pill_on(e, mx, my)) f = SF_PILL;
        else {
            double tt = ttype(e, mx, my);
            if (tt == e->T_BUILDING || tt == e->T_HALFBUILD) f = SF_WALL;
            else if (!buildable(e, mx, my)) f = SF_NOBUILD;
            else f = SF_TRUE;
        }
        if (lt) {
            lua_pushnumber(L, (double)sq);
            if (f == SF_TRUE) lua_pushboolean(L, 1); else lua_pushstring(L, s_sf_name[f]);
            lua_rawset(L, S_SQF);
        }
    }
    int isnew;
    hv = hm_put(L, &e->job->sqf, (uint64_t)sq, 0, &isnew);
    hv->i = f;
    return f;
}

static int lane_has(const Lane *l, int64_t s) {
    if (!l) return 0;
    for (int i = 0; i < l->n; i++) if (l->v[i] == s) return 1;
    return 0;
}

static int list_has(const int64_t *v, int n, int64_t s) {
    for (int i = 0; i < n; i++) if (v[i] == s) return 1;
    return 0;
}

/* slot_ok: may a planned blocker go on square k (0-based) of ray r? */
static int slot_ok(Ev *e, Rec *rec, Ray *r, int k, const Lane *lane, const int64_t *forbid, int nf) {
    lua_State *L = e->L;
    int64_t sq = r->sq[k];
    double mx = sq_mx(sq), my = sq_my(sq);
    int X = r->X;
    if (lane_has(lane, sq)) return 0;
    if (list_has(forbid, nf, sq)) return 0;
    if (r->link && r->sq_t[k] <= r->link_t) return 0;
    if (e->orth_only && X == e->A) {
        Pill *P = PI(e, X);
        if (fabs(mx - P->mx) + fabs(my - P->my) != 1) return 0;
    }
    int f = sqf_get(e, sq, mx, my);
    if (f == SF_AREA) return 0;
    double cx = mx * 256 + 128, cy = my * 256 + 128;
    double dxp = cx - rec->px, dyp = cy - rec->py;
    double bd = e->blocker_min_dist * 256;
    if (dxp * dxp + dyp * dyp < bd * bd) return 0;
    Pill *P = PI(e, X);
    double sx = P->mx * 256 + 128, sy = P->my * 256 + 128;
    double d1 = (cx - sx) * (cx - sx) + (cy - sy) * (cy - sy);
    double d2 = (rec->px - sx) * (rec->px - sx) + (rec->py - sy) * (rec->py - sy);
    if (d1 >= d2) return 0;
    if (f != SF_TRUE) return 0;
    int om = (rec->flags & RF_OMXY) != 0;
    double rk = ((om ? rec->omx : 0) * 256 + (om ? rec->omy : 0)) * 65536 + (double)sq;
    HVal *hv = hm_find(&e->reach, dbits(rk), 0);
    int rc;
    if (hv) rc = (int)hv->i;
    else {
        lua_pushvalue(L, S_LGM_REACH);
        if (om) { lua_pushnumber(L, rec->omx); lua_pushnumber(L, rec->omy); }
        else { lua_pushnil(L); lua_pushnil(L); }
        lua_pushnumber(L, mx);
        lua_pushnumber(L, my);
        lua_call(L, 4, 1);
        rc = lua_toboolean(L, -1);
        lua_pop(L, 1);
        int isnew;
        hv = hm_put(L, &e->reach, dbits(rk), 0, &isnew);
        hv->i = rc;
        e->reach_calls += 1;
    }
    return rc;
}

/* trip: the builder's shortest trip from (ox, oy) through n squares. Every
 * order is tried in the Lua's order (the lists for 1 .. 3, then the swap
 * walk of perms); the first strict minimum wins. */
static const int s_perm3[6][3] = { {1,2,3}, {1,3,2}, {2,1,3}, {2,3,1}, {3,1,2}, {3,2,1} };
typedef struct { int n; double ox, oy, mx[WS_MAXK], my[WS_MAXK], best; int border[WS_MAXK]; int a[WS_MAXK + 1]; } TripCx;
static void trip_try(TripCx *t, const int *pm) {
    double x = t->ox, y = t->oy, len = 0;
    for (int i = 0; i < t->n; i++) {
        int q = pm[i] - 1;
        double mx = t->mx[q], my = t->my[q];
        len = len + sqrt((mx - x) * (mx - x) + (my - y) * (my - y));
        x = mx; y = my;
    }
    if (len < t->best) { t->best = len; for (int i = 0; i < t->n; i++) t->border[i] = pm[i]; }
}
static void trip_gen(TripCx *t, int k) {
    if (k > t->n) { trip_try(t, t->a + 1); return; }
    for (int i = k; i <= t->n; i++) {
        int s = t->a[k]; t->a[k] = t->a[i]; t->a[i] = s;
        trip_gen(t, k + 1);
        s = t->a[k]; t->a[k] = t->a[i]; t->a[i] = s;
    }
}
static double trip(double ox, double oy, const int64_t *sqs, int n, int *order) {
    TripCx t;
    t.n = n; t.ox = ox; t.oy = oy; t.best = HUGE_VAL;
    for (int i = 0; i < n; i++) { t.mx[i] = sq_mx(sqs[i]); t.my[i] = sq_my(sqs[i]); }
    if (n == 1) { int p[1] = { 1 }; trip_try(&t, p); }
    else if (n == 2) { int p1[2] = { 1, 2 }, p2[2] = { 2, 1 }; trip_try(&t, p1); trip_try(&t, p2); }
    else if (n == 3) { for (int i = 0; i < 6; i++) trip_try(&t, s_perm3[i]); }
    else if (n == 0) { t.best = 0; }
    else { for (int i = 1; i <= n; i++) t.a[i] = i; trip_gen(&t, 1); }
    for (int i = 0; i < n; i++) order[i] = t.border[i];
    return t.best;
}

/* One uncovered path of the search. */
typedef struct {
    Ray   *r;
    double need;
    UBase *ub;
    int    nc;
    int64_t *cand;
} UPath;

static int ub_on(const UBase *u, int64_t s, double *v) {
    for (int i = 0; i < u->n_on; i++) if (u->on_sq[i] == s) { *v = u->on_v[i]; return 1; }
    return 0;
}

static void sols_push(Ev *e, SolList *l, Sol *s) {
    if (l->n >= l->cap) {
        int nc = l->cap ? l->cap * 2 : 16;
        Sol **nv = NEWN(e, Sol *, nc);
        if (l->n) memcpy(nv, l->v, sizeof(Sol *) * (size_t)l->n);
        l->v = nv; l->cap = nc;
    }
    l->v[l->n++] = s;
}

/* The search state (WS). */
typedef struct {
    Ev *e;
    int nU, nC;
    double P, W, nodes;
    int64_t *C;
    uint32_t *mk;
    double *onv;          /* [j * nC + i]; NAN: not on that path */
    int **pc; int *npc;
    double *rem;
    uint8_t *used;
    int64_t pick[WS_MAXK];
    uint8_t kind[WS_MAXK];
    SolList *sols;
    UPath *U;
    int nch;
} WSE;

static void sol_add(WSE *S, int n, uint32_t m) {
    Sol *so = NEW(S->e, Sol);
    memset(so, 0, sizeof(*so));
    so->n = n;
    for (int i = 0; i < n; i++) { so->sq[i] = S->pick[i]; so->pill[i] = S->kind[i]; }
    so->mask = m;
    sols_push(S->e, S->sols, so);
}

static void ws_enum(WSE *S, int pl, int wl, uint32_t m, int depth) {
    S->nodes = S->nodes + 1;
    int nU = S->nU, nC = S->nC;
    int j = -1;
    for (int jj = 0; jj < nU; jj++) if (S->rem[jj] > 0) { j = jj; break; }
    if (j < 0) {
        if (pl == 0 && wl == 0) sol_add(S, depth, m);
        return;
    }
    if (pl + wl == 0) return;
    double P = S->P, W = S->W;
    for (int jj = j; jj < nU; jj++) {
        double need = S->rem[jj];
        if (need > 0) {
            int fr = 0;
            for (int q = 0; q < S->npc[jj]; q++) if (!S->used[S->pc[jj][q]]) fr++;
            int np = fr < pl ? fr : pl;
            int nw = fr - np;
            if (nw > wl) nw = wl;
            if (np * P + nw * W < need) return;
        }
    }
    int *pc = S->pc[j];
    int npc = S->npc[j];
    int outl[256];
    int n_out = 0;
    for (int q = 0; q < npc; q++) {
        int i = pc[q];
        if (S->used[i]) continue;
        S->used[i] = 1;
        outl[n_out++] = i;
        uint32_t m2 = m & S->mk[i];
        if (m2 != 0) {
            int64_t s = S->C[i];
            for (int pass = 1; pass <= 2; pass++) {
                int kind; double shots;
                if (pass == 1 && pl > 0) { kind = 1; shots = P; }
                else if (pass == 2 && wl > 0) { kind = 0; shots = W; }
                else continue;
                S->pick[depth] = s; S->kind[depth] = (uint8_t)kind;
                for (int jj = 0; jj < nU; jj++) {
                    double ev = S->onv[jj * nC + i];
                    if (ev == ev) S->rem[jj] = S->rem[jj] - (shots - ev);
                }
                ws_enum(S, kind ? pl - 1 : pl, kind ? wl : wl - 1, m2, depth + 1);
                for (int jj = 0; jj < nU; jj++) {
                    double ev = S->onv[jj * nC + i];
                    if (ev == ev) S->rem[jj] = S->rem[jj] + (shots - ev);
                }
            }
        }
    }
    for (int q = 0; q < n_out; q++) S->used[outl[q]] = 0;
}

static int i64_cmp(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}
static int int_cmp(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

/* the key of a set: sq * 2 + (pill and 1 or 0), sorted, joined by "," */
static void sol_key(const int64_t *sq, const uint8_t *pill, int n, char *out, size_t sz) {
    int64_t ks[WS_MAXK];
    for (int i = 0; i < n; i++) ks[i] = sq[i] * 2 + (pill[i] ? 1 : 0);
    qsort(ks, (size_t)n, sizeof(int64_t), i64_cmp);
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        char t[32];
        fmt_num(t, sizeof(t), (double)ks[i]);
        size_t tl = strlen(t);
        if (o + tl + 2 >= sz) break;
        if (i > 0) out[o++] = ',';
        memcpy(out + o, t, tl); o += tl;
        out[o] = 0;
    }
}

static uint64_t str_hash(const char *s) {
    uint64_t h = 1469598103934665603ull;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ull; }
    return h;
}

static uint32_t mask_of(Ev *e, int64_t s) {
    HVal *hv = hm_find(&e->wsmask, (uint64_t)s, 0);
    return hv ? (uint32_t)hv->i : 0;
}

/* the slow search (a blocker that adds no cover on a path) */
static void ws_dfs(WSE *S, int pl, int wl, uint32_t m) {
    Ev *e = S->e;
    S->nodes = S->nodes + 1;
    int nU = S->nU;
    UPath *U = S->U;
    int j = -1;
    for (int jj = 0; jj < nU; jj++) if (S->rem[jj] > 0) { j = jj; break; }
    if (j < 0) {
        if (pl == 0 && wl == 0) {
            char key[80];
            sol_key(S->pick, S->kind, S->nch, key, sizeof(key));
            uint64_t h = str_hash(key);
            int isnew;
            HVal *hv = hm_put(e->L, &e->wsseen, h, 0, &isnew);
            SolList *bl = isnew ? NULL : (SolList *)hv->p;
            int seen = 0;
            if (bl) for (int i = 0; i < bl->n; i++) if (!strcmp(bl->v[i]->key, key)) { seen = 1; break; }
            if (!seen) {
                if (!bl) {
                    bl = NEW(e, SolList); memset(bl, 0, sizeof(*bl));
                    hv->p = bl;
                }
                sol_add(S, S->nch, m);
                Sol *so = S->sols->v[S->sols->n - 1];
                memcpy(so->key, key, sizeof(key));
                sols_push(e, bl, so);
            }
        }
        return;
    }
    if (pl + wl == 0) return;
    for (int jj = 0; jj < nU; jj++) {
        double need = S->rem[jj];
        if (need > 0) {
            int fr = 0;
            for (int q = 0; q < U[jj].nc; q++)
                if (!list_has(S->pick, S->nch, U[jj].cand[q])) fr++;
            int np = fr < pl ? fr : pl;
            int nw = fr - np < wl ? fr - np : wl;
            if (np * e->pill_drop_shots + nw * e->wall_shots < need) return;
        }
    }
    for (int q = 0; q < U[j].nc; q++) {
        int64_t s = U[j].cand[q];
        uint32_t m2 = m & mask_of(e, s);
        if (m2 == 0 || list_has(S->pick, S->nch, s)) continue;
        for (int pass = 1; pass <= 2; pass++) {
            int kind; double shots;
            if (pass == 1 && pl > 0) { kind = 1; shots = e->pill_drop_shots; }
            else if (pass == 2 && wl > 0) { kind = 0; shots = e->wall_shots; }
            else continue;
            S->pick[S->nch] = s; S->kind[S->nch] = (uint8_t)kind; S->nch++;
            for (int jj = 0; jj < nU; jj++) {
                double ev;
                if (ub_on(U[jj].ub, s, &ev)) S->rem[jj] = S->rem[jj] - (shots - ev);
            }
            ws_dfs(S, kind ? pl - 1 : pl, kind ? wl : wl - 1, m2);
            for (int jj = 0; jj < nU; jj++) {
                double ev;
                if (ub_on(U[jj].ub, s, &ev)) S->rem[jj] = S->rem[jj] + (shots - ev);
            }
            S->nch--;
        }
    }
}

/* ws_take: the solutions of size k go to each aim still without one */
static uint32_t ws_take(Ev *e, WSOut *found, uint32_t active, int nA, SolList *sols, double k) {
    uint32_t got = 0;
    for (int i = 1; i <= nA; i++) {
        uint32_t bi = 1u << (i - 1);
        if (active & bi) {
            AimOut *a = &found->a[i - 1];
            int any = 0;
            for (int q = 0; q < sols->n; q++) {
                if (sols->v[q]->mask & bi) {
                    if (!any) { memset(&a->sols, 0, sizeof(a->sols)); any = 1; }
                    sols_push(e, &a->sols, sols->v[q]);
                }
            }
            if (any) { a->has = 1; a->k = k; got |= bi; }
        }
    }
    return active & ~got;
}

/* The last chain pill's ray (rec.rays[rec.ri[last]]) or NULL. */
static Ray *last_ray(Rec *rec) {
    int last = rec->chain->v[rec->chain->n - 1];
    int i = ri_of(rec, last);
    return i ? rec->rays->v[i - 1] : NULL;
}

/* ex[sq]: the shots of the last non ally-pill block on sq, else 0 */
static double ray_ex(const Ray *r, int64_t sq) {
    double v = 0;
    for (int b = 0; b < r->n_block; b++) {
        const Blk *be = r->blk[b];
        if (be->kind != BK_ALLY_PILL && be->sq == sq) v = be->shots;
    }
    return v;
}

static void ws_keys(Ev *e, Sol *so, Rec *rec, UPath *U, int nU, Ray *lr) {
    if (so->keys) return;
    double depth = lr ? lr->cover : 0;
    int two = 0;
    for (int i = 0; i < so->n; i++) {
        double shots = so->pill[i] ? e->pill_drop_shots : e->wall_shots;
        if (lr) {
            int on = 0;
            for (int k = 0; k < lr->n_sq; k++) if (lr->sq[k] == so->sq[i]) { on = 1; break; }
            if (on) depth = depth + shots - ray_ex(lr, so->sq[i]);
        }
        int n = 0;
        double ev;
        for (int j = 0; j < nU; j++) if (ub_on(U[j].ub, so->sq[i], &ev)) n++;
        if (n >= 2) two = 1;
    }
    int om = (rec->flags & RF_OMXY) != 0;
    so->trip = trip(om ? rec->omx : floor(rec->px / 256), om ? rec->omy : floor(rec->py / 256),
                    so->sq, so->n, so->order);
    sol_key(so->sq, so->pill, so->n, so->key, sizeof(so->key));
    so->depth = depth; so->two = two; so->keys = 1;
}

static WSOut *ws_run(Ev *e, Rec *rec, const Aims *aims, UPath *U, int nU) {
    WSOut *out = NEW(e, WSOut);
    memset(out, 0, sizeof(*out));
    int nA = aims->n;
    out->nA = nA;
    double npb = e->num_pill_blockers;
    double P = e->pill_drop_shots, W = e->wall_shots;
    double kcap = e->build_max;
    if ((rec->flags & RF_KMAX) && rec->kmax < kcap) kcap = rec->kmax;
    if (kcap < 1) return out;
    double bp = dmin(npb, kcap);
    double bw = kcap - bp;
    int slow = P < W;
    for (int j = 0; j < nU; j++) {
        double fr = U[j].nc;
        double np = dmin(fr, bp);
        double nw = dmin(fr - np, bw);
        if (np * P + nw * W < U[j].need) return out;
        for (int q = 0; q < U[j].nc; q++) {
            double ev = 0;
            ub_on(U[j].ub, U[j].cand[q], &ev);
            if (ev >= W) slow = 1;
        }
    }
    uint32_t active = (1u << nA) - 1;
    int any = 0;
    WSE S;
    memset(&S, 0, sizeof(S));
    S.e = e; S.nU = nU; S.P = P; S.W = W; S.U = U;
    S.rem = NEWN(e, double, nU > 0 ? nU : 1);
    if (slow) {
        e->wall_slow += 1;
        for (int j = 0; j < nU; j++) S.rem[j] = U[j].need;
        S.nodes = 0;
        for (double k = 1; k <= kcap; k++) {
            SolList *sols = NEW(e, SolList); memset(sols, 0, sizeof(*sols));
            S.sols = sols;
            hm_clear(&e->wsseen);
            double p = dmin(npb, k);
            S.nch = 0;
            ws_dfs(&S, (int)p, (int)(k - p), active);
            if (sols->n > 0) active = ws_take(e, out, active, nA, sols, k);
            for (int i = 0; i < nA; i++) if (out->a[i].has) any = 1;
            if (any) break;
        }
        e->wall_nodes = e->wall_nodes + S.nodes;
    } else {
        int tot = 0;
        for (int j = 0; j < nU; j++) tot += U[j].nc;
        int64_t *Cs = NEWN(e, int64_t, tot > 0 ? tot : 1);
        int nC = 0;
        for (int j = 0; j < nU; j++)
            for (int q = 0; q < U[j].nc; q++)
                if (!list_has(Cs, nC, U[j].cand[q])) Cs[nC++] = U[j].cand[q];
        qsort(Cs, (size_t)nC, sizeof(int64_t), i64_cmp);
        S.C = Cs; S.nC = nC;
        S.mk = NEWN(e, uint32_t, nC > 0 ? nC : 1);
        for (int i = 0; i < nC; i++) S.mk[i] = mask_of(e, Cs[i]);
        S.onv = NEWN(e, double, nU * nC > 0 ? nU * nC : 1);
        S.pc = NEWN(e, int *, nU > 0 ? nU : 1);
        S.npc = NEWN(e, int, nU > 0 ? nU : 1);
        S.used = NEWN(e, uint8_t, nC > 0 ? nC : 1);
        memset(S.used, 0, (size_t)(nC > 0 ? nC : 1));
        for (int j = 0; j < nU; j++) {
            for (int i = 0; i < nC; i++) {
                double ev;
                S.onv[j * nC + i] = ub_on(U[j].ub, Cs[i], &ev) ? ev : NAN;
            }
            if (U[j].nc > 256) luaL_error(e->L, "gh_pt2_v2: a path has over 256 squares");
            S.pc[j] = NEWN(e, int, U[j].nc > 0 ? U[j].nc : 1);
            S.npc[j] = U[j].nc;
            for (int q = 0; q < U[j].nc; q++) {
                int64_t s = U[j].cand[q];
                int lo = 0, hi = nC - 1, at = 0;
                while (lo <= hi) {
                    int mid = (lo + hi) / 2;
                    if (Cs[mid] == s) { at = mid; break; }
                    if (Cs[mid] < s) lo = mid + 1; else hi = mid - 1;
                }
                S.pc[j][q] = at;
            }
            qsort(S.pc[j], (size_t)U[j].nc, sizeof(int), int_cmp);
            S.rem[j] = U[j].need;
        }
        S.nodes = 0;
        for (double k = 1; k <= kcap; k++) {
            SolList *sols = NEW(e, SolList); memset(sols, 0, sizeof(*sols));
            S.sols = sols;
            double p = dmin(npb, k);
            ws_enum(&S, (int)p, (int)(k - p), active, 0);
            if (sols->n > 0) active = ws_take(e, out, active, nA, sols, k);
            for (int i = 0; i < nA; i++) if (out->a[i].has) any = 1;
            if (any) break;
        }
        e->wall_nodes = e->wall_nodes + S.nodes;
    }
    if (!any) return out;
    Ray *lr = last_ray(rec);
    for (int i = 0; i < nA; i++) {
        AimOut *f = &out->a[i];
        if (!f->has) continue;
        Sol *best = NULL;
        int bi = 0;
        for (int q = 0; q < f->sols.n; q++) {
            Sol *so = f->sols.v[q];
            ws_keys(e, so, rec, U, nU, lr);
            int b;
            if (!best) b = 1;
            else if (so->depth != best->depth) b = so->depth > best->depth;
            else if (so->two != best->two) b = so->two;
            else if (fabs(so->trip - best->trip) > 1e-9) b = so->trip < best->trip;
            else b = strcmp(so->key, best->key) < 0;
            if (b) { best = so; bi = q; }
        }
        f->best = bi;
    }
    return out;
}

/* ws_input_key (S4b): every input the search and its pick read. */
static void ws_input_key(Ev *e, Rec *rec, const Aims *aims, UPath *U, int nU, SBuf *b) {
    for (int i = 0; i < aims->n; i++) { if (i) sb_str(b, ","); sb_num(b, aims->v[i]); }
    sb_str(b, "|"); sb_num(b, e->num_pill_blockers);
    sb_str(b, "|"); sb_num(b, e->build_max);
    sb_str(b, "|"); sb_num(b, e->pill_drop_shots);
    sb_str(b, "|"); sb_num(b, e->wall_shots);
    sb_str(b, "|"); if (rec->flags & RF_KMAX) sb_num(b, rec->kmax); else sb_str(b, "nil");
    int om = (rec->flags & RF_OMXY) != 0;
    sb_str(b, "|"); sb_num(b, om ? rec->omx : floor(rec->px / 256));
    sb_str(b, "|"); sb_num(b, om ? rec->omy : floor(rec->py / 256));
    for (int j = 0; j < nU; j++) {
        sb_str(b, "|"); sb_num(b, U[j].need);
        for (int q = 0; q < U[j].nc; q++) {
            int64_t s = U[j].cand[q];
            double ev = 0;
            ub_on(U[j].ub, s, &ev);
            sb_str(b, " "); sb_num(b, (double)s); sb_str(b, ":"); sb_num(b, ev);
            sb_str(b, ":"); sb_num(b, (double)mask_of(e, s));
        }
    }
    Ray *lr = last_ray(rec);
    if (lr) {
        sb_str(b, "|L "); sb_num(b, lr->cover);
        for (int k = 0; k < lr->n_sq; k++) {
            int64_t q = lr->sq[k];
            int cs = 0;
            for (int j = 0; j < nU && !cs; j++) cs = list_has(U[j].cand, U[j].nc, q);
            if (cs) {
                sb_str(b, " "); sb_num(b, (double)q); sb_str(b, ":"); sb_num(b, ray_ex(lr, q));
            }
        }
    }
}

/* The memo entry of a search: one userdata, the WSOut flat. */
static void wm_store(Ev *e, WSOut *o, double slow, double nodes) {
    lua_State *L = e->L;
    int nsol = 0;
    for (int i = 0; i < o->nA; i++) if (o->a[i].has) nsol += o->a[i].sols.n;
    size_t sz = sizeof(WMBlob) + sizeof(WMAim) * (size_t)o->nA + sizeof(Sol) * (size_t)nsol;
    WMBlob *b = (WMBlob *)lua_newuserdata(L, sz);
    b->slow = slow; b->nodes = nodes; b->nA = o->nA; b->nsol = nsol;
    WMAim *wa = (WMAim *)(b + 1);
    Sol *ws = (Sol *)(wa + o->nA);
    int q = 0;
    for (int i = 0; i < o->nA; i++) {
        AimOut *a = &o->a[i];
        wa[i].has = a->has; wa[i].k = a->k; wa[i].best = a->best;
        wa[i].nsol = a->has ? a->sols.n : 0;
        for (int s = 0; s < wa[i].nsol; s++) ws[q++] = *a->sols.v[s];
    }
}

static WSOut *wm_load(Ev *e, const WMBlob *b) {
    WSOut *out = NEW(e, WSOut);
    memset(out, 0, sizeof(*out));
    out->nA = b->nA;
    const WMAim *wa = (const WMAim *)(b + 1);
    const Sol *ws = (const Sol *)(wa + b->nA);
    int q = 0;
    for (int i = 0; i < b->nA; i++) {
        AimOut *a = &out->a[i];
        a->has = wa[i].has; a->k = wa[i].k; a->best = wa[i].best;
        for (int s = 0; s < wa[i].nsol; s++) {
            Sol *so = NEW(e, Sol);
            *so = ws[q++];
            sols_push(e, &a->sols, so);
        }
    }
    return out;
}

static double wm_getn(lua_State *L, int t, const char *k) {
    lua_getfield(L, t, k);
    double v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}
static void wm_setn(lua_State *L, int t, const char *k, double v) {
    lua_pushnumber(L, v);
    lua_setfield(L, t, k);
}

static WSOut *search_walls_all(Ev *e, Rec *rec, const Aims *aims) {
    lua_State *L = e->L;
    int nA = aims->n;
    if (nA == 0) { WSOut *o = NEW(e, WSOut); memset(o, 0, sizeof(*o)); return o; }
    hm_clear(&e->wslane);
    for (int i = 0; i < nA; i++) {
        uint32_t bi = 1u << i;
        Lane *l = NULL;
        for (int q = 0; q < rec->lanes->n; q++) if (rec->lanes->ai[q] == aims->v[i]) { l = rec->lanes->ln[q]; break; }
        if (!l) continue;
        for (int k = 0; k < l->n; k++) {
            int isnew;
            HVal *hv = hm_put(L, &e->wslane, (uint64_t)l->v[k], 0, &isnew);
            hv->i = isnew ? (int64_t)bi : (int64_t)((uint32_t)hv->i | bi);
        }
    }
    uint32_t all_aims = (1u << nA) - 1;
    WallBase *base = rec->wb;
    if (!base) {
        base = NEW(e, WallBase);
        memset(base, 0, sizeof(*base));
        int cap = 1;
        for (int i = 0; i < rec->T->n; i++) cap += rec->rays->v[i]->n_sq;
        base->forbid = NEWN(e, int64_t, cap);
        for (int i = 0; i < rec->T->n; i++) {
            int X = rec->T->v[i];
            Ray *r = rec->rays->v[i];
            if (in_chain(rec->chain, X) && r->link && !r->cut && in_chain(rec->chain, r->link)) {
                for (int k = 0; k < r->n_sq; k++)
                    if (r->sq_t[k] <= r->link_t && !list_has(base->forbid, base->nf, r->sq[k]))
                        base->forbid[base->nf++] = r->sq[k];
            }
        }
        base->nU = rec->uncovered->n;
        base->U = NEWN(e, UBase, base->nU > 0 ? base->nU : 1);
        for (int u = 0; u < base->nU; u++) {
            Ray *r = rec->rays->v[rec->uncovered->v[u] - 1];
            UBase *ub = &base->U[u];
            ub->r = r;
            ub->need = r->bar - r->cover;
            ub->n_on = r->n_sq;
            ub->on_sq = NEWN(e, int64_t, r->n_sq > 0 ? r->n_sq : 1);
            ub->on_v = NEWN(e, double, r->n_sq > 0 ? r->n_sq : 1);
            for (int k = 0; k < r->n_sq; k++) {
                ub->on_sq[k] = r->sq[k];
                ub->on_v[k] = ray_ex(r, r->sq[k]);
            }
        }
        rec->wb = base;
    }
    int nU = base->nU;
    UPath *U = NEWN(e, UPath, nU > 0 ? nU : 1);
    for (int j = 0; j < nU; j++) {
        U[j].r = base->U[j].r; U[j].need = base->U[j].need; U[j].ub = &base->U[j];
        U[j].nc = 0; U[j].cand = NEWN(e, int64_t, base->U[j].r->n_sq > 0 ? base->U[j].r->n_sq : 1);
    }
    SlotMemo *memo = rec->sm;
    if (!memo) {
        memo = NEW(e, SlotMemo); memset(memo, 0, sizeof(*memo));
        rec->sm = memo;
    }
    hm_clear(&e->wsmask);
    for (int j = 0; j < nU; j++) {
        Ray *r = U[j].r;
        for (int k = 0; k < r->n_sq; k++) {
            int64_t s = r->sq[k];
            HVal *hv = hm_find(&e->wsmask, (uint64_t)s, 0);
            uint32_t m;
            if (hv) m = (uint32_t)hv->i;
            else {
                HVal *lv = hm_find(&e->wslane, (uint64_t)s, 0);
                m = lv ? (all_aims & ~(uint32_t)lv->i) : all_aims;
                if (m != 0) {
                    int at = -1;
                    for (int q = 0; q < memo->n; q++) if (memo->sq[q] == s) { at = q; break; }
                    int l;
                    if (at >= 0) l = memo->v[at];
                    else {
                        l = 0;
                        for (int j2 = 0; j2 < nU && !l; j2++) {
                            double ev;
                            if (ub_on(U[j2].ub, s, &ev)) {
                                Ray *r2 = U[j2].r;
                                for (int k2 = 0; k2 < r2->n_sq; k2++) {
                                    if (r2->sq[k2] == s) {
                                        if (slot_ok(e, rec, r2, k2, NULL, base->forbid, base->nf)) l = 1;
                                        break;
                                    }
                                }
                            }
                        }
                        if (memo->n >= memo->cap) {
                            int nc = memo->cap ? memo->cap * 2 : 32;
                            int64_t *ns = NEWN(e, int64_t, nc);
                            uint8_t *nv = NEWN(e, uint8_t, nc);
                            if (memo->n) { memcpy(ns, memo->sq, sizeof(int64_t) * (size_t)memo->n); memcpy(nv, memo->v, (size_t)memo->n); }
                            memo->sq = ns; memo->v = nv; memo->cap = nc;
                        }
                        memo->sq[memo->n] = s; memo->v[memo->n] = (uint8_t)l; memo->n++;
                    }
                    if (!l) m = 0;
                }
                int isnew;
                hv = hm_put(L, &e->wsmask, (uint64_t)s, 0, &isnew);
                hv->i = (int64_t)m;
            }
            if (m != 0) U[j].cand[U[j].nc++] = s;
        }
    }
    int wm = e->wall_memo;
    int has_wkey = lua_toboolean(L, S_WKEY);
    if (!wm && !has_wkey) return ws_run(e, rec, aims, U, nU);
    SBuf kb;
    sb_init(&kb, L);
    ws_input_key(e, rec, aims, U, nU, &kb);
    if (has_wkey) {
        lua_pushvalue(L, S_WKEY);
        lua_pushlstring(L, kb.p ? kb.p : "", kb.n);
        lua_call(L, 1, 0);
    }
    if (!wm) { sb_free(&kb); return ws_run(e, rec, aims, U, nU); }
    /* PT2_WALL_MEMO: the C entries sit in the goal's table (inp._wm_t)
     * beside the Lua's, under "C|" .. key */
    int t0 = lua_gettop(L);
    lua_pushstring(L, "C|");
    lua_pushlstring(L, kb.p ? kb.p : "", kb.n);
    lua_concat(L, 2);
    int ki = lua_gettop(L);
    sb_free(&kb);
    lua_getfield(L, S_INP, "_wm_t");
    int mt = lua_gettop(L);
    int has_mt = lua_istable(L, mt);
    int iswm = lua_istable(L, S_WM);
    if (has_mt) {
        lua_pushvalue(L, ki);
        lua_rawget(L, mt);
        if (lua_type(L, -1) == LUA_TUSERDATA) {
            WMBlob *b = (WMBlob *)lua_touserdata(L, -1);
            if (iswm) {
                wm_setn(L, S_WM, "hits", wm_getn(L, S_WM, "hits") + 1);
                wm_setn(L, S_WM, "hit_nodes", wm_getn(L, S_WM, "hit_nodes") + b->nodes);
            }
            e->wall_slow = e->wall_slow + b->slow;
            e->wall_nodes = e->wall_nodes + b->nodes;
            e->memo_nodes = e->memo_nodes + b->nodes;
            WSOut *o = wm_load(e, b);
            lua_settop(L, t0);
            return o;
        }
        lua_pop(L, 1);
    }
    if (iswm) wm_setn(L, S_WM, "misses", wm_getn(L, S_WM, "misses") + 1);
    double s0 = e->wall_slow, n0 = e->wall_nodes;
    WSOut *out = ws_run(e, rec, aims, U, nU);
    if (!has_mt) { lua_settop(L, t0); return out; }
    if (iswm && wm_getn(L, S_WM, "n") >= e->wall_memo_max) {
        lua_newtable(L);
        lua_replace(L, mt);
        lua_newtable(L);
        lua_getfield(L, S_INP, "_wm_goal");
        lua_pushvalue(L, mt);
        lua_settable(L, -3);
        lua_setfield(L, S_WM, "by");
        wm_setn(L, S_WM, "n", 0);
        wm_setn(L, S_WM, "full", wm_getn(L, S_WM, "full") + 1);
        lua_pushvalue(L, mt);
        lua_setfield(L, S_INP, "_wm_t");
    }
    lua_pushvalue(L, ki);
    wm_store(e, out, e->wall_slow - s0, e->wall_nodes - n0);
    lua_rawset(L, mt);
    if (iswm) {
        wm_setn(L, S_WM, "miss_nodes", wm_getn(L, S_WM, "miss_nodes") + e->wall_nodes - n0);
        wm_setn(L, S_WM, "n", wm_getn(L, S_WM, "n") + 1);
    }
    lua_settop(L, t0);
    return out;
}

/* ── Start check (pt2_start.lua) ────────────────────────────────────── */
#define PS_SHELL_ARMOUR  5
#define PS_HEAT_MAX_HITS 4

static double ps_pill_interval(Ev *e, double anger) {
    int on = e->rise;
    double bump = e->anger_bump;
    double hits = 0; int at_min = 0;
    if (!(bump <= 0)) {
        double a = anger;
        if (on) {
            hits = ceil(a / bump - 1e-6);
            if (hits < 0) hits = 0;
            if (hits >= 3) { hits = PS_HEAT_MAX_HITS; at_min = 1; }
        } else {
            hits = floor(a / bump + 0.5);
            if (hits < 0) hits = 0; else if (hits > PS_HEAT_MAX_HITS) hits = PS_HEAT_MAX_HITS;
        }
    }
    double r = e->pill_attack_ticks, lo = e->pill_attack_min_ticks;
    if (at_min) r = lo;
    else if (on) {
        double div = e->pill_angry_divisor;
        if (div < 1) div = 1;
        for (double i = 1; i <= hits; i++) {
            if (r > lo) { r = floor(r / div); if (r < lo) r = lo; }
        }
    } else {
        for (double i = 1; i <= hits; i++) r = floor(r / 2);
    }
    if (r < lo) r = lo;
    if (r < 1) r = 1;
    return r;
}

/* pill_speed: inp.pill_interval(X), else from the pill's anger */
static double pill_speed(Ev *e, int X) {
    lua_State *L = e->L;
    if (lua_toboolean(L, S_PILL_INTERVAL)) {
        lua_pushvalue(L, S_PILL_INTERVAL);
        push_pill(e, X);
        lua_call(L, 1, 1);
        int has = lua_toboolean(L, -1);
        double iv = lua_tonumber(L, -1);
        lua_pop(L, 1);
        if (has) return iv;
    }
    double anger = 0;
    int got = 0;
    if (lua_toboolean(L, S_ANGER_OF)) {
        lua_pushvalue(L, S_ANGER_OF);
        push_pill(e, X);
        lua_call(L, 1, 1);
        if (lua_toboolean(L, -1)) { anger = lua_tonumber(L, -1); got = 1; }
        lua_pop(L, 1);
    }
    if (!got) {
        push_pill(e, X);
        lua_getfield(L, -1, "anger");
        anger = lua_isnil(L, -1) ? 0 : lua_tonumber(L, -1);
        lua_pop(L, 2);
    }
    return ps_pill_interval(e, anger);
}

/* ctx.lgm_ticks (with its memo); has = 0: no answer (nil) */
static double lgm_ticks(Ev *e, double ox, double oy, double mx, double my, double bx, double by, int *has) {
    lua_State *L = e->L;
    double k = ((ox * 256 + oy) * 256 + mx) * 256 + my;
    k = (k * 256 + bx) * 256 + by;
    HVal *hv = hm_find(&e->lgt, dbits(k), 0);
    double v;
    if (hv) v = hv->d;
    else {
        lua_pushvalue(L, S_LGM_TICKS);
        lua_pushnumber(L, ox); lua_pushnumber(L, oy);
        lua_pushnumber(L, mx); lua_pushnumber(L, my);
        lua_pushnumber(L, bx); lua_pushnumber(L, by);
        lua_call(L, 6, 1);
        if (lua_type(L, -1) == LUA_TNUMBER) v = lua_tonumber(L, -1);
        else v = NAN;      /* nil or false */
        lua_pop(L, 1);
        int isnew;
        hv = hm_put(L, &e->lgt, dbits(k), 0, &isnew);
        hv->d = v;
    }
    *has = v == v;
    return v;
}

/* PS.trip_ticks; returns 0 for nil */
static int trip_ticks(Ev *e, double ox, double oy, double mx, double my, int has_b, double bmx, double bmy,
                      double *out) {
    if (lua_toboolean(e->L, S_LGM_TICKS)) {
        int has;
        double t = lgm_ticks(e, ox, oy, mx, my, has_b ? bmx : mx, has_b ? bmy : my, &has);
        if (has) {
            if (t < 0) return 0;
            *out = t;
            return 1;
        }
    }
    double dx = mx - ox, dy = my - oy;
    *out = sqrt(dx * dx + dy * dy) * LGM_TICKS_PER_SQ;
    return 1;
}

/* PS.wall_ticks over the plan; NULL for nil */
static double *wall_ticks(Ev *e, double ox, double oy, const Plan *walls) {
    double tx = ox, ty = oy;
    double t = 0;
    double *out = NEWN(e, double, walls->n);
    for (int i = 0; i < walls->n; i++) {
        const PlanE *w = &walls->v[i];
        if (i == 0) {
            if (!trip_ticks(e, ox, oy, w->mx, w->my, 0, 0, 0, &t)) return NULL;
        } else {
            const PlanE *p = &walls->v[i - 1];
            double back, go;
            if (!trip_ticks(e, p->mx, p->my, tx, ty, 1, p->mx, p->my, &back)) return NULL;
            if (!trip_ticks(e, tx, ty, w->mx, w->my, 0, 0, 0, &go)) return NULL;
            t = t + LGM_BUILD_TIME + back + go;
        }
        out[i] = t;
    }
    return out;
}

static double shells_through(double exposure, double interval, double cover_now) {
    if (exposure <= 0) return 0;
    double v = ceil(exposure / interval) - cover_now;
    return v > 0 ? v : 0;
}

typedef struct { double t, shots; } CovE;
typedef struct { double speed; int has_to; int to; int nc; CovE *cov; int count; } RisePill;

static double rise_shells(Ev *e, RisePill *ps, int n, double exposure, double flight) {
    double attack = e->pill_attack_ticks, lo = e->pill_attack_min_ticks;
    double div = e->pill_angry_divisor; if (div < 1) div = 1;
    double cool_t = e->pill_cooldown_ticks;
    double reload = e->tank_reload; if (reload < 1) reload = 1;
    double *speed = NEWN(e, double, n), *cnt = NEWN(e, double, n), *cool = NEWN(e, double, n);
    double *used = NEWN(e, double, n), *lh = NEWN(e, double, n);
    for (int i = 0; i < n; i++) {
        speed[i] = ps[i].speed; cnt[i] = ps[i].speed;
        cool[i] = ps[i].speed < attack ? cool_t : 0;
        used[i] = 0; lh[i] = 0;
    }
    double total = 0;
    for (double t = 0; t <= exposure - 1; t++) {
        for (int i = 0; i < n; i++) {
            double h = lh[i];
            lh[i] = 0;
            if (i == 0 && t >= flight && luamod(t - flight, reload) == 0) h = h + 1;
            for (double q = 1; q <= h; q++) {
                if (speed[i] > lo) { speed[i] = floor(speed[i] / div); if (speed[i] < lo) speed[i] = lo; }
                cool[i] = cool_t;
            }
        }
        for (int i = 0; i < n; i++) {
            if (cnt[i] < speed[i]) cnt[i] = cnt[i] + 1;
            if (cool[i] > 0) {
                cool[i] = cool[i] - 1;
                if (cool[i] == 0) {
                    speed[i] = speed[i] + 1;
                    if (speed[i] < attack) cool[i] = cool_t;
                }
            }
            if (cnt[i] >= speed[i]) {
                cnt[i] = 0;
                double avail = -used[i];
                for (int c = 0; c < ps[i].nc; c++) if (ps[i].cov[c].t <= t) avail = avail + ps[i].cov[c].shots;
                if (avail > 0) used[i] = used[i] + 1;
                else if (ps[i].has_to) lh[ps[i].to - 1] = lh[ps[i].to - 1] + 1;
                else if (ps[i].count) total = total + 1;
            }
        }
    }
    return total;
}

static double rise_check(Ev *e, Rec *rec, double ox, double oy, double exp) {
    PList *chain = rec->chain;
    int n = chain->n;
    double *ts = wall_ticks(e, ox, oy, rec->plan);
    RisePill *ps = NEWN(e, RisePill, n);
    StartLink *keep = NEWN(e, StartLink, n);
    for (int k = 1; k <= n; k++) {
        int X = chain->v[k - 1];
        Pill *P = PI(e, X);
        RisePill *rp = &ps[k - 1];
        StartLink *kp = &keep[k - 1];
        memset(rp, 0, sizeof(*rp)); memset(kp, 0, sizeof(*kp));
        rp->speed = pill_speed(e, X);
        kp->mx = P->mx; kp->my = P->my; kp->np = -1;
        double cover_now = 0;
        PlanSq *pl = NULL; int np = -1;
        if (k < n) { rp->has_to = 1; rp->to = k + 1; kp->has_to = 1; kp->to = k + 1; }
        else {
            Ray *r = rec->rays->v[ri_of(rec, X) - 1];
            pl = NEWN(e, PlanSq, r->n_block);
            for (int j = 0; j < r->n_block; j++) {
                Blk *b = r->blk[j];
                if (b->kind == BK_PLANNED) {
                    if (np < 0) np = 0;
                    pl[np].mx = sq_mx(b->sq); pl[np].my = sq_my(b->sq); pl[np].shots = b->shots; np++;
                } else cover_now = cover_now + b->shots;
            }
            rp->count = np >= 0;
            kp->has_count = 1; kp->count = rp->count;
        }
        kp->cover_now = cover_now; kp->np = np; kp->pl = pl;
        int nc = (cover_now > 0 ? 1 : 0) + (np > 0 ? np : 0);
        rp->cov = NEWN(e, CovE, nc);
        rp->nc = 0;
        if (cover_now > 0) { rp->cov[0].t = 0; rp->cov[0].shots = cover_now; rp->nc = 1; }
        for (int q = 0; q < np; q++) {
            double t = HUGE_VAL;
            if (ts) {
                for (int i = 0; i < rec->plan->n; i++) {
                    PlanE *w = &rec->plan->v[i];
                    /* wt[my * 256 + mx] = ts[i]: the last plan entry on a square wins */
                    if (w->my * 256 + w->mx == pl[q].my * 256 + pl[q].mx) t = ts[i];
                }
            }
            rp->cov[rp->nc].t = t; rp->cov[rp->nc].shots = pl[q].shots; rp->nc++;
        }
    }
    Pill *A = PI(e, chain->v[0]);
    double fdx = A->mx * 256 + 128 - rec->px, fdy = A->my * 256 + 128 - rec->py;
    double flight = ceil(sqrt(fdx * fdx + fdy * fdy) / e->env_speed);
    rec->n_sc = n; rec->sc = keep; rec->has_sc = 1;
    return rise_shells(e, ps, n, exp, flight);
}

static void start_check(Ev *e, Rec *rec) {
    rec->flags |= RF_START;
    rec->start_now = 1; rec->exposure = 0; rec->shells_through = 0; rec->has_sp = 0; rec->start_why = 0;
    rec->has_sc = 0;
    Plan *plan = rec->plan;
    if (!plan || plan->n == 0) return;
    int om = (rec->flags & RF_OMXY) != 0;
    double ox = om ? rec->omx : floor(rec->px / 256);
    double oy = om ? rec->omy : floor(rec->py / 256);
    double *ts = wall_ticks(e, ox, oy, plan);
    if (!ts) { rec->start_now = 0; rec->exposure = -1; rec->start_why = 3; return; }
    double exp = ts[plan->n - 1];
    double shells = 0;
    StartPill *pills = NEWN(e, StartPill, rec->T->n);
    int npl = 0;
    for (int i = 0; i < rec->T->n; i++) {
        int X = rec->T->v[i];
        Ray *r = rec->rays->v[i];
        PlanSq *pl = NEWN(e, PlanSq, r->n_block);
        int np = 0; double now_sh = 0;
        for (int k = 0; k < r->n_block; k++) {
            Blk *b = r->blk[k];
            if (b->kind == BK_PLANNED) { pl[np].mx = sq_mx(b->sq); pl[np].my = sq_my(b->sq); pl[np].shots = b->shots; np++; }
            else now_sh = now_sh + b->shots;
        }
        if (np > 0) {
            int ci = in_chain(rec->chain, X);
            if (!ci) {
                if (r->tank && now_sh < (r->has_bar ? r->bar : 0)) {
                    rec->start_now = 0; rec->exposure = exp; rec->start_why = 1;
                    return;
                }
            } else if (!r->link_cover) {
                shells = shells + shells_through(exp, pill_speed(e, X), now_sh);
                StartPill *sp = &pills[npl++];
                sp->mx = PI(e, X)->mx; sp->my = PI(e, X)->my; sp->cover_now = now_sh; sp->np = np; sp->pl = pl;
            }
        }
    }
    if (npl > 0 && e->rise) shells = rise_check(e, rec, ox, oy, exp);
    rec->exposure = exp; rec->shells_through = shells;
    rec->sp = pills; rec->n_sp = npl; rec->has_sp = 1;
    rec->start_now = e->armour - shells * PS_SHELL_ARMOUR >= e->finish_min_armour;
    if (!rec->start_now) rec->start_why = 2;
}

/* ── Extra cover walls ──────────────────────────────────────────────── */
typedef struct { int64_t sq; double mx, my, d; } XCand;
static int xc_lt(void *cx, void *a, void *b) {
    (void)cx;
    const XCand *x = (const XCand *)a, *y = (const XCand *)b;
    if (x->d != y->d) return x->d < y->d;
    return x->sq < y->sq;
}

static void extra_cover(Ev *e, Rec *rec) {
    rec->flags |= RF_XCOVER;
    rec->n_xc = 0; rec->xc = NULL;
    double n = dmin(e->extra_cover_walls, e->build_max - (rec->plan ? rec->plan->n : 0));
    if (n <= 0 || !rec->chain) return;
    Ray *r = last_ray(rec);
    if (!r) return;
    Lane *lane = NULL;
    for (int q = 0; q < rec->lanes->n; q++) if (rec->lanes->ai[q] == rec->aim_idx) { lane = rec->lanes->ln[q]; break; }
    int cap = 1;
    for (int i = 0; i < rec->T->n; i++) cap += rec->rays->v[i]->n_sq;
    int64_t *forbid = NEWN(e, int64_t, cap); int nf = 0;
    for (int i = 0; i < rec->T->n; i++) {
        int X = rec->T->v[i];
        Ray *ry = rec->rays->v[i];
        if (in_chain(rec->chain, X) && ry->link && !ry->cut && in_chain(rec->chain, ry->link)) {
            for (int k = 0; k < ry->n_sq; k++)
                if (ry->sq_t[k] <= ry->link_t && !list_has(forbid, nf, ry->sq[k])) forbid[nf++] = ry->sq[k];
        }
    }
    int om = (rec->flags & RF_OMXY) != 0;
    double ox = om ? rec->omx : floor(rec->px / 256);
    double oy = om ? rec->omy : floor(rec->py / 256);
    XCand *cs = NEWN(e, XCand, r->n_sq);
    void **cp = NEWN(e, void *, r->n_sq + 1);
    int nc = 0;
    for (int k = 0; k < r->n_sq; k++) {
        int64_t q = r->sq[k];
        int used = 0;
        if (rec->plan) for (int i = 0; i < rec->plan->n; i++) if (rec->plan->v[i].sq == q) { used = 1; break; }
        if (!used && slot_ok(e, rec, r, k, lane, forbid, nf)) {
            XCand *c = &cs[nc];
            c->sq = q; c->mx = sq_mx(q); c->my = sq_my(q);
            c->d = (c->mx - ox) * (c->mx - ox) + (c->my - oy) * (c->my - oy);
            cp[++nc] = c;
        }
    }
    if (nc == 0) return;
    lsort(e->L, cp, nc, xc_lt, NULL);
    int m = (int)dmin(n, nc);
    rec->xc = NEWN(e, XWall, m);
    for (int i = 0; i < m; i++) {
        XCand *c = (XCand *)cp[i + 1];
        rec->xc[i].sq = c->sq; rec->xc[i].mx = c->mx; rec->xc[i].my = c->my; rec->xc[i].shots = e->wall_shots;
    }
    rec->n_xc = m;
}

/* ── Plans ──────────────────────────────────────────────────────────── */
static int same_chain(const PList *a, const PList *b) {
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++) if (a->v[i] != b->v[i]) return 0;
    return 1;
}

static Rec *confirm_plan(Ev *e, Rec *rec, int ai, const PlanE *items, int n) {
    Extra *extra = extra_new(e);
    Plan *plan = NEW(e, Plan); memset(plan, 0, sizeof(*plan));
    plan->v = NEWN(e, PlanE, n); plan->cap = n;
    for (int i = 0; i < n; i++) {
        extra_set(e, extra, items[i].sq, items[i].shots);
        PlanE *p = &plan->v[plan->n++];
        *p = items[i];
        p->mx = sq_mx(items[i].sq); p->my = sq_my(items[i].sq);
    }
    int why, wa;
    Rec *f = eval_point(e, rec->px, rec->py, extra, ai, 0, 0, 0, &why, &wa);
    if (f && f->covered && same_chain(f->chain, rec->chain)) {
        f->nudge_wu = rec->nudge_wu;
        f->plan = plan; f->extra = extra; f->flags |= RF_PLAN;
        if (rec->flags & RF_OMXY) { f->omx = rec->omx; f->omy = rec->omy; f->flags |= RF_OMXY; }
        else f->flags &= ~RF_OMXY;
        f->clear = rec->clear; f->lanes = rec->lanes;
        return f;
    }
    return NULL;
}

static void plan_empty(Ev *e, Rec *rec) {
    rec->plan = NEW(e, Plan); memset(rec->plan, 0, sizeof(*rec->plan));
    rec->extra = extra_new(e);
    rec->flags |= RF_PLAN;
}

/* the items of solution so in its trip order */
static PlanE *sol_items(Ev *e, const Sol *so) {
    PlanE *it = NEWN(e, PlanE, so->n);
    for (int m = 0; m < so->n; m++) {
        int oi = so->order[m] - 1;
        it[m].sq = so->sq[oi];
        it[m].kind_pill = so->pill[oi];
        it[m].shots = so->pill[oi] ? e->pill_drop_shots : e->wall_shots;
        it[m].mx = sq_mx(so->sq[oi]); it[m].my = sq_my(so->sq[oi]);
    }
    return it;
}

/* PT2_AIM_AVOID_CHAIN_NEIGHBOUR (Nov 24, Andrew's yes): nb_set, aim_beside
 * and aim_nb as the Lua. The squares of a record's chain members: every
 * chain pill but A, and every blocker on a chain pill's ray. n = 0: none. */
typedef struct { int n; int64_t *sq; } NbSet;
static NbSet nb_set(Ev *e, const Rec *rec) {
    NbSet o; o.n = 0; o.sq = NULL;
    if (!rec->chain) return o;
    Pill *A = PI(e, e->A);
    int64_t a_sq = (int64_t)(A->my * 256 + A->mx);
    int cap = 0;
    for (int c = 0; c < rec->chain->n; c++) {
        cap += 1;
        int i = rec->rays ? ri_of(rec, rec->chain->v[c]) : 0;
        if (i) cap += rec->rays->v[i - 1]->n_block;
    }
    o.sq = NEWN(e, int64_t, cap + 1);
    for (int c = 0; c < rec->chain->n; c++) {
        int X = rec->chain->v[c];
        if (X != e->A) { Pill *P = PI(e, X); o.sq[o.n++] = (int64_t)(P->my * 256 + P->mx); }
        int i = rec->rays ? ri_of(rec, X) : 0;
        if (i) {
            Ray *r = rec->rays->v[i - 1];
            for (int k = 0; k < r->n_block; k++) if (r->blk[k]->sq != a_sq) o.sq[o.n++] = r->blk[k]->sq;
        }
    }
    return o;
}
static int nb_has(const NbSet *s, int64_t q) {
    for (int i = 0; i < s->n; i++) if (s->sq[i] == q) return 1;
    return 0;
}
static int aim_beside(Ev *e, int ai, const NbSet *s) {
    if (!s || s->n == 0) return 0;
    double ox = s_aims9[ai - 1][0], oy = s_aims9[ai - 1][1];
    int dx = ox < 128 ? -1 : (ox > 128 ? 1 : 0);
    int dy = oy < 128 ? -1 : (oy > 128 ? 1 : 0);
    Pill *A = PI(e, e->A);
    double ax = A->mx, ay = A->my;
    if (dx && nb_has(s, (int64_t)(ay * 256 + ax + dx))) return 1;
    if (dy && nb_has(s, (int64_t)((ay + dy) * 256 + ax))) return 1;
    if (dx && dy && nb_has(s, (int64_t)((ay + dy) * 256 + ax + dx))) return 1;
    return 0;
}
static void aim_nb(Ev *e, Rec *fin) {
    if (!e->aim_nb_on) return;
    NbSet s = nb_set(e, fin);
    int nb = aim_beside(e, fin->aim_idx, &s);
    if (nb && fin->covered && fin->plan->n == 0) {
        Aims *aims = clear_aims_of(e, fin);
        for (int i = 0; i < aims->n; i++) {
            int ai = aims->v[i];
            if (!aim_beside(e, ai, &s)) {
                fin->aim_idx = ai;
                for (int j = 0; j < fin->lanes->n; j++)
                    if (fin->lanes->ai[j] == ai) { fin->lane = fin->lanes->ln[j]; break; }
                nb = 0;
                e->aimnb_moved += 1;
                break;
            }
        }
    }
    fin->aimnb = nb;
    fin->flags |= RF_AIMNB;
}

typedef struct { Sol *so; int ai; int n; int nb; } S6Pair;
static int s6_lt(void *cx, void *pa, void *pb) {
    (void)cx;
    const S6Pair *x = (const S6Pair *)pa, *y = (const S6Pair *)pb;
    const Sol *a = x->so, *b = y->so;
    if (x->nb != y->nb) return !x->nb;
    if (a->depth != b->depth) return a->depth > b->depth;
    if (a->two != b->two) return a->two;
    if (fabs(a->trip - b->trip) > 1e-9) return a->trip < b->trip;
    if (x->n != y->n) return x->n < y->n;
    return strcmp(a->key, b->key) < 0;
}

static Rec *plan_point(Ev *e, Rec *rec) {
    if (rec->covered) { plan_empty(e, rec); return rec; }
    Aims *aims = clear_aims_of(e, rec);
    WSOut *all = search_walls_all(e, rec, aims);
    /* PT2_AIM_AVOID_CHAIN_NEIGHBOUR: an aim beside a chain member last */
    NbSet nbs; nbs.n = 0; nbs.sq = NULL;
    if (e->aim_nb_on) nbs = nb_set(e, rec);
    int bi = -1, best_nb = 0;
    for (int i = 0; i < aims->n && i < all->nA; i++) {
        AimOut *s = &all->a[i];
        if (!s->has) continue;
        int nb = aim_beside(e, aims->v[i], &nbs);
        int b = 0;
        if (bi < 0) b = 1;
        else {
            AimOut *bo = &all->a[bi];
            Sol *ss = s->sols.v[s->best], *bs = bo->sols.v[bo->best];
            if (nb != best_nb) b = !nb;
            else if (s->k != bo->k) b = s->k < bo->k;
            else if (ss->depth != bs->depth) b = ss->depth > bs->depth;
            else if (ss->two != bs->two) b = ss->two;
            else if (fabs(ss->trip - bs->trip) > 1e-9) b = ss->trip < bs->trip;
        }
        if (b) { bi = i; best_nb = nb; }
    }
    e->planned += 1;
    if (bi >= 0) {
        AimOut *bo = &all->a[bi];
        Sol *bs = bo->sols.v[bo->best];
        Rec *f = confirm_plan(e, rec, aims->v[bi], sol_items(e, bs), bs->n);
        if (f) return f;
        e->confirm_fail += 1;
        int tot = 0;
        for (int i = 0; i < aims->n && i < all->nA; i++)
            if (all->a[i].has && all->a[i].k == bo->k) tot += all->a[i].sols.n;
        S6Pair *pr = NEWN(e, S6Pair, tot);
        void **pp = NEWN(e, void *, tot + 1);
        int np = 0;
        for (int i = 0; i < aims->n && i < all->nA; i++) {
            AimOut *a = &all->a[i];
            if (a->has && a->k == bo->k) {
                int nb = aim_beside(e, aims->v[i], &nbs);
                for (int q = 0; q < a->sols.n; q++) {
                    pr[np].so = a->sols.v[q]; pr[np].ai = aims->v[i]; pr[np].n = i + 1; pr[np].nb = nb;
                    pp[np + 1] = &pr[np];
                    np++;
                }
            }
        }
        lsort(e->L, pp, np, s6_lt, NULL);
        int tries = 1;
        for (int i = 2; i <= np; i++) {
            if (tries >= S6_TRIES) break;
            S6Pair *q = (S6Pair *)pp[i];
            tries++;
            e->confirm_retry += 1;
            f = confirm_plan(e, rec, q->ai, sol_items(e, q->so), q->so->n);
            if (f) { e->confirm_rescued += 1; return f; }
        }
    }
    plan_empty(e, rec);
    return rec;
}

/* ── Leeway ─────────────────────────────────────────────────────────── */
static void nb_init(void) {
    if (s_nb_ready) return;
    for (int k = 0; k <= 7; k++) {
        double a = k * PT2_PI / 4;
        NB[k][0] = cos(a); NB[k][1] = sin(a);
    }
    s_nb_ready = 1;
}

static double leeway(Ev *e, Rec *rec, double ring_wu, LeePt **out) {
    double n = 0;
    LeePt *res = NEWN(e, LeePt, 8);
    for (int k = 0; k < 8; k++) {
        double qx = floor(rec->px + NB[k][0] * ring_wu + 0.5);
        double qy = floor(rec->py + NB[k][1] * ring_wu + 0.5);
        int why, wa;
        Rec *q = eval_point(e, qx, qy, (rec->flags & RF_PLAN) ? rec->extra : NULL, 0, 0, 0, 0, &why, &wa);
        int ok = q && q->covered && q->max_ok && q->chain->n == rec->chain->n;
        if (ok) n = n + 1;
        if (!ok && q) {
            why = W_NONE; wa = 0;
            if (q->uncovered->n > 0) { why = W_OVERLAP; wa = q->rays->v[q->uncovered->v[0] - 1]->X; }
            if (why == W_NONE && q->chain->n != rec->chain->n) why = W_CHAIN;
        }
        res[k].x = qx; res[k].y = qy; res[k].ok = ok;
        if (ok) { res[k].why = W_NONE; res[k].why_arg = 0; }
        else if (why == W_NONE) { res[k].why = W_QMARK; res[k].why_arg = 0; }
        else { res[k].why = why; res[k].why_arg = wa; }
    }
    *out = res;
    return n;
}

/* ── Score and rank ─────────────────────────────────────────────────── */
static void score_of(Ev *e, Rec *fin) {
    fin->n_built = fin->plan->n;
    fin->blk = (e->own_pill_rank && fin->ally_cover) ? 2 : 1;
    fin->score = fin->base + fin->n_built * e->build_cost;
    fin->flags |= RF_SCORED;
    aim_nb(e, fin);
    if (e->swb && fin->safe) start_check(e, fin);
}

static void carry(Rec *fin, const Rec *rec) {
    fin->deg = rec->deg; fin->cx = rec->cx; fin->cy = rec->cy; fin->mx = rec->mx; fin->my = rec->my;
    fin->radius = rec->radius;
    fin->base = rec->base;
    fin->flags |= RF_POS | RF_BASE;
}

/* OLD.better(a, b, cap, band) */
static int old_better(Ev *e, const Rec *a, const Rec *b) {
    if (a->safe != b->safe) return a->safe;
    if (a->kills != b->kills) return a->kills > b->kills;
    if (e->has_band && e->band > 0) {
        double sa = a->score, sb = b->score;
        double ta = (a->flags & RF_BAND) ? a->tier_min : sa;
        double tb = (b->flags & RF_BAND) ? b->tier_min : sb;
        double m = sa;
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

/* M.better(a, b, cap, band, pec, swb, no_s0) */
static int better(Ev *e, const Rec *a, const Rec *b, int no_s0) {
    if (a->safe == b->safe && a->kills == b->kills) {
        int na = (a->flags & RF_AIMNB) ? a->aimnb : 0, nb = (b->flags & RF_AIMNB) ? b->aimnb : 0;
        if (na != nb) return nb;
    }
    if ((e->pec || e->swb) && e->has_band && e->band > 0 && a->safe == b->safe && a->kills == b->kills) {
        double sa = a->score, sb = b->score;
        double m = sa;
        if (sb < m) m = sb;
        double ta = (a->flags & RF_BAND) ? a->tier_min : sa;
        double tb = (b->flags & RF_BAND) ? b->tier_min : sb;
        if (ta < m) m = ta;
        if (tb < m) m = tb;
        if (sa <= m + e->band && sb <= m + e->band) {
            if (e->pec) {
                double na = (a->flags & RF_SCORED) ? a->n_built : 0, nb = (b->flags & RF_SCORED) ? b->n_built : 0;
                if (na != nb) return na < nb;
            }
            if (e->swb) {
                int ga = (a->flags & RF_START) ? a->start_now : 0, gb = (b->flags & RF_START) ? b->start_now : 0;
                if (ga != gb) return ga;
            }
        }
    }
    if (old_better(e, a, b)) return 1;
    if (old_better(e, b, a)) return 0;
    if (no_s0) return 0;
    double da = (a->flags & RF_POS) ? a->deg : 0, db = (b->flags & RF_POS) ? b->deg : 0;
    if (da != db) return da < db;
    return ((a->flags & RF_POS) ? a->radius : 0) > ((b->flags & RF_POS) ? b->radius : 0);
}
static int better_lt(void *cx, void *a, void *b) { return better((Ev *)cx, (Rec *)a, (Rec *)b, 0); }

static void set_band(Ev *e, Rec **sc, int n) {
    if (!e->has_band || !(e->band > 0)) return;
    double kmin[64]; int ksafe[64]; double kkills[64]; int nk = 0;
    for (int i = 1; i <= n; i++) {
        Rec *r = sc[i];
        int j;
        for (j = 0; j < nk; j++) if (ksafe[j] == r->safe && kkills[j] == r->kills) break;
        if (j == nk) {
            if (nk >= 64) luaL_error(e->L, "gh_pt2_v2: too many tiers");
            ksafe[nk] = r->safe; kkills[nk] = r->kills; kmin[nk] = r->score; nk++;
        } else if (r->score < kmin[j]) kmin[j] = r->score;
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

static void rerank(Ev *e) {
    set_band(e, e->scored, e->nscored);
    lsort(e->L, (void **)e->scored, e->nscored, better_lt, e);
}

static int pre_rank(void *cx, void *pa, void *pb) {
    (void)cx;
    const Rec *a = (const Rec *)pa, *b = (const Rec *)pb;
    if (a->kills != b->kills) return a->kills > b->kills;
    if (a->covered != b->covered) return a->covered;
    if (a->base != b->base) return a->base < b->base;
    return ((a->flags & RF_POS) ? a->deg : 0) < ((b->flags & RF_POS) ? b->deg : 0);
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
            Ray *r = cast(e, &e->ar, sx, sy, hx, hy, e->pill_len, NULL, 0, &scratch);
            int linked = e->link_covers && ci && r->link != 0 && ci < b->chain->n
                         && r->link == b->chain->v[ci]
                         && !(e->link_clear_front && link_cut(e, r->front));
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
    h->why_pill = first_why;
    return h;
}

/* ── Output ─────────────────────────────────────────────────────────── */
/* An object goes out once per output; later places get the same table. */
static int memo_get(Ev *e, Memo *m) {
    if (!m->memo || m->mg != e->c->mgen) return 0;
    lua_rawgeti(e->L, S_MEMO, m->memo);
    return 1;
}
static int s_memo_n;
static void memo_put(Ev *e, Memo *m) {
    lua_State *L = e->L;
    int k = (int)lua_objlen(L, S_MEMO) + 1;
    lua_pushvalue(L, -1);
    lua_rawseti(L, S_MEMO, k);
    m->memo = k; m->mg = e->c->mgen;
}

static void setnum(lua_State *L, const char *k, double v) { lua_pushnumber(L, v); lua_setfield(L, -2, k); }
static void setbool(lua_State *L, const char *k, int v) { lua_pushboolean(L, v); lua_setfield(L, -2, k); }

static void push_plist(Ev *e, PList *l) {
    lua_State *L = e->L;
    if (memo_get(e, &l->m)) return;
    lua_createtable(L, l->n, 0);
    for (int i = 0; i < l->n; i++) { push_pill(e, l->v[i]); lua_rawseti(L, -2, i + 1); }
    memo_put(e, &l->m);
}

static void push_in_chain(Ev *e, PList *chain) {
    lua_State *L = e->L;
    if (memo_get(e, &chain->m_ic)) return;
    lua_createtable(L, 0, chain->n);
    for (int i = 0; i < chain->n; i++) {
        push_pill(e, chain->v[i]);
        lua_pushinteger(L, i + 1);
        lua_rawset(L, -3);
    }
    memo_put(e, &chain->m_ic);
}

static void push_ri(Ev *e, Rec *r) {
    lua_State *L = e->L;
    if (memo_get(e, &r->m_ri)) return;
    lua_createtable(L, 0, r->T->n);
    for (int i = 0; i < r->T->n; i++) {
        push_pill(e, r->T->v[i]);
        lua_pushinteger(L, i + 1);
        lua_rawset(L, -3);
    }
    memo_put(e, &r->m_ri);
}

static void push_lane(Ev *e, Lane *l) {
    lua_State *L = e->L;
    if (!l) { lua_pushnil(L); return; }
    if (memo_get(e, &l->m)) return;
    lua_createtable(L, l->n, 0);
    for (int i = 0; i < l->n; i++) { lua_pushnumber(L, (double)l->v[i]); lua_rawseti(L, -2, i + 1); }
    memo_put(e, &l->m);
}

static void push_aims(Ev *e, Aims *a) {
    lua_State *L = e->L;
    if (memo_get(e, &a->m)) return;
    lua_createtable(L, a->n, 0);
    for (int i = 0; i < a->n; i++) { lua_pushinteger(L, a->v[i]); lua_rawseti(L, -2, i + 1); }
    memo_put(e, &a->m);
}

static void push_lanes(Ev *e, Lanes *ls) {
    lua_State *L = e->L;
    if (memo_get(e, &ls->m)) return;
    lua_createtable(L, 0, ls->n);
    for (int i = 0; i < ls->n; i++) {
        if (!ls->ln[i]) continue;
        push_lane(e, ls->ln[i]);
        lua_rawseti(L, -2, ls->ai[i]);
    }
    memo_put(e, &ls->m);
}

static void push_extra(Ev *e, Extra *x) {
    lua_State *L = e->L;
    if (memo_get(e, &x->m)) return;
    lua_createtable(L, 0, x->n);
    for (int i = 0; i < x->n; i++) {
        lua_pushnumber(L, (double)x->sq[i]);
        lua_pushnumber(L, x->shots[i]);
        lua_rawset(L, -3);
    }
    memo_put(e, &x->m);
}

static void push_blk(Ev *e, Blk *b) {
    lua_State *L = e->L;
    if (memo_get(e, &b->m)) return;
    lua_createtable(L, 0, 5);
    setnum(L, "sq", (double)b->sq);
    setnum(L, "shots", b->shots);
    lua_pushstring(L, s_bk_name[b->kind]); lua_setfield(L, -2, "kind");
    setnum(L, "t", b->t);
    if (b->pill) { push_pill(e, b->pill); lua_setfield(L, -2, "pill"); }
    memo_put(e, &b->m);
}

static void push_ray(Ev *e, Ray *r) {
    lua_State *L = e->L;
    if (memo_get(e, &r->m)) return;
    lua_createtable(L, 0, 24);
    memo_put(e, &r->m);
    Ray *b = r->src ? r->src : r;
    setnum(L, "angle", r->angle);
    lua_pushstring(L, r->tank ? "tank" : "reach"); lua_setfield(L, -2, "hit");
    setnum(L, "t", r->t);
    if (r->link) {
        push_pill(e, r->link); lua_setfield(L, -2, "link");
        setnum(L, "link_t", r->link_t);
    }
    setnum(L, "cover", r->cover);
    setnum(L, "front", r->front);
    if (!memo_get(e, &b->m_sq)) {
        lua_createtable(L, r->n_sq, 0);
        for (int i = 0; i < r->n_sq; i++) { lua_pushnumber(L, (double)r->sq[i]); lua_rawseti(L, -2, i + 1); }
        memo_put(e, &b->m_sq);
    }
    lua_setfield(L, -2, "sq");
    if (!memo_get(e, &b->m_sqt)) {
        lua_createtable(L, r->n_sq, 0);
        for (int i = 0; i < r->n_sq; i++) { lua_pushnumber(L, r->sq_t[i]); lua_rawseti(L, -2, i + 1); }
        memo_put(e, &b->m_sqt);
    }
    lua_setfield(L, -2, "sq_t");
    Ray *bo = r->own_blk ? r : b;
    if (!memo_get(e, &bo->m_bl)) {
        lua_createtable(L, r->n_block, 0);
        for (int i = 0; i < r->n_block; i++) { push_blk(e, r->blk[i]); lua_rawseti(L, -2, i + 1); }
        memo_put(e, &bo->m_bl);
    }
    lua_setfield(L, -2, "block");
    setnum(L, "n_sq", r->n_sq);
    setnum(L, "n_block", r->n_block);
    setnum(L, "end_x", r->end_x);
    setnum(L, "end_y", r->end_y);
    if (r->X) { push_pill(e, r->X); lua_setfield(L, -2, "X"); }
    if (r->src) {
        setbool(L, "cut_blk", r->cut_blk);
        setbool(L, "cut_front", r->cut_front);
        setbool(L, "cut", r->cut);
    }
    if (r->has_bar) {
        setnum(L, "bar", r->bar);
        setbool(L, "link_cover", r->link_cover);
        setbool(L, "ok", r->ok);
    }
}

static void push_raylist(Ev *e, RayList *rl) {
    lua_State *L = e->L;
    if (memo_get(e, &rl->m)) return;
    lua_createtable(L, rl->n, 0);
    memo_put(e, &rl->m);
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
    case W_CANT: lua_pushstring(L, "CANT"); break;
    default: lua_pushnil(L); break;
    }
}

static void push_plansq(Ev *e, PlanSq *pl, int n) {
    lua_State *L = e->L;
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        lua_createtable(L, 0, 3);
        setnum(L, "mx", pl[i].mx); setnum(L, "my", pl[i].my); setnum(L, "shots", pl[i].shots);
        lua_rawseti(L, -2, i + 1);
    }
}

static void push_plan(Ev *e, Plan *p) {
    lua_State *L = e->L;
    if (memo_get(e, &p->m)) return;
    lua_createtable(L, p->n, 0);
    memo_put(e, &p->m);
    for (int i = 0; i < p->n; i++) {
        PlanE *pe = &p->v[i];
        lua_createtable(L, 0, 5);
        setnum(L, "sq", (double)pe->sq); setnum(L, "mx", pe->mx); setnum(L, "my", pe->my);
        setnum(L, "shots", pe->shots);
        lua_pushstring(L, pe->kind_pill ? "pill" : "wall"); lua_setfield(L, -2, "kind");
        lua_rawseti(L, -2, i + 1);
    }
}

static void push_rec(Ev *e, Rec *r) {
    lua_State *L = e->L;
    if (memo_get(e, &r->m)) return;
    lua_createtable(L, 0, 56);
    memo_put(e, &r->m);
    setnum(L, "px", r->px); setnum(L, "py", r->py); setnum(L, "nudge_wu", r->nudge_wu);
    push_plist(e, r->T); lua_setfield(L, -2, "T");
    push_raylist(e, r->rays); lua_setfield(L, -2, "rays");
    push_ri(e, r); lua_setfield(L, -2, "ri");
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
    if (!memo_get(e, &r->uncovered->m)) {
        lua_createtable(L, r->uncovered->n, 0);
        for (int i = 0; i < r->uncovered->n; i++) { lua_pushinteger(L, r->uncovered->v[i]); lua_rawseti(L, -2, i + 1); }
        memo_put(e, &r->uncovered->m);
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
        push_plan(e, r->plan); lua_setfield(L, -2, "plan");
        push_extra(e, r->extra); lua_setfield(L, -2, "extra");
    }
    if (r->flags & RF_KMAX) setnum(L, "kmax", r->kmax);
    if (r->flags & RF_SCORED) {
        setnum(L, "n_built", r->n_built); setnum(L, "blk", r->blk); setnum(L, "score", r->score);
    }
    if (r->flags & RF_UNSEARCH) setbool(L, "unsearched", 1);
    if (r->flags & RF_AIMNB) setbool(L, "aimnb", r->aimnb);
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
    if (r->flags & RF_START) {
        setbool(L, "start_now", r->start_now);
        setnum(L, "exposure", r->exposure);
        setnum(L, "shells_through", r->shells_through);
        if (r->start_why) { lua_pushstring(L, s_start_why[r->start_why]); lua_setfield(L, -2, "start_why"); }
        if (r->has_sp) {
            lua_createtable(L, r->n_sp, 0);
            for (int i = 0; i < r->n_sp; i++) {
                StartPill *sp = &r->sp[i];
                lua_createtable(L, 0, 4);
                setnum(L, "mx", sp->mx); setnum(L, "my", sp->my); setnum(L, "cover_now", sp->cover_now);
                push_plansq(e, sp->pl, sp->np); lua_setfield(L, -2, "planned");
                lua_rawseti(L, -2, i + 1);
            }
            lua_setfield(L, -2, "start_pills");
        }
        if (r->has_sc) {
            lua_createtable(L, r->n_sc, 0);
            for (int i = 0; i < r->n_sc; i++) {
                StartLink *sc = &r->sc[i];
                lua_createtable(L, 0, 6);
                setnum(L, "mx", sc->mx); setnum(L, "my", sc->my); setnum(L, "cover_now", sc->cover_now);
                if (sc->has_to) setnum(L, "to", sc->to);
                if (sc->np >= 0) { push_plansq(e, sc->pl, sc->np); lua_setfield(L, -2, "planned"); }
                if (sc->has_count) setbool(L, "count", sc->count);
                lua_rawseti(L, -2, i + 1);
            }
            lua_setfield(L, -2, "start_chain");
        }
    }
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
    if ((r->flags & RF_XCOVER) && r->n_xc > 0) {
        lua_createtable(L, r->n_xc, 0);
        for (int i = 0; i < r->n_xc; i++) {
            XWall *w = &r->xc[i];
            lua_createtable(L, 0, 5);
            setnum(L, "sq", (double)w->sq); setnum(L, "mx", w->mx); setnum(L, "my", w->my);
            setnum(L, "shots", w->shots);
            lua_pushstring(L, "wall"); lua_setfield(L, -2, "kind");
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "extra_cover");
    }
}

/* ── Setup ──────────────────────────────────────────────────────────── */
static int std_list(lua_State *L, int t, const char *k, double *v, int max, int *n) {
    lua_getfield(L, t, k);
    int ok = lua_istable(L, -1);
    if (ok) {
        int lt = lua_gettop(L);
        lua_getfield(L, lt, "n");
        ok = lua_type(L, -1) == LUA_TNUMBER;
        double c = ok ? lua_tonumber(L, -1) : 0;
        lua_pop(L, 1);
        if (!ok || c < 0 || c > max || c != floor(c)) ok = 0;
        for (int i = 0; ok && i < (int)c; i++) {
            lua_rawgeti(L, lt, i + 1);
            if (lua_isnil(L, -1)) v[i] = NAN;
            else if (lua_type(L, -1) == LUA_TNUMBER) v[i] = lua_tonumber(L, -1);
            else ok = 0;
            lua_pop(L, 1);
        }
        if (ok) *n = (int)c;
    }
    lua_pop(L, 1);
    return ok;
}

/* inp.c_std (as gh_pt2_old.c std_setup) */
static void std_setup(Ev *e) {
    lua_State *L = e->L;
    int t0 = lua_gettop(L);
    e->n_map = e->n_stay = e->n_std = e->n_bld = 0;
    lua_getfield(L, S_INP, "c_std");
    if (!lua_istable(L, -1)) { lua_settop(L, t0); return; }
    int t = lua_gettop(L);
    double cost[PT2_TT_N], wt[PT2_STD_LIST];
    int nw = 0;
    for (int i = 0; i < PT2_TT_N; i++) cost[i] = 9999;
    int ok_std = 0;
    lua_getfield(L, t, "cost");
    if (lua_istable(L, -1)) {
        int ct = lua_gettop(L);
        ok_std = 1;
        lua_pushnil(L);
        while (lua_next(L, ct) != 0) {
            if (lua_type(L, -2) != LUA_TNUMBER || lua_type(L, -1) != LUA_TNUMBER) ok_std = 0;
            else {
                double k = lua_tonumber(L, -2);
                if (k < 0 || k >= PT2_TT_N || k != floor(k)) ok_std = 0;
                else cost[(int)k] = lua_tonumber(L, -1);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    if (ok_std) ok_std = std_list(L, t, "water", wt, PT2_STD_LIST, &nw);
    if (ok_std) {
        for (int i = 0; i < PT2_TT_N; i++) {
            int w = 0;
            for (int j = 0; j < nw; j++) if ((double)i == wt[j]) w = 1;
            e->std_ok[i] = !(cost[i] >= 9999) && !w;
        }
    }
    int ok_bld = std_list(L, t, "nobuild", e->nb, PT2_STD_LIST, &e->nb_n);
    lua_getfield(L, t, "no_builder");
    e->no_builder = lua_toboolean(L, -1);
    lua_pop(L, 1);
    int ok_stay = 1;
    e->st_n = 0;
    lua_getfield(L, t, "rects");
    if (lua_istable(L, -1)) {
        int rt = lua_gettop(L);
        for (int i = 1; ok_stay; i++) {
            lua_rawgeti(L, rt, i);
            if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
            if (!lua_istable(L, -1) || e->st_n >= PT2_STAY_MAX) ok_stay = 0;
            for (int j = 0; ok_stay && j < 4; j++) {
                lua_rawgeti(L, -1, j + 1);
                if (lua_type(L, -1) != LUA_TNUMBER) ok_stay = 0;
                else e->st_r[4 * e->st_n + j] = lua_tonumber(L, -1);
                lua_pop(L, 1);
            }
            if (ok_stay) e->st_n++;
            lua_pop(L, 1);
        }
    } else if (lua_toboolean(L, -1)) ok_stay = 0;
    lua_pop(L, 1);
    static const struct { const char *k; int slot; } fs[4] = {
        { "in_map", S_IN_MAP }, { "stay_in", S_STAY_IN },
        { "standable", S_STANDABLE }, { "buildable", S_BUILDABLE },
    };
    int on[4];
    for (int i = 0; i < 4; i++) {
        lua_getfield(L, t, fs[i].k);
        on[i] = !lua_isnil(L, -1) && lua_rawequal(L, -1, fs[i].slot);
        lua_pop(L, 1);
    }
    e->n_map = on[0];
    e->n_stay = on[1] && ok_stay;
    e->n_std = on[2] && ok_std;
    e->n_bld = on[3] && ok_bld;
    lua_settop(L, t0);
}

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
    lua_getfield(L, LUA_REGISTRYINDEX, "gh_pt2_v2_ctx");
    Pt2Ctx *c = (Pt2Ctx *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!c) luaL_error(L, "gh_pt2_v2: no ctx");
    return c;
}

static int ctx_gc(lua_State *L) {
    Pt2Ctx *c = (Pt2Ctx *)lua_touserdata(L, 1);
    free(c->tt_stamp); free(c->tt_val); free(c->po_stamp); free(c->po_val); free(c->ba_stamp);
    free(c->cg); free(c->tg);
    for (int i = 0; i < c->n_offs; i++) free(c->offs[i].o);
    free(c->offs);
    memset(c, 0, sizeof(*c));
    return 0;
}

static int job_gc(lua_State *L) {
    Job *j = (Job *)lua_touserdata(L, 1);
    if (j->freed) return 0;
    ar_free(&j->ar);
    hm_free(&j->rays); hm_free(&j->sqm); hm_free(&j->sqf);
    free(j->pills); free(j->sqf_new);
    j->pills = NULL; j->sqf_new = NULL;
    j->freed = 1;
    return 0;
}

static int ev_gc(lua_State *L) {
    Ev *e = (Ev *)lua_touserdata(L, 1);
    ar_free(&e->ar);
    hm_free(&e->reach); hm_free(&e->lgt); hm_free(&e->lgt_v); hm_free(&e->cwm);
    hm_free(&e->wsmask); hm_free(&e->wslane); hm_free(&e->wsseen);
    return 0;
}

/* push a new job userdata (its uservalue: { pills, pidx }) */
static void job_new(lua_State *L) {
    Job *j = (Job *)lua_newuserdata(L, sizeof(Job));
    memset(j, 0, sizeof(*j));
    if (luaL_newmetatable(L, "gh_pt2_v2_job_mt")) {
        lua_pushcfunction(L, job_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
    lua_createtable(L, 2, 0);
    lua_newtable(L); lua_rawseti(L, -2, 1);
    lua_newtable(L); lua_rawseti(L, -2, 2);
    lua_setfenv(L, -2);
}

/* Each step: the map reads are cached for this step only. */
static void step_reads(Ev *e) {
    lua_State *L = e->L;
    Pt2Ctx *c = e->c;
    c->gen++;
    if (c->gen == 0) {
        memset(c->tt_stamp, 0, sizeof(uint32_t) * PT2_MAP_SQ);
        memset(c->po_stamp, 0, sizeof(uint32_t) * PT2_MAP_SQ);
        memset(c->ba_stamp, 0, sizeof(uint32_t) * PT2_MAP_SQ);
        c->gen = 1;
    }
    e->om_sent = 0;
    e->tt_hits = 0;
    e->cg_on = cg_ready(e);
    e->bs_on = lua_toboolean(L, S_BASE_STOPS);
    e->ba_tab = lua_istable(L, S_BASE_AT);
    if (e->bs_on && e->ba_tab) {
        lua_pushnil(L);
        while (lua_next(L, S_BASE_AT) != 0) {
            lua_pop(L, 1);
            if (lua_type(L, -1) == LUA_TNUMBER) {
                double key = lua_tonumber(L, -1);
                if (key >= 0 && key < PT2_MAP_SQ && key == floor(key)) c->ba_stamp[(int)key] = c->gen;
            }
        }
    }
}

static void step_end(Ev *e) {
    lua_State *L = e->L;
    sync_om(e);
    if (e->tt_hits > 0 && lua_toboolean(L, S_COUNT_READS)) {
        lua_pushvalue(L, S_COUNT_READS);
        lua_pushnumber(L, e->tt_hits);
        lua_call(L, 1, 0);
    }
    e->tt_hits = 0;
}

static double rise_on(lua_State *L, int inp) {
    lua_getfield(L, inp, "start_anger_rise");
    if (!lua_isnil(L, -1)) { int v = lua_toboolean(L, -1); lua_pop(L, 1); return v; }
    lua_pop(L, 1);
    lua_getglobal(L, "package");
    int on = 1;
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "loaded");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "constants");
            if (lua_istable(L, -1)) {
                lua_getfield(L, -1, "PT2_START_ANGER_RISE");
                if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) on = 0;
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return on;
}

static void ev_read_inp(Ev *e) {
    lua_State *L = e->L;
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
    e->link_clear_front = getbool(L, S_INP, "link_clear_front");
    {
        lua_getfield(L, S_OLD, "link_cut");
        lua_pop(L, 1);
    }
    e->link_cut_min_shots = getnum(L, S_INP, "link_cut_min_shots", 0, NULL);
    e->leeway_on = getbool(L, S_INP, "leeway_on");
    e->own_pill_rank = getbool(L, S_INP, "own_pill_rank");
    e->orth_only = getbool(L, S_INP, "orth_only");
    e->aim_nudge = getbool(L, S_INP, "aim_nudge");
    e->keep_all = getbool(L, S_INP, "keep_all");
    e->max_chain = getnum(L, S_INP, "max_chain", 0, NULL);
    e->leeway_ring_wu = getnum(L, S_INP, "leeway_ring_wu", 0, NULL);
    e->leeway_top = getnum(L, S_INP, "leeway_top", 0, NULL);
    e->pill_range = getnum(L, S_INP, "pill_range", 0, NULL);
    e->range_margin = getnum(L, S_INP, "range_margin", 0, NULL);
    e->blocker_min_dist = getnum(L, S_INP, "blocker_min_dist", 0, NULL);
    e->nudge_step_wu = getnum(L, S_INP, "nudge_step_wu", 0, NULL);
    e->nudge_max_steps = getnum(L, S_INP, "nudge_max_steps", 0, NULL);
    e->leeway_min = getnum(L, S_INP, "leeway_min", 0, &e->has_leeway_min);
    e->band = getnum(L, S_INP, "travel_band", 0, &e->has_band);
    e->finish_max_anger = getnum(L, S_INP, "finish_max_anger", 0, &e->has_fma);
    e->finish_min_armour = getnum(L, S_INP, "finish_min_armour", 5, NULL);
    e->armour = getnum(L, S_INP, "armour", 0, &e->has_armour);
    e->pec = getbool(L, S_INP, "prefer_existing_cover");
    e->swb = getbool(L, S_INP, "start_while_building");
    e->aim_nb_on = getbool(L, S_INP, "aim_avoid_nb");
    e->chains_only = getbool(L, S_INP, "chains_only");
    e->xcover_later = getbool(L, S_INP, "_xcover_later");
    e->extra_cover_walls = getnum(L, S_INP, "extra_cover_walls", 0, NULL);
    e->anger_bump = getnum(L, S_INP, "anger_bump", 0.3333, NULL);
    e->pill_attack_ticks = getnum(L, S_INP, "pill_attack_ticks", 100, &e->has_pat);
    e->pill_attack_min_ticks = getnum(L, S_INP, "pill_attack_min_ticks", 6, &e->has_pamt);
    e->pill_angry_divisor = getnum(L, S_INP, "pill_angry_divisor", 2, &e->has_pad);
    e->pill_cooldown_ticks = getnum(L, S_INP, "pill_cooldown_ticks", 32, &e->has_pct);
    e->tank_reload = getnum(L, S_INP, "tank_reload", 13, &e->has_trl);
    e->env_speed = getnum(L, S_ENV, "shell_speed", 32, &e->has_env_speed);
    e->rise = (int)rise_on(L, S_INP);
    {
        int h;
        double ap = getnum(L, S_INP, "aim_points", 0, &h);
        e->aim9 = h && ap == 9;
        e->naims = e->aim9 ? 9 : 5;
    }
    e->wall_memo = getbool(L, S_INP, "wall_memo");
    e->wall_memo_max = getnum(L, S_INP, "wall_memo_max", 2000, NULL);
    e->has_clk = lua_toboolean(L, S_CLK);
    e->T_BUILDING = getnum(L, S_ENV, "T_BUILDING", NAN, NULL);
    e->T_HALFBUILD = getnum(L, S_ENV, "T_HALFBUILD", NAN, NULL);
    e->T_BOAT = getnum(L, S_ENV, "T_BOAT", NAN, NULL);
    e->T_FOREST = getnum(L, S_ENV, "T_FOREST", NAN, NULL);
    e->e_wall_shots = getnum(L, S_ENV, "wall_shots", 0, NULL);
    e->e_half_wall_shots = getnum(L, S_ENV, "half_wall_shots", 0, NULL);
    e->e_boat_shots = getnum(L, S_ENV, "boat_shots", 0, NULL);
    e->forest_transparent = getbool(L, S_ENV, "forest_transparent");
    e->sh_speed = getnum(L, S_ENV, "shell_speed", PT2_SHELL_SPEED, NULL);
    e->sh_add = getnum(L, S_ENV, "shell_start_add", PT2_START_ADD, NULL);
    e->sh_life = getnum(L, S_ENV, "shell_life", PT2_SHELL_LIFE, NULL);
    e->sh_hit_r = getnum(L, S_ENV, "tank_hit_r", PT2_TANK_HIT_R, NULL);
    e->pill_len = getnum(L, S_ENV, "pill_fire_len", PT2_PILL_FIRE_LEN, NULL);
    /* shell_range(env, len) for the tank and the pill */
    {
        double lt = life_ticks(e, PT2_TANK_FIRE_LEN), lp = life_ticks(e, e->pill_len);
        e->ts_s1 = (e->sh_add + 1) * e->sh_speed; e->ts_slast = (e->sh_add + lt) * e->sh_speed;
        e->ts_step = e->sh_speed; e->ts_hit = e->sh_hit_r;
        e->ps_s1 = (e->sh_add + 1) * e->sh_speed; e->ps_slast = (e->sh_add + lp) * e->sh_speed;
        e->ps_step = e->sh_speed; e->ps_hit = e->sh_hit_r;
    }
    e->ts_boats = getbool(L, S_ENV, "ts_boats");
    e->ts_nt = 0;
    lua_getfield(L, S_ENV, "ts_tanks");
    if (lua_istable(L, -1)) {
        int tl = lua_gettop(L);
        int nn = 0;
        for (;; nn++) {
            lua_rawgeti(L, tl, nn + 1);
            int isnil = lua_isnil(L, -1);
            if (!isnil) {
                if (nn >= 2 * PT2_TS_MAX_TANKS)
                    luaL_error(L, "gh_pt2_v2: env.ts_tanks has over %d numbers", 2 * PT2_TS_MAX_TANKS);
                e->ts_t[nn] = lua_tonumber(L, -1);
            }
            lua_pop(L, 1);
            if (isnil) break;
        }
        if (nn % 2 != 0) luaL_error(L, "gh_pt2_v2: env.ts_tanks has %d numbers", nn);
        e->ts_nt = nn / 2;
    }
    lua_pop(L, 1);
    lua_getfield(L, S_INP, "radii_offsets");
    e->nradii = 0;
    if (lua_istable(L, -1)) {
        for (int i = 1;; i++) {
            lua_rawgeti(L, -1, i);
            if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
            if (e->nradii >= PT2_MAXRADII) luaL_error(L, "gh_pt2_v2: over %d radii", PT2_MAXRADII);
            e->radii[e->nradii++] = lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

/* The slots of a step, from the state's table (fenv) */
static void slots_push(lua_State *L) {
    lua_settop(L, 2);
    lua_getfenv(L, S_ST);
    int t = lua_gettop(L);
    for (int i = S_INP; i <= S_TOP; i++) lua_rawgeti(L, t, i);
    lua_remove(L, t);
    if (lua_gettop(L) != S_TOP) luaL_error(L, "gh_pt2_v2: stack");
}

/* ── The slicer (M.slicer) ──────────────────────────────────────────── */
static double work_of(Ev *e) {
    return SLICE_W_CAST * (e->casts + e->casts_confirm) + SLICE_W_SHOT * e->n_shot
        + SLICE_W_NODE * (e->wall_nodes - e->memo_nodes) + SLICE_W_POINT * e->n_eval;
}

/* 1: stop here (the Lua's yield) */
static int unit_fn(Ev *e, double est, int has_est, int fin) {
    if (!e->sl_on) return 0;
    double wu = work_of(e);
    int si = (int)e->sl_slices;
    if (fin) { if (si < 64) e->sl_wu[si] = wu - e->sl_start; return 0; }
    if (has_est && !e->sl_has_budget) {
        double t = e->sl_ticks < 1 ? 1 : e->sl_ticks;
        double b = ceil(est / t);
        e->sl_budget = b < 1 ? 1 : b;
        e->sl_has_budget = 1;
    }
    if (e->sl_has_budget && wu - e->sl_last > e->sl_budget) e->sl_over += 1;
    e->sl_last = wu;
    if (!e->sl_has_budget || wu - e->sl_start < e->sl_budget || e->sl_slices >= e->sl_ticks) return 0;
    if (si < 64) e->sl_wu[si] = wu - e->sl_start;
    e->sl_start = wu;
    e->sl_slices += 1;
    return 1;
}

#define UNIT(n, est, has_est) do { \
        if (unit_fn(e, (est), (has_est), 0)) { e->pc = (n); step_end(e); return 0; } \
        case (n):; \
    } while (0)

static void vec_push(Ev *e, Rec ***v, int *n, int *cap, Rec *r) {
    if (*n + 1 >= *cap) {
        int nc = *cap ? *cap * 2 : 64;
        Rec **nv = NEWN(e, Rec *, nc);
        if (*n) memcpy(nv, *v, sizeof(Rec *) * (size_t)(*n + 1));
        *v = nv; *cap = nc;
    }
    (*v)[++(*n)] = r;
}

static void all_push(Ev *e, Rec *rec, Reject *rj) {
    if (e->nall >= e->capall) {
        int nc = e->capall ? e->capall * 2 : 256;
        AllE *na = NEWN(e, AllE, nc);
        if (e->nall) memcpy(na, e->all, sizeof(AllE) * (size_t)e->nall);
        e->all = na; e->capall = nc;
    }
    e->all[e->nall].rec = rec; e->all[e->nall].rej = rj; e->nall++;
}

static double kmin_of(Ev *e, double need) {
    if (need <= 0) return 0;
    return ceil(need / e->sb);
}

static int single_lt(void *cx, void *pa, void *pb) {
    (void)cx;
    const Rec *a = (const Rec *)pa, *b = (const Rec *)pb;
    int ha = (a->flags & RF_LB) != 0, hb = (b->flags & RF_LB) != 0;
    if (ha != hb || (ha && a->lb != b->lb)) {
        if (!ha || !hb) return 0;   /* nil < number: not met (all or none have lb) */
        return a->lb < b->lb;
    }
    return a->deg < b->deg;
}
static int order_lt(void *cx, void *pa, void *pb) {
    (void)cx;
    const Rec *a = (const Rec *)pa, *b = (const Rec *)pb;
    if (a->base != b->base) return a->base < b->base;
    return a->deg < b->deg;
}
static int co_lt(void *cx, void *pa, void *pb) {
    (void)cx;
    const Rec *a = (const Rec *)pa, *b = (const Rec *)pb;
    if ((a->kmin == 0) != (b->kmin == 0)) return a->kmin == 0;
    if (a->lb != b->lb) return a->lb < b->lb;
    return a->deg < b->deg;
}

static double score_fn(Ev *e, double deg, double mx, double my, double cx, double cy) {
    lua_State *L = e->L;
    lua_pushvalue(L, S_SCORE_FN);
    lua_pushnumber(L, deg); lua_pushnumber(L, mx); lua_pushnumber(L, my);
    lua_pushnumber(L, cx); lua_pushnumber(L, cy);
    lua_call(L, 5, 1);
    double v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

static void points_of_ring(Ev *e) {
    lua_State *L = e->L;
    e->np = 0;
    e->nsteps = floor(360 / e->angle_step + 0.5);
    int need = (int)(e->nsteps > 0 ? e->nsteps : 0) + 1;
    if (need > e->cap_pp) {
        e->cap_pp = need;
        e->pp_deg = NEWN(e, double, need); e->pp_cx = NEWN(e, double, need); e->pp_cy = NEWN(e, double, need);
        e->pp_mx = NEWN(e, double, need); e->pp_my = NEWN(e, double, need);
        e->pp_px = NEWN(e, double, need); e->pp_py = NEWN(e, double, need);
        e->pp_hasA = NEWN(e, int, need); e->pp_cant = NEWN(e, int, need);
    }
    for (double s = 0; s <= e->nsteps - 1; s++) {
        double deg = s * e->angle_step;
        if (lua_toboolean(L, S_BANNED)) {
            lua_pushvalue(L, S_BANNED);
            lua_pushnumber(L, deg);
            lua_call(L, 1, 1);
            int ban = lua_toboolean(L, -1);
            lua_pop(L, 1);
            if (ban) continue;
        }
        double cx = e->pcx + sin(deg * PT2_LUA_RAD) * e->R;
        double cy = e->pcy - cos(deg * PT2_LUA_RAD) * e->R;
        double mx = floor(cx), my = floor(cy);
        if (in_map(e, mx, my) && stay_in(e, mx, my) && standable(e, mx, my)) {
            e->n_eval += 1;
            double px = floor(cx * 256), py = floor(cy * 256);
            int hasA = A_in_range(e, px, py);
            int is_cant = 0;
            if (hasA) {
                is_cant = cant(e, e->asets, px, py);
                if (is_cant) e->cant += 1; else e->survive += 1;
            }
            int q = e->np++;
            e->pp_deg[q] = deg; e->pp_cx[q] = cx; e->pp_cy[q] = cy; e->pp_mx[q] = mx; e->pp_my[q] = my;
            e->pp_px[q] = px; e->pp_py[q] = py; e->pp_hasA[q] = hasA; e->pp_cant[q] = is_cant;
        }
    }
}

static void point_of_ring(Ev *e, int q) {
    double deg = e->pp_deg[q], cx = e->pp_cx[q], cy = e->pp_cy[q], mx = e->pp_mx[q], my = e->pp_my[q];
    double px = e->pp_px[q], py = e->pp_py[q];
    Rec *rec = NULL;
    int why = W_NONE, wa = 0;
    if (!e->pp_hasA[q]) why = W_A_OUT;
    else if (e->pp_cant[q]) why = W_CANT;
    else rec = eval_point(e, px, py, NULL, 0, 1, 0, e->lazy, &why, &wa);
    if (rec) {
        if (rec->nudge_wu > 0) {
            cx = rec->px / 256; cy = rec->py / 256;
            mx = floor(cx); my = floor(cy);
        }
        rec->deg = deg; rec->cx = cx; rec->cy = cy; rec->mx = mx; rec->my = my; rec->radius = e->R;
        rec->flags |= RF_POS;
        rec->base = score_fn(e, deg, mx, my, cx, cy);
        rec->flags |= RF_BASE;
        double ax = cx + sin(deg * PT2_LUA_RAD) * e->approach_offset;
        double ay = cy - cos(deg * PT2_LUA_RAD) * e->approach_offset;
        rec->omx = floor(ax); rec->omy = floor(ay); rec->flags |= RF_OMXY;
        vec_push(e, &e->cands, &e->ncands, &e->cap_cands, rec);
    } else {
        e->n_reject += 1;
        if (e->keep_all) {
            Reject *rj = NEW(e, Reject);
            rj->deg = deg; rj->cx = cx; rj->cy = cy; rj->mx = mx; rj->my = my; rj->why = why; rj->why_arg = wa;
            all_push(e, NULL, rj);
        }
    }
}

/* the full record of a lazy stub q */
static Rec *full_of(Ev *e, Rec *q) {
    int why, wa;
    Rec *full = eval_point(e, q->px, q->py, NULL, q->aim_idx, 0, 0, 0, &why, &wa);
    if (!full) luaL_error(e->L, "gh_pt2_v2: a lazy point has no full record");
    full->nudge_wu = q->nudge_wu;
    full->deg = q->deg; full->cx = q->cx; full->cy = q->cy; full->mx = q->mx; full->my = q->my;
    full->radius = q->radius; full->flags |= RF_POS;
    full->base = q->base; full->flags |= RF_BASE;
    full->omx = q->omx; full->omy = q->omy; full->flags |= RF_OMXY;
    return full;
}

static void scored_push(Ev *e, Rec *r) { vec_push(e, &e->scored, &e->nscored, &e->cap_scored, r); }

/* evaluate_lua as a resumable step: 0 = stopped for a yield, 1 = done */
static int ev_run(Ev *e) {
    Rec *rec, *fin;
    switch (e->pc) {
    case 0:
        for (e->ri = 0; e->ri < e->nradii; e->ri++) {
            e->R = e->radius + e->radii[e->ri];
            e->ncands = 0;
            points_of_ring(e);
            if (!e->has_surv1) {
                e->surv1 = e->survive; e->has_surv1 = 1;
                UNIT(1, SLICE_PER_SURVIVOR * e->surv1, 1);
            }
            for (e->q = 0; e->q < e->np; e->q++) {
                point_of_ring(e, e->q);
                UNIT(2, 0, 0);
            }
            if (!e->lazy) lsort(e->L, (void **)e->cands, e->ncands, pre_rank, NULL);
            e->lone_r = e->ncands > 0;
            for (int i = 1; i <= e->ncands; i++) if (e->cands[i]->T->n != 1) { e->lone_r = 0; break; }
            e->nscored = 0;
            e->has_singles_co = 0;
            if (e->lazy) {
                e->nsingles = 0;
                e->singles = NEWN(e, Rec *, e->ncands + 1);
                for (e->ci = 1; e->ci <= e->ncands; e->ci++) {
                    rec = e->cands[e->ci];
                    if (rec->flags & RF_LAZY) { e->singles[++e->nsingles] = rec; continue; }
                    fin = plan_point(e, rec);
                    carry(fin, rec);
                    score_of(e, fin);
                    scored_push(e, fin);
                    UNIT(3, 0, 0);
                }
                e->co_bc = e->build_cost;
                e->sb = e->wall_shots;
                if (e->co) e->has_singles_co = 1;
                if (e->num_pill_blockers > 0 && e->pill_drop_shots > e->sb) e->sb = e->pill_drop_shots;
                if (!e->co) {
                    for (int i = 1; i <= e->nsingles; i++) {
                        Rec *q = e->singles[i];
                        q->lb = q->base + kmin_of(e, q->a_need) * e->co_bc;
                        q->flags |= RF_LB;
                    }
                }
                lsort(e->L, (void **)e->singles, e->nsingles, single_lt, NULL);
                e->has_m = 0;
                if (!e->co) {
                    for (e->ci = 1; e->ci <= e->nsingles; e->ci++) {
                        Rec *q = e->singles[e->ci];
                        if (e->has_m && q->lb > e->m + e->band) {
                            e->plan_skipped += e->nsingles - e->ci + 1;
                            break;
                        }
                        Rec *full = full_of(e, q);
                        fin = NULL;
                        if (!full->covered && e->has_m && e->co_bc > 0) {
                            double kmax = floor((e->m + e->band - full->base) / e->co_bc + 1e-9);
                            double km = 0;
                            for (int u = 0; u < full->uncovered->n; u++) {
                                Ray *r = full->rays->v[full->uncovered->v[u] - 1];
                                double k = kmin_of(e, r->bar - r->cover);
                                if (k > km) km = k;
                            }
                            if (km > kmax) {
                                plan_empty(e, full);
                                e->plan_cut += 1;
                                fin = full;
                            } else {
                                full->kmax = kmax; full->flags |= RF_KMAX;
                            }
                        }
                        if (!fin) fin = plan_point(e, full);
                        carry(fin, full);
                        score_of(e, fin);
                        scored_push(e, fin);
                        if (fin->safe && fin->kills == 1 && (!e->has_m || fin->score < e->m)) { e->m = fin->score; e->has_m = 1; }
                        UNIT(4, 0, 0);
                    }
                }
            } else if (e->lone_r && e->has_band && e->band > 0) {
                e->lone += 1;
                e->has_m = 0;
                e->norder = 0;
                e->order = NEWN(e, Rec *, e->ncands + 1);
                for (int i = 1; i <= e->ncands; i++) {
                    rec = e->cands[i];
                    rec->done = NULL;
                    if (rec->covered) {
                        plan_empty(e, rec);
                        score_of(e, rec);
                        if (rec->safe && (!e->has_m || rec->score < e->m)) { e->m = rec->score; e->has_m = 1; }
                    } else e->order[++e->norder] = rec;
                }
                lsort(e->L, (void **)e->order, e->norder, order_lt, NULL);
                for (e->ci = 1; e->ci <= e->norder; e->ci++) {
                    rec = e->order[e->ci];
                    if (e->has_m && rec->base > e->m + e->band) break;
                    fin = plan_point(e, rec);
                    carry(fin, rec);
                    score_of(e, fin);
                    rec->done = fin;
                    if (fin->safe && (!e->has_m || fin->score < e->m)) { e->m = fin->score; e->has_m = 1; }
                    UNIT(5, 0, 0);
                }
                for (int i = 1; i <= e->ncands; i++) {
                    rec = e->cands[i];
                    fin = rec->covered ? rec : rec->done;
                    if (!fin) {
                        fin = rec;
                        plan_empty(e, fin);
                        fin->flags |= RF_UNSEARCH;
                        score_of(e, fin);
                        e->plan_skipped += 1;
                    }
                    scored_push(e, fin);
                }
            } else {
                for (e->ci = 1; e->ci <= e->ncands; e->ci++) {
                    rec = e->cands[e->ci];
                    fin = plan_point(e, rec);
                    carry(fin, rec);
                    score_of(e, fin);
                    scored_push(e, fin);
                    UNIT(6, 0, 0);
                }
            }
            rerank(e);
            if (e->leeway_on) {
                e->ntop = (int)dmin(e->leeway_top, e->nscored);
                if (e->ntop < 0) e->ntop = 0;
                e->top = NEWN(e, Rec *, e->ntop + 1);
                for (int i = 1; i <= e->ntop; i++) e->top[i] = e->scored[i];
                e->again = 1;
                while (e->again) {
                    e->again = 0;
                    e->X = NULL;
                    for (int i = 1; i <= e->nscored; i++) {
                        Rec *r = e->scored[i];
                        if (r->safe && r->kills >= 2) { e->X = r; break; }
                    }
                    if (e->X) {
                        e->Lr = e->X;
                        if (e->X->flags & RF_LEEWAY) {
                            for (int i = 1; i <= e->nscored; i++) {
                                Rec *r = e->scored[i];
                                if (r->safe && r->kills == e->X->kills && r->score < e->Lr->score) e->Lr = r;
                            }
                        }
                        if (!(e->Lr->flags & RF_LEEWAY)) {
                            UNIT(7, 0, 0);
                            e->Lr->leeway = leeway(e, e->Lr, e->leeway_ring_wu, &e->Lr->lpts);
                            e->Lr->flags |= RF_LEEWAY;
                            e->leeway_more += 1;
                            if (e->Lr->leeway < e->leeway_min) {
                                e->Lr->safe = 0; e->Lr->flags |= RF_WHYUNSAFE;
                                e->leeway_more_fail += 1;
                            }
                            e->again = 1;
                            rerank(e);
                        }
                    }
                }
                if (!e->X) {
                    int any = 0;
                    for (int i = 1; i <= e->ntop; i++) {
                        Rec *r = e->top[i];
                        if (r->kills < 2 && !(r->flags & RF_LEEWAY)) {
                            r->leeway = leeway(e, r, e->leeway_ring_wu, &r->lpts);
                            r->flags |= RF_LEEWAY;
                            e->leeway_single += 1;
                            any = 1;
                        }
                    }
                    if (any) rerank(e);
                }
            }
            if (e->co && e->has_singles_co && e->nsingles > 0) {
                int has_chain = 0;
                for (int i = 1; i <= e->nscored; i++) {
                    Rec *r = e->scored[i];
                    if (r->safe && r->kills >= 2) { has_chain = 1; break; }
                }
                if (!has_chain) {
                    for (int i = 1; i <= e->nsingles; i++) {
                        Rec *q = e->singles[i];
                        if (!(q->flags & RF_ANEED)) {
                            Ray *v = ray_view(e, base_ray(e, e->A, q->px, q->py), NULL);
                            q->a_need = q->a_bar - v->cover;
                            q->flags |= RF_ANEED;
                        }
                        q->kmin = kmin_of(e, q->a_need);
                        q->lb = q->base + q->kmin * e->build_cost;
                        q->flags |= RF_KMIN | RF_LB;
                    }
                    lsort(e->L, (void **)e->singles, e->nsingles, co_lt, NULL);
                    for (e->ci = 1; e->ci <= e->nsingles; e->ci++) {
                        e->co_single_eval += 1;
                        {
                            Rec *full = full_of(e, e->singles[e->ci]);
                            fin = plan_point(e, full);
                            carry(fin, full);
                            if (fin->safe) { e->no_chain = 1; break; }
                        }
                        UNIT(8, 0, 0);
                    }
                }
            }
            if (e->no_chain) { e->co_stop += 1; e->best = NULL; break; }
            for (int i = 1; i <= e->nscored; i++) all_push(e, e->scored[i], NULL);
            if (e->nscored >= 1 && (!e->best || better(e, e->scored[1], e->best, 0))) e->best = e->scored[1];
            if (e->best && e->best->safe) break;
        }
        if (e->best && e->best->safe && (e->best->flags & RF_PLAN) && e->best->plan->n > 0) {
            e->best->hold = hold_point(e, e->best);
            e->best->flags |= RF_HOLD;
        }
        if (e->best && e->best->safe && e->extra_cover_walls > 0 && !e->xcover_later) extra_cover(e, e->best);
        unit_fn(e, 0, 0, 1);
        e->pc = 99;
        return 1;
    default:
        return 1;
    }
}

static void stat_put(lua_State *L, const char *k, double v) { setnum(L, k, v); }

static void push_result(Ev *e) {
    lua_State *L = e->L;
    e->c->mgen++;
    if (e->c->mgen == 0) e->c->mgen = 1;
    lua_newtable(L);
    lua_replace(L, S_MEMO);
    /* M.last_work */
    lua_createtable(L, 0, 8);
    setnum(L, "casts", e->casts + e->casts_confirm);
    setnum(L, "shots", e->n_shot);
    setnum(L, "nodes", e->wall_nodes);
    setnum(L, "points", e->n_eval);
    setnum(L, "cant", e->cant);
    setnum(L, "survive", e->survive);
    setnum(L, "planned", e->planned);
    if (e->has_surv1) setnum(L, "surv1", e->surv1);
    lua_setfield(L, S_M, "last_work");
    lua_createtable(L, 0, 8);
    int res = lua_gettop(L);
    if (e->best) { push_rec(e, e->best); lua_setfield(L, res, "best"); }
    if (e->keep_all) {
        lua_createtable(L, e->nall, 0);
        for (int i = 0; i < e->nall; i++) {
            if (e->all[i].rec) push_rec(e, e->all[i].rec);
            else {
                Reject *rj = e->all[i].rej;
                lua_createtable(L, 0, 6);
                setnum(L, "deg", rj->deg); setnum(L, "cx", rj->cx); setnum(L, "cy", rj->cy);
                setnum(L, "mx", rj->mx); setnum(L, "my", rj->my);
                push_why(e, rj->why, rj->why_arg); lua_setfield(L, -2, "rejected");
            }
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, res, "all");
    }
    setnum(L, "n_eval", e->n_eval);
    setnum(L, "n_reject", e->n_reject);
    push_plist(e, e->hostile); lua_setfield(L, res, "hostile");
    lua_pushvalue(L, S_INP); lua_setfield(L, res, "inp");
    if (e->no_chain) { lua_pushstring(L, "single"); lua_setfield(L, res, "no_chain"); }
    lua_createtable(L, 0, 2);
    lua_createtable(L, 0, 40);
    stat_put(L, "sq_reads", e->sq_reads); stat_put(L, "casts", e->casts);
    stat_put(L, "casts_confirm", e->casts_confirm); stat_put(L, "ray_reuse", e->ray_reuse);
    stat_put(L, "nudged", e->nudged); stat_put(L, "planned", e->planned);
    stat_put(L, "confirm_fail", e->confirm_fail); stat_put(L, "wall_nodes", e->wall_nodes);
    stat_put(L, "cant", e->cant); stat_put(L, "survive", e->survive); stat_put(L, "lone", e->lone);
    stat_put(L, "plan_skipped", e->plan_skipped); stat_put(L, "lazy", e->lazy_n);
    stat_put(L, "plan_cut", e->plan_cut); stat_put(L, "must_cut", e->must_cut);
    stat_put(L, "must_cut_wrong", e->must_cut_wrong); stat_put(L, "co_stop", e->co_stop);
    stat_put(L, "co_single_eval", e->co_single_eval);
    stat_put(L, "loss_pass", 0); stat_put(L, "loss_safe", 0);
    stat_put(L, "chain_agree", 0); stat_put(L, "chain_differ", 0); stat_put(L, "chain_unsure", 0);
    stat_put(L, "wall_slow", e->wall_slow);
    stat_put(L, "confirm_checked", 0); stat_put(L, "confirm_mismatch", 0); stat_put(L, "confirm_winner", 0);
    stat_put(L, "confirm_retry", e->confirm_retry); stat_put(L, "confirm_rescued", e->confirm_rescued);
    stat_put(L, "reach_calls", e->reach_calls); stat_put(L, "reach_sq_calls", 0); stat_put(L, "reach_differs", 0);
    stat_put(L, "leeway_more", e->leeway_more); stat_put(L, "leeway_more_fail", e->leeway_more_fail);
    stat_put(L, "leeway_single", e->leeway_single);
    stat_put(L, "aimnb_moved", e->aimnb_moved);
    lua_setfield(L, -2, "stat");
    {
        static const char *tmk[] = { "wedges", "masks", "cast", "aim", "walls", "confirm", "leeway",
                                     "score_fn", "start", "xcover", "total", "pir", "nudge", "evb",
                                     "dfs", "sort", NULL };
        lua_createtable(L, 0, 16);
        for (int i = 0; tmk[i]; i++) setnum(L, tmk[i], 0);
        lua_setfield(L, -2, "tm");
    }
    lua_setfield(L, res, "v2");
}

/* gh_pt2_v2.begin(inp, M, OLD [, ticks]): a new evaluate state */
static int l_begin(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TTABLE);
    int has_ticks = lua_type(L, 4) == LUA_TNUMBER;
    double ticks = has_ticks ? lua_tonumber(L, 4) : 0;
    lua_settop(L, 3);
    Pt2Ctx *c = get_ctx(L);
    if (!c->tt_stamp) {
        c->tt_stamp = (uint32_t *)calloc(PT2_MAP_SQ, sizeof(uint32_t));
        c->tt_val = (double *)calloc(PT2_MAP_SQ, sizeof(double));
        c->po_stamp = (uint32_t *)calloc(PT2_MAP_SQ, sizeof(uint32_t));
        c->po_val = (int *)calloc(PT2_MAP_SQ, sizeof(int));
        c->ba_stamp = (uint32_t *)calloc(PT2_MAP_SQ, sizeof(uint32_t));
        if (!c->tt_stamp || !c->tt_val || !c->po_stamp || !c->po_val || !c->ba_stamp)
            return luaL_error(L, "gh_pt2_v2: out of memory");
    }
    nb_init();
    /* 4: the state */
    Ev *e = (Ev *)lua_newuserdata(L, sizeof(Ev));
    memset(e, 0, sizeof(*e));
    if (luaL_newmetatable(L, "gh_pt2_v2_ev_mt")) {
        lua_pushcfunction(L, ev_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
    int st = lua_gettop(L);
    /* 5: the slot table */
    lua_createtable(L, S_TOP, 0);
    int t = lua_gettop(L);
#define SLOT(i) lua_rawseti(L, t, (i))
    lua_pushvalue(L, 1); SLOT(S_INP);
    lua_pushvalue(L, 2); SLOT(S_M);
    lua_pushvalue(L, 3); SLOT(S_OLD);
    lua_getfield(L, 1, "env");
    if (!lua_istable(L, -1)) return luaL_error(L, "gh_pt2_v2.begin: inp.env is not a table");
    int env = lua_gettop(L);
    lua_pushvalue(L, env); SLOT(S_ENV);
    /* the job: inp._v2_cache.c_job, else a new one */
    lua_getfield(L, 1, "_v2_cache");
    int cache = lua_gettop(L);
    int has_cache = lua_istable(L, cache);
    int jobi = 0;
    if (has_cache) {
        lua_getfield(L, cache, "c_job");
        if (lua_type(L, -1) != LUA_TUSERDATA) {
            lua_pop(L, 1);
            job_new(L);
            lua_pushvalue(L, -1);
            lua_setfield(L, cache, "c_job");
        }
    } else job_new(L);
    jobi = lua_gettop(L);
    lua_pushvalue(L, jobi); SLOT(S_JOB);
    lua_getfenv(L, jobi);
    lua_rawgeti(L, -1, 1); SLOT(S_PILLS);
    lua_rawgeti(L, -1, 2); SLOT(S_PIDX);
    lua_pop(L, 1);
    if (has_cache) {
        lua_getfield(L, cache, "sqf");
        SLOT(S_SQF);
    }
    lua_newtable(L); SLOT(S_MEMO);
    static const struct { int env; const char *k; int slot; } fs[] = {
        { 1, "ttype", S_TTYPE }, { 1, "pill_on", S_PILL_ON }, { 1, "is_ally", S_IS_ALLY },
        { 1, "cover_of_pill", S_COVER }, { 1, "base_stops", S_BASE_STOPS },
        { 0, "in_map", S_IN_MAP }, { 0, "stay_in", S_STAY_IN }, { 0, "standable", S_STANDABLE },
        { 0, "buildable", S_BUILDABLE }, { 0, "lgm_reach", S_LGM_REACH }, { 0, "score_fn", S_SCORE_FN },
        { 0, "banned", S_BANNED }, { 0, "anger_of", S_ANGER_OF }, { 0, "chain_skip", S_CHAIN_SKIP },
        { 1, "base_at", S_BASE_AT }, { 1, "count_reads", S_COUNT_READS }, { 0, "lgm_ticks", S_LGM_TICKS },
        { 0, "pill_interval", S_PILL_INTERVAL }, { 0, "v2_wkey", S_WKEY }, { 0, "v2_clock", S_CLK },
        { 0, NULL, 0 }
    };
    for (int i = 0; fs[i].k; i++) {
        lua_getfield(L, fs[i].env ? env : 1, fs[i].k);
        SLOT(fs[i].slot);
    }
    lua_getfield(L, 3, "pill_name"); SLOT(S_PILL_NAME);
    lua_getfield(L, 2, "WM"); SLOT(S_WM);
#undef SLOT
    lua_settop(L, t);
    lua_setfenv(L, st);       /* the state keeps its slots */
    /* run the setup as a step: slots in place */
    lua_pushvalue(L, st);
    lua_replace(L, 1);
    lua_settop(L, 1);
    lua_pushnil(L);
    slots_push(L);
    e->L = L; e->c = c;
    Job *j = (Job *)lua_touserdata(L, S_JOB);
    e->job = j;
    step_reads(e);
    std_setup(e);
    ev_read_inp(e);
    e->sl_on = has_ticks;
    e->sl_ticks = ticks; e->sl_slices = 1;
    lua_getfield(L, S_INP, "A");
    if (!lua_istable(L, -1)) return luaL_error(L, "gh_pt2_v2.begin: inp.A is not a table");
    e->A = pill_index(e, -1);
    lua_pop(L, 1);
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
    e->pcx = A->mx + 0.5; e->pcy = A->my + 0.5;
    e->asets = aim_wedges(e, e->A);
    e->lazy = (!e->keep_all) && e->has_band && e->band > 0;
    e->co = e->chains_only;
    if (e->co) e->lazy = 1;
    step_end(e);
    lua_settop(L, 1);
    return 1;
}

/* gh_pt2_v2.step(st [, paused]): true, result when done; false when the
 * slicer asks for a yield */
static int l_step(lua_State *L) {
    Ev *e = (Ev *)luaL_checkudata(L, 1, "gh_pt2_v2_ev_mt");
    if (e->done) return luaL_error(L, "gh_pt2_v2.step: the evaluate is done");
    if (lua_type(L, 2) == LUA_TNUMBER) e->paused += lua_tonumber(L, 2);
    slots_push(L);
    e->L = L;
    e->c = get_ctx(L);
    e->job = (Job *)lua_touserdata(L, S_JOB);
    if (e->pc != 0) step_reads(e);
    else {
        /* the first step goes on from the setup's reads */
        e->om_sent = 0;
        e->cg_on = cg_ready(e);
    }
    if (!ev_run(e)) { lua_pushboolean(L, 0); return 1; }
    step_end(e);
    push_result(e);
    e->done = 1;
    lua_pushboolean(L, 1);
    lua_insert(L, -2);
    return 2;
}

/* gh_pt2_v2.evaluate(inp, M, OLD) */
static int l_evaluate(lua_State *L) {
    lua_settop(L, 3);
    lua_pushcfunction(L, l_begin);
    lua_insert(L, 1);
    lua_call(L, 3, 1);
    lua_pushcfunction(L, l_step);
    lua_insert(L, 1);
    lua_call(L, 1, 2);
    return 1;
}

/* gh_pt2_v2.slices(st): the slicer's record { slices, wu = {}, over, budget } */
static int l_slices(lua_State *L) {
    Ev *e = (Ev *)luaL_checkudata(L, 1, "gh_pt2_v2_ev_mt");
    lua_createtable(L, 0, 4);
    setnum(L, "slices", e->sl_slices);
    setnum(L, "over", e->sl_over);
    if (e->sl_has_budget) setnum(L, "budget", e->sl_budget);
    lua_createtable(L, (int)e->sl_slices, 0);
    for (int i = 1; i <= (int)e->sl_slices && i < 64; i++) {
        lua_pushnumber(L, e->sl_wu[i]);
        lua_rawseti(L, -2, i);
    }
    lua_setfield(L, -2, "wu");
    return 1;
}

static const luaL_Reg gh_pt2_v2_lib[] = {
    { "begin", l_begin },
    { "step", l_step },
    { "evaluate", l_evaluate },
    { "slices", l_slices },
    { NULL, NULL }
};

void naPt2V2Register(lua_State *L) {
    Pt2Ctx *c = (Pt2Ctx *)lua_newuserdata(L, sizeof(Pt2Ctx));
    memset(c, 0, sizeof(*c));
    if (luaL_newmetatable(L, "gh_pt2_v2_ctx_mt")) {
        lua_pushcfunction(L, ctx_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, "gh_pt2_v2_ctx");
    luaL_newlib(L, gh_pt2_v2_lib);
    lua_setglobal(L, "gh_pt2_v2");
}
