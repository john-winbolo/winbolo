/*
 * mapReadFromMemory — in-memory BMAP parser.
 *
 * mapReadFromMemory parses an on-disk .map file image held in RAM
 * (e.g. a WinBolo.net map downloaded over HTTP) without spilling it
 * to a temp file. It shares its parse core with the historical
 * mapRead(FILE*), so the two must agree byte-for-byte on every valid
 * input and reject the same malformed ones.
 *
 * Strategy:
 *   1. Build a non-trivial map programmatically (terrain runs +
 *      pillboxes + bases + starts), write it to a real .map file with
 *      mapWrite, then read those exact bytes back two ways — through
 *      mapRead (disk) and mapReadFromMemory (the file bytes loaded
 *      into a buffer) — and assert the two decoded worlds are
 *      identical cell-by-cell and record-by-record. This exercises
 *      the full run-length decoder, the fread record paths and the
 *      ungetc-at-run-boundary path through the memory backend.
 *   2. Decode a hand-built blob straight from memory and pin the
 *      field values (independent of mapWrite).
 *   3. Reject NULL / zero-length / bad-magic / truncated buffers.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "bolo_map.h"
#include "game_sim.h"
#include "client_mappreview.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "test_harness.h"

/* Each test builds its map path with utScratchPath, so the two tests below
 * that write one cannot delete it under each other. They used to share
 * "data/maps/.test_read_memory.map": both wrote it, both slurped it and both
 * removed it on the way out, so under `ctest -j` whichever lost the race
 * failed on a file the other had already taken away. Writing under the
 * scratch directory rather than data/maps also keeps a test that dies before
 * its cleanup from leaving a dotfile in the map tree. */
#define MAP_LEAF "read_memory.map"

/* Read an entire file into a malloc'd buffer. Returns NULL on error;
 * sets *outLen to the byte count on success. */
static uint8_t *slurp(const char *path, size_t *outLen) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) { free(buf); return NULL; }
    *outLen = got;
    return buf;
}

/* Build a deterministic, non-trivial map and write it to `path`.
 * Returns false on any setup failure. */
static bool build_and_write_map(const char *path) {
    map mp = NULL;
    pillboxes pb = NULL;
    bases bs = NULL;
    starts ss = NULL;
    mapCreate(&mp);
    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);

    /* Terrain: lay several horizontal bands of differing terrain so
     * the run-length encoder emits multi-run rows (same-runs and
     * diff-runs), which the reader must walk including the ungetc at
     * each run-header boundary. Stay inside the playable area. */
    for (int y = 20; y < 60; y++) {
        for (int x = 20; x < 200; x++) {
            BYTE t;
            if (y < 30)       t = GRASS;
            else if (y < 40)  t = ROAD;
            else if (y < 50)  t = (x & 1) ? GRASS : ROAD; /* alternating */
            else              t = BUILDING;
            mp->mapItem[x][y] = t;
        }
    }

    /* Two pillboxes, two bases, two starts with distinct field values.
     * The set APIs take a pointer-to-handle (pillboxes *), so pass &.
     * pillsSetPill reads the pill caps off a sim; there is no game behind
     * this list, and every value below is inside the classic ones. */
    pillsSetNumPills(&pb, 2);
    pillbox p0 = {0}; p0.x = 40; p0.y = 40; p0.owner = 0xFF; p0.armour = 15; p0.speed = 50;
    pillbox p1 = {0}; p1.x = 80; p1.y = 45; p1.owner = 0xFF; p1.armour = 10; p1.speed = 50;
    pillsSetPill(ut_rules_only_sim(), &pb, &p0, 1);
    pillsSetPill(ut_rules_only_sim(), &pb, &p1, 2);

    basesSetNumBases(&bs, 2);
    base b0 = {0}; b0.x = 50; b0.y = 50; b0.owner = 0xFF; b0.armour = 90; b0.shells = 90; b0.mines = 90;
    base b1 = {0}; b1.x = 90; b1.y = 55; b1.owner = 0xFF; b1.armour = 80; b1.shells = 40; b1.mines = 10;
    basesSetBase(&bs, &b0, 1);
    basesSetBase(&bs, &b1, 2);

    startsSetNumStarts(&ss, 2);
    start s0 = {0}; s0.x = 60; s0.y = 60; s0.dir = 8;
    start s1 = {0}; s1.x = 100; s1.y = 65; s1.dir = 3;
    startsSetStart(&ss, &s0, 1);
    startsSetStart(&ss, &s1, 2);

    /* No mkdir: utScratchPath created the directory this path sits in. */
    bool ok = mapWrite((char *)path, &mp, &pb, &bs, &ss);

    mapDestroy(&mp);
    pillsDestroy(&pb);
    basesDestroy(&bs);
    startsDestroy(&ss);
    return ok;
}

int run_map_read_memory_matches_file(void) {
    char mapPath[1024];

    UT_ASSERT(utScratchPath(mapPath, sizeof(mapPath), MAP_LEAF));
    UT_ASSERT_MSG(build_and_write_map(mapPath), "failed to build/write test map");

    size_t blobLen = 0;
    uint8_t *blob = slurp(mapPath, &blobLen);
    UT_ASSERT_MSG(blob != NULL, "failed to slurp written map");

    /* Path A: read from disk. */
    map fMp = NULL; pillboxes fPb = NULL; bases fBs = NULL; starts fSs = NULL;
    mapCreate(&fMp); pillsCreate(&fPb); basesCreate(&fBs); startsCreate(&fSs);
    bool okFile = mapRead((char *)mapPath, &fMp, &fPb, &fBs, &fSs);

    /* Path B: read the same bytes from memory. */
    map mMp = NULL; pillboxes mPb = NULL; bases mBs = NULL; starts mSs = NULL;
    mapCreate(&mMp); pillsCreate(&mPb); basesCreate(&mBs); startsCreate(&mSs);
    bool okMem = mapReadFromMemory(blob, (int)blobLen, &mMp, &mPb, &mBs, &mSs);

    int rc = 0;

    if (!okFile || !okMem) {
        fprintf(stderr, "FAIL %s:%d: parse mismatch okFile=%d okMem=%d\n",
                __FILE__, __LINE__, (int)okFile, (int)okMem);
        rc = 1;
    }

    /* Counts agree. */
    if (rc == 0 &&
        (pillsGetNumPills(&fPb) != pillsGetNumPills(&mPb) ||
         basesGetNumBases(&fBs) != basesGetNumBases(&mBs) ||
         startsGetNumStarts(&fSs) != startsGetNumStarts(&mSs))) {
        fprintf(stderr, "FAIL %s:%d: count mismatch pills %d/%d bases %d/%d starts %d/%d\n",
                __FILE__, __LINE__,
                pillsGetNumPills(&fPb), pillsGetNumPills(&mPb),
                basesGetNumBases(&fBs), basesGetNumBases(&mBs),
                startsGetNumStarts(&fSs), startsGetNumStarts(&mSs));
        rc = 1;
    }

    /* Every terrain cell agrees. */
    if (rc == 0) {
        for (int y = 0; y < MAP_ARRAY_SIZE && rc == 0; y++) {
            for (int x = 0; x < MAP_ARRAY_SIZE; x++) {
                if (fMp->mapItem[x][y] != mMp->mapItem[x][y]) {
                    fprintf(stderr,
                            "FAIL %s:%d: terrain mismatch at (%d,%d) file=%d mem=%d\n",
                            __FILE__, __LINE__, x, y,
                            fMp->mapItem[x][y], mMp->mapItem[x][y]);
                    rc = 1;
                    break;
                }
            }
        }
    }

    /* Every pillbox / base / start record agrees on its wire fields. */
    if (rc == 0) {
        BYTE n = pillsGetNumPills(&fPb);
        for (BYTE i = 1; i <= n && rc == 0; i++) {
            pillbox a, b;
            pillsGetPill(&fPb, &a, i);
            pillsGetPill(&mPb, &b, i);
            if (a.x != b.x || a.y != b.y || a.owner != b.owner ||
                a.armour != b.armour || a.speed != b.speed) {
                fprintf(stderr, "FAIL %s:%d: pill %d mismatch\n", __FILE__, __LINE__, i);
                rc = 1;
            }
        }
    }
    if (rc == 0) {
        BYTE n = basesGetNumBases(&fBs);
        for (BYTE i = 1; i <= n && rc == 0; i++) {
            base a, b;
            basesGetBase(&fBs, &a, i);
            basesGetBase(&mBs, &b, i);
            if (a.x != b.x || a.y != b.y || a.owner != b.owner ||
                a.armour != b.armour || a.shells != b.shells || a.mines != b.mines) {
                fprintf(stderr, "FAIL %s:%d: base %d mismatch\n", __FILE__, __LINE__, i);
                rc = 1;
            }
        }
    }
    if (rc == 0) {
        BYTE n = startsGetNumStarts(&fSs);
        for (BYTE i = 1; i <= n && rc == 0; i++) {
            start a, b;
            startsGetStartStruct(&fSs, &a, i);
            startsGetStartStruct(&mSs, &b, i);
            if (a.x != b.x || a.y != b.y || a.dir != b.dir) {
                fprintf(stderr, "FAIL %s:%d: start %d mismatch\n", __FILE__, __LINE__, i);
                rc = 1;
            }
        }
    }

    free(blob);
    mapDestroy(&fMp); pillsDestroy(&fPb); basesDestroy(&fBs); startsDestroy(&fSs);
    mapDestroy(&mMp); pillsDestroy(&mPb); basesDestroy(&mBs); startsDestroy(&mSs);
    SDL_RemovePath(mapPath);
    return rc;
}

/* A minimal hand-built valid blob: header, version 1, one pill, one
 * base, one start, then the empty-runs terminator. Decoded straight
 * from memory; pins the record decode independent of mapWrite. */
static const uint8_t kHandMap[] = {
    'B','M','A','P','B','O','L','O',
    0x01,                       /* version */
    0x01, 0x01, 0x01,           /* numPills, numBases, numStarts */
    50, 50, 0xFF, 15, 50,       /* pill: x y owner armour speed */
    60, 60, 0xFF, 90, 90, 90,   /* base: x y owner armour shells mines */
    70, 70, 9,                  /* start: x y dir */
    0x04, 0xFF, 0xFF, 0xFF,     /* run terminator */
};

int run_map_read_memory_handbuilt(void) {
    map mp = NULL; pillboxes pb = NULL; bases bs = NULL; starts ss = NULL;
    mapCreate(&mp); pillsCreate(&pb); basesCreate(&bs); startsCreate(&ss);

    bool ok = mapReadFromMemory(kHandMap, (int)sizeof(kHandMap), &mp, &pb, &bs, &ss);

    int rc = 0;
    if (!ok) {
        fprintf(stderr, "FAIL %s:%d: hand-built map must decode\n", __FILE__, __LINE__);
        rc = 1;
    }
    if (rc == 0) {
        pillbox p; base b; start s;
        if (pillsGetNumPills(&pb) != 1 || basesGetNumBases(&bs) != 1 ||
            startsGetNumStarts(&ss) != 1) {
            fprintf(stderr, "FAIL %s:%d: hand-built counts wrong\n", __FILE__, __LINE__);
            rc = 1;
        }
        if (rc == 0) {
            pillsGetPill(&pb, &p, 1);
            basesGetBase(&bs, &b, 1);
            startsGetStartStruct(&ss, &s, 1);
            if (p.x != 50 || p.y != 50 || p.armour != 15 ||
                b.x != 60 || b.shells != 90 || s.x != 70 || s.dir != 9) {
                fprintf(stderr, "FAIL %s:%d: hand-built field mismatch\n", __FILE__, __LINE__);
                rc = 1;
            }
        }
    }

    mapDestroy(&mp); pillsDestroy(&pb); basesDestroy(&bs); startsDestroy(&ss);
    return rc;
}


#define MINED_PILL_LEAF "mined_pill.map"

/* The same load-time pill fixup run_map_pill_mine_cleared_on_load pins on the
 * resync-blob path, on the path a .map file takes. A pillbox standing on a
 * mined square hides the mine from everything that could set it off, so the
 * loader strips it back to the ground it was laid on. The .map format has no
 * field for a pill riding in a tank, so only the cleared case exists here. */
int run_map_read_clears_mine_under_pill(void) {
    map mp = NULL; pillboxes pb = NULL; bases bs = NULL; starts ss = NULL;
    char mapPath[1024];
    size_t blobLen = 0;
    uint8_t *blob;
    pillbox got;
    bool wrote;
    int rc = 0;

    mapCreate(&mp); pillsCreate(&pb); basesCreate(&bs); startsCreate(&ss);

    /* A block of land big enough that the run encoder has something to say,
     * with a single mined square at the pill's position. */
    for (int y = 30; y < 70; y++) {
        for (int x = 30; x < 70; x++) {
            mp->mapItem[x][y] = GRASS;
        }
    }
    mp->mapItem[40][40] = MINE_GRASS;

    pillsSetNumPills(&pb, 1);
    {
        pillbox p = {0};
        p.x = 40; p.y = 40; p.owner = 0xFF; p.armour = 15; p.speed = 50;
        pillsSetPill(ut_rules_only_sim(), &pb, &p, 1);
    }

    UT_ASSERT(utScratchPath(mapPath, sizeof(mapPath), MINED_PILL_LEAF));
    wrote = mapWrite((char *)mapPath, &mp, &pb, &bs, &ss);
    mapDestroy(&mp); pillsDestroy(&pb); basesDestroy(&bs); startsDestroy(&ss);
    UT_ASSERT_MSG(wrote, "failed to write the mined-pill map");

    blob = slurp(mapPath, &blobLen);
    UT_ASSERT_MSG(blob != NULL, "failed to slurp the mined-pill map");

    mapCreate(&mp); pillsCreate(&pb); basesCreate(&bs); startsCreate(&ss);
    if (!mapReadFromMemory(blob, (int)blobLen, &mp, &pb, &bs, &ss)) {
        fprintf(stderr, "FAIL %s:%d: mined-pill map must decode\n", __FILE__, __LINE__);
        rc = 1;
    }

    if (rc == 0) {
        /* mapReadStream centres the map, which moves the pill with it, so ask
         * the decoded record where the pill ended up rather than assuming. The
         * terrain byte is read straight out of mapItem: a mine has to stay
         * distinguishable from the ground under it. */
        pillsGetPill(&pb, &got, 1);
        if (mp->mapItem[got.x][got.y] != GRASS) {
            fprintf(stderr,
                    "FAIL %s:%d: mine under a pill survived the file load: "
                    "(%u,%u) = %u, expected GRASS(%u)\n",
                    __FILE__, __LINE__, (unsigned)got.x, (unsigned)got.y,
                    (unsigned)mp->mapItem[got.x][got.y], (unsigned)GRASS);
            rc = 1;
        }
    }

    free(blob);
    mapDestroy(&mp); pillsDestroy(&pb); basesDestroy(&bs); startsDestroy(&ss);
    SDL_RemovePath(mapPath);
    return rc;
}

/* The conversion the WBN preview path actually relies on: on-disk
 * .map bytes in RAM -> runtime compressed map -> MapPreview. The
 * round-tripped preview must match the directly-parsed map. */
int run_map_convert_file_to_compressed(void) {
    char mapPath[1024];

    UT_ASSERT(utScratchPath(mapPath, sizeof(mapPath), MAP_LEAF));
    UT_ASSERT_MSG(build_and_write_map(mapPath), "failed to build/write test map");

    size_t blobLen = 0;
    uint8_t *blob = slurp(mapPath, &blobLen);
    UT_ASSERT_MSG(blob != NULL, "failed to slurp written map");

    /* Direct parse for the reference world. */
    map rMp = NULL; pillboxes rPb = NULL; bases rBs = NULL; starts rSs = NULL;
    mapCreate(&rMp); pillsCreate(&rPb); basesCreate(&rBs); startsCreate(&rSs);
    bool okRef = mapReadFromMemory(blob, (int)blobLen, &rMp, &rPb, &rBs, &rSs);

    int compLen = 0;
    BYTE *comp = clientMapConvertFileToCompressed(blob, (int)blobLen, &compLen);

    int rc = 0;
    MapPreview *mpv = NULL;

    if (!okRef || !comp || compLen <= 0) {
        fprintf(stderr, "FAIL %s:%d: convert failed okRef=%d comp=%p len=%d\n",
                __FILE__, __LINE__, (int)okRef, (void *)comp, compLen);
        rc = 1;
    }

    if (rc == 0) {
        mpv = clientMapPreviewLoadFromBuffer(comp, compLen);
        if (!mpv) {
            fprintf(stderr, "FAIL %s:%d: compressed buffer must reload\n",
                    __FILE__, __LINE__);
            rc = 1;
        }
    }

    if (rc == 0 &&
        (clientMapPreviewGetPillCount(mpv) != pillsGetNumPills(&rPb) ||
         clientMapPreviewGetBaseCount(mpv) != basesGetNumBases(&rBs) ||
         clientMapPreviewGetStartCount(mpv) != startsGetNumStarts(&rSs))) {
        fprintf(stderr, "FAIL %s:%d: converted counts differ\n", __FILE__, __LINE__);
        rc = 1;
    }

    /* Spot-check terrain across the bands we set (all inside the
     * playable area, so mapGetPos returns real values). */
    if (rc == 0) {
        for (int y = 20; y < 60 && rc == 0; y++) {
            for (int x = 20; x < 200; x++) {
                BYTE want = rMp->mapItem[x][y];
                BYTE got  = clientMapPreviewGetTerrain(mpv, (BYTE)x, (BYTE)y);
                if (want != got) {
                    fprintf(stderr,
                            "FAIL %s:%d: converted terrain (%d,%d) want=%d got=%d\n",
                            __FILE__, __LINE__, x, y, want, got);
                    rc = 1;
                    break;
                }
            }
        }
    }

    if (mpv) clientMapPreviewDestroy(mpv);
    free(comp);
    free(blob);
    mapDestroy(&rMp); pillsDestroy(&rPb); basesDestroy(&rBs); startsDestroy(&rSs);
    SDL_RemovePath(mapPath);
    return rc;
}

/* Decode a blob via mapReadFromMemory; returns the bool result. */
static bool try_decode(const uint8_t *data, int len) {
    map mp = NULL; pillboxes pb = NULL; bases bs = NULL; starts ss = NULL;
    mapCreate(&mp); pillsCreate(&pb); basesCreate(&bs); startsCreate(&ss);
    bool ok = mapReadFromMemory(data, len, &mp, &pb, &bs, &ss);
    mapDestroy(&mp); pillsDestroy(&pb); basesDestroy(&bs); startsDestroy(&ss);
    return ok;
}

int run_map_read_memory_rejects_garbage(void) {
    /* NULL / zero / negative length. */
    UT_ASSERT_MSG(!try_decode(NULL, 32), "NULL data must fail");
    UT_ASSERT_MSG(!try_decode(kHandMap, 0), "zero length must fail");
    UT_ASSERT_MSG(!try_decode(kHandMap, -5), "negative length must fail");

    /* Wrong magic. */
    static const uint8_t badMagic[] = {
        'N','O','T','B','O','L','O','!', 0x01, 0,0,0, 0x04,0xFF,0xFF,0xFF };
    UT_ASSERT_MSG(!try_decode(badMagic, (int)sizeof(badMagic)),
                  "bad magic must fail");

    /* Wrong version. */
    static const uint8_t badVer[] = {
        'B','M','A','P','B','O','L','O', 0x02, 0,0,0, 0x04,0xFF,0xFF,0xFF };
    UT_ASSERT_MSG(!try_decode(badVer, (int)sizeof(badVer)),
                  "bad version must fail");

    /* Truncated header (only the magic, nothing else). */
    static const uint8_t shortHdr[] = { 'B','M','A','P','B','O','L','O' };
    UT_ASSERT_MSG(!try_decode(shortHdr, (int)sizeof(shortHdr)),
                  "truncated header must fail");

    /* Claims one pill but the record is cut short. */
    static const uint8_t shortPill[] = {
        'B','M','A','P','B','O','L','O', 0x01, 0x01, 0x00, 0x00, 50, 50 };
    UT_ASSERT_MSG(!try_decode(shortPill, (int)sizeof(shortPill)),
                  "truncated pill record must fail");

    return 0;
}
