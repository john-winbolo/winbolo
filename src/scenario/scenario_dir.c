/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Dir
 *Filename:      scenario_dir.c
 *Purpose:
 *  Reads the server's scenarios directory into the list a
 *  client is told about. See scenario_dir.h for what is
 *  read and what is skipped.
 *
 *  This file compiles under the scenario_host profile: it
 *  sees src/bolo/public/ and src/bolo/scenario_api/, and
 *  nothing under src/bolo/internal/.
 *********************************************************/

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "platform_types.h" /* BOLO_STATIC_ASSERT */
#include "server_sim.h"   /* serverSimConsoleMessage */
#include "wire_limits.h"  /* LOBBY_PACKAGE_UPLOAD_MAX_BYTES — the cap a
                           * package is held to everywhere else */

#include "scenario_dir.h"
#include "scenario_manifest.h"
#include "scenario_manifest_json.h"
#include "scenario_package.h"
#include "scenario_validate.h"

/* A manifest's name and description are copied straight into an entry, so the
 * two headers' lengths have to agree. They are stated twice because a frontend
 * includes scenario_host.h and cannot reach scenario_api/; this file sees both,
 * and is where a change to one without the other fails to build. */
BOLO_STATIC_ASSERT(SCN_DIR_NAME_LEN == SCN_SCENARIO_NAME_LEN,
                   scenario_dir_name_matches_the_manifest);
BOLO_STATIC_ASSERT(SCN_DIR_DESC_LEN == SCN_SCENARIO_DESC_LEN,
                   scenario_dir_description_matches_the_manifest);

/* One line about a file that could not be listed. */
#define SCN_DIR_LINE_LEN 512

static void scnDirSay(const char *fmt, ...) {
    char    line[SCN_DIR_LINE_LEN];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    serverSimConsoleMessage(line);
}

/* Does name end with ext, ignoring case? */
static bool scnDirHasExt(const char *name, const char *ext) {
    size_t n = strlen(name);
    size_t e = strlen(ext);

    if (n <= e) {
        return false;
    }
    return SDL_strcasecmp(name + n - e, ext) == 0;
}

/* The whole file, into a buffer the caller frees. False when it is not there,
 * cannot be read, or is above the cap a package is held to anywhere else. */
static bool scnDirReadFile(const char *path, uint8_t **out, size_t *outLen) {
    FILE    *f;
    long     size;
    uint8_t *buf;
    size_t   got;

    *out    = NULL;
    *outLen = 0;
    f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    size = ftell(f);
    if (size <= 0 || (unsigned long)size > LOBBY_PACKAGE_UPLOAD_MAX_BYTES) {
        fclose(f);
        return false;
    }
    rewind(f);
    buf = (uint8_t *)malloc((size_t)size);
    if (buf == NULL) {
        fclose(f);
        return false;
    }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        return false;
    }
    *out    = buf;
    *outLen = got;
    return true;
}

/* A .scenario file: the container on its own, and its manifest.json read
 * straight out of it. scnPackageOpen takes a buffer that starts at the magic,
 * which a package file does — the trailer hunt is only for maps. No Lua runs
 * here, so a container lists on a server that would refuse to run its script. */
static bool scnDirReadPackage(const char *path, ScenarioManifest *out) {
    uint8_t        *file     = NULL;
    size_t          fileLen  = 0;
    uint8_t        *json     = NULL;
    size_t          jsonLen  = 0;
    ScnPackage     *p;
    ScnManifestDoc *doc;
    const ScenarioManifest *values;
    char            err[SCN_DIR_LINE_LEN];
    bool            ok = false;

    if (!scnDirReadFile(path, &file, &fileLen)) {
        scnDirSay("scenarios: %s could not be read", path);
        return false;
    }
    err[0] = '\0';
    p = scnPackageOpen(file, fileLen, err, sizeof(err));
    if (p == NULL) {
        scnDirSay("scenarios: %s is not a scenario package: %s", path, err);
        free(file);
        return false;
    }
    err[0] = '\0';
    if (!scnPackageReadEntry(p, SCN_PACKAGE_MANIFEST_ENTRY,
                             SCN_PACKAGE_MANIFEST_MAX_BYTES, &json, &jsonLen,
                             err, sizeof(err))) {
        if (err[0] != '\0') {
            scnDirSay("scenarios: %s: %s", path, err);
        } else {
            scnDirSay("scenarios: %s carries no %s", path,
                      SCN_PACKAGE_MANIFEST_ENTRY);
        }
        scnPackageClose(p);
        free(file);
        return false;
    }
    err[0] = '\0';
    doc = scnManifestParse(json, jsonLen, NULL, err, sizeof(err));
    if (doc == NULL) {
        scnDirSay("scenarios: %s has a manifest that will not parse: %s", path,
                  err);
    } else {
        values = scnManifestValues(doc);
        if (values != NULL) {
            *out = *values;
            ok   = true;
        }
        scnManifestFree(doc);
    }
    free(json);
    scnPackageClose(p);
    free(file);
    return ok;
}

/* How many seats the lobby template asks for, over every team. Held at 255:
 * the count travels in one byte, and a template asking for more than a game
 * holds is the validator's to complain about, not this list's. */
static uint8_t scnDirBots(const ScenarioManifest *m) {
    unsigned total = 0;
    uint8_t  i;

    for (i = 0; i < m->lobby.numTeams && i < MAX_TANKS; i++) {
        total += (unsigned)m->lobby.teams[i].bots;
    }
    if (total > 255u) {
        total = 255u;
    }
    return (uint8_t)total;
}

static void scnDirFill(ScnDirEntry *e, const char *file,
                       const ScenarioManifest *m) {
    memset(e, 0, sizeof(*e));
    SDL_strlcpy(e->file, file, sizeof(e->file));
    SDL_strlcpy(e->name, m->name, sizeof(e->name));
    SDL_strlcpy(e->description, m->description, sizeof(e->description));
    e->maxPlayers = m->lobby.maxPlayers;
    e->bots       = scnDirBots(m);
    e->bound      = m->bound;
}

/* File-name order. Two scenarios may share a manifest name and two files in
 * one directory may not, so the file name is what orders the list. */
static int scnDirCmp(const void *a, const void *b) {
    const ScnDirEntry *ea = (const ScnDirEntry *)a;
    const ScnDirEntry *eb = (const ScnDirEntry *)b;

    return SDL_strcasecmp(ea->file, eb->file);
}

int scnDirList(const char *dir, ScnDirEntry *out, int max) {
    char             **files;
    int                fileCount = 0;
    int                n         = 0;
    int                i;
    ScnValidateResult *check;

    if (dir == NULL || dir[0] == '\0' || out == NULL || max <= 0) {
        return -1;
    }
    files = SDL_GlobDirectory(dir, NULL, 0, &fileCount);
    if (files == NULL) {
        return -1;
    }

    /* One of these, reused down the directory: a result carries the whole
       manifest and the issue list with it, which is more than a server thread's
       stack should hold per file. */
    check = (ScnValidateResult *)malloc(sizeof(*check));
    if (check == NULL) {
        SDL_free(files);
        return -1;
    }

    for (i = 0; i < fileCount && n < max; i++) {
        const char  *name = files[i];
        char         path[1024];
        SDL_PathInfo info;
        bool         ok = false;

        if (name == NULL || name[0] == '\0' || name[0] == '.') {
            continue;
        }
        if (strlen(name) >= SCN_DIR_FILE_LEN) {
            scnDirSay("scenarios: %s has too long a name to offer", name);
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        /* A directory named like a scenario is not one, and is skipped as
           silently as any other thing in here that is not ours. */
        if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE) {
            continue;
        }

        memset(check, 0, sizeof(*check));
        if (scnDirHasExt(name, SCN_SCENARIO_PACKAGE_EXT)) {
            ok = scnDirReadPackage(path, &check->manifest);
        } else if (scnDirHasExt(name, SCN_SCENARIO_SCRIPT_EXT)) {
            /* A NULL sim leaves out the two checks that read a map, which is
               the whole of what a scenario offered without one can be held to.
               The issues are not read here: a file with a problem against it is
               still a file the host should be able to see. */
            (void)scenarioValidateScript(NULL, path, check);
            ok = check->haveManifest;
            if (!ok) {
                scnDirSay("scenarios: %s declares no scenario table", path);
            }
        } else {
            /* Anything else in the directory is not ours. */
            continue;
        }

        if (ok) {
            scnDirFill(&out[n], name, &check->manifest);
            n++;
        }
    }

    free(check);
    SDL_free(files);

    if (n > 1) {
        qsort(out, (size_t)n, sizeof(out[0]), scnDirCmp);
    }
    return n;
}
