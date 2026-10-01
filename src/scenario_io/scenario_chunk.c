/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Scenario Chunk
 *Filename:      scenario_chunk.c
 *Author:        John Morrison
 *Purpose:
 *  The manifest and the script into a container, and the
 *  container on to the end of the map. And the Workshop item
 *  a published file names, stamped into the container a
 *  .scenario file or a packed map already carries.
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

#include <SDL3/SDL.h> /* SDL_RenamePath — how the written map is put in place */

#include "bolo_map_validate.h" /* boloMapBodyLength — where a map file ends */
#include "wire_limits.h"       /* LOBBY_PACKAGE_UPLOAD_MAX_BYTES — the most
                                  one entry of a rewritten container reads */

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
        chunkErr(err, errLen, "%s: the file could not be opened", path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        chunkErr(err, errLen, "%s: the file could not be measured", path);
        return false;
    }
    /* One past the length, so a file with no bytes in it still allocates and
       is refused for not being a map or a container rather than for being
       out of memory. */
    buf = (uint8_t *)malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        chunkErr(err, errLen, "%s: no memory to read the file into", path);
        return false;
    }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        chunkErr(err, errLen, "%s: the file could not be read to its end",
                 path);
        return false;
    }
    *out    = buf;
    *outLen = got;
    return true;
}

/* The map's body and the container after it, written to a file beside the
 * target and moved over it. A write cut off before the move leaves that file
 * rather than a map with half a container on the end of it, and a move that
 * will not go through leaves it too — the file at the target is untouched
 * either way, and the written bytes are still there to look at.
 *
 * A body of length 0 writes the container on its own, which is what a
 * .scenario file is. */
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
        chunkErr(err, errLen, "%s: could not be opened to write the file into",
                 tmp);
        return false;
    }
    if ((bodyLen > 0 && fwrite(body, 1, bodyLen, f) != bodyLen) ||
        fwrite(container, 1, containerLen, f) != containerLen) {
        fclose(f);
        remove(tmp);
        chunkErr(err, errLen, "%s: the packed file could not be written out",
                 tmp);
        return false;
    }
    if (fclose(f) != 0) {
        remove(tmp);
        chunkErr(err, errLen, "%s: the packed file could not be closed", tmp);
        return false;
    }

    /* SDL_RenamePath replaces the destination in one step on every platform:
       rename() where that already does it, MoveFileExW with
       MOVEFILE_REPLACE_EXISTING on Windows. The removing rename this used to
       do there could lose both copies: a sharing violation on the rename left
       the map already deleted and then deleted the temporary file as well, so
       a map that was open in another program came back as nothing at all.
       SDL's reason goes into the message because that sharing violation is
       the failure an author hits, and it is what says to close the map
       elsewhere and pack again. */
    if (!SDL_RenamePath(tmp, path)) {
        chunkErr(err, errLen,
                 "%s: the packed file could not be put in place: %s", path,
                 SDL_GetError());
        return false;
    }
    return true;
}

/* Does path end in ext, ignoring case? */
static bool chunkHasSuffix(const char *path, const char *ext) {
    size_t n = strlen(path);
    size_t e = strlen(ext);

    return n > e && SDL_strcasecmp(path + n - e, ext) == 0;
}

/* One container rebuilt with the Workshop item and author set in its
 * manifest. manifest.json is parsed, given the two fields and written back,
 * so a key this build does not know stays in it; every other entry is copied
 * across as the bytes it held, in the order the archive held them.
 *
 * manifest.json and the script entry are stored, as the writers that make a
 * container store them; the rest is deflated. *out is malloc'd and the
 * caller frees it. */
static bool chunkRewriteWorkshop(const uint8_t *chunk, size_t chunkLen,
                                 uint64_t id, uint64_t author,
                                 uint8_t **out, size_t *outLen,
                                 char *err, size_t errLen) {
    ScnPackage      *p       = NULL;
    uint8_t         *json    = NULL;
    size_t           jsonLen = 0;
    ScnManifestDoc  *doc     = NULL;
    char            *text    = NULL;
    const char      *script  = NULL;
    ScnPackageEntry *entries = NULL;
    uint8_t        **held    = NULL;
    int              count   = 0;
    int              i;
    bool             ok = false;

    p = scnPackageOpen(chunk, chunkLen, err, errLen);
    if (p == NULL) {
        return false;
    }
    if (!scnPackageReadEntry(p, SCN_PACKAGE_MANIFEST_ENTRY,
                             SCN_PACKAGE_MANIFEST_MAX_BYTES, &json, &jsonLen,
                             err, errLen)) {
        if (err != NULL && errLen > 0 && err[0] == '\0') {
            chunkErr(err, errLen, "the container's %s could not be read",
                     SCN_PACKAGE_MANIFEST_ENTRY);
        }
        goto done;
    }
    doc = scnManifestParse(json, jsonLen, NULL, err, errLen);
    if (doc == NULL) {
        goto done;
    }
    scnManifestSetWorkshop(doc, id, author);
    text = scnManifestWrite(doc, err, errLen);
    if (text == NULL) {
        goto done;
    }
    script = scnManifestScriptEntry(doc);

    count = scnPackageEntryCount(p);
    entries = (ScnPackageEntry *)calloc((size_t)(count > 0 ? count : 1),
                                        sizeof(*entries));
    held = (uint8_t **)calloc((size_t)(count > 0 ? count : 1), sizeof(*held));
    if (entries == NULL || held == NULL) {
        chunkErr(err, errLen, "no memory to rebuild the container");
        goto done;
    }
    for (i = 0; i < count; i++) {
        const char *name = scnPackageEntryName(p, i);
        size_t      len  = 0;

        entries[i].name = name;
        if (strcmp(name, SCN_PACKAGE_MANIFEST_ENTRY) == 0) {
            entries[i].bytes   = (const uint8_t *)text;
            entries[i].len     = strlen(text);
            entries[i].deflate = false;
            continue;
        }
        if (!scnPackageReadEntry(p, name, LOBBY_PACKAGE_UPLOAD_MAX_BYTES,
                                 &held[i], &len, err, errLen)) {
            if (err != NULL && errLen > 0 && err[0] == '\0') {
                chunkErr(err, errLen, "entry \"%s\" could not be read", name);
            }
            goto done;
        }
        entries[i].bytes   = held[i];
        entries[i].len     = len;
        entries[i].deflate = script == NULL || strcmp(name, script) != 0;
    }
    ok = scnPackageWrite(entries, count, out, outLen, err, errLen);

done:
    if (held != NULL) {
        for (i = 0; i < count; i++) {
            free(held[i]);
        }
        free(held);
    }
    free(entries);
    free(text);
    scnManifestFree(doc);
    free(json);
    scnPackageClose(p);
    return ok;
}

bool scnIoSetWorkshopId(const char *path, uint64_t id, uint64_t author,
                        char *err, size_t errLen) {
    uint8_t       *file         = NULL;
    size_t         fileLen      = 0;
    const uint8_t *chunk        = NULL;
    size_t         chunkLen     = 0;
    size_t         body         = 0;
    uint8_t       *container    = NULL;
    size_t         containerLen = 0;
    bool           isMap;
    bool           ok = false;

    if (err != NULL && errLen > 0) {
        err[0] = '\0';
    }
    if (path == NULL || path[0] == '\0') {
        chunkErr(err, errLen, "there is no file to stamp a Workshop item on");
        return false;
    }
    isMap = chunkHasSuffix(path, ".map");
    if (!isMap && !chunkHasSuffix(path, ".scenario")) {
        chunkErr(err, errLen,
                 "%s: a loose script carries no manifest; pack it first",
                 path);
        return false;
    }

    if (!packReadWhole(path, &file, &fileLen, err, errLen)) {
        goto done;
    }
    if (isMap) {
        /* The body is kept byte for byte and only what follows it is
           rebuilt. A map with nothing after it has no manifest to stamp. */
        if (!scnPackageFindInMap(file, fileLen, &chunk, &chunkLen)) {
            chunkErr(err, errLen, "%s: the map carries no scenario", path);
            goto done;
        }
        body = (size_t)(chunk - file);
    } else {
        chunk    = file;
        chunkLen = fileLen;
    }

    if (!chunkRewriteWorkshop(chunk, chunkLen, id, author, &container,
                              &containerLen, err, errLen)) {
        goto done;
    }
    if (!packWriteMap(path, file, body, container, containerLen, err,
                      errLen)) {
        goto done;
    }
    ok = true;

done:
    free(container);
    free(file);
    return ok;
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
