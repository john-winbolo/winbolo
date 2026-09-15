/*
 * Where a .map file's map data ends, and the three things that need to know.
 *
 * boloMapBodyLength (src/bolo/bolo_map.c) measures a BMAP without parsing
 * one. scnPackageFindInMap uses it to find a container appended after the
 * map, serverSimReadMapFile uses it to keep that container off the wire, and
 * scenarioHostMapHasScript uses it to tag a packed map as scripted.
 *
 * The maps here are built byte by byte rather than through mapWrite, so the
 * refusal cases can bend one field at a time.
 *
 * run_scenario_map_body_length
 *      — an empty map and a map with runs each measure their own length;
 *        bytes appended after one do not move the end; and a wrong magic, a
 *        wrong version, a count past what a map holds, a run running off the
 *        end and a buffer with no terminator in it are each refused
 * run_scenario_map_find_container
 *      — a plain map carries nothing, four stray bytes are not a chunk, and
 *        a real container appended to a map comes back as bytes scnPackageOpen
 *        accepts
 * run_scenario_map_has_script_chunk
 *      — a packed map with no loose script is scripted, a plain map is not, a
 *        loose script still counts, and with scripts off neither does
 * run_scenario_map_preview_truncates
 *      — a packed map read through serverSimReadMapFile comes back as the map
 *        alone, with no container left in what the caller is handed, and a
 *        plain map comes back at its own length and its own bytes
 * run_scenario_map_preview_passes_plain_bytes
 *      — and a file that is not a map at all is still read for the caller
 *        when it is inside the upload cap, and still refused when it is not
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "bolo_map_validate.h"     /* boloMapBodyLength */
#include "everard_map.h"           /* E_MAP — the sim the preview case needs */
#include "gametype.h"              /* gameOpen */
#include "server_sim.h"            /* serverSimReadMapFile, create/destroy */
#include "server_sim_lifecycle.h"  /* serverSimSetUploadPersistDir */
#include "wire_limits.h"           /* LOBBY_MAP_UPLOAD_MAX_BYTES */
#include "scenario_host.h"         /* scenarioHostMapHasScript, the suffix */
#include "scenario_package.h"
#include "test_harness.h"

/* Where mbRunMap's first run header starts: the twelve-byte preamble, then
 * one pill, two bases and one start. */
#define MB_FIRST_RUN (12 + 5 + (2 * 6) + 3)

/* The smallest whole map: the preamble with no pills, bases or starts, then
 * the run header that says there are no more runs. */
static size_t mbEmptyMap(uint8_t *out) {
    size_t n = 0;

    memcpy(out + n, "BMAPBOLO", 8);
    n += 8;
    out[n++] = 1;   /* version */
    out[n++] = 0;   /* pills */
    out[n++] = 0;   /* bases */
    out[n++] = 0;   /* starts */
    out[n++] = 4;
    out[n++] = 255;
    out[n++] = 255;
    out[n++] = 255;
    return n;
}

/* A map with entities and two runs in it, so the walk steps over a datalen
 * rather than meeting the terminator straight away. The entity and run data
 * is filler: nothing here decodes it. */
static size_t mbRunMap(uint8_t *out) {
    size_t n = 0;

    memcpy(out + n, "BMAPBOLO", 8);
    n += 8;
    out[n++] = 1;   /* version */
    out[n++] = 1;   /* one pill */
    out[n++] = 2;   /* two bases */
    out[n++] = 1;   /* one start */
    memset(out + n, 0x11, 5);
    n += 5;
    memset(out + n, 0x22, 12);
    n += 12;
    memset(out + n, 0x33, 3);
    n += 3;

    out[n++] = 10;  /* four header bytes and six of data */
    out[n++] = 3;
    out[n++] = 0;
    out[n++] = 20;
    memset(out + n, 0xAB, 6);
    n += 6;

    out[n++] = 9;   /* four header bytes and five of data */
    out[n++] = 4;
    out[n++] = 0;
    out[n++] = 20;
    memset(out + n, 0xCD, 5);
    n += 5;

    out[n++] = 4;
    out[n++] = 255;
    out[n++] = 255;
    out[n++] = 255;
    return n;
}

/* A container holding nothing but a manifest, which is all scnPackageOpen
 * insists on. The caller frees it. */
static bool mbContainer(uint8_t **out, size_t *outLen) {
    static const char kManifest[] = "{\"manifest\":1}";
    ScnPackageEntry   e;
    char              err[256];

    memset(&e, 0, sizeof(e));
    e.name = "manifest.json";
    e.bytes = (const uint8_t *)kManifest;
    e.len = sizeof(kManifest) - 1;
    return scnPackageWrite(&e, 1, out, outLen, err, sizeof(err));
}

/* Write bytes, then more bytes, to one file. */
static bool mbWriteFile(const char *path, const uint8_t *a, size_t aLen,
                        const uint8_t *b, size_t bLen) {
    FILE *f = fopen(path, "wb");

    if (f == NULL) {
        return false;
    }
    if (aLen > 0 && fwrite(a, 1, aLen, f) != aLen) {
        fclose(f);
        return false;
    }
    if (bLen > 0 && fwrite(b, 1, bLen, f) != bLen) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

int run_scenario_map_body_length(void) {
    uint8_t plain[128];
    uint8_t bent[256];
    size_t  mapLen;
    size_t  body = 0;

    /* An empty map measures itself. */
    mapLen = mbEmptyMap(plain);
    UT_ASSERT(boloMapBodyLength(plain, mapLen, &body));
    UT_ASSERT_MSG(body == mapLen, "an empty map measured %u of %u",
                  (unsigned)body, (unsigned)mapLen);

    /* And so does one with entities and runs. */
    mapLen = mbRunMap(plain);
    UT_ASSERT(boloMapBodyLength(plain, mapLen, &body));
    UT_ASSERT_MSG(body == mapLen, "a map with runs measured %u of %u",
                  (unsigned)body, (unsigned)mapLen);

    /* Bytes after the terminator are not map data. */
    memcpy(bent, plain, mapLen);
    memset(bent + mapLen, 0x5A, 64);
    UT_ASSERT(boloMapBodyLength(bent, mapLen + 64, &body));
    UT_ASSERT_MSG(body == mapLen, "a trailer moved the end to %u of %u",
                  (unsigned)body, (unsigned)mapLen);

    /* The refusals, one bent field at a time. */
    memcpy(bent, plain, mapLen);
    bent[0] = 'X';
    body = 1;
    UT_ASSERT_MSG(!boloMapBodyLength(bent, mapLen, &body),
                  "a wrong magic measured");
    UT_ASSERT_MSG(body == 0, "a refusal still wrote %u", (unsigned)body);

    memcpy(bent, plain, mapLen);
    bent[8] = 2;
    UT_ASSERT_MSG(!boloMapBodyLength(bent, mapLen, &body),
                  "a version 2 map measured");

    memcpy(bent, plain, mapLen);
    bent[9] = MAX_PILLS + 1;
    UT_ASSERT_MSG(!boloMapBodyLength(bent, mapLen, &body),
                  "a pill count past the map's own measured");

    memcpy(bent, plain, mapLen);
    bent[MB_FIRST_RUN] = 200;   /* more run data than the buffer holds */
    UT_ASSERT_MSG(!boloMapBodyLength(bent, mapLen, &body),
                  "a run running off the end measured");

    /* A buffer that stops before any terminator: a whole preamble with no
     * entities, and nothing after it. */
    mbEmptyMap(bent);
    UT_ASSERT_MSG(!boloMapBodyLength(bent, 12, &body),
                  "a map with no run headers in the buffer measured");

    /* And one too short to hold even the preamble. */
    UT_ASSERT_MSG(!boloMapBodyLength(bent, 4, &body),
                  "four bytes measured as a map");
    return 0;
}

int run_scenario_map_find_container(void) {
    uint8_t        plain[128];
    uint8_t        stray[192];
    uint8_t       *container = NULL;
    uint8_t       *file = NULL;
    const uint8_t *chunk = NULL;
    size_t         mapLen;
    size_t         containerLen = 0;
    size_t         chunkLen = 0;
    char           err[256];
    ScnPackage    *p;

    mapLen = mbRunMap(plain);

    /* A plain map has nothing after it. */
    UT_ASSERT_MSG(!scnPackageFindInMap(plain, mapLen, &chunk, &chunkLen),
                  "a plain map answered with a container");
    UT_ASSERT(chunk == NULL && chunkLen == 0);

    /* Four bytes past the end are not a chunk: a header is ten. */
    memcpy(stray, plain, mapLen);
    memcpy(stray + mapLen, "WBSC", 4);
    UT_ASSERT_MSG(!scnPackageFindInMap(stray, mapLen + 4, &chunk, &chunkLen),
                  "four trailing bytes answered as a container");

    /* Ten bytes that are not the magic are not one either. */
    memcpy(stray, plain, mapLen);
    memset(stray + mapLen, 0x5A, 32);
    UT_ASSERT_MSG(!scnPackageFindInMap(stray, mapLen + 32, &chunk, &chunkLen),
                  "a trailer with no magic answered as a container");

    /* A real container, appended the way a packed map carries one. */
    UT_ASSERT_MSG(mbContainer(&container, &containerLen),
                  "the fixture container could not be written");
    file = (uint8_t *)malloc(mapLen + containerLen);
    UT_ASSERT(file != NULL);
    memcpy(file, plain, mapLen);
    memcpy(file + mapLen, container, containerLen);

    UT_ASSERT_MSG(
        scnPackageFindInMap(file, mapLen + containerLen, &chunk, &chunkLen),
        "a packed map answered with no container");
    UT_ASSERT_MSG(chunk == file + mapLen, "the chunk started somewhere else");
    UT_ASSERT_MSG(chunkLen == containerLen, "the chunk is %u bytes, wrote %u",
                  (unsigned)chunkLen, (unsigned)containerLen);

    p = scnPackageOpen(chunk, chunkLen, err, sizeof(err));
    UT_ASSERT_MSG(p != NULL, "what was found would not open: %s", err);
    UT_ASSERT(scnPackageHasEntry(p, "manifest.json"));
    scnPackageClose(p);

    free(file);
    free(container);
    return 0;
}

int run_scenario_map_has_script_chunk(void) {
    char     packedPath[1024];
    char     plainPath[1024];
    char     scriptPath[1024];
    uint8_t  plain[128];
    uint8_t *container = NULL;
    size_t   mapLen;
    size_t   containerLen = 0;
    FILE    *f;
    int      rc = 0;

    UT_ASSERT(utScratchPath(packedPath, sizeof(packedPath), "packed.map"));
    UT_ASSERT(utScratchPath(plainPath, sizeof(plainPath), "plain.map"));
    UT_ASSERT(utScratchPath(scriptPath, sizeof(scriptPath),
                            "plain" SCN_SCRIPT_SUFFIX));

    mapLen = mbRunMap(plain);
    UT_ASSERT_MSG(mbContainer(&container, &containerLen),
                  "the fixture container could not be written");
    UT_ASSERT(mbWriteFile(packedPath, plain, mapLen, container, containerLen));
    UT_ASSERT(mbWriteFile(plainPath, plain, mapLen, NULL, 0));

    if (!scenarioHostMapHasScript(packedPath)) {
        fprintf(stderr, "FAIL %s:%d: a packed map is not scripted\n",
                __FILE__, __LINE__);
        rc = 1;
    }
    if (rc == 0 && scenarioHostMapHasScript(plainPath)) {
        fprintf(stderr, "FAIL %s:%d: a plain map is scripted\n",
                __FILE__, __LINE__);
        rc = 1;
    }

    /* A loose script beside a plain map still answers yes. */
    if (rc == 0) {
        f = fopen(scriptPath, "wb");
        if (f == NULL) {
            fprintf(stderr, "FAIL %s:%d: the loose script could not be written\n",
                    __FILE__, __LINE__);
            rc = 1;
        } else {
            fputs("scenario = { api = 1 }\n", f);
            fclose(f);
            if (!scenarioHostMapHasScript(plainPath)) {
                fprintf(stderr, "FAIL %s:%d: a loose script is not scripted\n",
                        __FILE__, __LINE__);
                rc = 1;
            }
        }
    }

    /* With scripts off no map is scripted, whichever way it carries one. */
    if (rc == 0) {
        scenarioHostSetEnabled(false);
        if (scenarioHostMapHasScript(packedPath) ||
            scenarioHostMapHasScript(plainPath)) {
            fprintf(stderr,
                    "FAIL %s:%d: a map is scripted with scripts switched off\n",
                    __FILE__, __LINE__);
            rc = 1;
        }
        scenarioHostSetEnabled(true);
    }

    remove(scriptPath);
    remove(plainPath);
    remove(packedPath);
    free(container);
    return rc;
}

int run_scenario_map_preview_truncates(void) {
    char           dir[1024];
    char           mapPath[1024];
    uint8_t        plain[128];
    uint8_t       *container = NULL;
    uint8_t       *got = NULL;
    const uint8_t *chunk = NULL;
    size_t         mapLen;
    size_t         containerLen = 0;
    size_t         gotLen = 0;
    size_t         chunkLen = 0;
    BYTE           emap[6000] = E_MAP;
    ServerSim     *sim;
    int            rc = 0;

    UT_ASSERT(utScratchPath(dir, sizeof(dir), NULL));
    UT_ASSERT(utScratchPath(mapPath, sizeof(mapPath), "preview.map"));

    mapLen = mbRunMap(plain);
    UT_ASSERT_MSG(mbContainer(&container, &containerLen),
                  "the fixture container could not be written");
    UT_ASSERT(mbWriteFile(mapPath, plain, mapLen, container, containerLen));

    sim = serverSimCreateCompressed(emap, 5097, "Everard Island", gameOpen,
                                    false, 0, -1);
    UT_ASSERT(sim != NULL);
    /* The virtual Uploads folder is how a sim's map resolution is pointed at
       a directory, so the case reads its own file without touching the map
       tree or the sim's fields. */
    serverSimSetUploadPersistDir(sim, dir);

    if (!serverSimReadMapFile(sim, "Uploads/preview.map", &got, &gotLen)) {
        fprintf(stderr, "FAIL %s:%d: the packed map would not read\n",
                __FILE__, __LINE__);
        rc = 1;
    } else {
        if (gotLen != mapLen) {
            fprintf(stderr,
                    "FAIL %s:%d: the preview is %u bytes, the map is %u "
                    "and the file is %u\n",
                    __FILE__, __LINE__, (unsigned)gotLen, (unsigned)mapLen,
                    (unsigned)(mapLen + containerLen));
            rc = 1;
        } else if (memcmp(got, plain, mapLen) != 0) {
            fprintf(stderr, "FAIL %s:%d: the preview is not the map's bytes\n",
                    __FILE__, __LINE__);
            rc = 1;
        } else if (scnPackageFindInMap(got, gotLen, &chunk, &chunkLen)) {
            fprintf(stderr, "FAIL %s:%d: the container reached the caller\n",
                    __FILE__, __LINE__);
            rc = 1;
        }
        free(got);
    }

    /* And a plain map still reads whole. */
    if (rc == 0) {
        char plainPath[1024];
        if (utScratchPath(plainPath, sizeof(plainPath), "preview_plain.map") &&
            mbWriteFile(plainPath, plain, mapLen, NULL, 0)) {
            if (!serverSimReadMapFile(sim, "Uploads/preview_plain.map", &got,
                                      &gotLen)) {
                fprintf(stderr, "FAIL %s:%d: a plain map would not read\n",
                        __FILE__, __LINE__);
                rc = 1;
            } else {
                /* Its own length and its own bytes: a truncation one run
                   short of the end would still be shorter than the file. */
                if (gotLen != mapLen) {
                    fprintf(stderr,
                            "FAIL %s:%d: a plain map read back %u of %u\n",
                            __FILE__, __LINE__, (unsigned)gotLen,
                            (unsigned)mapLen);
                    rc = 1;
                } else if (memcmp(got, plain, mapLen) != 0) {
                    fprintf(stderr,
                            "FAIL %s:%d: a plain map read back other bytes\n",
                            __FILE__, __LINE__);
                    rc = 1;
                }
                free(got);
            }
            remove(plainPath);
        }
    }

    serverSimDestroy(sim);
    free(container);
    remove(mapPath);
    return rc;
}

int run_scenario_map_preview_passes_plain_bytes(void) {
    enum { MB_FILLER_LEN = 3000 };

    char       dir[1024];
    char       smallPath[1024];
    char       bigPath[1024];
    uint8_t   *filler;
    uint8_t   *got = NULL;
    size_t     gotLen = 0;
    size_t     bigLen = (size_t)LOBBY_MAP_UPLOAD_MAX_BYTES + 1;
    size_t     i;
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim;
    int        rc = 0;

    UT_ASSERT(utScratchPath(dir, sizeof(dir), NULL));
    UT_ASSERT(utScratchPath(smallPath, sizeof(smallPath), "not_a_map.bin"));
    UT_ASSERT(utScratchPath(bigPath, sizeof(bigPath), "not_a_map_big.bin"));

    filler = (uint8_t *)malloc(bigLen);
    UT_ASSERT(filler != NULL);
    for (i = 0; i < bigLen; i++) {
        filler[i] = (uint8_t)((i * 7u) + 3u);
    }
    UT_ASSERT(mbWriteFile(smallPath, filler, MB_FILLER_LEN, NULL, 0));
    UT_ASSERT(mbWriteFile(bigPath, filler, bigLen, NULL, 0));

    sim = serverSimCreateCompressed(emap, 5097, "Everard Island", gameOpen,
                                    false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetUploadPersistDir(sim, dir);

    /* Inside the cap, the caller gets the file. Nothing here is a map, and
       reading bytes for a caller is not the same as validating them. */
    if (!serverSimReadMapFile(sim, "Uploads/not_a_map.bin", &got, &gotLen)) {
        fprintf(stderr, "FAIL %s:%d: bytes that are not a map would not read\n",
                __FILE__, __LINE__);
        rc = 1;
    } else {
        if (gotLen != (size_t)MB_FILLER_LEN) {
            fprintf(stderr, "FAIL %s:%d: read back %u bytes of %u\n",
                    __FILE__, __LINE__, (unsigned)gotLen,
                    (unsigned)MB_FILLER_LEN);
            rc = 1;
        } else if (memcmp(got, filler, (size_t)MB_FILLER_LEN) != 0) {
            fprintf(stderr, "FAIL %s:%d: read back other bytes\n",
                    __FILE__, __LINE__);
            rc = 1;
        }
        free(got);
        got = NULL;
        gotLen = 0;
    }

    /* Over the cap with no map at the front of it: refused, and the caller's
       pointers are left as they were found. */
    if (rc == 0) {
        if (serverSimReadMapFile(sim, "Uploads/not_a_map_big.bin", &got,
                                 &gotLen)) {
            fprintf(stderr, "FAIL %s:%d: a file over the cap read back %u bytes\n",
                    __FILE__, __LINE__, (unsigned)gotLen);
            free(got);
            rc = 1;
        } else if (got != NULL || gotLen != 0) {
            fprintf(stderr,
                    "FAIL %s:%d: a refused read left a pointer or a length "
                    "behind (%u bytes)\n",
                    __FILE__, __LINE__, (unsigned)gotLen);
            rc = 1;
        }
    }

    serverSimDestroy(sim);
    free(filler);
    remove(bigPath);
    remove(smallPath);
    return rc;
}
