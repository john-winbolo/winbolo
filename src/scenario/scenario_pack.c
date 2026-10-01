/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Scenario Pack
 *Filename:      scenario_pack.c
 *Author:        John Morrison
 *Purpose:
 *  Writes a scenario into a container: a map's into the map
 *  file, the script beside it and the manifest its table
 *  parsed to, handed to scenario_chunk.c to be framed and
 *  written; and a loose script into a .scenario file of its
 *  own, the same pair framed here.
 *
 *  The manifest is whatever the validator's parse produced
 *  and nothing else. No field is filled in on the way out —
 *  a script that states no game leaves the manifest's game
 *  empty, because the host holds a package's manifest
 *  against the same table this was read from and anything
 *  added here would make a package that refuses itself.
 *
 *  A script that does not check out is not packed. The list
 *  of what is wrong with it belongs to -validate, so the
 *  refusal here says how many problems there are and sends
 *  the caller there for them.
 *********************************************************/

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>          /* SDL_RenamePath — how a .scenario is put in
                                * place */

#include "platform_types.h"    /* BOLO_STATIC_ASSERT */
#include "server_sim.h"        /* serverSimCreate / serverSimDestroy */

#include "scenario_chunk.h"    /* scnIoWriteMapChunk — the write itself */
#include "scenario_host.h"     /* SCN_SCRIPT_PATH_MAX */
#include "scenario_manifest.h"
#include "scenario_manifest_json.h"
#include "scenario_pack.h"
#include "scenario_package.h"
#include "scenario_validate.h"

/* What a .scenario is written through before it is moved over the target. */
#define PACK_SCRIPT_TEMP_SUFFIX ".packing"

/* The two path sizes are stated once each, on either side of the fence:
   this one is the runtime's and cannot be seen from scenario_io, so the
   buffer a write goes through has to be at least as long as the name this
   file hands over. */
BOLO_STATIC_ASSERT(SCN_IO_MAP_PATH_MAX >= SCN_SCRIPT_PATH_MAX,
                   chunk_path_buffer_covers_a_script_path);

static void packErr(char *err, size_t errLen, const char *fmt, ...) {
    va_list ap;

    if (err == NULL || errLen == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, errLen, fmt, ap);
    va_end(ap);
    err[errLen - 1] = '\0';
}

bool scnPackMap(const char *mapPath, char *err, size_t errLen) {
    ServerSim         *sim;
    ScnValidateResult *result = NULL;
    char              *src    = NULL;
    size_t             srcLen = 0;
    char               script[SCN_SCRIPT_PATH_MAX];
    bool               checked;
    bool               ok = false;

    if (err != NULL && errLen > 0) {
        err[0] = '\0';
    }
    if (mapPath == NULL || mapPath[0] == '\0') {
        packErr(err, errLen, "there is no map to pack");
        return false;
    }
    if (!scnScriptPath(mapPath, script, sizeof(script))) {
        packErr(err, errLen, "%s leaves no room for a script name beside it",
                mapPath);
        return false;
    }

    /* The rules and the tags are checked against the map the script ships
       with, so the sim is built before the script is read. serverSimCreate
       takes a path it only reads. */
    sim = serverSimCreate((char *)mapPath, gameOpen, false, 0, -1);
    if (sim == NULL) {
        packErr(err, errLen, "%s: the map could not be loaded", mapPath);
        return false;
    }

    /* On the heap rather than the stack: a result carries the whole manifest
       and the issue list with it, which is more than this frame should hold. */
    result = (ScnValidateResult *)malloc(sizeof(*result));
    if (result == NULL) {
        packErr(err, errLen, "%s: out of memory reading the script", mapPath);
        goto done;
    }

    /* Where the manifest comes from: the same parse an author checking the
       script would get, rather than a second read of the table. */
    checked = scenarioValidateMap(sim, mapPath, result);

    if (result->haveManifest == false && result->count == 0) {
        packErr(err, errLen,
                "%s: no scenario script beside it, so there is nothing to pack",
                mapPath);
        goto done;
    }
    if (checked == false) {
        /* A script that does not check out is not packed, and the list of
           what is wrong with it is -validate's to print. */
        packErr(err, errLen,
                "%s: %u problem%s; run -validate on the map to see %s", script,
                (unsigned)result->count, (result->count == 1) ? "" : "s",
                (result->count == 1) ? "it" : "them");
        goto done;
    }

    if (!scnReadFile(script, &src, &srcLen, err, errLen)) {
        if (err == NULL || errLen == 0 || err[0] == '\0') {
            packErr(err, errLen, "%s: the script is no longer there", script);
        }
        goto done;
    }

    if (!scnIoWriteMapChunk(mapPath, &result->manifest, src, srcLen, err,
                            errLen)) {
        goto done;
    }
    ok = true;

done:
    free(result);
    free(src);
    serverSimDestroy(sim);
    return ok;
}

/* The container into a .scenario file: written beside the target, then moved
   over it, so a write cut off partway leaves any file already there as it
   was. Nothing is left beside the target when a step fails. */
static bool packWritePackage(const char *outPath, const uint8_t *bytes,
                             size_t len, char *err, size_t errLen) {
    char  tmp[SCN_SCRIPT_PATH_MAX + sizeof(PACK_SCRIPT_TEMP_SUFFIX)];
    FILE *f;
    int   n;

    n = snprintf(tmp, sizeof(tmp), "%s%s", outPath, PACK_SCRIPT_TEMP_SUFFIX);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        packErr(err, errLen,
                "%s leaves no room beside it for the file a pack is written "
                "through", outPath);
        return false;
    }

    f = fopen(tmp, "wb");
    if (f == NULL) {
        packErr(err, errLen,
                "%s: could not be opened to write the package into", tmp);
        return false;
    }
    if (fwrite(bytes, 1, len, f) != len) {
        fclose(f);
        remove(tmp);
        packErr(err, errLen, "%s: the package could not be written out", tmp);
        return false;
    }
    if (fclose(f) != 0) {
        remove(tmp);
        packErr(err, errLen, "%s: the package could not be closed", tmp);
        return false;
    }

    /* SDL_RenamePath replaces an existing target in one step on every
       platform. SDL's reason goes into the message: a target open in another
       program is the failure an author meets. */
    if (!SDL_RenamePath(tmp, outPath)) {
        packErr(err, errLen, "%s: the package could not be put in place: %s",
                outPath, SDL_GetError());
        remove(tmp);
        return false;
    }
    return true;
}

bool scnPackScript(const char *luaPath, const char *outPath,
                   char *err, size_t errLen) {
    ScnValidateResult *result    = NULL;
    ScnManifestDoc    *doc       = NULL;
    char              *json      = NULL;
    char              *src       = NULL;
    size_t             srcLen    = 0;
    uint8_t           *container = NULL;
    size_t             containerLen = 0;
    ScnPackageEntry    entries[2];
    bool               checked;
    bool               ok = false;

    if (err != NULL && errLen > 0) {
        err[0] = '\0';
    }
    if (luaPath == NULL || luaPath[0] == '\0') {
        packErr(err, errLen, "there is no script to pack");
        return false;
    }
    if (outPath == NULL || outPath[0] == '\0') {
        packErr(err, errLen, "%s: there is nowhere to pack it to", luaPath);
        return false;
    }

    /* On the heap for the reason scnPackMap gives. */
    result = (ScnValidateResult *)calloc(1, sizeof(*result));
    if (result == NULL) {
        packErr(err, errLen, "%s: out of memory reading the script", luaPath);
        return false;
    }

    /* Checked as a scenario directory checks a loose script: no sim, so the
       one check that reads a map is left out, which is all a script with no
       map can be held to. */
    checked = scenarioValidateScript(NULL, luaPath, result);

    if (result->haveManifest == false) {
        /* No file at all comes back true with nothing said; a file that did
           not run, or ran and declared no scenario table, says why in its one
           issue. */
        if (result->count == 0) {
            packErr(err, errLen, "%s: no such script, so there is nothing to "
                    "pack", luaPath);
        } else if (strstr(result->issues[0].message, luaPath) != NULL) {
            /* The message names the script already, as the reader's do; a
               second copy of the path in front could push the reason itself
               off the end of the caller's buffer. */
            packErr(err, errLen, "%s", result->issues[0].message);
        } else {
            packErr(err, errLen, "%s: %s", luaPath, result->issues[0].message);
        }
        goto done;
    }
    if (checked == false) {
        packErr(err, errLen,
                "%s: %u problem%s; run -validate on the script to see %s",
                luaPath, (unsigned)result->count,
                (result->count == 1) ? "" : "s",
                (result->count == 1) ? "it" : "them");
        goto done;
    }
    if (result->manifest.bound) {
        /* A bound scenario is written for one map and ships inside it, so a
           package of its own would be a scenario with no map to play on. */
        packErr(err, errLen,
                "%s: the script is bound to a map; pack it into that map "
                "instead", luaPath);
        goto done;
    }

    if (!scnReadFile(luaPath, &src, &srcLen, err, errLen)) {
        if (err == NULL || errLen == 0 || err[0] == '\0') {
            packErr(err, errLen, "%s: the script is no longer there", luaPath);
        }
        goto done;
    }

    doc = scnManifestFromValues(&result->manifest, err, errLen);
    if (doc == NULL) {
        goto done;
    }
    json = scnManifestWrite(doc, err, errLen);
    if (json == NULL) {
        goto done;
    }

    /* Stored rather than deflated, as a map's container stores the two. */
    memset(entries, 0, sizeof(entries));
    entries[0].name  = SCN_PACKAGE_MANIFEST_ENTRY;
    entries[0].bytes = (const uint8_t *)json;
    entries[0].len   = strlen(json);
    entries[1].name  = SCN_PACKAGE_SCRIPT_ENTRY;
    entries[1].bytes = (const uint8_t *)src;
    entries[1].len   = srcLen;
    if (!scnPackageWrite(entries, 2, &container, &containerLen, err, errLen)) {
        goto done;
    }

    if (!packWritePackage(outPath, container, containerLen, err, errLen)) {
        goto done;
    }
    ok = true;

done:
    free(container);
    free(json);
    scnManifestFree(doc);
    free(src);
    free(result);
    return ok;
}
