/*
 * Round-trip and edge cases for the WBN key wire codec.
 * wbnKeyEncode and wbnKeyDecode wrap the zero-pad + NUL-scan
 * convention shared by PACKET_WBN_REKEY and the wbnJoinKey field
 * on JOIN_REQUEST. The helpers are hoisted into src/bolo/ so this
 * test can link against the same bolo_static the production code
 * pulls in.
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "transport_udp.h"      /* WBN_JOIN_KEY_WIRE_LEN */
#include "winbolonet_core.h"    /* WINBOLONET_KEY_LEN */
#include "wbn_key_codec.h"
#include "test_harness.h"

static int encode_decode_roundtrip(void) {
    /* 32 chars — fills the prefix exactly, NUL lands on byte 32. */
    const char *original = "abcdefghijklmnopqrstuvwxyz012345";
    uint8_t wire[WBN_JOIN_KEY_WIRE_LEN];
    char decoded[WINBOLONET_KEY_LEN];

    UT_ASSERT_MSG(strlen(original) == 32,
                  "test setup: expected 32-char source, got %zu",
                  strlen(original));

    memset(wire, 0xFF, sizeof(wire));
    wbnKeyEncode(wire, original);

    memset(decoded, 0xAA, sizeof(decoded));
    UT_ASSERT(wbnKeyDecode(decoded, wire) == TRUE);
    UT_ASSERT_MSG(strcmp(decoded, original) == 0,
                  "round-trip mismatch: got \"%s\"", decoded);
    return 0;
}

static int encode_zero_pads_trailing(void) {
    uint8_t wire[WBN_JOIN_KEY_WIRE_LEN];
    const char *src = "0123456789";    /* 10 chars */
    size_t i;

    memset(wire, 0xFF, sizeof(wire));
    wbnKeyEncode(wire, src);

    UT_ASSERT_MSG(memcmp(wire, src, 10) == 0,
                  "encoded prefix wrong");
    for (i = 10; i < WBN_JOIN_KEY_WIRE_LEN; i++) {
        UT_ASSERT_MSG(wire[i] == 0x00,
                      "byte %zu = 0x%02x, expected 0x00", i, wire[i]);
    }
    return 0;
}

static int decode_malformed_no_nul(void) {
    uint8_t wire[WBN_JOIN_KEY_WIRE_LEN];
    char dest[WINBOLONET_KEY_LEN];

    /* Fill the first WINBOLONET_KEY_LEN (33) bytes with non-NUL bytes
     * so the decoder's NUL-scan never finds a terminator. Trailing
     * bytes are arbitrary; the decoder doesn't read them. */
    memset(wire, 'X', sizeof(wire));
    memset(dest, 0xAA, sizeof(dest));

    UT_ASSERT(wbnKeyDecode(dest, wire) == FALSE);
    /* Production code leaves dest untouched on malformed. */
    {
        size_t i;
        for (i = 0; i < sizeof(dest); i++) {
            UT_ASSERT_MSG(dest[i] == (char)0xAA,
                          "dest[%zu] = 0x%02x, expected 0xAA "
                          "(decoder must not write on malformed)",
                          i, (unsigned char)dest[i]);
        }
    }
    return 0;
}

static int decode_empty(void) {
    uint8_t wire[WBN_JOIN_KEY_WIRE_LEN];
    char dest[WINBOLONET_KEY_LEN];

    /* src[0] == 0 → empty key; the rest doesn't matter. */
    memset(wire, 0xCC, sizeof(wire));
    wire[0] = '\0';
    memset(dest, 0xAA, sizeof(dest));

    UT_ASSERT(wbnKeyDecode(dest, wire) == TRUE);
    UT_ASSERT_MSG(dest[0] == '\0',
                  "expected empty string, got dest[0] = 0x%02x",
                  (unsigned char)dest[0]);
    return 0;
}

static int encode_truncates_oversize(void) {
    /* 64 chars — twice WINBOLONET_KEY_LEN - 1. Only the first 32
     * survive; byte 32 is the NUL from the zero-pad. */
    const char *oversize =
        "abcdefghijklmnopqrstuvwxyz012345"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ6789!@";
    uint8_t wire[WBN_JOIN_KEY_WIRE_LEN];
    size_t i;

    UT_ASSERT_MSG(strlen(oversize) == 64,
                  "test setup: expected 64-char source, got %zu",
                  strlen(oversize));

    memset(wire, 0xFF, sizeof(wire));
    wbnKeyEncode(wire, oversize);

    UT_ASSERT_MSG(memcmp(wire, oversize, WINBOLONET_KEY_LEN - 1) == 0,
                  "first 32 bytes of wire don't match source");
    for (i = WINBOLONET_KEY_LEN - 1; i < WBN_JOIN_KEY_WIRE_LEN; i++) {
        UT_ASSERT_MSG(wire[i] == 0x00,
                      "byte %zu = 0x%02x, expected zero-pad",
                      i, wire[i]);
    }
    return 0;
}

int run_wbn_rekey_codec(void) {
    int rc;
    rc = encode_decode_roundtrip();    if (rc) return rc;
    rc = encode_zero_pads_trailing();  if (rc) return rc;
    rc = decode_malformed_no_nul();    if (rc) return rc;
    rc = decode_empty();               if (rc) return rc;
    rc = encode_truncates_oversize();  if (rc) return rc;
    return 0;
}
