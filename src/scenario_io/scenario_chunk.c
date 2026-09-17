/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Chunk
 *Filename:      scenario_chunk.c
 *Author:        John Morrison
 *Purpose:
 *  The manifest and the script into a container, and the
 *  container on to the end of the map.
 *
 *  The write goes through a file beside the target and a
 *  rename over it. A map is the only copy of itself, and a
 *  write that stops halfway must leave that copy alone.
 *********************************************************/

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* MoveFileExA, which is how the written map is put in place there. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "bolo_map_validate.h" /* boloMapBodyLength — where a map file ends */

#include "scenario_chunk.h"
#include "scenario_manifest_json.h"
#include "scenario_package.h"

/* Room for the name the write is made through: the map's path and the
 * suffix below. */
#define PACK_TEMP_SUFFIX ".packing"

static void chunkErr(char *err, size_t errLen, const char *fmt, ...) {
    va_list ap;

    if (err == NULL || errLen == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, errLen, fmt, ap);
    va_end(ap);
    err[errLen - 1] = '\0';
}

/* The whole file, into a buffer the caller frees. */
static bool packReadWhole(const char *path, uint8_t **out, size_t *outLen,
                          char *err, size_t errLen) {
    FILE    *f;
    long     size;
    uint8_t *buf;
    size_t   got;

    *out    = NULL;
    *outLen = 0;

    f = fopen(path, "rb");
    if (f == NULL) {
        chunkErr(err, errLen, "%s: the map could not be opened", path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        chunkErr(err, errLen, "%s: the map could not be measured", path);
        return false;
    }
    /* One past the length, so a file with no bytes in it still allocates and
       is refused for not being a map rather than for being out of memory. */
    buf = (uint8_t *)malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        chunkErr(err, errLen, "%s: no memory to read the map into", path);
        return false;
    }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        chunkErr(err, errLen, "%s: the map could not be read to its end", path);
        return false;
    }
    *out    = buf;
    *outLen = got;
    return true;
}

/* The map's body and the container after it, written to a file beside the
 * target and moved over it. A write cut off before the move leaves that file
 * rather than a map with half a container on the end of it, and a move that
 * will not go through leaves it too — the map at the target is untouched
 * either way, and the written bytes are still there to look at. */
static bool packWriteMap(const char *path, const uint8_t *body, size_t bodyLen,
                         const uint8_t *container, size_t containerLen,
                         char *err, size_t errLen) {
    char  tmp[SCN_IO_MAP_PATH_MAX + sizeof(PACK_TEMP_SUFFIX)];
    FILE *f;
    int   n;

    n = snprintf(tmp, sizeof(tmp), "%s%s", path, PACK_TEMP_SUFFIX);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        chunkErr(err, errLen,
                 "%s leaves no room beside it for the file a pack is written "
                 "through", path);
        return false;
    }

    f = fopen(tmp, "wb");
    if (f == NULL) {
        chunkErr(err, errLen, "%s: could not be opened to write the map into",
                 tmp);
        return false;
    }
    if (fwrite(body, 1, bodyLen, f) != bodyLen ||
        fwrite(container, 1, containerLen, f) != containerLen) {
        fclose(f);
        remove(tmp);
        chunkErr(err, errLen, "%s: the packed map could not be written out",
                 tmp);
        return false;
    }
    if (fclose(f) != 0) {
        remove(tmp);
        chunkErr(err, errLen, "%s: the packed map could not be closed", tmp);
        return false;
    }

#ifdef _WIN32
    /* MoveFileExA replaces the destination in one step. The removing rename
       this used to do could lose both copies: a sharing violation on the
       rename left the map already deleted and then deleted the temporary
       file as well, so a map that was open in another program came back as
       nothing at all. */
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        chunkErr(err, errLen, "%s: the packed map could not be put in place",
                 path);
        return false;
    }
#else
    /* rename replaces the destination here, so there is nothing to remove
       first and nothing to lose if it fails. */
    if (rename(tmp, path) != 0) {
        chunkErr(err, errLen, "%s: the packed map could not be put in place",
                 path);
        return false;
    }
#endif
    return true;
}

bool scnIoWriteMapChunk(const char *mapPath, const ScenarioManifest *m,
                        const char *script, size_t scriptLen,
                        char *err, size_t errLen) {
    ScnManifestDoc *doc          = NULL;
    char           *json         = NULL;
    uint8_t        *file         = NULL;
    size_t          fileLen      = 0;
    uint8_t        *container    = NULL;
    size_t          containerLen = 0;
    size_t          body         = 0;
    ScnPackageEntry entries[2];
    bool            ok = false;

    doc = scnManifestFromValues(m, err, errLen);
    if (doc == NULL) {
        goto done;
    }
    json = scnManifestWrite(doc, err, errLen);
    if (json == NULL) {
        goto done;
    }

    /* Stored rather than deflated. Both entries are small, and a stored one
       is what any zip tool shows as it was written. */
    memset(entries, 0, sizeof(entries));
    entries[0].name  = SCN_PACKAGE_MANIFEST_ENTRY;
    entries[0].bytes = (const uint8_t *)json;
    entries[0].len   = strlen(json);
    entries[1].name  = SCN_PACKAGE_SCRIPT_ENTRY;
    entries[1].bytes = (const uint8_t *)script;
    entries[1].len   = scriptLen;
    if (!scnPackageWrite(entries, 2, &container, &containerLen, err, errLen)) {
        goto done;
    }

    if (!packReadWhole(mapPath, &file, &fileLen, err, errLen)) {
        goto done;
    }
    /* Where the map data ends, so a container already on the file is replaced
       rather than written after. */
    if (!boloMapBodyLength((const unsigned char *)file, fileLen, &body)) {
        chunkErr(err, errLen, "%s: the end of the map data could not be found",
                 mapPath);
        goto done;
    }
    if (!packWriteMap(mapPath, file, body, container, containerLen, err,
                      errLen)) {
        goto done;
    }
    ok = true;

done:
    free(file);
    free(container);
    free(json);
    scnManifestFree(doc);
    return ok;
}
