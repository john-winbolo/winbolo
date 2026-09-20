/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Pack
 *Filename:      scenario_pack.c
 *Author:        John Morrison
 *Purpose:
 *  Writes a map's scenario into the map file: the script
 *  beside it and the manifest its table parsed to, handed to
 *  scenario_chunk.c to be framed and written.
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

#include "platform_types.h"    /* BOLO_STATIC_ASSERT */
#include "server_sim.h"        /* serverSimCreate / serverSimDestroy */

#include "scenario_chunk.h"    /* scnIoWriteMapChunk — the write itself */
#include "scenario_host.h"     /* SCN_SCRIPT_PATH_MAX */
#include "scenario_manifest.h"
#include "scenario_pack.h"
#include "scenario_validate.h"

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
