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

#endif /* GH_PT2_WALK_H */
