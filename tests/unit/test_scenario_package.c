/*
 * The WBSC container (src/scenario_io/scenario_package.c): the ten framing
 * bytes, the ZIP archive behind them, and the two handles that have to be
 * able to read different containers at the same time.
 *
 * Everything here stays in memory. A container is built with
 * scnPackageWrite, read back with scnPackageOpen, and the bad cases are
 * copies of a good container with a byte or two changed, so each refusal is
 * the only thing wrong with the buffer it is asserted against.
 *
 * run_scenario_package_round_trip
 *      — a manifest, a script and a brain file go in, the framing comes
 *        out as magic, version 1 and the length of the archive behind it,
 *        and every entry reads back byte for byte, stored or deflated
 * run_scenario_package_bad_framing
 *      — the six refusals: a buffer too short to hold a header, a magic
 *        that is not WBSC, a version that is not 1, a length past the end
 *        of the buffer, a payload that is not a ZIP, and no manifest.json.
 *        Each returns NULL, and each says something different
 * run_scenario_package_entry_names
 *      — brains/A/x.lua, brains/A/y.lua and brains/B/init.lua name two
 *        brains, A then B; an entry nothing reads still shows in the entry
 *        list; and an entry name that climbs out of the container is
 *        refused while it is being opened
 * run_scenario_package_two_open
 *      — two containers open at once, read turn about, each handle seeing
 *        only its own entries
 * run_scenario_package_entry_cap
 *      — an entry is measured against the caller's cap before it is read: at
 *        the cap it comes back whole, a byte under it is refused and the
 *        refusal names the entry and the figure, and nothing is handed back
 *
 * The cap's other case — an entry whose header understates what the archive
 * holds — is not driven from here. scnPackageWrite takes the bytes and not a
 * declared size; minizip writes the size it measured, so there is no way
 * through this API to write a header that lies. What holds that case is in
 * scnPackageReadEntry itself: the loop asks for no more than what is left of
 * the declared size, and the CRC minizip checks as the entry closes is what
 * turns the short read into a refusal.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "scenario_package.h"
#include "test_harness.h"

static const char kManifest[] = "{\"manifest\":1,\"name\":\"Raiders\"}";
static const char kScript[] =
    "-- main.lua\n"
    "local scenario = { api = 1, name = 'Raiders' }\n"
    "local scenario = { api = 1, name = 'Raiders' }\n"
    "local scenario = { api = 1, name = 'Raiders' }\n"
    "return scenario\n";
static const char kBrain[] = "return { name = 'raiders', tick = function() end }\n";

/* The cap the reads below are made under. Well above every fixture here, so a
 * read that refuses is refusing for the reason the case is about. */
#define PKG_TEST_MAX (64u * 1024u)

/* Read one entry and hold it against what was written. */
static int entryMatches(ScnPackage *p, const char *name, const char *expect,
                        size_t expectLen) {
    uint8_t *bytes = NULL;
    size_t len = 0;
    char err[256];

    err[0] = '\0';
    if (!scnPackageReadEntry(p, name, PKG_TEST_MAX, &bytes, &len, err,
                             sizeof(err))) {
        UT_FAIL("%s did not read back: %s", name, err);
    }
    if (len != expectLen) {
        free(bytes);
        UT_FAIL("%s came back %u bytes, wrote %u", name, (unsigned)len,
                (unsigned)expectLen);
    }
    if (expectLen > 0 && memcmp(bytes, expect, expectLen) != 0) {
        free(bytes);
        UT_FAIL("%s came back with different bytes", name);
    }
    free(bytes);
    return 0;
}

int run_scenario_package_round_trip(void) {
    ScnPackageEntry entries[3];
    uint8_t *packed = NULL;
    size_t packedLen = 0;
    uint32_t framedLen;
    char err[256];
    ScnPackage *p;
    int rc;

    memset(entries, 0, sizeof(entries));
    entries[0].name = "manifest.json";
    entries[0].bytes = (const uint8_t *)kManifest;
    entries[0].len = sizeof(kManifest) - 1;
    entries[0].deflate = false;
    entries[1].name = "main.lua";
    entries[1].bytes = (const uint8_t *)kScript;
    entries[1].len = sizeof(kScript) - 1;
    entries[1].deflate = true;
    entries[2].name = "brains/raiders/init.lua";
    entries[2].bytes = (const uint8_t *)kBrain;
    entries[2].len = sizeof(kBrain) - 1;
    entries[2].deflate = true;

    if (!scnPackageWrite(entries, 3, &packed, &packedLen, err, sizeof(err))) {
        UT_FAIL("scnPackageWrite refused: %s", err);
    }
    UT_ASSERT(packed != NULL);
    UT_ASSERT(packedLen > 10);

    /* The framing: magic, version 1, and a length that is exactly the
     * archive sitting behind the header. */
    UT_ASSERT(memcmp(packed, "WBSC", 4) == 0);
    UT_ASSERT_MSG(packed[4] == 1 && packed[5] == 0, "version is %u,%u",
                  (unsigned)packed[4], (unsigned)packed[5]);
    framedLen = (uint32_t)packed[6] | ((uint32_t)packed[7] << 8) |
                ((uint32_t)packed[8] << 16) | ((uint32_t)packed[9] << 24);
    UT_ASSERT_MSG((size_t)framedLen == packedLen - 10,
                  "length says %u, the buffer holds %u after the header",
                  (unsigned)framedLen, (unsigned)(packedLen - 10));

    p = scnPackageOpen(packed, packedLen, err, sizeof(err));
    UT_ASSERT_MSG(p != NULL, "open refused a container it just wrote: %s", err);

    UT_ASSERT_MSG(scnPackageEntryCount(p) == 3, "entry count is %d",
                  scnPackageEntryCount(p));
    UT_ASSERT(strcmp(scnPackageEntryName(p, 0), "manifest.json") == 0);
    UT_ASSERT(strcmp(scnPackageEntryName(p, 1), "main.lua") == 0);
    UT_ASSERT(strcmp(scnPackageEntryName(p, 2), "brains/raiders/init.lua") == 0);
    UT_ASSERT(scnPackageEntryName(p, 3) == NULL);
    UT_ASSERT(scnPackageHasEntry(p, "manifest.json"));
    UT_ASSERT(!scnPackageHasEntry(p, "credits.txt"));

    rc = entryMatches(p, "manifest.json", kManifest, sizeof(kManifest) - 1);
    if (rc != 0) { scnPackageClose(p); free(packed); return rc; }
    rc = entryMatches(p, "main.lua", kScript, sizeof(kScript) - 1);
    if (rc != 0) { scnPackageClose(p); free(packed); return rc; }
    rc = entryMatches(p, "brains/raiders/init.lua", kBrain, sizeof(kBrain) - 1);
    if (rc != 0) { scnPackageClose(p); free(packed); return rc; }

    UT_ASSERT_MSG(scnPackageBrainCount(p) == 1, "brain count is %d",
                  scnPackageBrainCount(p));
    UT_ASSERT(strcmp(scnPackageBrainName(p, 0), "raiders") == 0);

    scnPackageClose(p);
    free(packed);
    return 0;
}

int run_scenario_package_bad_framing(void) {
    ScnPackageEntry entries[2];
    uint8_t *packed = NULL;
    uint8_t *copy = NULL;
    uint8_t *scriptOnly = NULL;
    uint8_t junk[40];
    size_t packedLen = 0;
    size_t scriptOnlyLen = 0;
    char err[6][256];
    char writeErr[256];
    ScnPackage *p;
    int i;
    int j;

    memset(entries, 0, sizeof(entries));
    entries[0].name = "manifest.json";
    entries[0].bytes = (const uint8_t *)kManifest;
    entries[0].len = sizeof(kManifest) - 1;
    entries[1].name = "main.lua";
    entries[1].bytes = (const uint8_t *)kScript;
    entries[1].len = sizeof(kScript) - 1;

    if (!scnPackageWrite(entries, 2, &packed, &packedLen, writeErr,
                         sizeof(writeErr))) {
        UT_FAIL("scnPackageWrite refused: %s", writeErr);
    }

    /* 1. Shorter than the ten-byte header. */
    p = scnPackageOpen(packed, 9, err[0], sizeof(err[0]));
    UT_ASSERT_MSG(p == NULL, "a nine-byte buffer opened");
    UT_ASSERT(err[0][0] != '\0');

    /* 2. Something other than WBSC in the first four bytes. */
    copy = (uint8_t *)malloc(packedLen);
    UT_ASSERT(copy != NULL);
    memcpy(copy, packed, packedLen);
    copy[0] = 'X';
    p = scnPackageOpen(copy, packedLen, err[1], sizeof(err[1]));
    UT_ASSERT_MSG(p == NULL, "a container with the wrong magic opened");
    UT_ASSERT(err[1][0] != '\0');

    /* 3. A version this build does not read. */
    memcpy(copy, packed, packedLen);
    copy[4] = 2;
    p = scnPackageOpen(copy, packedLen, err[2], sizeof(err[2]));
    UT_ASSERT_MSG(p == NULL, "a version 2 container opened");
    UT_ASSERT(err[2][0] != '\0');

    /* 4. A length claiming more archive than the buffer holds. */
    memcpy(copy, packed, packedLen);
    copy[6] = (uint8_t)((packedLen - 10 + 64) & 0xFF);
    copy[7] = (uint8_t)(((packedLen - 10 + 64) >> 8) & 0xFF);
    copy[8] = (uint8_t)(((packedLen - 10 + 64) >> 16) & 0xFF);
    copy[9] = (uint8_t)(((packedLen - 10 + 64) >> 24) & 0xFF);
    p = scnPackageOpen(copy, packedLen, err[3], sizeof(err[3]));
    UT_ASSERT_MSG(p == NULL, "a length past the end of the buffer opened");
    UT_ASSERT(err[3][0] != '\0');

    /* 5. Framing over something that is not a ZIP archive. */
    memset(junk, 'x', sizeof(junk));
    memcpy(junk, "WBSC", 4);
    junk[4] = 1;
    junk[5] = 0;
    junk[6] = (uint8_t)(sizeof(junk) - 10);
    junk[7] = 0;
    junk[8] = 0;
    junk[9] = 0;
    p = scnPackageOpen(junk, sizeof(junk), err[4], sizeof(err[4]));
    UT_ASSERT_MSG(p == NULL, "a payload that is not a ZIP opened");
    UT_ASSERT(err[4][0] != '\0');

    /* 6. A well-formed archive with no manifest in it. */
    if (!scnPackageWrite(&entries[1], 1, &scriptOnly, &scriptOnlyLen, writeErr,
                         sizeof(writeErr))) {
        free(copy);
        free(packed);
        UT_FAIL("scnPackageWrite refused a script-only archive: %s", writeErr);
    }
    p = scnPackageOpen(scriptOnly, scriptOnlyLen, err[5], sizeof(err[5]));
    UT_ASSERT_MSG(p == NULL, "a container with no manifest.json opened");
    UT_ASSERT(err[5][0] != '\0');

    /* Each refusal says its own thing, so a caller can tell them apart. */
    for (i = 0; i < 6; i++) {
        for (j = i + 1; j < 6; j++) {
            if (strcmp(err[i], err[j]) == 0) {
                free(scriptOnly);
                free(copy);
                free(packed);
                UT_FAIL("refusals %d and %d share one message: %s", i, j,
                        err[i]);
            }
        }
    }

    free(scriptOnly);
    free(copy);
    free(packed);
    return 0;
}

int run_scenario_package_entry_names(void) {
    ScnPackageEntry entries[5];
    ScnPackageEntry climbing[2];
    uint8_t *packed = NULL;
    uint8_t *bad = NULL;
    size_t packedLen = 0;
    size_t badLen = 0;
    char err[256];
    ScnPackage *p;
    bool sawNotes = false;
    int i;

    memset(entries, 0, sizeof(entries));
    entries[0].name = "manifest.json";
    entries[0].bytes = (const uint8_t *)kManifest;
    entries[0].len = sizeof(kManifest) - 1;
    entries[1].name = "brains/A/x.lua";
    entries[1].bytes = (const uint8_t *)kBrain;
    entries[1].len = sizeof(kBrain) - 1;
    entries[2].name = "brains/A/y.lua";
    entries[2].bytes = (const uint8_t *)kBrain;
    entries[2].len = sizeof(kBrain) - 1;
    entries[3].name = "brains/B/init.lua";
    entries[3].bytes = (const uint8_t *)kBrain;
    entries[3].len = sizeof(kBrain) - 1;
    /* Nothing reads this one. The loader ignores it; the entry list keeps it
     * so a validator can say it is there. */
    entries[4].name = "notes.txt";
    entries[4].bytes = (const uint8_t *)"an author's note\n";
    entries[4].len = 17;

    if (!scnPackageWrite(entries, 5, &packed, &packedLen, err, sizeof(err))) {
        UT_FAIL("scnPackageWrite refused: %s", err);
    }
    p = scnPackageOpen(packed, packedLen, err, sizeof(err));
    UT_ASSERT_MSG(p != NULL, "open refused: %s", err);

    UT_ASSERT_MSG(scnPackageBrainCount(p) == 2, "brain count is %d",
                  scnPackageBrainCount(p));
    UT_ASSERT(strcmp(scnPackageBrainName(p, 0), "A") == 0);
    UT_ASSERT(strcmp(scnPackageBrainName(p, 1), "B") == 0);
    UT_ASSERT(scnPackageBrainName(p, 2) == NULL);

    UT_ASSERT_MSG(scnPackageEntryCount(p) == 5, "entry count is %d",
                  scnPackageEntryCount(p));
    for (i = 0; i < scnPackageEntryCount(p); i++) {
        if (strcmp(scnPackageEntryName(p, i), "notes.txt") == 0) {
            sawNotes = true;
        }
    }
    UT_ASSERT_MSG(sawNotes, "the unread entry is missing from the entry list");
    UT_ASSERT(scnPackageHasEntry(p, "notes.txt"));

    scnPackageClose(p);
    free(packed);

    /* An entry name that would climb out of the directory it is extracted
     * into never gets as far as a caller. */
    memset(climbing, 0, sizeof(climbing));
    climbing[0].name = "manifest.json";
    climbing[0].bytes = (const uint8_t *)kManifest;
    climbing[0].len = sizeof(kManifest) - 1;
    climbing[1].name = "brains/../evil.lua";
    climbing[1].bytes = (const uint8_t *)kBrain;
    climbing[1].len = sizeof(kBrain) - 1;

    if (!scnPackageWrite(climbing, 2, &bad, &badLen, err, sizeof(err))) {
        UT_FAIL("scnPackageWrite refused the climbing name: %s", err);
    }
    err[0] = '\0';
    p = scnPackageOpen(bad, badLen, err, sizeof(err));
    UT_ASSERT_MSG(p == NULL, "a container with a .. entry opened");
    UT_ASSERT(err[0] != '\0');
    free(bad);
    return 0;
}

int run_scenario_package_two_open(void) {
    static const char kManifestA[] = "{\"manifest\":1,\"name\":\"A\"}";
    static const char kManifestB[] =
        "{\"manifest\":1,\"name\":\"B\",\"note\":\"the longer of the two\"}";
    static const char kScriptA[] = "return { api = 1, name = 'A' }\n";
    static const char kScriptB[] =
        "return { api = 1, name = 'B', rounds = 3, teams = 4 }\n";

    ScnPackageEntry a[2];
    ScnPackageEntry b[2];
    uint8_t *packedA = NULL;
    uint8_t *packedB = NULL;
    size_t lenA = 0;
    size_t lenB = 0;
    char err[256];
    ScnPackage *pa;
    ScnPackage *pb;
    int rc;

    memset(a, 0, sizeof(a));
    a[0].name = "manifest.json";
    a[0].bytes = (const uint8_t *)kManifestA;
    a[0].len = sizeof(kManifestA) - 1;
    a[1].name = "main.lua";
    a[1].bytes = (const uint8_t *)kScriptA;
    a[1].len = sizeof(kScriptA) - 1;
    a[1].deflate = true;

    memset(b, 0, sizeof(b));
    b[0].name = "manifest.json";
    b[0].bytes = (const uint8_t *)kManifestB;
    b[0].len = sizeof(kManifestB) - 1;
    b[1].name = "brains/rover/init.lua";
    b[1].bytes = (const uint8_t *)kScriptB;
    b[1].len = sizeof(kScriptB) - 1;

    if (!scnPackageWrite(a, 2, &packedA, &lenA, err, sizeof(err))) {
        UT_FAIL("scnPackageWrite refused A: %s", err);
    }
    if (!scnPackageWrite(b, 2, &packedB, &lenB, err, sizeof(err))) {
        free(packedA);
        UT_FAIL("scnPackageWrite refused B: %s", err);
    }

    pa = scnPackageOpen(packedA, lenA, err, sizeof(err));
    UT_ASSERT_MSG(pa != NULL, "open refused A: %s", err);
    pb = scnPackageOpen(packedB, lenB, err, sizeof(err));
    UT_ASSERT_MSG(pb != NULL, "open refused B: %s", err);

    /* Turn about, so a reader context shared between the handles would hand
     * one of them the other's archive. */
    rc = entryMatches(pa, "manifest.json", kManifestA, sizeof(kManifestA) - 1);
    if (rc == 0) {
        rc = entryMatches(pb, "manifest.json", kManifestB,
                          sizeof(kManifestB) - 1);
    }
    if (rc == 0) {
        rc = entryMatches(pa, "main.lua", kScriptA, sizeof(kScriptA) - 1);
    }
    if (rc == 0) {
        rc = entryMatches(pb, "brains/rover/init.lua", kScriptB,
                          sizeof(kScriptB) - 1);
    }
    if (rc == 0) {
        rc = entryMatches(pa, "manifest.json", kManifestA,
                          sizeof(kManifestA) - 1);
    }

    if (rc == 0) {
        /* Neither handle knows anything about the other's entries. */
        if (scnPackageHasEntry(pa, "brains/rover/init.lua")) {
            rc = 1;
            fprintf(stderr, "FAIL %s:%d: A sees B's brain entry\n",
                    __FILE__, __LINE__);
        } else if (scnPackageHasEntry(pb, "main.lua")) {
            rc = 1;
            fprintf(stderr, "FAIL %s:%d: B sees A's script\n",
                    __FILE__, __LINE__);
        } else if (scnPackageBrainCount(pa) != 0 ||
                   scnPackageBrainCount(pb) != 1) {
            rc = 1;
            fprintf(stderr,
                    "FAIL %s:%d: brain counts are %d and %d, wanted 0 and 1\n",
                    __FILE__, __LINE__, scnPackageBrainCount(pa),
                    scnPackageBrainCount(pb));
        }
    }

    scnPackageClose(pa);
    scnPackageClose(pb);
    free(packedA);
    free(packedB);
    return rc;
}

/* ── The cap on what one entry may inflate to ─────────────────────── */

/* A container is ten bytes of framing over a ZIP archive, and a ZIP entry's
 * uncompressed size is four bytes in its header that say whatever was written
 * there. A reader that believes them allocates whatever a container asks for,
 * so every read names the figure it is prepared to hold and the entry is
 * measured against it before anything is allocated. */
int run_scenario_package_entry_cap(void) {
    enum { kBodyLen = 4096 };
    ScnPackageEntry entries[2];
    char           *body;
    uint8_t        *packed = NULL;
    uint8_t        *bytes  = NULL;
    size_t          packedLen = 0;
    size_t          len       = 0;
    char            err[256];
    ScnPackage     *p;

    body = (char *)malloc(kBodyLen);
    UT_ASSERT(body != NULL);
    memset(body, 'x', kBodyLen);

    memset(entries, 0, sizeof(entries));
    entries[0].name  = "manifest.json";
    entries[0].bytes = (const uint8_t *)kManifest;
    entries[0].len   = sizeof(kManifest) - 1;
    entries[1].name  = "main.lua";
    entries[1].bytes = (const uint8_t *)body;
    entries[1].len   = kBodyLen;
    /* Deflated, so the entry on the wire is a few dozen bytes and the size the
       reader is asked to trust is the header's rather than the archive's. */
    entries[1].deflate = true;

    err[0] = '\0';
    if (!scnPackageWrite(entries, 2, &packed, &packedLen, err, sizeof(err))) {
        free(body);
        UT_FAIL("scnPackageWrite refused: %s", err);
    }
    UT_ASSERT_MSG(packedLen < (size_t)kBodyLen,
                  "the container is %lu bytes and the entry declares %d: the "
                  "fixture did not deflate", (unsigned long)packedLen,
                  (int)kBodyLen);

    p = scnPackageOpen(packed, packedLen, err, sizeof(err));
    if (p == NULL) {
        free(packed);
        free(body);
        UT_FAIL("open refused a container it just wrote: %s", err);
    }

    /* At the cap, the whole entry. */
    err[0] = '\0';
    UT_ASSERT_MSG(scnPackageReadEntry(p, "main.lua", (size_t)kBodyLen, &bytes,
                                      &len, err, sizeof(err)),
                  "an entry exactly at the cap was refused: %s", err);
    UT_ASSERT_MSG(len == (size_t)kBodyLen, "the entry came back %lu bytes",
                  (unsigned long)len);
    UT_ASSERT(bytes != NULL && memcmp(bytes, body, kBodyLen) == 0);
    free(bytes);
    bytes = NULL;
    len   = 0;

    /* One byte under it, nothing at all — and the reason names both the entry
       and the figure, so an operator holding a container they cannot read
       knows which entry and how far over it is. */
    err[0] = '\0';
    UT_ASSERT_MSG(!scnPackageReadEntry(p, "main.lua", (size_t)kBodyLen - 1,
                                       &bytes, &len, err, sizeof(err)),
                  "an entry a byte over the cap was read anyway");
    UT_ASSERT_MSG(bytes == NULL, "a refused read handed back a buffer");
    UT_ASSERT_MSG(len == 0, "a refused read reported %lu bytes",
                  (unsigned long)len);
    UT_ASSERT_MSG(err[0] != '\0', "the refusal said nothing");
    UT_ASSERT_MSG(strstr(err, "main.lua") != NULL,
                  "the refusal does not name the entry: %s", err);
    UT_ASSERT_MSG(strstr(err, "4095") != NULL,
                  "the refusal does not name the cap: %s", err);

    /* A cap of nothing refuses an entry that holds something. */
    err[0] = '\0';
    UT_ASSERT(!scnPackageReadEntry(p, "main.lua", 0, &bytes, &len, err,
                                   sizeof(err)));
    UT_ASSERT(err[0] != '\0');

    /* The manifest beside it is well inside its own cap and still reads. */
    err[0] = '\0';
    UT_ASSERT_MSG(scnPackageReadEntry(p, "manifest.json",
                                      SCN_PACKAGE_MANIFEST_MAX_BYTES, &bytes,
                                      &len, err, sizeof(err)),
                  "the manifest was refused under its own cap: %s", err);
    UT_ASSERT(len == sizeof(kManifest) - 1);
    free(bytes);
    bytes = NULL;

    /* An entry that is not in the container gives no reason, which is what
       tells the callers apart from a refusal they should report. */
    err[0] = '\0';
    UT_ASSERT(!scnPackageReadEntry(p, "credits.txt", PKG_TEST_MAX, &bytes, &len,
                                   err, sizeof(err)));
    UT_ASSERT_MSG(err[0] == '\0',
                  "an entry that is not there gave a reason: %s", err);

    scnPackageClose(p);
    free(packed);
    free(body);
    return 0;
}
