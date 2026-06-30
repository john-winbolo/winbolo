/*
 * $Id$
 *
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * xoshiro128++ by Blackman & Vigna, public domain
 * (https://prng.di.unimi.it/).
 * SplitMix64 by Vigna, public domain.
 */


/*********************************************************
*Name:          Bolo Rand
*Filename:      bolo_rand.c
*Author:        John Morrison
*Purpose:
*  Process-wide deterministic PRNG (xoshiro128++ seeded by
*  SplitMix64). Replaces the libc PRNG (see bolo_rand.h) so the
*  same seed produces the same byte stream on every platform we
*  ship.
*  Pure uint32_t/uint64_t arithmetic — no signed math, no float,
*  no implementation-defined behaviour.
*********************************************************/

#include <stddef.h>   /* NULL */
#include <stdint.h>

#include "bolo_rand.h"

/* xoshiro128++ state: four 32-bit words. SplitMix64 seeding guarantees
 * at least one of these is non-zero, so the all-zero trap can't fire. */
static uint32_t s_state[4];

static inline uint32_t rotl(uint32_t x, int k) {
    return (x << k) | (x >> (32 - k));
}

static uint64_t splitmix64_next(uint64_t *z) {
    *z += 0x9e3779b97f4a7c15ULL;
    uint64_t y = *z;
    y = (y ^ (y >> 30)) * 0xbf58476d1ce4e5b9ULL;
    y = (y ^ (y >> 27)) * 0x94d049bb133111ebULL;
    return y ^ (y >> 31);
}

void bolo_srand(uint64_t seed) {
    uint64_t z = seed;
    s_state[0] = (uint32_t)splitmix64_next(&z);
    s_state[1] = (uint32_t)splitmix64_next(&z);
    s_state[2] = (uint32_t)splitmix64_next(&z);
    s_state[3] = (uint32_t)splitmix64_next(&z);

    /* SplitMix64 essentially never produces four consecutive low-32-bit
     * zeros from any seed, but the xoshiro128++ contract forbids the
     * all-zero state regardless — keep the guard so the API has no trap. */
    if ((s_state[0] | s_state[1] | s_state[2] | s_state[3]) == 0) {
        s_state[0] = 1;
    }
}

uint32_t bolo_rand(void) {
    const uint32_t result = rotl(s_state[0] + s_state[3], 7) + s_state[0];
    const uint32_t t = s_state[1] << 9;

    s_state[2] ^= s_state[0];
    s_state[3] ^= s_state[1];
    s_state[1] ^= s_state[2];
    s_state[0] ^= s_state[3];

    s_state[2] ^= t;
    s_state[3] = rotl(s_state[3], 11);

    return result;
}

uint32_t bolo_rand_below(uint32_t n) {
    /* Reject values that would skew the distribution. Pure integer
     * math, no float, portable. */
    uint32_t limit = UINT32_MAX - (UINT32_MAX % n);
    uint32_t r;
    do { r = bolo_rand(); } while (r >= limit);
    return r % n;
}

void bolo_rand_save(BoloRandState *out) {
    if (out == NULL) return;
    out->s[0] = s_state[0];
    out->s[1] = s_state[1];
    out->s[2] = s_state[2];
    out->s[3] = s_state[3];
}

void bolo_rand_restore(const BoloRandState *in) {
    if (in == NULL) return;
    s_state[0] = in->s[0];
    s_state[1] = in->s[1];
    s_state[2] = in->s[2];
    s_state[3] = in->s[3];
}
