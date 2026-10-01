/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_scenario_pack.c
 * Purpose:
 *   Reading a map's packed scenario and writing one back,
 *   either on to the map or as a .scenario file.
 *
 *   The order of operations follows scnPackMap, which is the
 *   server's -pack: check first, refuse on anything at all,
 *   and only then hand the manifest and the source to the
 *   writer. The editor differs in where the two come from —
 *   the manifest is the forms' and the source is the pane's,
 *   neither of which has to be on disk — and in building no
 *   ServerSim, which leaves out the one check that reads a
 *   map: the tags, which ask how many pills, bases and starts
 *   the map carries. The rules are checked either way, against
 *   the classic table.
 *********************************************************/

#include "mapeditor_scenario_pack.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scenario_chunk.h"        /* scnIoWriteMapChunk */
#include "scenario_host.h"         /* SCN_SCRIPT_MAX_BYTES */
#include "scenario_manifest_json.h"
#include "scenario_package.h"

/* The largest map file this will read to look for a container on the end of
 * it. A .map is tens of kilobytes and a container carrying brain entries is
 * megabytes, so this is generous rather than tight: it is here so a file that
 * measures a gigabyte is turned down before a buffer is asked for. */
#define ME_PACK_MAP_MAX_BYTES (32u * 1024u * 1024u)

/* What a mod is called when its manifest has no name yet. */
#define ME_PACK_UNNAMED_MOD "scenario"

/* The extension a mod file takes. This restates SCN_SCENARIO_PACKAGE_EXT in
 * src/scenario/scenario_dir.h, which this file cannot include: that header
 * pulls scenario_defs.h out of scenario_api/, and the editor is built under
 * the gui profile, which grants only public/. Change one and this has to move
 * with it, or a mod the editor writes is one the scenario directory does not
 * list. */
#define ME_PACK_MOD_EXT ".scenario"

static void mePackErr(char *err, size_t errLen, const char *fmt, ...) {
    va_list ap;

    if (err == NULL || errLen == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, errLen, fmt, ap);
    va_end(ap);
    err[errLen - 1] = '\0';
}

/* The whole file, into a buffer the caller frees. The package reader points
 * into these bytes rather than copying them, so the buffer has to outlive the
 * handle opened on it. */
static bool mePackReadWhole(const char *path, uint8_t **out, size_t *outLen,
                            char *err, size_t errLen) {
    FILE    *f;
    long     size;
    uint8_t *buf;
    size_t   got;

    *out    = NULL;
    *outLen = 0;

    f = fopen(path, "rb");
    if (f == NULL) {
        mePackErr(err, errLen, "%s: the map could not be opened", path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        mePackErr(err, errLen, "%s: the map could not be measured", path);
        return false;
    }
    if ((size_t)size > (size_t)ME_PACK_MAP_MAX_BYTES) {
        fclose(f);
        mePackErr(err, errLen, "%s: the map is too large to look inside", path);
        return false;
    }
    buf = (uint8_t *)malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        mePackErr(err, errLen, "%s: no memory to read the map into", path);
        return false;
    }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        mePackErr(err, errLen, "%s: the map could not be read to its end", path);
        return false;
    }
    *out    = buf;
    *outLen = got;
    return true;
}

/* ── Reading what is already on a map ─────────────────────────────── */

bool meScenarioReadFromMap(const char *mapPath, ScenarioManifest *outManifest,
                           char **outScript, size_t *outScriptLen,
                           bool *outFound, char *err, size_t errLen) {
    uint8_t                *file     = NULL;
    size_t                  fileLen  = 0;
    const uint8_t          *chunk    = NULL;
    size_t                  chunkLen = 0;
    ScnPackage             *p        = NULL;
    ScnManifestDoc         *doc      = NULL;
    uint8_t                *json     = NULL;
    size_t                  jsonLen  = 0;
    uint8_t                *lua      = NULL;
    size_t                  luaLen   = 0;
    const char             *entry;
    const ScenarioManifest *values;
    bool                    ok = false;

    if (err != NULL && errLen > 0) {
        err[0] = '\0';
    }
    if (outFound != NULL) {
        *outFound = false;
    }
    if (outScript != NULL) {
        *outScript = NULL;
    }
    if (outScriptLen != NULL) {
        *outScriptLen = 0;
    }
    if (mapPath == NULL || mapPath[0] == '\0' || outManifest == NULL ||
        outScript == NULL || outScriptLen == NULL || outFound == NULL) {
        mePackErr(err, errLen, "there is no map to read a scenario from");
        return false;
    }

    if (!mePackReadWhole(mapPath, &file, &fileLen, err, errLen)) {
        return false;
    }

    /* Nothing after the map data: an ordinary map, and not a fault. */
    if (!scnPackageFindInMap(file, fileLen, &chunk, &chunkLen)) {
        free(file);
        return true;
    }

    p = scnPackageOpen(chunk, chunkLen, err, errLen);
    if (p == NULL) {
        free(file);
        return false;
    }

    if (!scnPackageReadEntry(p, SCN_PACKAGE_MANIFEST_ENTRY,
                             SCN_PACKAGE_MANIFEST_MAX_BYTES, &json, &jsonLen,
                             err, errLen)) {
        /* An entry that is simply not there leaves err empty, and a container
           with no manifest in it is one nothing can read. */
        if (err == NULL || errLen == 0 || err[0] == '\0') {
            mePackErr(err, errLen, "%s: the scenario on this map has no %s",
                      mapPath, SCN_PACKAGE_MANIFEST_ENTRY);
        }
        goto done;
    }

    doc = scnManifestParse(json, jsonLen, NULL, err, errLen);
    if (doc == NULL) {
        goto done;
    }
    values = scnManifestValues(doc);
    if (values == NULL) {
        mePackErr(err, errLen, "%s: the scenario's manifest could not be read",
                  mapPath);
        goto done;
    }
    *outManifest = *values;

    /* The script sits at whichever entry the manifest names, and at main.lua
       when it names none. The same cap the pane opens a loose script under,
       because this is the text that ends up in it. */
    entry = scnManifestScriptEntry(doc);
    if (entry == NULL || entry[0] == '\0') {
        entry = SCN_PACKAGE_SCRIPT_ENTRY;
    }
    if (scnPackageHasEntry(p, entry)) {
        if (!scnPackageReadEntry(p, entry, (size_t)SCN_SCRIPT_MAX_BYTES, &lua,
                                 &luaLen, err, errLen)) {
            if (err == NULL || errLen == 0 || err[0] == '\0') {
                mePackErr(err, errLen, "%s: %s could not be read", mapPath,
                          entry);
            }
            goto done;
        }
        /* The read leaves a 0 byte past the content, so the bytes are already
           a C string and are handed over as one. */
        *outScript    = (char *)lua;
        *outScriptLen = luaLen;
        lua           = NULL;
    }

    *outFound = true;
    ok        = true;

done:
    free(lua);
    free(json);
    scnManifestFree(doc);
    scnPackageClose(p);
    free(file);
    return ok;
}

/* ── Writing ──────────────────────────────────────────────────────── */

void meScenarioModManifest(const ScenarioManifest *in, ScenarioManifest *out) {
    if (out == NULL) {
        return;
    }
    if (in == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }

    *out = *in;
    /* A mod plays over a map it has never seen, so it can say nothing about
       that map's entities or squares. The validator refuses a package with
       bound false that fills either of them. */
    out->bound = false;
    memset(out->pillTags, 0, sizeof(out->pillTags));
    memset(out->baseTags, 0, sizeof(out->baseTags));
    memset(out->startTags, 0, sizeof(out->startTags));
    memset(out->regions, 0, sizeof(out->regions));
    out->numRegions = 0;
}

/* ── The scenario the map came with ───────────────────────────────── */

void meScenarioPackedInit(MEScenarioPacked *p) {
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof(*p));
}

void meScenarioPackedClear(MEScenarioPacked *p) {
    if (p == NULL) {
        return;
    }
    free(p->script);
    memset(p, 0, sizeof(*p));
}

bool meScenarioPackedSet(MEScenarioPacked *p, const ScenarioManifest *m,
                         const char *script, size_t scriptLen) {
    char *copy = NULL;

    if (p == NULL || m == NULL) {
        return false;
    }

    if (script != NULL) {
        copy = (char *)malloc(scriptLen + 1);
        if (copy == NULL) {
            /* Nothing kept rather than a manifest with no script beside it:
               putting half of a package back would be worse than saying it
               could not be kept. */
            meScenarioPackedClear(p);
            return false;
        }
        if (scriptLen > 0) {
            memcpy(copy, script, scriptLen);
        }
        copy[scriptLen] = '\0';
    }

    free(p->script);
    p->manifest  = *m;
    p->script    = copy;
    p->scriptLen = (copy != NULL) ? scriptLen : 0;
    p->present   = true;
    return true;
}

bool meScenarioPackedRestore(const MEScenarioPacked *p, const char *mapPath,
                             char *err, size_t errLen) {
    if (err != NULL && errLen > 0) {
        err[0] = '\0';
    }
    if (p == NULL || !p->present) {
        return true; /* the map opened without one, so there is none to keep */
    }
    if (mapPath == NULL || mapPath[0] == '\0') {
        mePackErr(err, errLen, "there is no map to write the scenario back to");
        return false;
    }

    /* The same writer the pack action uses, on the bytes that were already on
       the file. Saved under a new name, the new file gets the chunk too. */
    return scnIoWriteMapChunk(mapPath, &p->manifest,
                              (p->script != NULL) ? p->script : "",
                              (p->script != NULL) ? p->scriptLen : 0, err,
                              errLen);
}

bool meScenarioPackIntoMap(const ScenarioManifest *m, const char *script,
                           size_t scriptLen, const char *mapPath,
                           char *err, size_t errLen) {
    if (err != NULL && errLen > 0) {
        err[0] = '\0';
    }
    if (m == NULL) {
        mePackErr(err, errLen, "there is no scenario to pack");
        return false;
    }
    if (mapPath == NULL || mapPath[0] == '\0') {
        mePackErr(err, errLen, "there is no map to pack the scenario into");
        return false;
    }
    /* A container on a map is that map's scenario. One that plays over any map
       is a mod and is saved as a .scenario file of its own. */
    if (!m->bound) {
        mePackErr(err, errLen,
                  "this scenario is not built for its map, so it is a mod: "
                  "save it as a .scenario file instead");
        return false;
    }

    /* The manifest and the source go over as they are. The writer replaces
       whatever container was on the file and leaves the map alone on any
       failure. */
    return scnIoWriteMapChunk(mapPath, m, (script != NULL) ? script : "",
                              (script != NULL) ? scriptLen : 0, err, errLen);
}

bool meScenarioWriteMod(const ScenarioManifest *m, const char *script,
                        size_t scriptLen, const char *modPath,
                        char *err, size_t errLen) {
    ScenarioManifest *mod;
    ScnManifestDoc  *doc       = NULL;
    char            *json      = NULL;
    uint8_t         *container = NULL;
    size_t           length    = 0;
    ScnPackageEntry  entries[2];
    int              count = 0;
    FILE            *f;
    bool             ok = false;

    if (err != NULL && errLen > 0) {
        err[0] = '\0';
    }
    if (m == NULL) {
        mePackErr(err, errLen, "there is no scenario to save");
        return false;
    }
    if (modPath == NULL || modPath[0] == '\0') {
        mePackErr(err, errLen, "there is nowhere to save the mod");
        return false;
    }

    /* The mod's copy lives only as long as it takes to build the document,
       and on the heap: a manifest is more than this frame should hold. */
    mod = (ScenarioManifest *)malloc(sizeof(*mod));
    if (mod == NULL) {
        mePackErr(err, errLen, "out of memory");
        return false;
    }
    meScenarioModManifest(m, mod);
    doc = scnManifestFromValues(mod, err, errLen);
    free(mod);
    if (doc == NULL) {
        return false;
    }
    json = scnManifestWrite(doc, err, errLen);
    scnManifestFree(doc);
    if (json == NULL) {
        return false;
    }

    /* Stored rather than deflated: a manifest and a script are small, and a
       stored entry is what any zip tool shows as it was written. A package
       with no script at all is a rules-only mod, which is a manifest and
       nothing else. */
    memset(entries, 0, sizeof(entries));
    entries[count].name    = SCN_PACKAGE_MANIFEST_ENTRY;
    entries[count].bytes   = (const uint8_t *)json;
    entries[count].len     = strlen(json);
    entries[count].deflate = false;
    count++;
    if (script != NULL && scriptLen > 0) {
        entries[count].name    = SCN_PACKAGE_SCRIPT_ENTRY;
        entries[count].bytes   = (const uint8_t *)script;
        entries[count].len     = scriptLen;
        entries[count].deflate = false;
        count++;
    }

    if (!scnPackageWrite(entries, count, &container, &length, err, errLen)) {
        free(json);
        return false;
    }

    f = fopen(modPath, "wb");
    if (f == NULL) {
        mePackErr(err, errLen, "%s: the mod could not be opened to write",
                  modPath);
        goto done;
    }
    if (fwrite(container, 1, length, f) != length) {
        fclose(f);
        /* Half a container is not a scenario, so it does not stay behind. */
        remove(modPath);
        mePackErr(err, errLen, "%s: the mod could not be written out", modPath);
        goto done;
    }
    if (fclose(f) != 0) {
        remove(modPath);
        mePackErr(err, errLen, "%s: the mod could not be closed", modPath);
        goto done;
    }
    ok = true;

done:
    free(container);
    free(json);
    return ok;
}

/* ── The check both writes stand on ───────────────────────────────── */

bool meScenarioManifestAgrees(const ScenarioManifest *fromForm,
                              const ScenarioManifest *fromScript,
                              char *key, size_t keyLen,
                              char *err, size_t errLen) {
    return scnManifestAgrees(fromForm, fromScript, key, keyLen, err, errLen);
}

/* ── The name a mod is offered under ──────────────────────────────── */

bool meScenarioModFileName(const ScenarioManifest *m, char *out,
                           size_t outLen) {
    static const char kExt[] = ME_PACK_MOD_EXT;
    const char       *name;
    size_t            extLen = sizeof(kExt) - 1;
    size_t            i;
    size_t            used = 0;

    if (out == NULL || outLen == 0) {
        return false;
    }
    out[0] = '\0';

    name = (m != NULL && m->name[0] != '\0') ? m->name : ME_PACK_UNNAMED_MOD;

    for (i = 0; name[i] != '\0'; i++) {
        unsigned char c = (unsigned char)name[i];

        /* A manifest's name is a title and may hold anything an author typed;
           a file name may not. */
        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':' ||
            c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
            c == '|') {
            c = '_';
        }
        if (used + 1 + extLen + 1 > outLen) {
            break;
        }
        out[used++] = (char)c;
    }

    /* A name that was nothing but separators, or that left no room at all. */
    while (used > 0 && (out[used - 1] == ' ' || out[used - 1] == '.')) {
        used--;
    }
    if (used == 0) {
        size_t stand = sizeof(ME_PACK_UNNAMED_MOD) - 1;

        if (stand + extLen + 1 > outLen) {
            return false;
        }
        memcpy(out, ME_PACK_UNNAMED_MOD, stand);
        used = stand;
    }

    memcpy(out + used, kExt, extLen + 1);
    return true;
}
