/*
 * Coverage for uploadFilenameIsSafe — the server-side validator that
 * gates incoming map upload payloads. The wire delivers a length-
 * prefixed name (possibly NUL-free), so the test always passes the
 * length explicitly and never relies on strlen.
 *
 * Categories exercised:
 *   - accepts: minimum length, mixed-case .MAP, prefix-of-reserved
 *     basenames, internal dots
 *   - basic rejects: NULL, zero-length, short, leading dot,
 *     path-separator bytes
 *   - hardening rejects: embedded NUL, control characters,
 *     trailing dot/space before .map, wrong suffix, bare ".map"
 *   - length cap (one over MAP_STR_SIZE basename + .map suffix)
 *   - Windows reserved basenames (CON, PRN, AUX, NUL, COM1, COM9,
 *     LPT1, LPT9 — both case variants for one to prove case
 *     insensitivity)
 */
#include <stdint.h>
#include <string.h>

#include "global.h"           /* MAP_STR_SIZE */
#include "transport_udp.h"    /* uploadFilenameIsSafe declaration */
#include "test_harness.h"

#define LIT_LEN(s) (sizeof(s) - 1)

static int expect_accept(const char *label,
                         const char *name, size_t nameLen) {
    if (!uploadFilenameIsSafe(name, nameLen)) {
        fprintf(stderr, "FAIL %s: expected accept for '%s' (len=%zu)\n",
                label, name ? name : "(null)", nameLen);
        return 1;
    }
    return 0;
}

static int expect_reject(const char *label,
                         const char *name, size_t nameLen) {
    if (uploadFilenameIsSafe(name, nameLen)) {
        fprintf(stderr, "FAIL %s: expected reject for '%s' (len=%zu)\n",
                label, name ? name : "(null)", nameLen);
        return 1;
    }
    return 0;
}

int run_upload_filename_safe(void) {
    int fail = 0;

    /* ── Accepts ─────────────────────────────────────────────── */
    fail |= expect_accept("basicMap",          "foo.map",      LIT_LEN("foo.map"));
    fail |= expect_accept("minimumLength",     "a.map",        LIT_LEN("a.map"));
    fail |= expect_accept("mixedCaseSuffix",   "Foo.MaP",      LIT_LEN("Foo.MaP"));
    fail |= expect_accept("upperCaseSuffix",   "foo.MAP",      LIT_LEN("foo.MAP"));
    /* Prefix-of-reserved but the full basename is not the reserved
     * name (CONS vs CON, COMM1 vs COM1, LPT10 vs LPT1). */
    fail |= expect_accept("prefixOfReservedCON",  "CONS.map",  LIT_LEN("CONS.map"));
    fail |= expect_accept("prefixOfReservedCOM1", "COMM1.map", LIT_LEN("COMM1.map"));
    fail |= expect_accept("prefixOfReservedLPT1", "LPT10.map", LIT_LEN("LPT10.map"));
    /* Internal dot in the basename — the validator only checks the
     * trailing ".map" suffix and a single byte preceding it. */
    fail |= expect_accept("internalDotBasename",  "a.b.map",   LIT_LEN("a.b.map"));

    /* ── Basic rejects ───────────────────────────────────────── */
    fail |= expect_reject("nullName",          NULL,           7);
    fail |= expect_reject("zeroLen",           "foo.map",      0);
    fail |= expect_reject("tooShortDotMap",    ".map",         LIT_LEN(".map"));
    fail |= expect_reject("tooShortFour",      "f.ma",         LIT_LEN("f.ma"));
    fail |= expect_reject("leadingDot",        ".foo.map",     LIT_LEN(".foo.map"));
    fail |= expect_reject("pathForwardSlash",  "a/b.map",      LIT_LEN("a/b.map"));
    fail |= expect_reject("pathBackSlash",     "a\\b.map",     LIT_LEN("a\\b.map"));
    fail |= expect_reject("pathColon",         "a:b.map",      LIT_LEN("a:b.map"));

    /* ── Hardening rejects ───────────────────────────────────── */
    /* Embedded NUL — must be detected via the nameLen scan, not by
     * relying on the implicit NUL terminator. */
    {
        const char embedNul[] = { 'a', '\0', 'b', '.', 'm', 'a', 'p' };
        fail |= expect_reject("embeddedNul", embedNul, sizeof(embedNul));
    }
    {
        const char ctrlBell[] = { 'a', '\x07', '.', 'm', 'a', 'p' };
        fail |= expect_reject("controlBell", ctrlBell, sizeof(ctrlBell));
    }
    {
        const char ctrlUs[] = { 'a', '\x1f', '.', 'm', 'a', 'p' };
        fail |= expect_reject("controlUnitSep", ctrlUs, sizeof(ctrlUs));
    }
    /* Windows strips trailing dots/spaces from basenames on creation,
     * which would bypass collision avoidance — both should reject. */
    fail |= expect_reject("trailingDotBypass",   "foo..map",   LIT_LEN("foo..map"));
    fail |= expect_reject("trailingSpaceBypass", "foo .map",   LIT_LEN("foo .map"));
    fail |= expect_reject("wrongSuffixTxt",      "foo.txt",    LIT_LEN("foo.txt"));
    fail |= expect_reject("wrongSuffixMap1",     "foo.map1",   LIT_LEN("foo.map1"));
    fail |= expect_reject("bareDotMap",          ".map",       LIT_LEN(".map"));

    /* ── Length cap: nameLen > MAP_STR_SIZE - 1 + 4 ──────────── */
    {
        /* Build a name of length (MAP_STR_SIZE - 1) + 4 + 1 — one byte
         * over the cap. Basename is all 'a' followed by ".map". */
        char tooLong[MAP_STR_SIZE + 5];
        size_t baseLen = (size_t)(MAP_STR_SIZE - 1) + 1; /* one over */
        size_t i;
        for (i = 0; i < baseLen; i++) tooLong[i] = 'a';
        memcpy(tooLong + baseLen, ".map", 4);
        fail |= expect_reject("lengthCapOneOver", tooLong, baseLen + 4);

        /* Exactly at the cap should accept — boundary check. */
        char atCap[MAP_STR_SIZE + 5];
        size_t baseLenOk = (size_t)(MAP_STR_SIZE - 1);
        for (i = 0; i < baseLenOk; i++) atCap[i] = 'a';
        memcpy(atCap + baseLenOk, ".map", 4);
        fail |= expect_accept("lengthCapAtBoundary", atCap, baseLenOk + 4);
    }

    /* ── Reserved Windows basenames ──────────────────────────── */
    fail |= expect_reject("reservedConUpper",  "CON.map",   LIT_LEN("CON.map"));
    fail |= expect_reject("reservedConLower",  "con.map",   LIT_LEN("con.map"));
    fail |= expect_reject("reservedConMixed",  "CoN.map",   LIT_LEN("CoN.map"));
    fail |= expect_reject("reservedPrn",       "PRN.map",   LIT_LEN("PRN.map"));
    fail |= expect_reject("reservedAux",       "AUX.map",   LIT_LEN("AUX.map"));
    fail |= expect_reject("reservedNul",       "NUL.map",   LIT_LEN("NUL.map"));
    fail |= expect_reject("reservedCom1",      "COM1.map",  LIT_LEN("COM1.map"));
    fail |= expect_reject("reservedCom9",      "COM9.map",  LIT_LEN("COM9.map"));
    fail |= expect_reject("reservedLpt1",      "LPT1.map",  LIT_LEN("LPT1.map"));
    fail |= expect_reject("reservedLpt9",      "LPT9.map",  LIT_LEN("LPT9.map"));

    return fail;
}
