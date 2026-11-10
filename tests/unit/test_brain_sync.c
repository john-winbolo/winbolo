/*
 * Bot Host brain sync (brain_sync.c): the list of a server's brain files
 * that a Bot Host downloads, the checks on the paths in it, and the host's
 * cache of those files.
 *
 *   1. brain_sync_path_check: relative '/' paths pass; absolute paths, "..",
 *      drive letters, backslashes, dot files and device names are refused.
 *   2. brain_sync_manifest_codec: a manifest made from a folder on disk keeps
 *      every name, path, size and hash through the wire form; a short,
 *      altered or unsafe one is refused.
 *   3. brain_sync_cache_plan: an empty cache needs every file; after the
 *      files are written every file is held; a new version with one changed
 *      file copies the others from the old version folder and needs only
 *      that one.
 *   4. brain_sync_chunk: a packed chunk unpacks to the same bytes, and a
 *      damaged or too large one is refused.
 *   5. brain_sync_shipped_match: an install whose brain files are the ones
 *      in the build's table passes; a brain the table lacks does not.
 *   6. brain_sync_shipped_edited: an edited or added file that the brain
 *      runs is refused and named; a file it does not run may change.
 *   7. brain_sync_shipped_missing: a shipped file gone from the install is
 *      refused and named.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "brain_sync.h"
#include "machine_id.h"   /* machineSha256 */
#include "test_harness.h"

/* ================================================================
 * 1. Path checks
 * ================================================================ */
int run_brain_sync_path_check(void) {
    static const char *const good[] = {
        "init.lua", "opt/init.lua", "lib/a/b/c.lua", "shield_stamp_cache.bin",
        "a b.lua", "my-brain_2.lua", "CONSOLE.lua", "com10.txt"
    };
    static const char *const bad[] = {
        "", "/init.lua", "../init.lua", "lib/../../x", "lib/./x.lua", "a//b",
        "a/", "C:/x.lua", "C:x.lua", "lib\\x.lua", ".git/config", "lib/.hidden",
        "x.lua.", "x.lua ", "CON", "nul.txt", "lib/COM1.lua", "lpt9",
        "a|b", "a*b", "a?b", "a\"b", "a<b", "a>b", "tab\there"
    };
    char longPath[BRAIN_SYNC_PATH_MAX + 8];
    size_t i;

    for (i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        UT_ASSERT_MSG(brainSyncPathSafe(good[i]), "refused \"%s\"", good[i]);
    }
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        UT_ASSERT_MSG(!brainSyncPathSafe(bad[i]), "passed \"%s\"", bad[i]);
    }
    memset(longPath, 'a', sizeof(longPath));
    longPath[sizeof(longPath) - 1] = '\0';
    UT_ASSERT(!brainSyncPathSafe(longPath));

    UT_ASSERT(brainSyncNameSafe("GoalHunter"));
    UT_ASSERT(!brainSyncNameSafe("Goal/Hunter"));
    UT_ASSERT(!brainSyncNameSafe(".."));
    UT_ASSERT(!brainSyncNameSafe(""));

    {
        char out[512];
        UT_ASSERT(brainSyncCacheFile(out, sizeof(out), "root", "B", 0x1234u,
                                     "opt/init.lua"));
        UT_ASSERT(strcmp(out, "root/B/0000000000001234/opt/init.lua") == 0);
        UT_ASSERT(!brainSyncCacheFile(out, sizeof(out), "root", "B", 1u,
                                      "../escape.lua"));
        UT_ASSERT(!brainSyncCacheFile(out, sizeof(out), "root", "..", 1u,
                                      "init.lua"));
        UT_ASSERT(!brainSyncCacheFile(out, sizeof(out), "root", "B", 1u,
                                      "C:/Windows/x"));
    }
    return 0;
}

/* ================================================================
 * Fixture: a brain folder in the scratch dir
 * ================================================================ */

static bool bs_write(const char *dir, const char *rel, const char *text) {
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%s/%s", dir, rel);
    return brainSyncWriteFile(path, text, strlen(text));
}

/* A brain "TestBrain" at <scratch>/<leaf>, collected into m. */
static int bs_make_brain(const char *leaf, const char *initText,
                         BrainSyncManifest *m, char *dirOut, size_t dirSz) {
    char dir[1024];
    if (!utScratchPath(dir, sizeof(dir), leaf)) return -1;
    if (!bs_write(dir, "init.lua", initText)) return -2;
    if (!bs_write(dir, "lib/util.lua", "return { n = 1 }\n")) return -3;
    if (!bs_write(dir, "lib/deep/more.lua", "return 42\n")) return -4;
    {
        /* A binary file with a NUL in it. */
        char path[1024];
        SDL_snprintf(path, sizeof(path), "%s/data.bin", dir);
        if (!brainSyncWriteFile(path, "\x01\x02\x03\x00\x05", 5)) return -5;
    }
    if (!bs_write(dir, ".hidden", "skip me")) return -6;
    memset(m, 0, sizeof(*m));
    m->debugMode = true;
    if (!brainSyncCollectBrain(&m->brains[0], "TestBrain", dir, true, NULL)) {
        return -7;
    }
    m->brainCount = 1;
    brainSyncComputeVersions(m);
    if (dirOut != NULL) SDL_strlcpy(dirOut, dir, dirSz);
    return 0;
}

/* ================================================================
 * 2. Manifest codec
 * ================================================================ */
int run_brain_sync_manifest_codec(void) {
    BrainSyncManifest m, d;
    uint8_t *blob, *bad;
    size_t len = 0, i;
    int r, f;

    r = bs_make_brain("brain", "print('hello')\n", &m, NULL, 0);
    UT_ASSERT_MSG(r == 0, "fixture failed (%d)", r);
    /* The dot file is left out; the other four are in, sorted by path. */
    UT_ASSERT_MSG(m.brains[0].fileCount == 4, "%d files",
                  m.brains[0].fileCount);
    UT_ASSERT(strcmp(m.brains[0].files[0].path, "data.bin") == 0);
    UT_ASSERT(strcmp(m.brains[0].files[1].path, "init.lua") == 0);
    UT_ASSERT(strcmp(m.brains[0].files[2].path, "lib/deep/more.lua") == 0);
    UT_ASSERT(strcmp(m.brains[0].files[3].path, "lib/util.lua") == 0);
    UT_ASSERT(m.brains[0].files[0].size == 5);
    UT_ASSERT(m.brains[0].files[0].hash ==
              brainSyncHash(BRAIN_SYNC_HASH_SEED, "\x01\x02\x03\x00\x05", 5));

    blob = brainSyncManifestEncode(&m, &len);
    UT_ASSERT(blob != NULL && len > 13);
    UT_ASSERT(brainSyncManifestDecode(blob, len, &d));
    UT_ASSERT(d.debugMode);
    UT_ASSERT(d.version == m.version);
    UT_ASSERT(d.brainCount == 1);
    UT_ASSERT(strcmp(d.brains[0].name, "TestBrain") == 0);
    UT_ASSERT(d.brains[0].version == m.brains[0].version);
    UT_ASSERT(d.brains[0].fileCount == m.brains[0].fileCount);
    UT_ASSERT(d.brains[0].totalBytes == m.brains[0].totalBytes);
    for (f = 0; f < d.brains[0].fileCount; f++) {
        UT_ASSERT(strcmp(d.brains[0].files[f].path,
                         m.brains[0].files[f].path) == 0);
        UT_ASSERT(d.brains[0].files[f].size == m.brains[0].files[f].size);
        UT_ASSERT(d.brains[0].files[f].hash == m.brains[0].files[f].hash);
    }
    UT_ASSERT(brainSyncFindBrain(&d, "TestBrain") != NULL);
    UT_ASSERT(brainSyncFindBrain(&d, "Other") == NULL);
    brainSyncFree(&d);

    /* Every short form is refused. */
    for (i = 0; i < len; i++) {
        UT_ASSERT_MSG(!brainSyncManifestDecode(blob, i, &d),
                      "a %u-byte prefix decoded", (unsigned)i);
    }
    /* A changed byte anywhere is refused: a header field, a name, a path,
     * a size or a hash, each caught by a check or a version. */
    bad = (uint8_t *)malloc(len + 1);
    UT_ASSERT(bad != NULL);
    for (i = 0; i < len; i++) {
        memcpy(bad, blob, len);
        bad[i] ^= 0x01;
        UT_ASSERT_MSG(!brainSyncManifestDecode(bad, len, &d),
                      "a change at byte %u decoded", (unsigned)i);
    }
    /* A trailing byte is refused. */
    memcpy(bad, blob, len);
    bad[len] = 0;
    UT_ASSERT(!brainSyncManifestDecode(bad, len + 1, &d));
    free(bad);
    free(blob);

    /* A path that leaves the cache is refused, even with the right
     * versions. */
    SDL_strlcpy(m.brains[0].files[3].path, "../../evil.lua",
                sizeof(m.brains[0].files[3].path));
    brainSyncComputeVersions(&m);
    blob = brainSyncManifestEncode(&m, &len);
    UT_ASSERT(blob != NULL);
    UT_ASSERT(!brainSyncManifestDecode(blob, len, &d));
    free(blob);
    brainSyncFree(&m);
    return 0;
}

/* ================================================================
 * 3. Cache plan
 * ================================================================ */

/* Write each file of the plan's needs from the brain folder into the
 * cache, as a Bot Host does after it downloads them. */
static int bs_fill_cache(const BrainSyncManifest *m, const BrainSyncPlan *p,
                         const char *root) {
    int i;
    for (i = 0; i < p->needCount; i++) {
        const BrainSyncBrain *br = &m->brains[p->need[i] >> 16];
        const BrainSyncFile *f = &br->files[p->need[i] & 0xffffu];
        char src[1024], dst[1024];
        size_t sz = 0;
        void *data;
        SDL_snprintf(src, sizeof(src), "%s/%s", br->dir, f->path);
        data = SDL_LoadFile(src, &sz);
        if (data == NULL) return -1;
        if (!brainSyncCacheFile(dst, sizeof(dst), root, br->name, br->version,
                                f->path) ||
            !brainSyncWriteFile(dst, data, sz)) {
            SDL_free(data);
            return -2;
        }
        SDL_free(data);
    }
    return 0;
}

int run_brain_sync_cache_plan(void) {
    BrainSyncManifest m, m2;
    BrainSyncPlan p;
    char root[1024], dir[1024], cached[1024];
    int r;

    r = bs_make_brain("brain", "print('one')\n", &m, dir, sizeof(dir));
    UT_ASSERT_MSG(r == 0, "fixture failed (%d)", r);
    UT_ASSERT(utScratchPath(root, sizeof(root), "cache"));

    /* Empty cache: every file to download. */
    UT_ASSERT(brainSyncPlanCache(&m, root, &p));
    UT_ASSERT(p.haveCount == 0 && p.copyCount == 0);
    UT_ASSERT(p.needCount == 4);
    UT_ASSERT(p.needBytes == m.brains[0].totalBytes);
    UT_ASSERT(bs_fill_cache(&m, &p, root) == 0);
    free(p.need);

    /* Second look: every file held, nothing to download. */
    UT_ASSERT(brainSyncPlanCache(&m, root, &p));
    UT_ASSERT(p.haveCount == 4 && p.copyCount == 0 && p.needCount == 0);
    UT_ASSERT(p.needBytes == 0);
    free(p.need);

    /* A cached file that was changed on disk is downloaded again. */
    UT_ASSERT(brainSyncCacheFile(cached, sizeof(cached), root, "TestBrain",
                                 m.brains[0].version, "lib/util.lua"));
    UT_ASSERT(brainSyncWriteFile(cached, "tampered", 8));
    UT_ASSERT(brainSyncPlanCache(&m, root, &p));
    UT_ASSERT(p.haveCount == 3 && p.needCount == 1);
    UT_ASSERT(strcmp(m.brains[0].files[p.need[0] & 0xffffu].path,
                     "lib/util.lua") == 0);
    UT_ASSERT(bs_fill_cache(&m, &p, root) == 0);
    free(p.need);

    /* The server changes one file: the new version gets the other three
     * from the old version folder and downloads only that one. */
    UT_ASSERT(bs_write(dir, "init.lua", "print('two')\n"));
    memset(&m2, 0, sizeof(m2));
    m2.debugMode = true;
    UT_ASSERT(brainSyncCollectBrain(&m2.brains[0], "TestBrain", dir, true,
                                    NULL));
    m2.brainCount = 1;
    brainSyncComputeVersions(&m2);
    UT_ASSERT(m2.brains[0].version != m.brains[0].version);
    UT_ASSERT(m2.version != m.version);
    UT_ASSERT(brainSyncPlanCache(&m2, root, &p));
    UT_ASSERT_MSG(p.haveCount == 0 && p.copyCount == 3 && p.needCount == 1,
                  "have %d copy %d need %d", p.haveCount, p.copyCount,
                  p.needCount);
    UT_ASSERT(strcmp(m2.brains[0].files[p.need[0] & 0xffffu].path,
                     "init.lua") == 0);
    UT_ASSERT(p.needBytes == (uint32_t)strlen("print('two')\n"));
    UT_ASSERT(bs_fill_cache(&m2, &p, root) == 0);
    free(p.need);

    /* Both versions are now held in full. */
    UT_ASSERT(brainSyncPlanCache(&m2, root, &p));
    UT_ASSERT(p.haveCount == 4 && p.needCount == 0);
    free(p.need);
    UT_ASSERT(brainSyncPlanCache(&m, root, &p));
    UT_ASSERT(p.haveCount == 4 && p.needCount == 0);
    free(p.need);

    brainSyncFree(&m);
    brainSyncFree(&m2);
    return 0;
}

/* ================================================================
 * 4. Chunks
 * ================================================================ */
int run_brain_sync_chunk(void) {
    uint32_t n = BRAIN_SYNC_CHUNK_BYTES, packedLen = 0, i;
    uint8_t *raw = (uint8_t *)malloc(n);
    uint8_t *out = (uint8_t *)malloc(n);
    uint8_t *packed;
    int64_t got;

    UT_ASSERT(raw != NULL && out != NULL);
    for (i = 0; i < n; i++) raw[i] = (uint8_t)((i * 7u) ^ (i >> 5));
    packed = brainSyncChunkPack(raw, n, &packedLen);
    UT_ASSERT(packed != NULL && packedLen > 4);
    got = brainSyncChunkUnpack(packed, packedLen, out, n);
    UT_ASSERT(got == (int64_t)n);
    UT_ASSERT(memcmp(raw, out, n) == 0);
    /* Too small a buffer, a cut chunk and a damaged one are refused. */
    UT_ASSERT(brainSyncChunkUnpack(packed, packedLen, out, n - 1) == -1);
    UT_ASSERT(brainSyncChunkUnpack(packed, packedLen / 2, out, n) == -1);
    UT_ASSERT(brainSyncChunkUnpack(packed, 3, out, n) == -1);
    packed[packedLen / 2] ^= 0xFF;
    UT_ASSERT(brainSyncChunkUnpack(packed, packedLen, out, n) == -1);
    free(packed);

    /* An empty piece (an empty file) is fine. */
    packed = brainSyncChunkPack(raw, 0, &packedLen);
    UT_ASSERT(packed != NULL);
    UT_ASSERT(brainSyncChunkUnpack(packed, packedLen, out, 0) == 0);
    free(packed);
    free(raw);
    free(out);
    return 0;
}

/* ================================================================
 * 5-7. The brain files a build shipped (brainSyncShippedCheck)
 * ================================================================ */

/* A table as cmake/brain_shipped_table.cmake writes it: every file of the
 * brain folder, with its SHA-256 in lower-case hex. */
#define BS_SHIP_MAX 16
static char bsShipPath[BS_SHIP_MAX][96];
static char bsShipSha[BS_SHIP_MAX][65];
static BrainSyncShippedFile bsShip[BS_SHIP_MAX];
static int bsShipCount;

static void bs_ship_row(const char *path, const char *text) {
    static const char digits[] = "0123456789abcdef";
    uint8_t sha[MACHINE_HASH_LEN];
    int i, n = bsShipCount++;
    machineSha256(text, strlen(text), sha);
    for (i = 0; i < MACHINE_HASH_LEN; i++) {
        bsShipSha[n][i * 2] = digits[sha[i] >> 4];
        bsShipSha[n][i * 2 + 1] = digits[sha[i] & 0x0f];
    }
    bsShipSha[n][64] = '\0';
    SDL_strlcpy(bsShipPath[n], path, sizeof(bsShipPath[n]));
    bsShip[n].path = bsShipPath[n];
    bsShip[n].sha256 = bsShipSha[n];
}

/* Write file rel of brain "Shipped" under root and put it in the table. */
static bool bs_ship(const char *root, const char *rel, const char *text) {
    char dir[1024], path[160];
    SDL_snprintf(dir, sizeof(dir), "%s/Shipped", root);
    SDL_snprintf(path, sizeof(path), "Shipped/%s", rel);
    bs_ship_row(path, text);
    return bs_write(dir, rel, text);
}

/* An install root at <scratch>/<leaf> with the brain "Shipped" as a build
 * ships it: an opt/ copy, a top-level init.lua that opt/ replaces, a
 * top-level file opt/ does not have, and a sub-folder an opt brain does
 * not run. The table also holds a second brain's file. */
static int bs_make_shipped(const char *leaf, char *root, size_t rootSz) {
    bsShipCount = 0;
    if (!utScratchPath(root, rootSz, leaf)) return -1;
    if (!bs_ship(root, "opt/init.lua", "require('lib')\n")) return -2;
    if (!bs_ship(root, "opt/lib.lua", "return { fast = true }\n")) return -3;
    if (!bs_ship(root, "init.lua", "print('full source')\n")) return -4;
    if (!bs_ship(root, "modes.txt", "default\n")) return -5;
    if (!bs_ship(root, "util.lua", "return 7\n")) return -6;
    if (!bs_ship(root, "src/deep.lua", "return 'not run'\n")) return -7;
    bs_ship_row("Other/init.lua", "print('other')\n");
    return 0;
}

static BrainSyncShippedResult bs_check(const char *root, char *file,
                                       size_t fileSz) {
    return brainSyncShippedCheck(bsShip, bsShipCount, root, "Shipped", file,
                                 fileSz);
}

/* 5. The files on disk are the shipped ones. */
int run_brain_sync_shipped_match(void) {
    char root[1024], file[BRAIN_SYNC_PATH_MAX];
    int r = bs_make_shipped("ship_match", root, sizeof(root));
    UT_ASSERT_MSG(r == 0, "fixture failed (%d)", r);

    UT_ASSERT(bs_check(root, file, sizeof(file)) == BRAIN_SYNC_SHIPPED_OK);
    UT_ASSERT(file[0] == '\0');
    /* A brain the table does not have is not run, nor is one with no
     * folder in the install. */
    UT_ASSERT(brainSyncShippedCheck(bsShip, bsShipCount, root, "NewBrain",
                                    file, sizeof(file)) ==
              BRAIN_SYNC_SHIPPED_NOT_SHIPPED);
    UT_ASSERT(brainSyncShippedCheck(bsShip, bsShipCount, root, "Other",
                                    file, sizeof(file)) ==
              BRAIN_SYNC_SHIPPED_UNREADABLE);
    UT_ASSERT(brainSyncShippedCheck(NULL, 0, root, "Shipped", file,
                                    sizeof(file)) ==
              BRAIN_SYNC_SHIPPED_NOT_SHIPPED);
    return 0;
}

/* 6. A file that was edited after it shipped, or added, is refused and
 *    named; a file the brain does not run may change. */
int run_brain_sync_shipped_edited(void) {
    char root[1024], dir[1100], file[BRAIN_SYNC_PATH_MAX];
    int r = bs_make_shipped("ship_edited", root, sizeof(root));
    UT_ASSERT_MSG(r == 0, "fixture failed (%d)", r);
    SDL_snprintf(dir, sizeof(dir), "%s/Shipped", root);

    /* The full-source init.lua and src/ are not what an opt brain runs. */
    UT_ASSERT(bs_write(dir, "init.lua", "print('edited, not run')\n"));
    UT_ASSERT(bs_write(dir, "src/deep.lua", "return 'edited'\n"));
    UT_ASSERT(bs_check(root, file, sizeof(file)) == BRAIN_SYNC_SHIPPED_OK);

    /* An edit to a file it runs. */
    UT_ASSERT(bs_write(dir, "opt/lib.lua", "return { fast = false }\n"));
    UT_ASSERT(bs_check(root, file, sizeof(file)) ==
              BRAIN_SYNC_SHIPPED_CHANGED);
    UT_ASSERT_MSG(strcmp(file, "opt/lib.lua") == 0, "named \"%s\"", file);
    UT_ASSERT(strcmp(brainSyncShippedResultText(BRAIN_SYNC_SHIPPED_CHANGED),
                     "changed since it shipped") == 0);

    /* Put back, it runs again; a file added beside it is refused. */
    UT_ASSERT(bs_write(dir, "opt/lib.lua", "return { fast = true }\n"));
    UT_ASSERT(bs_check(root, file, sizeof(file)) == BRAIN_SYNC_SHIPPED_OK);
    UT_ASSERT(bs_write(dir, "opt/extra.lua", "os.exit()\n"));
    UT_ASSERT(bs_check(root, file, sizeof(file)) == BRAIN_SYNC_SHIPPED_EXTRA);
    UT_ASSERT_MSG(strcmp(file, "opt/extra.lua") == 0, "named \"%s\"", file);
    return 0;
}

/* 7. A shipped file that is gone from the install is refused and named. */
int run_brain_sync_shipped_missing(void) {
    char root[1024], path[1200], file[BRAIN_SYNC_PATH_MAX];
    int r = bs_make_shipped("ship_missing", root, sizeof(root));
    UT_ASSERT_MSG(r == 0, "fixture failed (%d)", r);

    SDL_snprintf(path, sizeof(path), "%s/Shipped/util.lua", root);
    UT_ASSERT(SDL_RemovePath(path));
    UT_ASSERT(bs_check(root, file, sizeof(file)) ==
              BRAIN_SYNC_SHIPPED_MISSING);
    UT_ASSERT_MSG(strcmp(file, "util.lua") == 0, "named \"%s\"", file);

    /* Without opt/init.lua the folder lists as a full-source brain, which
     * is not the brain that shipped. */
    SDL_snprintf(path, sizeof(path), "%s/Shipped/opt/init.lua", root);
    UT_ASSERT(SDL_RemovePath(path));
    UT_ASSERT(bs_check(root, file, sizeof(file)) != BRAIN_SYNC_SHIPPED_OK);
    return 0;
}
