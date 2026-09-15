/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Manifest JSON
 *Filename:      scenario_manifest_json.c
 *Author:        John Morrison
 *Purpose:
 *  manifest.json into ScenarioManifest and back out again.
 *
 *  The decode is the twin of scnReadManifest in
 *  scenario_host.c: the same fields, the same defaults —
 *  api 1, bound true, a team fielded unless it says
 *  otherwise — and the same soft reports through
 *  ScnParseReport for the same shapes, so a rule name that
 *  names no rule reads the same whether an author wrote it
 *  in Lua or in JSON.
 *
 *  A doc keeps the tree it parsed. The emit writes the
 *  decoded fields back over a copy of that tree rather than
 *  building a fresh one, so a key this build does not
 *  know — at the root, inside lobby, inside rules, anywhere
 *  — is still in the text that comes out.
 *
 *  scnManifestAgrees compares two of these structs, one
 *  from each form. Rules and regions are compared by rule
 *  and by name rather than by position: Lua traverses both
 *  of those tables with lua_next, whose order is its own
 *  business. A team's init table is keyed the same way and
 *  is compared the same way. lobby.teams is a Lua array, so
 *  it is compared in order and the index is part of the key.
 *********************************************************/

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "server_sim.h"   /* serverSimConsoleMessage, and the entity counts */
#include "scenario_lua.h" /* scenarioLuaRuleIndex, scenarioLuaRuleName */
#include "scenario_manifest_json.h"
#include "scenario_table.h" /* scnTableClear, scnTableSet, scnTableGet */

/* One report or refusal line. The same length the host gives its own. */
#define MJ_LINE_LEN 512

struct ScnManifestDoc {
    cJSON           *root;   /* what was parsed, kept so unknown keys live */
    ScenarioManifest values;
    int              schema;
    char             script[SCN_MANIFEST_ENTRY_LEN];
    int              brainCount;
    char             brains[SCN_MANIFEST_BRAINS_MAX][SCN_BRAIN_LEN];
};

/* ── Saying things ────────────────────────────────────────────────── */

static void mjErr(char *err, size_t errLen, const char *fmt, ...) {
    va_list ap;
    if (err == NULL || errLen == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, errLen, fmt, ap);
    va_end(ap);
    err[errLen - 1] = '\0';
}

/* Say one line about the file being read, on the same three channels
 * scnReadRules says its own on: the operator, the caller's soft buffer and a
 * validator's issue list. A problem here drops the item it is about and the
 * rest of the manifest still applies. */
static void mjReport(ScnParseReport *rep, const char *key,
                     const char *fmt, ...) {
    char    line[MJ_LINE_LEN];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    serverSimConsoleMessage(line);
    if (rep == NULL) {
        return;
    }
    if (rep->soft != NULL && rep->softLen > 0) {
        snprintf(rep->soft, rep->softLen, "%s", line);
    }
    if (rep->sink != NULL) {
        scnIssueAdd(rep->sink, key, "%s", line);
    }
}

/* Copy truncated to the field's capacity, the way scnReadStr does. */
static void mjCopyStr(char *dst, size_t dstLen, const char *src) {
    size_t n;

    if (dst == NULL || dstLen == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= dstLen) {
        n = dstLen - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ── Reading one field ────────────────────────────────────────────── */

static double mjNumber(const cJSON *obj, const char *key, double dflt) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(it) ? it->valuedouble : dflt;
}

/* Only a real true or false counts, as lua_isboolean does in the Lua
 * reader: a number or a string under the key takes the default. */
static bool mjBool(const cJSON *obj, const char *key, bool dflt) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(it)) {
        return cJSON_IsTrue(it) ? true : false;
    }
    return dflt;
}

static void mjString(const cJSON *obj, const char *key, char *dst,
                     size_t dstLen) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring != NULL) {
        mjCopyStr(dst, dstLen, it->valuestring);
    }
}

/* One pair of a team's init table, stored the way the Lua reader stores it.
 * A string is itself; a number is its digits, because that is what
 * lua_tostring makes of a script's number before it reaches the table. JSON
 * has one number type and cannot say whether an author wrote 100 or 100.0,
 * so a whole number is written whole — a table of amounts is made of those.
 *
 * False for a value that is neither, which is what the Lua reader refuses a
 * value that is not a string or a number for, and for a pair that does not
 * fit. The caller treats the two the same because the validator's sentence
 * about them is the same one. */
static bool mjInitPair(ScnTable *t, const char *key, const cJSON *v) {
    char num[32];

    if (cJSON_IsString(v) && v->valuestring != NULL) {
        return scnTableSet(t, key, v->valuestring);
    }
    if (!cJSON_IsNumber(v)) {
        return false;
    }
    if (v->valuedouble >= -9007199254740992.0 &&
        v->valuedouble <= 9007199254740992.0 &&
        v->valuedouble == (double)(long long)v->valuedouble) {
        snprintf(num, sizeof(num), "%lld", (long long)v->valuedouble);
    } else {
        snprintf(num, sizeof(num), "%.14g", v->valuedouble);
    }
    return scnTableSet(t, key, num);
}

/* ── Decoding ─────────────────────────────────────────────────────── */

/* The table this team's bots are built with. The walk stops at the first
 * pair that does not fit, the pairs before it stay, and the key is named on
 * the team — the shape scnReadLobby leaves behind, so one sentence in the
 * validator reports a packaged manifest and a script's table alike. A field
 * that is not an object leaves the table empty and names no pair, the way a
 * field of the wrong type is left alone in the Lua reader. */
static void mjDecodeInit(const cJSON *t, ScnManifestTeam *team) {
    const cJSON *init = cJSON_GetObjectItemCaseSensitive(t, "init");
    const cJSON *it;

    scnTableClear(&team->init);
    team->initBadKey[0] = '\0';
    if (!cJSON_IsObject(init)) {
        return;
    }
    cJSON_ArrayForEach(it, init) {
        if (it->string == NULL) {
            continue;
        }
        if (!mjInitPair(&team->init, it->string, it)) {
            mjCopyStr(team->initBadKey, sizeof(team->initBadKey),
                      it->string);
            return;
        }
    }
}

static void mjDecodeLobby(const cJSON *root, ScnManifestLobby *lob,
                          ScnParseReport *rep) {
    const cJSON *lobby = cJSON_GetObjectItemCaseSensitive(root, "lobby");
    const cJSON *teams;
    const cJSON *t;

    if (!cJSON_IsObject(lobby)) {
        return;
    }
    lob->maxPlayers = (uint8_t)mjNumber(lobby, "max_players", 0);
    lob->extraTeams = mjBool(lobby, "extra_teams", false);

    teams = cJSON_GetObjectItemCaseSensitive(lobby, "teams");
    if (!cJSON_IsArray(teams)) {
        return;
    }
    cJSON_ArrayForEach(t, teams) {
        ScnManifestTeam *team;
        if (!cJSON_IsObject(t)) {
            continue;
        }
        if (lob->numTeams >= MAX_TANKS) {
            mjReport(rep, "lobby.teams",
                     "scenario: more than %d teams; the rest dropped",
                     MAX_TANKS);
            break;
        }
        team = &lob->teams[lob->numTeams];
        lob->numTeams++;
        team->id      = (uint8_t)mjNumber(t, "id", 0);
        team->bots    = (uint8_t)mjNumber(t, "bots", 0);
        team->maxBots = (uint8_t)mjNumber(t, "max_bots", 0);
        team->fielded = mjBool(t, "fielded", true);
        mjString(t, "brain", team->brain, sizeof(team->brain));
        mjDecodeInit(t, team);
    }
}

/* A key that names no rule is refused by name and the rest of the object
 * still applies, which is what the Lua reader does with the same mistake. */
static void mjDecodeRules(const cJSON *root, ScenarioManifest *m,
                          ScnParseReport *rep) {
    const cJSON *rules = cJSON_GetObjectItemCaseSensitive(root, "rules");
    const cJSON *item;

    if (!cJSON_IsObject(rules)) {
        return;
    }
    cJSON_ArrayForEach(item, rules) {
        int idx;
        if (item->string == NULL || !cJSON_IsNumber(item)) {
            continue;
        }
        idx = scenarioLuaRuleIndex(item->string);
        if (idx < 0) {
            char where[SCN_VALIDATE_KEY_LEN];
            snprintf(where, sizeof(where), "rules.%s", item->string);
            mjReport(rep, where, "scenario: no rule is named '%s'",
                     item->string);
        } else if (m->numRules >= SCN_MANIFEST_RULES_MAX) {
            mjReport(rep, "rules",
                     "scenario: more than %d rules; '%s' dropped",
                     SCN_MANIFEST_RULES_MAX, item->string);
        } else {
            m->rules[m->numRules].rule  = (uint16_t)idx;
            m->rules[m->numRules].value = item->valuedouble;
            m->numRules++;
        }
    }
}

static void mjAddTag(ScnManifestTags *out, const char *s, const char *kind,
                     int entity, const char *where, ScnParseReport *rep) {
    if (out->count >= SCN_TAGS_PER_ENTITY) {
        mjReport(rep, where,
                 "scenario: %s %d already carries %d tags; '%s' dropped",
                 kind, entity, SCN_TAGS_PER_ENTITY, s);
        return;
    }
    mjCopyStr(out->tag[out->count], SCN_TAG_LEN, s);
    out->count++;
}

/* A tags key is the 1-based entity index written as text, because a JSON
 * object key is always text. Nothing but digits is an index. */
static bool mjEntityIndex(const char *s, int *out) {
    long        v;
    const char *p;

    if (s == NULL || s[0] == '\0') {
        return false;
    }
    for (p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    v = strtol(s, NULL, 10);
    if (v < 0 || v > 0xFFFF) {
        return false;
    }
    *out = (int)v;
    return true;
}

static void mjDecodeTagKind(const cJSON *tags, const char *key,
                            ScnManifestTags *arr, int maxEntity,
                            const char *kind, ScnParseReport *rep) {
    const cJSON *kt = cJSON_GetObjectItemCaseSensitive(tags, key);
    const cJSON *entry;

    if (!cJSON_IsObject(kt)) {
        return;
    }
    cJSON_ArrayForEach(entry, kt) {
        char where[SCN_VALIDATE_KEY_LEN];
        int  e = 0;

        if (entry->string == NULL) {
            continue;
        }
        if (!mjEntityIndex(entry->string, &e)) {
            snprintf(where, sizeof(where), "tags.%s.%s", key, entry->string);
            mjReport(rep, where, "scenario: '%s' is not a %s index",
                     entry->string, kind);
            continue;
        }
        snprintf(where, sizeof(where), "tags.%s[%d]", key, e);
        if (e < 1 || e > maxEntity) {
            mjReport(rep, where,
                     "scenario: %s %d is not an index this map can hold",
                     kind, e);
        } else if (cJSON_IsString(entry) && entry->valuestring != NULL) {
            mjAddTag(&arr[e], entry->valuestring, kind, e, where, rep);
        } else if (cJSON_IsArray(entry)) {
            const cJSON *s;
            cJSON_ArrayForEach(s, entry) {
                if (cJSON_IsString(s) && s->valuestring != NULL) {
                    mjAddTag(&arr[e], s->valuestring, kind, e, where, rep);
                }
            }
        }
    }
}

static void mjDecodeTags(const cJSON *root, ScenarioManifest *m,
                         ScnParseReport *rep) {
    const cJSON *tags = cJSON_GetObjectItemCaseSensitive(root, "tags");

    if (!cJSON_IsObject(tags)) {
        return;
    }
    mjDecodeTagKind(tags, "pills",  m->pillTags,  MAX_PILLS,  "pill",  rep);
    mjDecodeTagKind(tags, "bases",  m->baseTags,  MAX_BASES,  "base",  rep);
    mjDecodeTagKind(tags, "starts", m->startTags, MAX_STARTS, "start", rep);
}

/* Regions are keyed by name here as they are in Lua, so a reader looks one
 * up by name rather than by the position it happened to land in. */
static void mjDecodeRegions(const cJSON *root, ScenarioManifest *m,
                            ScnParseReport *rep) {
    const cJSON *regions = cJSON_GetObjectItemCaseSensitive(root, "regions");
    const cJSON *entry;

    if (!cJSON_IsObject(regions)) {
        return;
    }
    cJSON_ArrayForEach(entry, regions) {
        ScnManifestRegion *reg;
        if (entry->string == NULL || !cJSON_IsObject(entry)) {
            continue;
        }
        if (m->numRegions >= SCN_REGIONS_MAX) {
            char where[SCN_VALIDATE_KEY_LEN];
            snprintf(where, sizeof(where), "regions.%s", entry->string);
            mjReport(rep, where, "scenario: more than %d regions; '%s' dropped",
                     SCN_REGIONS_MAX, entry->string);
            continue;
        }
        reg = &m->regions[m->numRegions];
        m->numRegions++;
        mjCopyStr(reg->name, sizeof(reg->name), entry->string);
        reg->x = (uint8_t)mjNumber(entry, "x", 0);
        reg->y = (uint8_t)mjNumber(entry, "y", 0);
        reg->w = (uint8_t)mjNumber(entry, "w", 0);
        reg->h = (uint8_t)mjNumber(entry, "h", 0);
    }
}

static void mjDecodeBrains(const cJSON *root, ScnManifestDoc *d,
                           ScnParseReport *rep) {
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "brains");
    const cJSON *s;

    if (!cJSON_IsArray(arr)) {
        return;
    }
    cJSON_ArrayForEach(s, arr) {
        if (!cJSON_IsString(s) || s->valuestring == NULL) {
            continue;
        }
        if (d->brainCount >= SCN_MANIFEST_BRAINS_MAX) {
            mjReport(rep, "brains",
                     "scenario: more than %d packaged brains; '%s' dropped",
                     SCN_MANIFEST_BRAINS_MAX, s->valuestring);
            break;
        }
        mjCopyStr(d->brains[d->brainCount], SCN_BRAIN_LEN, s->valuestring);
        d->brainCount++;
    }
}

/* Everything the schema names, out of the tree and into the struct.
 *
 * triggers is not read. Its schema is not settled, so nothing decodes it and
 * it rides along in the tree like any other key this build has no use for. */
static void mjDecode(ScnManifestDoc *d, ScnParseReport *rep) {
    ScenarioManifest *m = &d->values;

    mjCopyStr(d->script, sizeof(d->script), SCN_MANIFEST_SCRIPT_DEFAULT);
    mjString(d->root, "script", d->script, sizeof(d->script));
    if (d->script[0] == '\0') {
        mjCopyStr(d->script, sizeof(d->script), SCN_MANIFEST_SCRIPT_DEFAULT);
    }

    mjString(d->root, "name", m->name, sizeof(m->name));
    mjString(d->root, "description", m->description, sizeof(m->description));
    mjString(d->root, "game", m->game, sizeof(m->game));
    m->api   = (int)mjNumber(d->root, "api", 1);
    m->bound = mjBool(d->root, "bound", true);

    mjDecodeLobby(d->root, &m->lobby, rep);
    mjDecodeRules(d->root, m, rep);
    mjDecodeTags(d->root, m, rep);
    mjDecodeRegions(d->root, m, rep);
    mjDecodeBrains(d->root, d, rep);
}

/* ── The doc ──────────────────────────────────────────────────────── */

static ScnManifestDoc *mjWrap(cJSON *root) {
    ScnManifestDoc *d = (ScnManifestDoc *)calloc(1, sizeof(ScnManifestDoc));
    if (d == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    d->root   = root;
    d->schema = SCN_MANIFEST_SCHEMA_VERSION;
    return d;
}

ScnManifestDoc *scnManifestParse(const uint8_t *json, size_t len,
                                 ScnParseReport *rep,
                                 char *err, size_t errLen) {
    cJSON          *root;
    const cJSON    *version;
    ScnManifestDoc *d;
    int             schema;

    if (err != NULL && errLen > 0) {
        err[0] = '\0';
    }
    if (json == NULL || len == 0) {
        mjErr(err, errLen, "scenario: the manifest is empty");
        return NULL;
    }

    root = cJSON_ParseWithLength((const char *)json, len);
    if (root == NULL) {
        mjErr(err, errLen, "scenario: manifest.json is not JSON");
        return NULL;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        mjErr(err, errLen, "scenario: manifest.json is not a JSON object");
        return NULL;
    }

    version = cJSON_GetObjectItemCaseSensitive(root, "manifest");
    if (version == NULL) {
        cJSON_Delete(root);
        mjErr(err, errLen, "scenario: the manifest has no manifest version");
        return NULL;
    }
    if (!cJSON_IsNumber(version)) {
        cJSON_Delete(root);
        mjErr(err, errLen, "scenario: the manifest version is not a number");
        return NULL;
    }
    schema = (int)version->valuedouble;
    if (schema != SCN_MANIFEST_SCHEMA_VERSION) {
        cJSON_Delete(root);
        mjErr(err, errLen,
              "scenario: manifest version %d; this build reads version %d only",
              schema, SCN_MANIFEST_SCHEMA_VERSION);
        return NULL;
    }

    d = mjWrap(root);
    if (d == NULL) {
        mjErr(err, errLen, "scenario: out of memory reading the manifest");
        return NULL;
    }
    d->schema = schema;
    mjDecode(d, rep);
    return d;
}

ScnManifestDoc *scnManifestFromValues(const ScenarioManifest *m,
                                      char *err, size_t errLen) {
    ScnManifestDoc *d;
    cJSON          *root;

    if (err != NULL && errLen > 0) {
        err[0] = '\0';
    }
    if (m == NULL) {
        mjErr(err, errLen, "scenario: no scenario table to write a manifest from");
        return NULL;
    }
    /* An empty object rather than no tree at all: the emit path is one piece
     * of code either way, and an object with nothing in it carries no key
     * this build does not know. */
    root = cJSON_CreateObject();
    if (root == NULL) {
        mjErr(err, errLen, "scenario: out of memory writing the manifest");
        return NULL;
    }
    d = mjWrap(root);
    if (d == NULL) {
        mjErr(err, errLen, "scenario: out of memory writing the manifest");
        return NULL;
    }
    d->values = *m;
    mjCopyStr(d->script, sizeof(d->script), SCN_MANIFEST_SCRIPT_DEFAULT);
    return d;
}

void scnManifestFree(ScnManifestDoc *d) {
    if (d == NULL) {
        return;
    }
    cJSON_Delete(d->root);
    free(d);
}

const ScenarioManifest *scnManifestValues(const ScnManifestDoc *d) {
    return (d == NULL) ? NULL : &d->values;
}

int scnManifestSchemaVersion(const ScnManifestDoc *d) {
    return (d == NULL) ? 0 : d->schema;
}

const char *scnManifestScriptEntry(const ScnManifestDoc *d) {
    return (d == NULL) ? NULL : d->script;
}

int scnManifestBrainCount(const ScnManifestDoc *d) {
    return (d == NULL) ? 0 : d->brainCount;
}

const char *scnManifestBrainName(const ScnManifestDoc *d, int i) {
    if (d == NULL || i < 0 || i >= d->brainCount) {
        return NULL;
    }
    return d->brains[i];
}

/* ── Writing ──────────────────────────────────────────────────────── */

/* Replace a member in place where it is already there, so a key keeps the
 * position it was parsed at, and append it where it is not. */
static void mjPut(cJSON *obj, const char *key, cJSON *value) {
    if (obj == NULL || value == NULL) {
        cJSON_Delete(value);
        return;
    }
    if (cJSON_GetObjectItemCaseSensitive(obj, key) != NULL) {
        cJSON_ReplaceItemInObjectCaseSensitive(obj, key, value);
    } else {
        cJSON_AddItemToObject(obj, key, value);
    }
}

static void mjPutNumber(cJSON *obj, const char *key, double v) {
    mjPut(obj, key, cJSON_CreateNumber(v));
}

static void mjPutString(cJSON *obj, const char *key, const char *v) {
    mjPut(obj, key, cJSON_CreateString(v));
}

static void mjPutBool(cJSON *obj, const char *key, bool v) {
    mjPut(obj, key, cJSON_CreateBool(v ? 1 : 0));
}

/* The object under key, made if it is not there and replaced if what is
 * there is not an object. */
static cJSON *mjObjectFor(cJSON *parent, const char *key) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (it != NULL && cJSON_IsObject(it)) {
        return it;
    }
    it = cJSON_CreateObject();
    mjPut(parent, key, it);
    return it;
}

static cJSON *mjArrayFor(cJSON *parent, const char *key) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (it != NULL && cJSON_IsArray(it)) {
        return it;
    }
    it = cJSON_CreateArray();
    mjPut(parent, key, it);
    return it;
}

/* A team's init table as an object of names to text. What goes out is the
 * table the decode built rather than the object it was read from: an init
 * table is free-form, so every key in it is one this build reads and there
 * is no unknown key here to keep. A number read as text goes back out as
 * that text, which is the value the team's bots are built with. A team
 * holding no pairs carries no init key at all. */
static void mjEmitInit(cJSON *t, const ScnTable *init) {
    cJSON *obj;
    int    i;

    if (init->count == 0) {
        cJSON_DeleteItemFromObjectCaseSensitive(t, "init");
        return;
    }
    obj = cJSON_CreateObject();
    if (obj == NULL) {
        return;
    }
    for (i = 0; i < (int)init->count; i++) {
        cJSON_AddStringToObject(obj, init->kv[i].key, init->kv[i].value);
    }
    mjPut(t, "init", obj);
}

static void mjEmitTeams(cJSON *lobby, const ScnManifestLobby *lob) {
    cJSON *teams = mjArrayFor(lobby, "teams");
    int    i;

    if (teams == NULL) {
        return;
    }
    /* Each team object is updated where one is already there, so a key a
     * team carries that this build does not read stays with its team. */
    for (i = 0; i < (int)lob->numTeams; i++) {
        cJSON *t = cJSON_GetArrayItem(teams, i);
        if (t == NULL) {
            t = cJSON_CreateObject();
            if (t == NULL) {
                return;
            }
            cJSON_AddItemToArray(teams, t);
        } else if (!cJSON_IsObject(t)) {
            cJSON *fresh = cJSON_CreateObject();
            if (fresh == NULL) {
                return;
            }
            cJSON_ReplaceItemInArray(teams, i, fresh);
            t = fresh;
        }
        mjPutNumber(t, "id", lob->teams[i].id);
        mjPutNumber(t, "bots", lob->teams[i].bots);
        mjPutNumber(t, "max_bots", lob->teams[i].maxBots);
        mjPutBool(t, "fielded", lob->teams[i].fielded);
        mjPutString(t, "brain", lob->teams[i].brain);
        mjEmitInit(t, &lob->teams[i].init);
    }
    while (cJSON_GetArraySize(teams) > (int)lob->numTeams) {
        cJSON_DeleteItemFromArray(teams, cJSON_GetArraySize(teams) - 1);
    }
}

static void mjEmitTagKind(cJSON *tags, const char *key,
                          const ScnManifestTags *arr, int maxEntity) {
    cJSON *kt = mjObjectFor(tags, key);
    int    e;

    if (kt == NULL) {
        return;
    }
    for (e = 1; e <= maxEntity; e++) {
        char   name[16];
        cJSON *list;
        int    i;

        if (arr[e].count == 0) {
            continue;
        }
        snprintf(name, sizeof(name), "%d", e);
        list = cJSON_CreateArray();
        if (list == NULL) {
            return;
        }
        for (i = 0; i < (int)arr[e].count; i++) {
            cJSON *s = cJSON_CreateString(arr[e].tag[i]);
            if (s != NULL) {
                cJSON_AddItemToArray(list, s);
            }
        }
        mjPut(kt, name, list);
    }
}

/* The decoded fields written back over a copy of the parsed tree. Anything
 * the copy holds that is not written here is left exactly as it arrived. */
static void mjEmit(cJSON *root, const ScnManifestDoc *d) {
    const ScenarioManifest *m = &d->values;
    cJSON                  *lobby;
    cJSON                  *rules;
    cJSON                  *tags;
    cJSON                  *regions;
    cJSON                  *brains;
    int                     i;

    mjPutNumber(root, "manifest", d->schema);
    mjPutNumber(root, "api", m->api);
    mjPutString(root, "name", m->name);
    mjPutString(root, "description", m->description);
    mjPutString(root, "game", m->game);
    mjPutBool(root, "bound", m->bound);

    lobby = mjObjectFor(root, "lobby");
    if (lobby != NULL) {
        mjPutNumber(lobby, "max_players", m->lobby.maxPlayers);
        mjPutBool(lobby, "extra_teams", m->lobby.extraTeams);
        mjEmitTeams(lobby, &m->lobby);
    }

    rules = mjObjectFor(root, "rules");
    if (rules != NULL) {
        for (i = 0; i < (int)m->numRules; i++) {
            mjPutNumber(rules, scenarioLuaRuleName((int)m->rules[i].rule),
                        m->rules[i].value);
        }
    }

    tags = mjObjectFor(root, "tags");
    if (tags != NULL) {
        mjEmitTagKind(tags, "pills",  m->pillTags,  MAX_PILLS);
        mjEmitTagKind(tags, "bases",  m->baseTags,  MAX_BASES);
        mjEmitTagKind(tags, "starts", m->startTags, MAX_STARTS);
    }

    regions = mjObjectFor(root, "regions");
    if (regions != NULL) {
        for (i = 0; i < (int)m->numRegions; i++) {
            cJSON *r = mjObjectFor(regions, m->regions[i].name);
            if (r == NULL) {
                continue;
            }
            mjPutNumber(r, "x", m->regions[i].x);
            mjPutNumber(r, "y", m->regions[i].y);
            mjPutNumber(r, "w", m->regions[i].w);
            mjPutNumber(r, "h", m->regions[i].h);
        }
    }

    mjPutString(root, "script", d->script);

    brains = cJSON_CreateArray();
    if (brains != NULL) {
        for (i = 0; i < d->brainCount; i++) {
            cJSON *s = cJSON_CreateString(d->brains[i]);
            if (s != NULL) {
                cJSON_AddItemToArray(brains, s);
            }
        }
        mjPut(root, "brains", brains);
    }
}

char *scnManifestWrite(const ScnManifestDoc *d, char *err, size_t errLen) {
    cJSON *copy;
    char  *text;

    if (err != NULL && errLen > 0) {
        err[0] = '\0';
    }
    if (d == NULL) {
        mjErr(err, errLen, "scenario: there is no manifest to write");
        return NULL;
    }
    /* A copy, so writing a doc out twice gives the same text twice and the
     * doc the caller holds is not touched. */
    copy = cJSON_Duplicate(d->root, 1);
    if (copy == NULL) {
        mjErr(err, errLen, "scenario: out of memory writing the manifest");
        return NULL;
    }
    mjEmit(copy, d);
    text = cJSON_Print(copy);
    cJSON_Delete(copy);
    if (text == NULL) {
        mjErr(err, errLen, "scenario: the manifest could not be written out");
        return NULL;
    }
    return text;
}

/* ── The two forms against each other ─────────────────────────────── */

static bool mjDiffer(char *key, size_t keyLen, char *err, size_t errLen,
                     const char *which, const char *fmt, ...) {
    va_list ap;

    if (key != NULL && keyLen > 0) {
        snprintf(key, keyLen, "%s", which);
    }
    if (err != NULL && errLen > 0) {
        va_start(ap, fmt);
        vsnprintf(err, errLen, fmt, ap);
        va_end(ap);
        err[errLen - 1] = '\0';
    }
    return false;
}

/* Where in b the rule a names is set, or -1. Rules arrive from Lua in
 * whatever order lua_next walked the table in, so position means nothing. */
static int mjFindRule(const ScenarioManifest *m, uint16_t rule) {
    int i;
    for (i = 0; i < (int)m->numRules; i++) {
        if (m->rules[i].rule == rule) {
            return i;
        }
    }
    return -1;
}

/* Regions are keyed by name for the same reason. */
static int mjFindRegion(const ScenarioManifest *m, const char *name) {
    int i;
    for (i = 0; i < (int)m->numRegions; i++) {
        if (strcmp(m->regions[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Tags on one entity keep the order the file wrote them in — a single tag or
 * an ipairs array either side — so they are compared in order. */
static bool mjAgreeTagKind(const ScnManifestTags *a, const ScnManifestTags *b,
                           int maxEntity, const char *kind,
                           char *key, size_t keyLen,
                           char *err, size_t errLen) {
    int e;

    for (e = 1; e <= maxEntity; e++) {
        char where[SCN_VALIDATE_KEY_LEN];
        int  i;

        snprintf(where, sizeof(where), "tags.%s[%d]", kind, e);
        if (a[e].count != b[e].count) {
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the manifest puts %d tags here and the "
                            "script's table puts %d",
                            (int)a[e].count, (int)b[e].count);
        }
        for (i = 0; i < (int)a[e].count; i++) {
            if (strcmp(a[e].tag[i], b[e].tag[i]) != 0) {
                return mjDiffer(key, keyLen, err, errLen, where,
                                "scenario: the manifest tags this '%s' and the "
                                "script's table tags it '%s'",
                                a[e].tag[i], b[e].tag[i]);
            }
        }
    }
    return true;
}

/* The first init pair two forms of a team do not agree on, or NULL when they
 * agree. Compared by key rather than by position: a script's init table is
 * walked with lua_next, so the order its pairs land in the struct is Lua's
 * business and only the set of pairs can be held against the manifest's. A
 * key one side holds and the other does not is a disagreement about that key.
 *
 * initBadKey is not compared. It names what a reader could not take, which is
 * something to tell an author about rather than a description of the team. */
static const char *mjInitDiffers(const ScnTable *a, const ScnTable *b) {
    int i;

    for (i = 0; i < (int)a->count; i++) {
        const char *v = scnTableGet(b, a->kv[i].key);
        if (v == NULL || strcmp(v, a->kv[i].value) != 0) {
            return a->kv[i].key;
        }
    }
    for (i = 0; i < (int)b->count; i++) {
        if (scnTableGet(a, b->kv[i].key) == NULL) {
            return b->kv[i].key;
        }
    }
    return NULL;
}

bool scnManifestAgrees(const ScenarioManifest *fromJson,
                       const ScenarioManifest *fromLua,
                       char *key, size_t keyLen,
                       char *err, size_t errLen) {
    char where[SCN_VALIDATE_KEY_LEN];
    int  i;

    if (key != NULL && keyLen > 0) {
        key[0] = '\0';
    }
    if (err != NULL && errLen > 0) {
        err[0] = '\0';
    }
    if (fromJson == NULL || fromLua == NULL) {
        return mjDiffer(key, keyLen, err, errLen, "",
                        "scenario: there is nothing to compare the manifest "
                        "against");
    }

    if (strcmp(fromJson->name, fromLua->name) != 0) {
        return mjDiffer(key, keyLen, err, errLen, "name",
                        "scenario: the manifest calls this '%s' and the "
                        "script's table calls it '%s'",
                        fromJson->name, fromLua->name);
    }
    if (strcmp(fromJson->description, fromLua->description) != 0) {
        return mjDiffer(key, keyLen, err, errLen, "description",
                        "scenario: the manifest and the script's table "
                        "describe this differently");
    }
    if (fromJson->api != fromLua->api) {
        return mjDiffer(key, keyLen, err, errLen, "api",
                        "scenario: the manifest asks for api %d and the "
                        "script's table asks for %d",
                        fromJson->api, fromLua->api);
    }
    if (strcmp(fromJson->game, fromLua->game) != 0) {
        return mjDiffer(key, keyLen, err, errLen, "game",
                        "scenario: the manifest asks for game '%s' and the "
                        "script's table asks for '%s'",
                        fromJson->game, fromLua->game);
    }
    if (fromJson->bound != fromLua->bound) {
        return mjDiffer(key, keyLen, err, errLen, "bound",
                        "scenario: the manifest says bound %s and the "
                        "script's table says %s",
                        fromJson->bound ? "true" : "false",
                        fromLua->bound ? "true" : "false");
    }

    if (fromJson->lobby.maxPlayers != fromLua->lobby.maxPlayers) {
        return mjDiffer(key, keyLen, err, errLen, "lobby.max_players",
                        "scenario: the manifest seats %d players and the "
                        "script's table seats %d",
                        (int)fromJson->lobby.maxPlayers,
                        (int)fromLua->lobby.maxPlayers);
    }
    if (fromJson->lobby.extraTeams != fromLua->lobby.extraTeams) {
        return mjDiffer(key, keyLen, err, errLen, "lobby.extra_teams",
                        "scenario: the manifest and the script's table "
                        "disagree about extra teams");
    }
    if (fromJson->lobby.numTeams != fromLua->lobby.numTeams) {
        return mjDiffer(key, keyLen, err, errLen, "lobby.teams",
                        "scenario: the manifest names %d teams and the "
                        "script's table names %d",
                        (int)fromJson->lobby.numTeams,
                        (int)fromLua->lobby.numTeams);
    }
    /* lobby.teams is a Lua array, so its order is the file's own and the
     * index belongs in the key. */
    for (i = 0; i < (int)fromJson->lobby.numTeams; i++) {
        const ScnManifestTeam *a = &fromJson->lobby.teams[i];
        const ScnManifestTeam *b = &fromLua->lobby.teams[i];
        const char            *bad;

        if (a->id != b->id) {
            snprintf(where, sizeof(where), "lobby.teams[%d].id", i);
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the manifest says team %d and the "
                            "script's table says team %d",
                            (int)a->id, (int)b->id);
        }
        if (a->bots != b->bots) {
            snprintf(where, sizeof(where), "lobby.teams[%d].bots", i);
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the manifest asks for %d bots and the "
                            "script's table asks for %d",
                            (int)a->bots, (int)b->bots);
        }
        if (a->maxBots != b->maxBots) {
            snprintf(where, sizeof(where), "lobby.teams[%d].max_bots", i);
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the manifest caps this team at %d bots "
                            "and the script's table caps it at %d",
                            (int)a->maxBots, (int)b->maxBots);
        }
        if (a->fielded != b->fielded) {
            snprintf(where, sizeof(where), "lobby.teams[%d].fielded", i);
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the manifest and the script's table "
                            "disagree about whether this team is fielded");
        }
        if (strcmp(a->brain, b->brain) != 0) {
            snprintf(where, sizeof(where), "lobby.teams[%d].brain", i);
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the manifest gives this team brain '%s' "
                            "and the script's table gives it '%s'",
                            a->brain, b->brain);
        }
        bad = mjInitDiffers(&a->init, &b->init);
        if (bad != NULL) {
            snprintf(where, sizeof(where), "lobby.teams[%d].init.%s", i, bad);
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the manifest and the script's table "
                            "do not agree about this team's init '%s'", bad);
        }
    }

    if (fromJson->numRules != fromLua->numRules) {
        return mjDiffer(key, keyLen, err, errLen, "rules",
                        "scenario: the manifest sets %d rules and the "
                        "script's table sets %d",
                        (int)fromJson->numRules, (int)fromLua->numRules);
    }
    for (i = 0; i < (int)fromJson->numRules; i++) {
        int j = mjFindRule(fromLua, fromJson->rules[i].rule);
        snprintf(where, sizeof(where), "rules.%s",
                 scenarioLuaRuleName((int)fromJson->rules[i].rule));
        if (j < 0) {
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the manifest sets this rule and the "
                            "script's table does not");
        }
        if (fromJson->rules[i].value != fromLua->rules[j].value) {
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the manifest sets this rule to %g and "
                            "the script's table sets it to %g",
                            fromJson->rules[i].value, fromLua->rules[j].value);
        }
    }
    for (i = 0; i < (int)fromLua->numRules; i++) {
        if (mjFindRule(fromJson, fromLua->rules[i].rule) < 0) {
            snprintf(where, sizeof(where), "rules.%s",
                     scenarioLuaRuleName((int)fromLua->rules[i].rule));
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the script's table sets this rule and "
                            "the manifest does not");
        }
    }

    if (!mjAgreeTagKind(fromJson->pillTags, fromLua->pillTags, MAX_PILLS,
                        "pills", key, keyLen, err, errLen)) {
        return false;
    }
    if (!mjAgreeTagKind(fromJson->baseTags, fromLua->baseTags, MAX_BASES,
                        "bases", key, keyLen, err, errLen)) {
        return false;
    }
    if (!mjAgreeTagKind(fromJson->startTags, fromLua->startTags, MAX_STARTS,
                        "starts", key, keyLen, err, errLen)) {
        return false;
    }

    if (fromJson->numRegions != fromLua->numRegions) {
        return mjDiffer(key, keyLen, err, errLen, "regions",
                        "scenario: the manifest names %d regions and the "
                        "script's table names %d",
                        (int)fromJson->numRegions, (int)fromLua->numRegions);
    }
    for (i = 0; i < (int)fromJson->numRegions; i++) {
        const ScnManifestRegion *a = &fromJson->regions[i];
        const ScnManifestRegion *b;
        int                      j = mjFindRegion(fromLua, a->name);

        snprintf(where, sizeof(where), "regions.%s", a->name);
        if (j < 0) {
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the manifest names this region and the "
                            "script's table does not");
        }
        b = &fromLua->regions[j];
        if (a->x != b->x || a->y != b->y || a->w != b->w || a->h != b->h) {
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the manifest puts this region at "
                            "%d,%d %dx%d and the script's table puts it at "
                            "%d,%d %dx%d",
                            (int)a->x, (int)a->y, (int)a->w, (int)a->h,
                            (int)b->x, (int)b->y, (int)b->w, (int)b->h);
        }
    }
    for (i = 0; i < (int)fromLua->numRegions; i++) {
        if (mjFindRegion(fromJson, fromLua->regions[i].name) < 0) {
            snprintf(where, sizeof(where), "regions.%s",
                     fromLua->regions[i].name);
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the script's table names this region "
                            "and the manifest does not");
        }
    }

    return true;
}
