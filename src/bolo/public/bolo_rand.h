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

/* src/bolo/public/bolo_rand.h */

#ifndef BOLO_RAND_H
#define BOLO_RAND_H

#include <stdint.h>

/*
 * Process-wide deterministic PRNG. Same seed -> same sequence on every
 * platform (Linux, macOS, Windows, iOS, Android, WASM). Replaces libc
 * rand()/srand() everywhere outside src/third_party/.
 *
 * Not thread-safe. Not cryptographic.
 */

void     bolo_srand(uint64_t seed);
uint32_t bolo_rand(void);                /* full 32-bit value */
uint32_t bolo_rand_below(uint32_t n);    /* unbiased [0, n); n > 0 */

/* Snapshot of the generator state, for save/restore around draws that must not
 * perturb the deterministic stream — e.g. cosmetic bot naming, which should
 * not shift tank placement for a given seed. Save before the draws, restore
 * after, and the shared sequence resumes exactly where it was left. */
typedef struct { uint32_t s[4]; } BoloRandState;
void     bolo_rand_save(BoloRandState *out);
void     bolo_rand_restore(const BoloRandState *in);

#endif /* BOLO_RAND_H */
