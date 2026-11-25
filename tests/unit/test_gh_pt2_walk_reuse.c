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
 * test_gh_pt2_walk_reuse.c -- the walk reuse key of the pill take v2 tank
 * shot walk (brains/GoalHunter/c/gh_pt2_walk.h).
 *
 * gh_pt2_walk_reuse: pt2w_key_same is true only when every field is the
 * same, bit for bit (a change of one bit in any field, negative zero
 * against zero, another NaN, another extra list or count, another step
 * or a walk made in between all stop the reuse).
 */

#include "test_harness.h"
#include "gh_pt2_walk.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static double t_f32(double x) { return (double)(float)x; }
static double f_of_bits(uint32_t u) { float f; memcpy(&f, &u, sizeof(f)); return (double)f; }

static void flip(uint64_t *u, int bit) { *u ^= (uint64_t)1 << bit; }

int run_gh_pt2_walk_reuse(void) {
    int bad = 0, cases = 0;
    static int x1, x2;
    Pt2wKey base;
    memset(&base, 0, sizeof(base));
    base.ox = pt2w_bits(32123.0); base.oy = pt2w_bits(30200.5);
    base.angle = pt2w_bits(t_f32(17.3125)); base.tmx = pt2w_bits(126.0); base.tmy = pt2w_bits(118.0);
    base.extra = &x1; base.extra_n = 3; base.gen = 77; base.serial = 1234;
    /* the same key */
    { Pt2wKey k = base; cases++; if (!pt2w_key_same(&k, &base)) { bad++; fprintf(stderr, "FAIL reuse: same key not same\n"); } }
    /* one bit changed in each double field */
    for (int f = 0; f < 5; f++)
        for (int bit = 0; bit < 64; bit++) {
            Pt2wKey k = base;
            uint64_t *u = f == 0 ? &k.ox : f == 1 ? &k.oy : f == 2 ? &k.angle : f == 3 ? &k.tmx : &k.tmy;
            flip(u, bit);
            cases++;
            if (pt2w_key_same(&k, &base)) { if (bad++ < 10) fprintf(stderr, "FAIL reuse: field %d bit %d\n", f, bit); }
        }
    /* zero against negative zero, NaN against another NaN */
    {
        Pt2wKey a = base, b = base;
        a.ox = pt2w_bits(0.0); b.ox = pt2w_bits(-0.0);
        cases++; if (pt2w_key_same(&a, &b)) { bad++; fprintf(stderr, "FAIL reuse: 0 and -0\n"); }
        a = base; b = base;
        a.angle = pt2w_bits(f_of_bits(0x7fc00000u)); b.angle = pt2w_bits(f_of_bits(0x7fc00001u));
        cases++; if (pt2w_key_same(&a, &b)) { bad++; fprintf(stderr, "FAIL reuse: two NaNs\n"); }
        b.angle = a.angle;
        cases++; if (!pt2w_key_same(&a, &b)) { bad++; fprintf(stderr, "FAIL reuse: the same NaN bits\n"); }
    }
    /* the extra list, its count, the step and the walk serial */
    {
        Pt2wKey k;
        k = base; k.extra = &x2;  cases++; if (pt2w_key_same(&k, &base)) { bad++; fprintf(stderr, "FAIL reuse: extra\n"); }
        k = base; k.extra = NULL; cases++; if (pt2w_key_same(&k, &base)) { bad++; fprintf(stderr, "FAIL reuse: NULL extra\n"); }
        k = base; k.extra_n = 4;  cases++; if (pt2w_key_same(&k, &base)) { bad++; fprintf(stderr, "FAIL reuse: extra count\n"); }
        k = base; k.gen = 78;     cases++; if (pt2w_key_same(&k, &base)) { bad++; fprintf(stderr, "FAIL reuse: step\n"); }
        k = base; k.serial = 1235; cases++; if (pt2w_key_same(&k, &base)) { bad++; fprintf(stderr, "FAIL reuse: a walk between\n"); }
    }
    printf("gh_pt2_walk_reuse: %d cases, %d bad\n", cases, bad);
    return bad ? 1 : 0;
}
