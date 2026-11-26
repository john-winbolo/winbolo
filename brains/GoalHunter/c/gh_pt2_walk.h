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
 * gh_pt2_walk.h -- square jumps for the integer shell walk of gh_pt2_v2.c.
 *
 * walk_step's fast path moves one axis like this, from x0 with step xs
 * (1/256 world units per tick) and a carry c that starts at 0:
 *
 *     c += xs;  m = c >> 8;  c -= m * 256;  x += m;
 *
 * After each tick x * 256 + c == x0 * 256 + k * xs with 0 <= c < 256, so
 * after k ticks x == x0 + floor(k * xs / 256). That is pt2w_pos.
 *
 * The square on the axis is x >> 8 (walk_step uses the unsigned shift,
 * which changes at the same ticks: 2^32 is a multiple of 256).
 * pt2w_next gives the first tick after t at which the square changes, so
 * a walk that only does work on a new square can jump from one such tick
 * to the next instead of stepping every tick.
 *
 * Exact for |x0| < 2^30, |xs| < 2^20 and 0 <= t, k <= PT2W_MAX_T (the
 * limits walk_init's fast path and the caller check).
 */
#ifndef GH_PT2_WALK_H
#define GH_PT2_WALK_H

#include <stdint.h>

#define PT2W_MAX_T 4096
#define PT2W_NEVER INT32_MAX

typedef char pt2w_arith_shift64_ok[(((int64_t)-256 >> 8) == -1) ? 1 : -1];

/* The axis position after k ticks: x0 + floor(k * xs / 256). */
static inline int32_t pt2w_pos(int32_t x0, int32_t xs, int32_t k) {
    return x0 + (int32_t)(((int64_t)k * xs) >> 8);
}

/* The first tick k > t at which pt2w_pos(k) >> 8 differs from
 * pt2w_pos(t) >> 8, or PT2W_NEVER when xs == 0. */
static inline int32_t pt2w_next(int32_t x0, int32_t xs, int32_t t) {
    if (xs == 0) return PT2W_NEVER;
    int64_t q = (int64_t)(pt2w_pos(x0, xs, t) >> 8);
    if (xs > 0) {
        /* Up: floor(k * xs / 256) >= d, d = (q + 1) * 256 - x0 > 0. */
        int64_t d = (q + 1) * 256 - x0;
        return (int32_t)((256 * d + xs - 1) / xs);
    }
    /* Down: x(k) < q * 256, that is k * -xs > m, m = 256 * (x0 - q * 256) >= 0. */
    int64_t m = 256 * ((int64_t)x0 - q * 256);
    return (int32_t)(m / -(int64_t)xs + 1);
}

/* The first tick k > t at which either axis changes square. */
static inline int32_t pt2w_next2(int32_t x0, int32_t xs, int32_t y0, int32_t ys, int32_t t) {
    int32_t kx = pt2w_next(x0, xs, t), ky = pt2w_next(y0, ys, t);
    return kx < ky ? kx : ky;
}

/*
 * The same change ticks without a division per change (step without
 * dividing). One axis keeps its square q (pt2w_pos >> 8, arithmetic)
 * and k, the next tick the square changes. pt2w_next depends on t only
 * through q, and each change moves q by one square, so the next k
 * follows from the last one by adding a fixed step:
 *
 * Up (xs > 0): k = ceil(N / xs), N = 256 * d. The next square edge adds
 * 65536 to N. With k * xs - N = r (0 <= r < xs) and 65536 = qa * xs + ra,
 * the next k is k + qa + (r < ra) and r becomes r - ra (+ xs if r < ra).
 *
 * Down (xs < 0, s = -xs): k = floor(m / s) + 1. The next edge adds 65536
 * to m. With m % s = u, the next k is k + qa + (u + ra >= s). Keeping
 * r = s - 1 - u gives the same step as up: k + qa + (r < ra), and
 * r becomes r - ra (+ s if r < ra).
 *
 * More than one square change on one tick (|xs| > 65536) gives the same
 * k twice; the walk steps while k is the tick. The setup is the only
 * division (one for k, one for qa and ra). The values stay well inside
 * int32 for |x0| < 2^30, |xs| <= 2^20 and t <= PT2W_MAX_T.
 */
typedef struct {
    int32_t q;      /* the square on this axis, pt2w_pos >> 8 (arithmetic) */
    int32_t k;      /* the next tick the square changes, or PT2W_NEVER */
    int32_t r;      /* the remainder (see above) */
    int32_t s;      /* |xs| */
    int32_t qa, ra; /* 65536 / s, 65536 % s */
    int32_t dir;    /* +1, -1, or 0 for xs == 0 */
} Pt2wAxis;

/* The axis at tick 1 (the walk's first tick). */
static inline void pt2w_ax_init(Pt2wAxis *a, int32_t x0, int32_t xs) {
    a->q = pt2w_pos(x0, xs, 1) >> 8;
    if (xs == 0) {
        a->k = PT2W_NEVER; a->r = 0; a->s = 1; a->qa = 0; a->ra = 0; a->dir = 0;
        return;
    }
    if (xs > 0) {
        int32_t n = (int32_t)(256 * (((int64_t)a->q + 1) * 256 - x0));
        a->s = xs; a->dir = 1;
        a->k = (n + xs - 1) / xs;
        a->r = a->k * xs - n;
    } else {
        int32_t s = -xs;
        int32_t m = (int32_t)(256 * ((int64_t)x0 - (int64_t)a->q * 256));
        a->s = s; a->dir = -1;
        a->k = m / s + 1;
        a->r = s - 1 - m % s;
    }
    a->qa = 65536 / a->s; a->ra = 65536 % a->s;
}

/* The square changes at tick a->k: move q one square and find the next k. */
static inline void pt2w_ax_step(Pt2wAxis *a) {
    int32_t c = a->r < a->ra;
    a->q += a->dir;
    a->k += a->qa + c;
    a->r += (c ? a->s : 0) - a->ra;
}

/* The square index walk_step gives: the unsigned shift of the position,
 * that is q modulo 2^24. */
static inline int32_t pt2w_ax_sq(const Pt2wAxis *a) { return (int32_t)((uint32_t)a->q & 0xFFFFFFu); }

/* pt2w_ax_step when a->k == t, else no change, with no branch: which
 * axis changes square is hard to guess, so a branch there costs more
 * than the step. */
static inline void pt2w_ax_step_at(Pt2wAxis *a, int32_t t) {
    int32_t m = -(int32_t)(a->k == t);
    int32_t c = a->r < a->ra;
    a->q += a->dir & m;
    a->k += (a->qa + c) & m;
    a->r += ((a->s & -c) - a->ra) & m;
}

/* Past tick t on both axes (t is the smaller k; t <= PT2W_MAX_T). After
 * one step k is still t only when one tick crosses more than one square
 * edge (|xs| > 65536). */
static inline void pt2w_ax_pass(Pt2wAxis *ax, Pt2wAxis *ay, int32_t t) {
    pt2w_ax_step_at(ax, t);
    pt2w_ax_step_at(ay, t);
    while (ax->k == t) pt2w_ax_step(ax);
    while (ay->k == t) pt2w_ax_step(ay);
}

#include <string.h>

/* The bits of a double: the keys below compare bit for bit (negative
 * zero is not zero, a NaN is only its own bits). */
static inline uint64_t pt2w_bits(double d) { uint64_t u; memcpy(&u, &d, sizeof(u)); return u; }

/*
 * The walk reuse key. A walk's result (stop reason) depends on its
 * origin, f32 angle, target square, extra list, the step's map reads
 * (gen) and the evaluate's shell and tank inputs (fixed for one
 * evaluate). The reuse takes an earlier result only when every field
 * here is the same, bit for bit, and the earlier walk was the last walk
 * made (serial) and made no Lua read that is not kept in the step's
 * stamp caches.
 */
typedef struct {
    uint64_t ox, oy, angle, tmx, tmy;
    const void *extra;
    int      extra_n;
    uint32_t gen;
    uint64_t serial;
} Pt2wKey;

static inline int pt2w_key_same(const Pt2wKey *a, const Pt2wKey *b) {
    return a->ox == b->ox && a->oy == b->oy && a->angle == b->angle && a->tmx == b->tmx
           && a->tmy == b->tmy && a->extra == b->extra && a->extra_n == b->extra_n
           && a->gen == b->gen && a->serial == b->serial;
}

#endif /* GH_PT2_WALK_H */
