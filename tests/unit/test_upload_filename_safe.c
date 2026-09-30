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
#include "upload_policy.h"    /* UPLOAD_KIND_MAP, UPLOAD_KIND_SCRIPT */
#include "test_harness.h"

#define LIT_LEN(s) (sizeof(s) - 1)

static int expect_accept(uint8_t kind, const char *label,
                         const char *name, size_t nameLen) {
    if (!uploadFilenameIsSafe(kind, name, nameLen)) {
        fprintf(stderr, "FAIL %s: expected accept for '%s' (len=%zu)\n",
                label, name ? name : "(null)", nameLen);
        return 1;
    }
    return 0;
}

static int expect_reject(uint8_t kind, const char *label,
                         const char *name, size_t nameLen) {
    if (uploadFilenameIsSafe(kind, name, nameLen)) {
        fprintf(stderr, "FAIL %s: expected reject for '%s' (len=%zu)\n",
                label, name ? name : "(null)", nameLen);
        return 1;
    }
    return 0;
}

int run_upload_filename_safe(void) {
    int fail = 0;

    /* ── Accepts ─────────────────────────────────────────────── */
    fail |= expect_accept(UPLOAD_KIND_MAP, "basicMap",          "foo.map",      LIT_LEN("foo.map"));
    fail |= expect_accept(UPLOAD_KIND_MAP, "minimumLength",     "a.map",        LIT_LEN("a.map"));
    fail |= expect_accept(UPLOAD_KIND_MAP, "mixedCaseSuffix",   "Foo.MaP",      LIT_LEN("Foo.MaP"));
    fail |= expect_accept(UPLOAD_KIND_MAP, "upperCaseSuffix",   "foo.MAP",      LIT_LEN("foo.MAP"));
    /* Prefix-of-reserved but the full basename is not the reserved
     * name (CONS vs CON, COMM1 vs COM1, LPT10 vs LPT1). */
    fail |= expect_accept(UPLOAD_KIND_MAP, "prefixOfReservedCON",  "CONS.map",  LIT_LEN("CONS.map"));
    fail |= expect_accept(UPLOAD_KIND_MAP, "prefixOfReservedCOM1", "COMM1.map", LIT_LEN("COMM1.map"));
    fail |= expect_accept(UPLOAD_KIND_MAP, "prefixOfReservedLPT1", "LPT10.map", LIT_LEN("LPT10.map"));
    /* Internal dot in the basename — the validator only checks the
     * trailing ".map" suffix and a single byte preceding it. */
    fail |= expect_accept(UPLOAD_KIND_MAP, "internalDotBasename",  "a.b.map",   LIT_LEN("a.b.map"));

    /* ── Basic rejects ───────────────────────────────────────── */
    fail |= expect_reject(UPLOAD_KIND_MAP, "nullName",          NULL,           7);
    fail |= expect_reject(UPLOAD_KIND_MAP, "zeroLen",           "foo.map",      0);
    fail |= expect_reject(UPLOAD_KIND_MAP, "tooShortDotMap",    ".map",         LIT_LEN(".map"));
    fail |= expect_reject(UPLOAD_KIND_MAP, "tooShortFour",      "f.ma",         LIT_LEN("f.ma"));
    fail |= expect_reject(UPLOAD_KIND_MAP, "leadingDot",        ".foo.map",     LIT_LEN(".foo.map"));
    fail |= expect_reject(UPLOAD_KIND_MAP, "pathForwardSlash",  "a/b.map",      LIT_LEN("a/b.map"));
    fail |= expect_reject(UPLOAD_KIND_MAP, "pathBackSlash",     "a\\b.map",     LIT_LEN("a\\b.map"));
    fail |= expect_reject(UPLOAD_KIND_MAP, "pathColon",         "a:b.map",      LIT_LEN("a:b.map"));

    /* ── Hardening rejects ───────────────────────────────────── */
    /* Embedded NUL — must be detected via the nameLen scan, not by
     * relying on the implicit NUL terminator. */
    {
        const char embedNul[] = { 'a', '\0', 'b', '.', 'm', 'a', 'p' };
        fail |= expect_reject(UPLOAD_KIND_MAP, "embeddedNul", embedNul, sizeof(embedNul));
    }
    {
        const char ctrlBell[] = { 'a', '\x07', '.', 'm', 'a', 'p' };
        fail |= expect_reject(UPLOAD_KIND_MAP, "controlBell", ctrlBell, sizeof(ctrlBell));
    }
    {
        const char ctrlUs[] = { 'a', '\x1f', '.', 'm', 'a', 'p' };
        fail |= expect_reject(UPLOAD_KIND_MAP, "controlUnitSep", ctrlUs, sizeof(ctrlUs));
    }
    /* Windows strips trailing dots/spaces from basenames on creation,
     * which would bypass collision avoidance — both should reject. */
    fail |= expect_reject(UPLOAD_KIND_MAP, "trailingDotBypass",   "foo..map",   LIT_LEN("foo..map"));
    fail |= expect_reject(UPLOAD_KIND_MAP, "trailingSpaceBypass", "foo .map",   LIT_LEN("foo .map"));
    fail |= expect_reject(UPLOAD_KIND_MAP, "wrongSuffixTxt",      "foo.txt",    LIT_LEN("foo.txt"));
    fail |= expect_reject(UPLOAD_KIND_MAP, "wrongSuffixMap1",     "foo.map1",   LIT_LEN("foo.map1"));
    fail |= expect_reject(UPLOAD_KIND_MAP, "bareDotMap",          ".map",       LIT_LEN(".map"));

    /* ── Length cap: nameLen > MAP_STR_SIZE - 1 + 4 ──────────── */
    {
        /* Build a name of length (MAP_STR_SIZE - 1) + 4 + 1 — one byte
         * over the cap. Basename is all 'a' followed by ".map". */
        char tooLong[MAP_STR_SIZE + 5];
        size_t baseLen = (size_t)(MAP_STR_SIZE - 1) + 1; /* one over */
        size_t i;
        for (i = 0; i < baseLen; i++) tooLong[i] = 'a';
        memcpy(tooLong + baseLen, ".map", 4);
        fail |= expect_reject(UPLOAD_KIND_MAP, "lengthCapOneOver", tooLong, baseLen + 4);

        /* Exactly at the cap should accept — boundary check. */
        char atCap[MAP_STR_SIZE + 5];
        size_t baseLenOk = (size_t)(MAP_STR_SIZE - 1);
        for (i = 0; i < baseLenOk; i++) atCap[i] = 'a';
        memcpy(atCap + baseLenOk, ".map", 4);
        fail |= expect_accept(UPLOAD_KIND_MAP, "lengthCapAtBoundary", atCap, baseLenOk + 4);
    }

    /* ── Reserved Windows basenames ──────────────────────────── */
    fail |= expect_reject(UPLOAD_KIND_MAP, "reservedConUpper",  "CON.map",   LIT_LEN("CON.map"));
    fail |= expect_reject(UPLOAD_KIND_MAP, "reservedConLower",  "con.map",   LIT_LEN("con.map"));
    fail |= expect_reject(UPLOAD_KIND_MAP, "reservedConMixed",  "CoN.map",   LIT_LEN("CoN.map"));
    fail |= expect_reject(UPLOAD_KIND_MAP, "reservedPrn",       "PRN.map",   LIT_LEN("PRN.map"));
    fail |= expect_reject(UPLOAD_KIND_MAP, "reservedAux",       "AUX.map",   LIT_LEN("AUX.map"));
    fail |= expect_reject(UPLOAD_KIND_MAP, "reservedNul",       "NUL.map",   LIT_LEN("NUL.map"));
    fail |= expect_reject(UPLOAD_KIND_MAP, "reservedCom1",      "COM1.map",  LIT_LEN("COM1.map"));
    fail |= expect_reject(UPLOAD_KIND_MAP, "reservedCom9",      "COM9.map",  LIT_LEN("COM9.map"));
    fail |= expect_reject(UPLOAD_KIND_MAP, "reservedLpt1",      "LPT1.map",  LIT_LEN("LPT1.map"));
    fail |= expect_reject(UPLOAD_KIND_MAP, "reservedLpt9",      "LPT9.map",  LIT_LEN("LPT9.map"));

    /* A script name is not a map name, and an unknown kind takes nothing. */
    fail |= expect_reject(UPLOAD_KIND_MAP, "mapRefusesLua",     "mod.lua",   LIT_LEN("mod.lua"));
    fail |= expect_reject(2,               "unknownKind",       "foo.map",   LIT_LEN("foo.map"));

    return fail;
}

/* The script rule (UPLOAD_KIND_SCRIPT): .scenario or .lua, the map rule's
 * refusals, and the whole name within ScnDirEntry.file's 127 bytes. The
 * path-shape cases are the ones lobbyScenarioNameShapeOk refuses — an
 * absolute path, a drive letter, a ".." segment — proving this rule refuses
 * them too. */
int run_upload_filename_safe_script(void) {
    int fail = 0;
    const uint8_t k = UPLOAD_KIND_SCRIPT;

    /* ── Accepts ─────────────────────────────────────────────── */
    fail |= expect_accept(k, "modLua",            "mod.lua",        LIT_LEN("mod.lua"));
    fail |= expect_accept(k, "arenaScenario",     "Arena.scenario", LIT_LEN("Arena.scenario"));
    fail |= expect_accept(k, "upperCaseLua",      "MOD.LUA",        LIT_LEN("MOD.LUA"));
    fail |= expect_accept(k, "oneByteBase",       "a.lua",          LIT_LEN("a.lua"));

    /* ── Suffix ──────────────────────────────────────────────── */
    fail |= expect_reject(k, "mapSuffix",         "mod.map",        LIT_LEN("mod.map"));
    fail |= expect_reject(k, "txtSuffix",         "mod.txt",        LIT_LEN("mod.txt"));
    fail |= expect_reject(k, "bareLua",           ".lua",           LIT_LEN(".lua"));
    fail |= expect_reject(k, "bareScenario",      ".scenario",      LIT_LEN(".scenario"));
    fail |= expect_reject(k, "trailingDot",       "x.lua.",         LIT_LEN("x.lua."));
    fail |= expect_reject(k, "dotBeforeSuffix",   "x..lua",         LIT_LEN("x..lua"));
    fail |= expect_reject(k, "spaceBeforeSuffix", "x .scenario",    LIT_LEN("x .scenario"));

    /* ── Path shapes lobbyScenarioNameShapeOk refuses ───────── */
    fail |= expect_reject(k, "dotDotSegment",     "../x.lua",       LIT_LEN("../x.lua"));
    fail |= expect_reject(k, "driveLetter",       "C:x.lua",        LIT_LEN("C:x.lua"));
    fail |= expect_reject(k, "absolutePath",      "/x.lua",         LIT_LEN("/x.lua"));
    fail |= expect_reject(k, "backSlash",         "a\\b.lua",       LIT_LEN("a\\b.lua"));
    fail |= expect_reject(k, "backSlashLead",     "\\x.lua",        LIT_LEN("\\x.lua"));

    /* ── Bytes ───────────────────────────────────────────────── */
    {
        const char embedNul[] = { 'a', '\0', 'b', '.', 'l', 'u', 'a' };
        fail |= expect_reject(k, "embeddedNul", embedNul, sizeof(embedNul));
    }
    {
        const char ctrlBell[] = { 'a', '\x07', '.', 'l', 'u', 'a' };
        fail |= expect_reject(k, "controlBell", ctrlBell, sizeof(ctrlBell));
    }

    /* ── Reserved Windows basenames ──────────────────────────── */
    fail |= expect_reject(k, "reservedCon",       "CON.lua",        LIT_LEN("CON.lua"));
    fail |= expect_reject(k, "reservedNulLower",  "nul.scenario",   LIT_LEN("nul.scenario"));
    fail |= expect_accept(k, "prefixOfReserved",  "CONS.lua",       LIT_LEN("CONS.lua"));

    /* ── Length: 127 bytes accepted, 128 refused ─────────────── */
    {
        char name[128];
        size_t i;
        for (i = 0; i < 123; i++) name[i] = 'a';
        memcpy(name + 123, ".lua", 4);
        fail |= expect_accept(k, "length127", name, 127);

        for (i = 0; i < 124; i++) name[i] = 'a';
        memcpy(name + 124, ".lua", 4);
        fail |= expect_reject(k, "length128", name, 128);
    }

    return fail;
}
