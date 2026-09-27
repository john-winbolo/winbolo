/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Record JSON
 *Filename:      scenario_record_json.c
 *Author:        John Morrison
 *Purpose:
 *  scripts.json out of a ScnRecordDesc. A fresh tree built
 *  and printed once, so the text is the description and
 *  nothing else; see scenario_record_json.h for the shape.
 *********************************************************/

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "scenario_record_json.h"

static void rjErr(char *err, size_t errLen, const char *fmt, ...) {
    va_list ap;

    if (err == NULL || errLen == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, errLen, fmt, ap);
    va_end(ap);
}

/* A string field, with "" standing in for NULL so a row never loses a key. */
static bool rjPutString(cJSON *obj, const char *key, const char *s) {
    return cJSON_AddStringToObject(obj, key, s != NULL ? s : "") != NULL;
}

/* A rule value. A whole number goes out as an integer — 8 rather than
 * 8.0 or 8e0 — which is how an author wrote it and how a reader expects to
 * compare it. The bound keeps the conversion exact: every whole double
 * below 2^53 has an exact long long. Anything else, including a fraction,
 * is the number cJSON prints for it. */
static cJSON *rjRuleValue(double v) {
    if (v > -9007199254740992.0 && v < 9007199254740992.0 &&
        (double)(long long)v == v) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)v);
        return cJSON_CreateRaw(buf);
    }
    return cJSON_CreateNumber(v);
}

char *scnRecordJsonWrite(const ScnRecordDesc *d, char *err, size_t errLen) {
    cJSON *root;
    cJSON *rules;
    cJSON *regions;
    cJSON *scripts;
    char  *text;
    int    i;

    if (err != NULL && errLen > 0) {
        err[0] = '\0';
    }
    if (d == NULL) {
        rjErr(err, errLen, "scripts.json: no description");
        return NULL;
    }
    if (d->numRules < 0 || d->numRules > SCN_MANIFEST_RULES_MAX ||
        d->numRegions < 0 || d->numRegions > SCN_REGIONS_MAX ||
        d->numScripts < 0 || d->numScripts > SCN_RECORD_SCRIPTS_MAX) {
        rjErr(err, errLen,
              "scripts.json: a count is out of range (rules %d, regions %d, "
              "scripts %d)", d->numRules, d->numRegions, d->numScripts);
        return NULL;
    }

    root = cJSON_CreateObject();
    if (root == NULL) {
        rjErr(err, errLen, "scripts.json: out of memory");
        return NULL;
    }
    if (cJSON_AddNumberToObject(root, "version", SCN_RECORD_VERSION) == NULL ||
        !rjPutString(root, "map", d->map) ||
        cJSON_AddBoolToObject(root, "mods_enabled", d->modsEnabled) == NULL) {
        goto oom;
    }

    rules = cJSON_AddObjectToObject(root, "rules");
    if (rules == NULL) goto oom;
    for (i = 0; i < d->numRules; i++) {
        cJSON *v;
        if (d->rules[i].name == NULL || d->rules[i].name[0] == '\0') {
            continue;
        }
        v = rjRuleValue(d->rules[i].value);
        if (v == NULL) goto oom;
        if (!cJSON_AddItemToObject(rules, d->rules[i].name, v)) {
            cJSON_Delete(v);
            goto oom;
        }
    }

    regions = cJSON_AddArrayToObject(root, "regions");
    if (regions == NULL) goto oom;
    for (i = 0; i < d->numRegions; i++) {
        const ScnRecordRegion *r = &d->regions[i];
        cJSON *row = cJSON_CreateObject();
        if (row == NULL) goto oom;
        if (!cJSON_AddItemToArray(regions, row)) {
            cJSON_Delete(row);
            goto oom;
        }
        if (!rjPutString(row, "name", r->name) ||
            cJSON_AddNumberToObject(row, "x", r->x) == NULL ||
            cJSON_AddNumberToObject(row, "y", r->y) == NULL ||
            cJSON_AddNumberToObject(row, "w", r->w) == NULL ||
            cJSON_AddNumberToObject(row, "h", r->h) == NULL ||
            !rjPutString(row, "file", r->file)) {
            goto oom;
        }
    }

    scripts = cJSON_AddArrayToObject(root, "scripts");
    if (scripts == NULL) goto oom;
    for (i = 0; i < d->numScripts; i++) {
        const ScnRecordScript *s = &d->scripts[i];
        cJSON *row = cJSON_CreateObject();
        cJSON *manifest;
        if (row == NULL) goto oom;
        if (!cJSON_AddItemToArray(scripts, row)) {
            cJSON_Delete(row);
            goto oom;
        }
        if (!rjPutString(row, "file", s->file) ||
            !rjPutString(row, "source", s->source) ||
            !rjPutString(row, "kind", s->kind)) {
            goto oom;
        }
        /* Parsed and put in as an object rather than as a string, so a
           reader walks one tree instead of parsing twice. */
        manifest = cJSON_Parse(s->manifestJson != NULL ? s->manifestJson
                                                       : "");
        if (manifest == NULL || !cJSON_IsObject(manifest)) {
            cJSON_Delete(manifest);
            rjErr(err, errLen,
                  "scripts.json: the manifest of %s is not a JSON object",
                  s->file != NULL ? s->file : "(unnamed)");
            cJSON_Delete(root);
            return NULL;
        }
        if (!cJSON_AddItemToObject(row, "manifest", manifest)) {
            cJSON_Delete(manifest);
            goto oom;
        }
    }

    text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        rjErr(err, errLen, "scripts.json: out of memory");
    }
    return text;

oom:
    cJSON_Delete(root);
    rjErr(err, errLen, "scripts.json: out of memory");
    return NULL;
}
