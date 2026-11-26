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
 * test_gh_pt2_walk_jump.c -- the square jump math of the pill take v2
 * tank shot walk (brains/GoalHunter/c/gh_pt2_walk.h) against the
 * per-tick integer walk (gh_pt2_v2.c walk_step's fast path).
 *
 * For each walk: pt2w_pos is the stepped position on every tick, and the
 * ticks the jump visits (tick 1, then pt2w_next2 until past the life)
 * are exactly tick 1 and the ticks where the stepped square changes on
 * either axis. Cases: axis-aligned, negative steps, starts on and next
 * to square edges, off-map starts, steps of more than one square per
 * tick, lives of 0, 1 and 2 ticks and the longest life (PT2W_MAX_T), the
 * 256 real shell angles, and random walks.
 */

#include "test_harness.h"
#include "gh_pt2_walk.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>

static int s_bad, s_walks;

/* walk_step's fast path, one axis. */
typedef struct { int32_t x, c, s; } Axis;
static void ax_step(Axis *a) {
    a->c += a->s;
    int32_t m = a->c >> 8;
    a->c -= m * 256;
    a->x += m;
}
static int32_t sq_of(int32_t x) { return (int32_t)((uint32_t)x >> 8); }

static void check_walk(int32_t x0, int32_t y0, int32_t xs, int32_t ys, int32_t n) {
    Axis ax = { x0, 0, xs }, ay = { y0, 0, ys };
    int32_t jt = 1; /* the next tick the jump visits */
    int32_t px = 0, py = 0;
    s_walks++;
    for (int32_t t = 1; t <= n; t++) {
        ax_step(&ax);
        ax_step(&ay);
        if (pt2w_pos(x0, xs, t) != ax.x || pt2w_pos(y0, ys, t) != ay.x) {
            if (s_bad++ < 10)
                fprintf(stderr, "FAIL pos x0=%d y0=%d xs=%d ys=%d t=%d: stepped (%d,%d) pos (%d,%d)\n",
                        x0, y0, xs, ys, t, ax.x, ay.x, pt2w_pos(x0, xs, t), pt2w_pos(y0, ys, t));
            return;
        }
        int32_t sx = sq_of(ax.x), sy = sq_of(ay.x);
        int event = t == 1 || sx != px || sy != py;
        px = sx; py = sy;
        if (event != (t == jt)) {
            if (s_bad++ < 10)
                fprintf(stderr, "FAIL jump x0=%d y0=%d xs=%d ys=%d n=%d t=%d: square change %d, jump tick %d\n",
                        x0, y0, xs, ys, n, t, event, jt);
            return;
        }
        if (event) {
            int32_t nt = pt2w_next2(x0, xs, y0, ys, t);
            if (nt <= t) {
                if (s_bad++ < 10)
                    fprintf(stderr, "FAIL next x0=%d y0=%d xs=%d ys=%d t=%d: next %d\n", x0, y0, xs, ys, t, nt);
                return;
            }
            jt = nt;
        }
    }
}

/* The shell step of brad angle a at speed 32, as calc_distance_both. */
static double rnd(double x) { return x >= 0 ? floor(x + 0.5) : -floor(-x + 0.5); }
static void shell_step(int a, int32_t *xs, int32_t *ys) {
    double f = (double)(float)(a - 64);
    if (f < 0) f = (double)(float)(f + 256.0);
    double db = ((360.0 / 256.0) * f / 360.0) * (2.0 * 3.14159);
    *xs = (int32_t)rnd(32.0 * cos(db) * 256.0);
    *ys = (int32_t)rnd(32.0 * sin(db) * 256.0);
}

static uint32_t s_rng = 12345u;
static uint32_t rng(void) { s_rng = s_rng * 1664525u + 1013904223u; return s_rng >> 8; }

int run_gh_pt2_walk_jump(void) {
    s_bad = 0; s_walks = 0;
    static const int32_t starts[] = { 0, 1, 127, 128, 255, 256, 257, 511, 512, 32255, 32256, 32257,
                                      65535, 65536, -1, -255, -256, -257, 1073741823 - 70000, -1073741823 + 70000 };
    static const int32_t steps[] = { 0, 1, -1, 255, -255, 256, -256, 257, -257, 8192, -8192, 8191, -8191,
                                     8193, -8193, 5793, -5793, 65536, -65536, 70001, -70001, 1048575, -1048575 };
    static const int32_t lives[] = { 0, 1, 2, 3, 7, 50, 64, 128, PT2W_MAX_T };
    int ns = (int)(sizeof(starts) / sizeof(starts[0])), nk = (int)(sizeof(steps) / sizeof(steps[0]));
    int nn = (int)(sizeof(lives) / sizeof(lives[0]));
    /* Edges, axis-aligned and negative steps, every life. */
    for (int a = 0; a < ns; a++)
        for (int b = 0; b < nk; b++)
            for (int c = 0; c < nk; c++)
                for (int d = 0; d < nn; d++) {
                    int32_t n = lives[d];
                    /* keep inside the stated limits: |x| < 2^30 over the life */
                    if ((int64_t)n * (steps[b] < 0 ? -steps[b] : steps[b]) / 256 > 60000 && n > 128) continue;
                    if ((int64_t)n * (steps[c] < 0 ? -steps[c] : steps[c]) / 256 > 60000 && n > 128) continue;
                    check_walk(starts[a], starts[(a * 7 + 3) % ns], steps[b], steps[c], n);
                }
    /* The 256 shell angles from every start offset inside a square, life 50 and 64. */
    for (int ang = 0; ang < 256; ang++) {
        int32_t xs, ys;
        shell_step(ang, &xs, &ys);
        for (int o = 0; o < 256; o += 5) {
            check_walk(126 * 256 + o, 118 * 256 + (255 - o), xs, ys, 50);
            check_walk(126 * 256 + o, 118 * 256 + o, xs, ys, 64);
        }
    }
    /* Random walks. */
    for (int i = 0; i < 200000; i++) {
        int32_t x0 = (int32_t)(rng() % 131072) - 32768, y0 = (int32_t)(rng() % 131072) - 32768;
        int32_t xs = (int32_t)(rng() % 40001) - 20000, ys = (int32_t)(rng() % 40001) - 20000;
        if (i % 4 == 0) xs = 0;
        if (i % 9 == 0) ys = (ys / 256) * 256;
        int32_t n = (int32_t)(rng() % 130);
        if (i % 1000 == 0) n = PT2W_MAX_T;
        check_walk(x0, y0, xs, ys, n);
    }
    printf("gh_pt2_walk_jump: %d walks, %d bad\n", s_walks, s_bad);
    return s_bad ? 1 : 0;
}
