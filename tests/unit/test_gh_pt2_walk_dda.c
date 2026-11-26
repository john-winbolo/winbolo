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
 * test_gh_pt2_walk_dda.c -- the square change ticks of the pill take v2
 * tank shot walk made by adds (Pt2wAxis in
 * brains/GoalHunter/c/gh_pt2_walk.h) against the division way
 * (pt2w_next2, the walk before) and the per-tick integer walk
 * (walk_step's fast path).
 *
 * For each walk, on every tick of the life: the ticks both ways visit
 * are the same, they are exactly tick 1 and the ticks where the stepped
 * square changes, and the squares at those ticks are the stepped
 * squares (unsigned shift, as walk_step). Hand-made cases: starts on
 * square edges and corners, zero x or y speed, all 8 directions at
 * speeds below, at and above one square per tick, starts below 0 and
 * near the 2^30 limit, lives of 0, 1, 2, 3, 64, 128 and PT2W_MAX_T. Then
 * the 256 real shell angles from every start offset in a square, and
 * random walks.
 */

#include "test_harness.h"
#include "gh_pt2_walk.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>

static int s_bad, s_walks;
static long long s_events;

typedef struct { int32_t x, c, s; } Axis;
static void ax_step(Axis *a) {
    a->c += a->s;
    int32_t m = a->c >> 8;
    a->c -= m * 256;
    a->x += m;
}
static int32_t sq_of(int32_t x) { return (int32_t)((uint32_t)x >> 8); }

static void fail(const char *what, int32_t x0, int32_t y0, int32_t xs, int32_t ys, int32_t n, int32_t t) {
    if (s_bad++ < 10)
        fprintf(stderr, "FAIL %s x0=%d y0=%d xs=%d ys=%d n=%d t=%d\n", what, x0, y0, xs, ys, n, t);
}

static void check_walk(int32_t x0, int32_t y0, int32_t xs, int32_t ys, int32_t n) {
    Axis sx = { x0, 0, xs }, sy = { y0, 0, ys };
    Pt2wAxis ax, ay;
    pt2w_ax_init(&ax, x0, xs);
    pt2w_ax_init(&ay, y0, ys);
    int32_t ta = 1, tb = 1; /* the next tick the division way / the add way visits */
    int32_t px = 0, py = 0;
    s_walks++;
    for (int32_t t = 1; t <= n; t++) {
        ax_step(&sx);
        ax_step(&sy);
        int32_t qx = sq_of(sx.x), qy = sq_of(sy.x);
        int event = t == 1 || qx != px || qy != py;
        px = qx; py = qy;
        if (event != (t == ta)) { fail("div tick", x0, y0, xs, ys, n, t); return; }
        if (event != (t == tb)) { fail("add tick", x0, y0, xs, ys, n, t); return; }
        if (!event) continue;
        s_events++;
        if (pt2w_ax_sq(&ax) != qx || pt2w_ax_sq(&ay) != qy) { fail("add square", x0, y0, xs, ys, n, t); return; }
        if (sq_of(pt2w_pos(x0, xs, t)) != qx || sq_of(pt2w_pos(y0, ys, t)) != qy) {
            fail("div square", x0, y0, xs, ys, n, t);
            return;
        }
        ta = pt2w_next2(x0, xs, y0, ys, t);
        tb = ax.k < ay.k ? ax.k : ay.k;
        if (ta != tb) { fail("next tick", x0, y0, xs, ys, n, t); return; }
        if (tb <= n) pt2w_ax_pass(&ax, &ay, tb);
    }
}

static double rnd(double x) { return x >= 0 ? floor(x + 0.5) : -floor(-x + 0.5); }
static void shell_step(int a, int32_t *xs, int32_t *ys) {
    double f = (double)(float)(a - 64);
    if (f < 0) f = (double)(float)(f + 256.0);
    double db = ((360.0 / 256.0) * f / 360.0) * (2.0 * 3.14159);
    *xs = (int32_t)rnd(32.0 * cos(db) * 256.0);
    *ys = (int32_t)rnd(32.0 * sin(db) * 256.0);
}

static uint32_t s_rng = 424242u;
static uint32_t rng(void) { s_rng = s_rng * 1664525u + 1013904223u; return s_rng >> 8; }

int run_gh_pt2_walk_dda(void) {
    s_bad = 0; s_walks = 0; s_events = 0;
    /* Offsets inside a square: on the low edge, next to it, the middle
     * and the high edge (both axes: corners too). */
    static const int32_t offs[] = { 0, 1, 2, 127, 128, 129, 254, 255 };
    /* Bases: the map middle, square 0, below 0, near the 2^30 limit. */
    static const int32_t bases[] = { 126 * 256, 0, -256, -3 * 256, 1073741824 - 64 * 256, -1073741824 + 64 * 256 };
    /* Speeds (1/256 world units per tick): below, at and above one
     * square per tick (65536), the real shell speed (8192) and its
     * diagonal (5793), the walk's fast path limit. */
    static const int32_t speeds[] = { 1, 2, 3, 7, 255, 256, 257, 1000, 5793, 8191, 8192, 8193,
                                      65535, 65536, 65537, 100000, 1048575 };
    static const int32_t lives[] = { 0, 1, 2, 3, 64, 128, PT2W_MAX_T };
    static const int dirs[8][2] = { { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 } };
    int no = (int)(sizeof(offs) / sizeof(offs[0])), nb = (int)(sizeof(bases) / sizeof(bases[0]));
    int nsp = (int)(sizeof(speeds) / sizeof(speeds[0])), nl = (int)(sizeof(lives) / sizeof(lives[0]));
    for (int d = 0; d < 8; d++)
        for (int sp = 0; sp < nsp; sp++)
            for (int b = 0; b < nb; b++)
                for (int ox = 0; ox < no; ox++)
                    for (int oy = 0; oy < no; oy++)
                        for (int l = 0; l < nl; l++) {
                            int32_t n = lives[l];
                            int32_t s = speeds[sp];
                            /* inside the stated limits: |x| < 2^30 over the life */
                            if ((b >= 4) && (int64_t)n * s / 256 > 60 * 256) continue;
                            if (n == PT2W_MAX_T && (ox | oy) != 0 && ox != no - 1) continue;
                            check_walk(bases[b] + offs[ox], bases[(b + 1) % 4] + offs[oy],
                                       dirs[d][0] * s, dirs[d][1] * s, n);
                        }
    /* One axis still, the other at every speed, both ways, from every offset. */
    for (int sp = 0; sp < nsp; sp++)
        for (int o = 0; o < 256; o++) {
            check_walk(126 * 256 + o, 118 * 256 + o, speeds[sp], 0, 128);
            check_walk(126 * 256 + o, 118 * 256 + o, -speeds[sp], 0, 128);
            check_walk(126 * 256 + o, 118 * 256 + o, 0, speeds[sp], 128);
            check_walk(126 * 256 + o, 118 * 256 + o, 0, -speeds[sp], 128);
        }
    /* No speed at all. */
    for (int o = 0; o < 256; o += 17) check_walk(126 * 256 + o, 118 * 256 + o, 0, 0, 64);
    /* The 256 shell angles from every start offset inside a square. */
    for (int ang = 0; ang < 256; ang++) {
        int32_t xs, ys;
        shell_step(ang, &xs, &ys);
        for (int o = 0; o < 256; o++) {
            check_walk(126 * 256 + o, 118 * 256 + (255 - o), xs, ys, 64);
            check_walk(126 * 256 + o, 118 * 256 + o, xs, ys, 50);
        }
    }
    /* Random walks. */
    for (int i = 0; i < 200000; i++) {
        int32_t x0 = (int32_t)(rng() % 131072) - 32768, y0 = (int32_t)(rng() % 131072) - 32768;
        int32_t xs = (int32_t)(rng() % 200001) - 100000, ys = (int32_t)(rng() % 200001) - 100000;
        if (i % 4 == 0) xs = 0;
        if (i % 7 == 0) xs = (xs / 256) * 256;
        if (i % 9 == 0) ys = (ys / 65536) * 65536;
        if (i % 3 == 0) { xs /= 16; ys /= 16; }
        int32_t n = (int32_t)(rng() % 130);
        if (i % 1000 == 0) n = PT2W_MAX_T;
        check_walk(x0, y0, xs, ys, n);
    }
    printf("gh_pt2_walk_dda: %d walks, %lld change ticks compared, %d bad\n", s_walks, s_events, s_bad);
    return s_bad ? 1 : 0;
}
