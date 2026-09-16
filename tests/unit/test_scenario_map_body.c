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
 * run_scenario_map_body_use_local_compare
 *      — the two halves of the use-local check agree on a packed map: the
 *        length and hash the client now reports are the length and hash the
 *        server reads back, and the whole-file figures are not
 * run_scenario_map_has_script_cached
 *      — asking about the same maps twice opens a file the first round and
 *        none the second; a loose script dropped beside an unchanged map is
 *        seen on the next ask and taking it away is too; and a map whose own
 *        file changed is read again
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
#include "common/md5.h"    /* md5Compute — what the use-local check compares */
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

/* ── The use-local comparison, on a packed map ────────────────────── */

/* PACKET_LOBBY_MAP_USE_LOCAL is the client saying "I have this map; if your
 * copy is the same, use yours instead of taking my upload". The server
 * answers it from serverSimReadMapFile, which trims a packed map at its
 * terminator, so what the two sides compare has to be the map body: a client
 * that measured and hashed the whole file missed on every packed map both
 * sides already had, and uploaded one that did not need uploading.
 *
 * The client's half of that is transportUdpClientStartLobbyMapUploadFromPath,
 * which needs a connected transport to reach. What it now computes is
 * boloMapBodyLength over the file it loaded, then the hash over that much;
 * this case does the same arithmetic against the same file and holds it
 * against what the server answers, which is the comparison the handler makes.
 * The whole-file figures are checked too, so the case fails if the two ever
 * become the same thing and it stops proving anything. */
int run_scenario_map_body_use_local_compare(void) {
    char       dir[1024];
    char       mapPath[1024];
    uint8_t    plain[128];
    uint8_t   *container = NULL;
    uint8_t   *whole = NULL;
    uint8_t   *got = NULL;
    size_t     mapLen;
    size_t     containerLen = 0;
    size_t     wholeLen = 0;
    size_t     gotLen = 0;
    size_t     bodyLen = 0;
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim;
    uint8_t    clientMd5[16];
    uint8_t    serverMd5[16];
    uint8_t    wholeMd5[16];
    int        rc = 0;

    UT_ASSERT(utScratchPath(dir, sizeof(dir), NULL));
    UT_ASSERT(utScratchPath(mapPath, sizeof(mapPath), "uselocal.map"));

    mapLen = mbRunMap(plain);
    UT_ASSERT_MSG(mbContainer(&container, &containerLen),
                  "the fixture container could not be written");
    UT_ASSERT(mbWriteFile(mapPath, plain, mapLen, container, containerLen));

    /* The file as the client loads it: map plus container. */
    whole = (uint8_t *)malloc(mapLen + containerLen);
    UT_ASSERT(whole != NULL);
    memcpy(whole, plain, mapLen);
    memcpy(whole + mapLen, container, containerLen);
    wholeLen = mapLen + containerLen;
    UT_ASSERT_MSG(wholeLen > mapLen,
                  "the fixture's container is empty, so this case cannot tell "
                  "a whole-file compare from a body compare");

    /* What the client now measures and hashes. */
    UT_ASSERT_MSG(boloMapBodyLength(whole, wholeLen, &bodyLen),
                  "the packed file would not measure as a map");
    UT_ASSERT_MSG(bodyLen == mapLen,
                  "the client measured %u bytes of map in a %u byte file; "
                  "the map is %u", (unsigned)bodyLen, (unsigned)wholeLen,
                  (unsigned)mapLen);
    md5Compute(whole, bodyLen, clientMd5);
    md5Compute(whole, wholeLen, wholeMd5);

    sim = serverSimCreateCompressed(emap, 5097, "Everard Island", gameOpen,
                                    false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetUploadPersistDir(sim, dir);

    if (!serverSimReadMapFile(sim, "Uploads/uselocal.map", &got, &gotLen)) {
        fprintf(stderr, "FAIL %s:%d: the packed map would not read\n",
                __FILE__, __LINE__);
        rc = 1;
    } else {
        md5Compute(got, gotLen, serverMd5);
        /* The length the client reports and the length the server measures:
           the handler rejects outright when these differ. */
        if (gotLen != bodyLen) {
            fprintf(stderr,
                    "FAIL %s:%d: the client reports %u bytes and the server "
                    "reads %u\n", __FILE__, __LINE__, (unsigned)bodyLen,
                    (unsigned)gotLen);
            rc = 1;
        } else if (memcmp(clientMd5, serverMd5, 16) != 0) {
            fprintf(stderr,
                    "FAIL %s:%d: the two sides hash the same map body "
                    "differently\n", __FILE__, __LINE__);
            rc = 1;
        } else if (gotLen == wholeLen ||
                   memcmp(wholeMd5, serverMd5, 16) == 0) {
            fprintf(stderr,
                    "FAIL %s:%d: the whole file and the map body compare the "
                    "same, so this case cannot tell them apart\n",
                    __FILE__, __LINE__);
            rc = 1;
        }
        free(got);
    }

    /* And a plain map is unchanged by any of it: body and file are one. */
    if (rc == 0) {
        char plainPath[1024];
        if (utScratchPath(plainPath, sizeof(plainPath), "uselocal_plain.map") &&
            mbWriteFile(plainPath, plain, mapLen, NULL, 0)) {
            size_t plainBody = 0;
            if (!boloMapBodyLength(plain, mapLen, &plainBody) ||
                plainBody != mapLen) {
                fprintf(stderr,
                        "FAIL %s:%d: a plain map measured %u of its %u bytes\n",
                        __FILE__, __LINE__, (unsigned)plainBody,
                        (unsigned)mapLen);
                rc = 1;
            } else if (serverSimReadMapFile(sim, "Uploads/uselocal_plain.map",
                                            &got, &gotLen)) {
                if (gotLen != mapLen) {
                    fprintf(stderr,
                            "FAIL %s:%d: a plain map read back %u of %u\n",
                            __FILE__, __LINE__, (unsigned)gotLen,
                            (unsigned)mapLen);
                    rc = 1;
                }
                free(got);
            }
            remove(plainPath);
        }
    }

    serverSimDestroy(sim);
    free(whole);
    free(container);
    remove(mapPath);
    return rc;
}

/* ── The map-script answer is kept ────────────────────────────────── */

/* scenarioHostMapHasScript used to be two stats. A script can be packed into
 * the map file now, so it stats the name beside the map and then reads the
 * map's own head — and it is asked once per map on every listing, and from
 * the map chooser's render thread, so a directory of maps opened a file each
 * for every draw.
 *
 * The answer is kept per path, keyed on the file's size and modify time. This
 * case asks about the same maps twice and holds the second round to opening
 * nothing, through the counter the library keeps for exactly that; then it
 * rewrites one of the files and asks again, which has to come back to the
 * disk because the key moved. */
int run_scenario_map_has_script_cached(void) {
    char          packedPath[1024];
    char          plainPath[1024];
    char          loose[1024];
    uint8_t       plain[128];
    uint8_t      *container = NULL;
    size_t        mapLen;
    size_t        containerLen = 0;
    BYTE          emap[6000] = E_MAP;
    ServerSim    *sim;
    unsigned long before;
    unsigned long afterFirst;
    unsigned long afterSecond;
    unsigned long afterLooseAdded;
    unsigned long afterLooseGone;
    unsigned long afterRewrite;
    FILE         *f;
    int           rc = 0;

    UT_ASSERT(utScratchPath(packedPath, sizeof(packedPath), "cached_packed.map"));
    UT_ASSERT(utScratchPath(plainPath, sizeof(plainPath), "cached_plain.map"));
    /* Nothing beside either map, so the answer comes from the file itself and
       the read is what is being counted. */
    UT_ASSERT(utScratchPath(loose, sizeof(loose),
                            "cached_packed" SCN_SCRIPT_SUFFIX));
    remove(loose);
    UT_ASSERT(utScratchPath(loose, sizeof(loose),
                            "cached_plain" SCN_SCRIPT_SUFFIX));
    remove(loose);

    mapLen = mbRunMap(plain);
    UT_ASSERT_MSG(mbContainer(&container, &containerLen),
                  "the fixture container could not be written");
    UT_ASSERT(mbWriteFile(packedPath, plain, mapLen, container, containerLen));
    UT_ASSERT(mbWriteFile(plainPath, plain, mapLen, NULL, 0));

    /* The cache's lock is made where the lister's question is registered, as
       the scenarios directory cache's is, so this is what turns it on. */
    sim = serverSimCreateCompressed(emap, 5097, "Everard Island", gameOpen,
                                    false, 0, -1);
    UT_ASSERT(sim != NULL);
    scenarioHostRegisterMapScripted(sim);

    before = scenarioHostMapScriptOpens();
    UT_ASSERT_MSG(scenarioHostMapHasScript(packedPath),
                  "the packed map read as plain");
    UT_ASSERT_MSG(!scenarioHostMapHasScript(plainPath),
                  "the plain map read as scripted");
    afterFirst = scenarioHostMapScriptOpens();
    UT_ASSERT_MSG(afterFirst > before,
                  "the first listing opened no file at all (%lu), so this case "
                  "cannot tell a cached answer from a fresh one",
                  afterFirst - before);

    /* The same two again, which is the second listing. */
    UT_ASSERT_MSG(scenarioHostMapHasScript(packedPath),
                  "the packed map read as plain the second time");
    UT_ASSERT_MSG(!scenarioHostMapHasScript(plainPath),
                  "the plain map read as scripted the second time");
    afterSecond = scenarioHostMapScriptOpens();
    UT_ASSERT_MSG(afterSecond == afterFirst,
                  "the second listing opened %lu files, expected none",
                  afterSecond - afterFirst);

    /* A loose script dropped beside the plain map, which leaves the map file
       itself alone. It is part of the key, so the next ask is a fresh one and
       the map reads as scripted — the answer it would have had with no cache
       at all. No file is opened for it: a loose script is a stat, and it
       outranks anything packed into the map. */
    UT_ASSERT(utScratchPath(loose, sizeof(loose),
                            "cached_plain" SCN_SCRIPT_SUFFIX));
    f = fopen(loose, "wb");
    UT_ASSERT_MSG(f != NULL, "the loose script could not be written");
    fputs("scenario = { api = 1 }\n", f);
    fclose(f);

    UT_ASSERT_MSG(scenarioHostMapHasScript(plainPath),
                  "a map with a loose script dropped beside it still reads as "
                  "plain, so the cache is holding a stale row");
    afterLooseAdded = scenarioHostMapScriptOpens();
    UT_ASSERT_MSG(afterLooseAdded == afterSecond,
                  "answering a map with a loose script beside it opened %lu "
                  "files, expected none",
                  afterLooseAdded - afterSecond);

    /* And taking it away again puts the map back to plain. No read for that
       either, and for a different reason: the key is a value rather than a
       generation count, so a map back in a state the cache has already seen
       finds the row it kept then — the one from before the script was
       dropped, which is still held. */
    remove(loose);
    UT_ASSERT_MSG(!scenarioHostMapHasScript(plainPath),
                  "a map whose loose script was removed still reads as "
                  "scripted");
    afterLooseGone = scenarioHostMapScriptOpens();
    UT_ASSERT_MSG(afterLooseGone == afterLooseAdded,
                  "a map back in a state the cache had already seen opened "
                  "%lu files, expected none",
                  afterLooseGone - afterLooseAdded);

    /* And the map file is in the key too: the plain map becomes a packed one,
       and the answer is worked out again. SDL_GetPathInfo's modify time can
       be as coarse as a second, so the size is what moves here — the
       container makes the file longer. */
    UT_ASSERT(mbWriteFile(plainPath, plain, mapLen, container, containerLen));
    UT_ASSERT_MSG(scenarioHostMapHasScript(plainPath),
                  "a map that was repacked still reads as plain");
    afterRewrite = scenarioHostMapScriptOpens();
    UT_ASSERT_MSG(afterRewrite > afterLooseGone,
                  "a repacked map was answered from the old row without "
                  "opening the file");

    serverSimDestroy(sim);
    free(container);
    remove(packedPath);
    remove(plainPath);
    return rc;
}
