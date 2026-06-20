/*
 * Tests for the vendored MD5 implementation in src/bolo/md5.c.  The
 * MD5 helper underpins the PACKET_LOBBY_MAP_USE_LOCAL handshake — if
 * client and server disagree on the digest of the same bytes the
 * "use local file, skip upload" optimisation silently breaks, so the
 * algorithm correctness checks here are load-bearing for that flow.
 *
 * Three tests:
 *   - md5_rfc1321_vectors:       all seven test vectors from
 *                                RFC 1321 appendix A.5 (covers padding,
 *                                multi-block input, the boundary cases
 *                                around 56/64-byte buffers).
 *   - md5_streaming_matches_oneshot:
 *                                splitting input across multiple
 *                                md5Update calls (including 1-byte
 *                                pathological chunks) gives the same
 *                                digest as one-shot md5Compute.
 *   - md5_block_boundaries:      explicit coverage of inputs that sit
 *                                right on the 56-/64-byte buffer
 *                                boundaries inside md5Update — these
 *                                are where padding bugs typically
 *                                surface.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common/md5.h"
#include "test_harness.h"

/* Seven RFC 1321 test vectors. Inputs deliberately span the padding
 * boundaries (empty, 1 byte, multi-byte non-aligned, the 80-byte
 * vector that crosses the 64-byte block boundary). */
int run_md5_rfc1321_vectors(void) {
    struct {
        const char *input;
        const char *expectedHex;
    } cases[] = {
        { "",
          "d41d8cd98f00b204e9800998ecf8427e" },
        { "a",
          "0cc175b9c0f1b6a831c399e269772661" },
        { "abc",
          "900150983cd24fb0d6963f7d28e17f72" },
        { "message digest",
          "f96b697d7cb7938d525a2f31aaf161d0" },
        { "abcdefghijklmnopqrstuvwxyz",
          "c3fcd3d76192e4007dfb496cca67e13b" },
        { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
          "d174ab98d277d9f5a5611c2c9f419d9f" },
        { "12345678901234567890123456789012345678901234567890"
          "123456789012345678901234567890",
          "57edf4a22be3c955ac49da2e2107b67a" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t digest[16];
        char gotHex[33];
        md5Compute(cases[i].input, strlen(cases[i].input), digest);
        md5ToHex(digest, gotHex);
        UT_ASSERT_MSG(strcmp(gotHex, cases[i].expectedHex) == 0,
                      "input='%s': expected %s, got %s",
                      cases[i].input, cases[i].expectedHex, gotHex);
    }
    return 0;
}

/* Streaming vs one-shot for several chunk sizes.  The chunk loop
 * exercises md5Update's "less than buffer space remaining" branch
 * repeatedly; if any of the bit-count / buffer-position bookkeeping
 * is off the digest will diverge from the one-shot path. */
int run_md5_streaming_matches_oneshot(void) {
    /* Build a non-trivial buffer that crosses several block boundaries
     * with varied content (so a buggy memcpy of the wrong region
     * surfaces visibly). 250 bytes = 3 full 64-byte blocks + 58
     * trailing bytes, so the padding logic fires on the tail. */
    uint8_t buf[250];
    for (int i = 0; i < (int)sizeof(buf); i++) {
        buf[i] = (uint8_t)((i * 37 + 11) & 0xFF);
    }

    uint8_t expected[16];
    md5Compute(buf, sizeof(buf), expected);

    const size_t chunkSizes[] = { 1, 7, 17, 63, 64, 65, 128 };
    for (size_t c = 0; c < sizeof(chunkSizes) / sizeof(chunkSizes[0]); c++) {
        size_t chunk = chunkSizes[c];
        Md5Ctx ctx;
        md5Init(&ctx);
        for (size_t off = 0; off < sizeof(buf); off += chunk) {
            size_t n = (off + chunk > sizeof(buf)) ? (sizeof(buf) - off)
                                                   : chunk;
            md5Update(&ctx, buf + off, n);
        }
        uint8_t got[16];
        md5Final(got, &ctx);
        UT_ASSERT_MSG(memcmp(got, expected, 16) == 0,
                      "chunk size %zu produced a different digest",
                      chunk);
    }

    /* Calling md5Update with len==0 must be a no-op. */
    {
        Md5Ctx ctx;
        md5Init(&ctx);
        md5Update(&ctx, buf, sizeof(buf) / 2);
        md5Update(&ctx, "", 0);                       /* zero-len chunk */
        md5Update(&ctx, buf + sizeof(buf) / 2,
                  sizeof(buf) - sizeof(buf) / 2);
        uint8_t got[16];
        md5Final(got, &ctx);
        UT_ASSERT_MSG(memcmp(got, expected, 16) == 0,
                      "zero-length md5Update should be a no-op");
    }
    return 0;
}

/* Padding-boundary cases.  MD5 pads to a length ≡ 56 (mod 64).  The
 * three lengths exercised below put the bookkeeping into each of the
 * interesting branches:
 *   - 55: padLen = 1, fits inside current buffer
 *   - 56: padLen = 64, padding spills into a new block
 *   - 64: exact block boundary; first call to md5Final must flush
 *         the current block before starting padding
 *   - 119, 120: same shape one block up. */
int run_md5_block_boundaries(void) {
    const size_t lens[] = { 55, 56, 57, 63, 64, 65, 119, 120, 127, 128 };
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        size_t len = lens[i];
        uint8_t buf[256];
        for (size_t k = 0; k < len; k++) {
            buf[k] = (uint8_t)((k * 53 + 7) & 0xFF);
        }

        uint8_t expected[16];
        md5Compute(buf, len, expected);

        /* Hash the same input one byte at a time and compare. */
        Md5Ctx ctx;
        md5Init(&ctx);
        for (size_t k = 0; k < len; k++) {
            md5Update(&ctx, &buf[k], 1);
        }
        uint8_t got[16];
        md5Final(got, &ctx);
        UT_ASSERT_MSG(memcmp(got, expected, 16) == 0,
                      "byte-by-byte digest differs at len=%zu", len);
    }
    return 0;
}

/* md5ToHex must emit 32 lowercase hex chars + NUL.  The synthetic
 * digest below starts with a zero byte (proves leading zeros aren't
 * dropped) and includes 0xff (proves the high nibble lowercases to
 * "ff"); the rest is filled deterministically.  A real RFC vector is
 * also hexed end-to-end so the encoding matches a known digest. */
int run_md5_to_hex(void) {
    uint8_t digest[16];
    char out[33];

    digest[0] = 0x00;
    digest[1] = 0x0f;
    digest[2] = 0xa0;
    digest[3] = 0xff;
    for (int i = 4; i < 16; i++) {
        digest[i] = (uint8_t)(i * 16 + i); /* 0x44, 0x55, ... 0xff */
    }
    md5ToHex(digest, out);
    UT_ASSERT_MSG(strcmp(out, "000fa0ff445566778899aabbccddeeff") == 0,
                  "synthetic digest hexed to '%s'", out);
    UT_ASSERT_MSG(out[32] == '\0', "output not NUL-terminated");
    for (int i = 0; i < 32; i++) {
        char ch = out[i];
        UT_ASSERT_MSG((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'),
                      "non-lowercase-hex char '%c' at index %d", ch, i);
    }

    md5Compute("abc", 3, digest);
    md5ToHex(digest, out);
    UT_ASSERT_MSG(strcmp(out, "900150983cd24fb0d6963f7d28e17f72") == 0,
                  "md5(\"abc\") hexed to '%s'", out);
    return 0;
}
