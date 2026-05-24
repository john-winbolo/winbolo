/*
 * Golden-sequence test for the portable PRNG in src/bolo/bolo_rand.c.
 *
 * The whole point of bolo_rand is cross-platform bit-identical output:
 * libc rand()/srand() produces a different stream on Linux, macOS,
 * Windows, Android, iOS and WASM, so any seeded game-sim test that
 * touches the RNG diverges by platform. xoshiro128++ + SplitMix64 is
 * pure uint32_t / uint64_t arithmetic with zero implementation-defined
 * behaviour, so the byte stream must be identical everywhere.
 *
 * This test seeds with 0xDEADBEEF and asserts the first 16 outputs
 * against constants computed from an independent reference (the
 * xoshiro128++ and SplitMix64 algorithms as published at
 * https://prng.di.unimi.it/). If a port to a new platform regresses
 * the byte stream, this test fails on byte 0 — long before any
 * downstream sim test fails mysteriously.
 */

#include <stdint.h>

#include "bolo_rand.h"
#include "test_harness.h"

int run_bolo_rand_golden_sequence(void) {
    static const uint32_t kExpected[16] = {
        0xBE73818DU,
        0x521502D3U,
        0x7867C9E0U,
        0x8532A6CFU,
        0x22E0EC92U,
        0xC3985842U,
        0xAB1C1EA0U,
        0x13F631FAU,
        0x6C79F463U,
        0x11093AEEU,
        0x72269873U,
        0x1A1FCE1CU,
        0xAFA1C84AU,
        0xED33CC9DU,
        0x8AD3667CU,
        0xDE629127U,
    };

    bolo_srand(0xDEADBEEFULL);
    for (int i = 0; i < 16; i++) {
        uint32_t got = bolo_rand();
        UT_ASSERT_MSG(got == kExpected[i],
                      "bolo_rand()[%d] = 0x%08X, expected 0x%08X",
                      i, got, kExpected[i]);
    }
    return 0;
}
