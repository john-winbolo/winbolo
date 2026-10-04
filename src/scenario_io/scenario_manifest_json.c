/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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
 *  api 1, bound true, fill_to_caps false, needs_bots false,
 *  kind "scenario",
 *  a team fielded
 *  unless it says otherwise — and the same soft reports through
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
 *  it is compared in order and the index is part of the key,
 *  and triggers are an array with nothing but their order to
 *  tell one from another.
 *********************************************************/

#include <float.h>   /* DBL_MAX — what tells an infinity from a big number */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "server_sim.h"   /* serverSimConsoleMessage, and the entity counts */
#include "sim_rules_names.h" /* simRulesRuleIndex, simRulesRuleName */
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

/* One number field, held to what the field it lands in can carry.
 *
 * cJSON keeps every number as a double, and casting one outside the
 * destination's range to a uint8_t or an int is undefined behaviour rather
 * than a big number — "max_players": 1e30 is not a large cap, it is whatever
 * the compiler felt like. So the range is applied here, before the cast, and
 * a value outside it is reported the way a rule outside its range is: the
 * item still decodes, held at the end of the range it went past.
 *
 * NaN and infinity are not numbers this can hold at all, so they take the
 * default rather than a bound — being told "0 used" is more use to an author
 * than being told the value was clamped to 255.
 *
 * api is the one field that does not come through here: mjDecodeApi refuses
 * rather than clamps, because that number decides whether this build
 * understands the content at all.
 *
 * where is the dotted path the issue is reported under, and may be NULL for a
 * caller with nothing to report through. */
static double mjNumberIn(const cJSON *obj, const char *key, double dflt,
                         double lo, double hi, const char *where,
                         ScnParseReport *rep) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    double       v;

    if (!cJSON_IsNumber(it)) {
        return dflt;
    }
    v = it->valuedouble;
    if (v != v) {                       /* the one value unequal to itself */
        mjReport(rep, where != NULL ? where : key,
                 "scenario: %s is not a number; %g used", key, dflt);
        return dflt;
    }
    if (v > DBL_MAX || v < -DBL_MAX) {  /* an infinity */
        mjReport(rep, where != NULL ? where : key,
                 "scenario: %s has no value this can hold; %g used", key,
                 dflt);
        return dflt;
    }
    if (v < lo) {
        mjReport(rep, where != NULL ? where : key,
                 "scenario: %s is %g, and the range is %g to %g; %g used",
                 key, v, lo, hi, lo);
        return lo;
    }
    if (v > hi) {
        mjReport(rep, where != NULL ? where : key,
                 "scenario: %s is %g, and the range is %g to %g; %g used",
                 key, v, lo, hi, hi);
        return hi;
    }
    return v;
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

bool scnManifestParseId(const char *s, uint64_t *out) {
    uint64_t    v = 0;
    const char *p;

    if (out != NULL) {
        *out = 0;
    }
    if (s == NULL || s[0] == '\0' || out == NULL) {
        return false;
    }
    for (p = s; *p != '\0'; p++) {
        unsigned digit;

        if (*p < '0' || *p > '9') {
            return false;
        }
        digit = (unsigned)(*p - '0');
        if (v > (UINT64_MAX - digit) / 10u) {
            return false;               /* more than 64 bits hold */
        }
        v = v * 10u + digit;
    }
    *out = v;
    return true;
}

/* workshop_id or workshop_author, read through scnManifestParseId. Absent,
 * and null, is 0 without a word, the way an absent kind is a scenario.
 * Anything else that does not parse is reported and read as 0 — a JSON
 * number included, because by the time cJSON has made a double of it the
 * low digits of a real id are already gone. */
static uint64_t mjDecodeId(const cJSON *root, const char *key,
                           ScnParseReport *rep) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    uint64_t     v  = 0;

    if (it == NULL || cJSON_IsNull(it)) {
        return 0;
    }
    if (cJSON_IsString(it) && scnManifestParseId(it->valuestring, &v)) {
        return v;
    }
    mjReport(rep, key,
             "scenario: %s is not a string of digits naming a Steam id; 0 "
             "used", key);
    return 0;
}

void scnManifestTakeIdentity(ScenarioManifest *m,
                             ScnIdentitySeen authorSeen,
                             const char *author, size_t authorLen,
                             ScnIdentitySeen updatedSeen,
                             const char *updated,
                             ScnParseReport *rep) {
    ScnValidateResult *sink = rep != NULL ? rep->sink : NULL;
    char               shown[SCN_UPDATED_LEN + 24];
    bool               same;

    if (m == NULL) {
        return;
    }
    m->author[0]  = '\0';
    m->updated[0] = '\0';

    if (authorSeen == scnIdentityAbsent) {
        scnWarnAdd(sink, "author",
                   "scenario: no author; the lobby shows unknown");
    } else if (authorSeen == scnIdentityNotString) {
        scnWarnAdd(sink, "author",
                   "scenario: author is not a string; unknown used");
    } else {
        same = scnIdentityCleanAuthor(m->author, sizeof(m->author), author,
                                      authorLen);
        if (m->author[0] == '\0') {
            scnWarnAdd(sink, "author",
                       "scenario: author is empty; unknown used");
        } else if (!same) {
            scnWarnAdd(sink, "author",
                       "scenario: author held control characters, broken "
                       "UTF-8, spaces at an end or more than %d bytes; '%s' "
                       "used", SCN_AUTHOR_LEN - 1, m->author);
        }
    }

    if (updatedSeen == scnIdentityAbsent) {
        scnWarnAdd(sink, "updated",
                   "scenario: no updated time; write the time the content "
                   "last changed as YYYY-MM-DDTHH:MMZ in UTC");
    } else if (updatedSeen == scnIdentityNotString) {
        scnWarnAdd(sink, "updated",
                   "scenario: updated is not a string, and an updated time "
                   "is YYYY-MM-DDTHH:MMZ in UTC; unknown used");
    } else if (scnIdentityUpdatedValid(updated)) {
        mjCopyStr(m->updated, sizeof(m->updated), updated);
    } else {
        /* Cleaned before it is quoted: the value is the file's, not ours. */
        (void)scnIdentityCleanAuthor(shown, sizeof(shown), updated,
                                     updated != NULL ? strlen(updated) : 0);
        scnWarnAdd(sink, "updated",
                   "scenario: updated is '%s', and an updated time is "
                   "YYYY-MM-DDTHH:MMZ in UTC naming a real minute; unknown "
                   "used", shown);
    }
}

/* author and updated off the root, through scnManifestTakeIdentity. A JSON
 * null is the key left out. */
static void mjDecodeIdentity(const cJSON *root, ScenarioManifest *m,
                             ScnParseReport *rep) {
    const cJSON    *a  = cJSON_GetObjectItemCaseSensitive(root, "author");
    const cJSON    *u  = cJSON_GetObjectItemCaseSensitive(root, "updated");
    ScnIdentitySeen as = scnIdentityAbsent;
    ScnIdentitySeen us = scnIdentityAbsent;

    if (cJSON_IsString(a) && a->valuestring != NULL) {
        as = scnIdentityString;
    } else if (a != NULL && !cJSON_IsNull(a)) {
        as = scnIdentityNotString;
    }
    if (cJSON_IsString(u) && u->valuestring != NULL) {
        us = scnIdentityString;
    } else if (u != NULL && !cJSON_IsNull(u)) {
        us = scnIdentityNotString;
    }
    scnManifestTakeIdentity(m, as,
                            as == scnIdentityString ? a->valuestring : NULL,
                            as == scnIdentityString ? strlen(a->valuestring)
                                                    : 0,
                            us,
                            us == scnIdentityString ? u->valuestring : NULL,
                            rep);
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
    lob->maxPlayers = (uint8_t)mjNumberIn(lobby, "max_players", 0, 0, 255,
                                          "lobby.max_players", rep);
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
        {
            char where[SCN_VALIDATE_KEY_LEN];
            snprintf(where, sizeof(where), "lobby.teams[%u]",
                     (unsigned)lob->numTeams);
            team->id      = (uint8_t)mjNumberIn(t, "id", 0, 0, 255, where, rep);
            team->bots    = (uint8_t)mjNumberIn(t, "bots", 0, 0, 255, where,
                                                rep);
            team->maxBots = (uint8_t)mjNumberIn(t, "max_bots", 0, 0, 255,
                                                where, rep);
        }
        team->fielded = mjBool(t, "fielded", true);
        mjString(t, "brain", team->brain, sizeof(team->brain));
        /* The brain mode this team's bots play in and the level inside it,
           on the same footing as the brain itself: a key the script's table
           can state, so a package that carries the table in JSON has to be
           able to carry these too. Left out is "", which leaves the seats on
           whatever mode the lobby would have given them. */
        mjString(t, "mode", team->mode, sizeof(team->mode));
        mjString(t, "difficulty", team->difficulty, sizeof(team->difficulty));
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
        idx = simRulesRuleIndex(item->string);
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
        {
            char where[SCN_VALIDATE_KEY_LEN];
            snprintf(where, sizeof(where), "regions.%s", entry->string);
            reg->x = (uint8_t)mjNumberIn(entry, "x", 0, 0, 255, where, rep);
            reg->y = (uint8_t)mjNumberIn(entry, "y", 0, 0, 255, where, rep);
            reg->w = (uint8_t)mjNumberIn(entry, "w", 0, 0, 255, where, rep);
            reg->h = (uint8_t)mjNumberIn(entry, "h", 0, 0, 255, where, rep);
        }
    }
}

/* ── Triggers ─────────────────────────────────────────────────────── */

/* The seven operators a where-row may test with, in the order
 * ScnTrigCompare declares them. Entry 0 is SCN_TRIG_CMP_UNKNOWN, which has
 * no word: it is what a name none of the seven match reads as, so giving it
 * one would let it back out as an operator. */
static const char *const mjTrigOps[] = {
    NULL, "eq", "ne", "lt", "lte", "gt", "gte", "in"
};

/* The operator's name, or NULL for SCN_TRIG_CMP_UNKNOWN and for a value
 * outside the enum. Used by the encoder, which writes back what the decode
 * stored, and by the Lua writer, which writes the same word into a script's
 * own table. Both answer an unnamed operator with "", which reads back as
 * SCN_TRIG_CMP_UNKNOWN rather than as one of the seven. */
const char *scnManifestTrigOpName(ScnTrigCompare op) {
    if ((int)op < 0 || (size_t)op >= sizeof(mjTrigOps) / sizeof(mjTrigOps[0])) {
        return NULL;
    }
    return mjTrigOps[(int)op];
}

/* An operator name as the enum, or SCN_TRIG_CMP_UNKNOWN for one this does
 * not know and for NULL. An unknown operator is not refused here: what the
 * vocabulary allows is checked against the catalogue, which this library
 * cannot see. It is kept apart from the seven so that the check has
 * something to refuse — read as eq, a misspelled operator would quietly
 * test something the author did not write.
 *
 * Public because scnReadManifest reads the same seven words out of a
 * script's table. One table of names rather than two keeps the two readers
 * from drifting apart over which word means which test. */
ScnTrigCompare scnManifestTrigOpFrom(const char *name) {
    size_t i;

    if (name == NULL) {
        return SCN_TRIG_CMP_UNKNOWN;
    }
    for (i = 1; i < sizeof(mjTrigOps) / sizeof(mjTrigOps[0]); i++) {
        if (strcmp(name, mjTrigOps[i]) == 0) {
            return (ScnTrigCompare)i;
        }
    }
    return SCN_TRIG_CMP_UNKNOWN;
}

/* The word for each kind, in enum order, so a kind indexes its own name.
 * scnKindUnknown has no row: it is what a word neither of these matches
 * reads as, and a name for it would let it back out as a kind. */
static const char *const mjKinds[] = {
    "scenario", "mod"
};

const char *scnManifestKindName(ScnManifestKind kind) {
    if ((int)kind < 0 ||
        (size_t)kind >= sizeof(mjKinds) / sizeof(mjKinds[0])) {
        /* Including scnKindUnknown, which no manifest holds. Written as the
           word an absent key reads as, so nothing a file could be left in
           comes back out as a kind nobody wrote. */
        return mjKinds[scnKindScenario];
    }
    return mjKinds[(int)kind];
}

ScnManifestKind scnManifestKindFrom(const char *name) {
    size_t i;

    if (name == NULL) {
        return scnKindUnknown;
    }
    for (i = 0; i < sizeof(mjKinds) / sizeof(mjKinds[0]); i++) {
        if (strcmp(name, mjKinds[i]) == 0) {
            return (ScnManifestKind)i;
        }
    }
    return scnKindUnknown;
}

/* One value or argument out of the tree.
 *
 * The four kinds a file may state: a number, a string, a boolean, and the
 * object {"field": "<name>"} naming a field of the hook's payload. Anything
 * else — a null, a bare array, an object without a field key — is stored as
 * NONE and reported, so a row that holds one still decodes and the author is
 * told which slot did not read.
 *
 * text is where a string goes when it fits. One that does not goes on the
 * action's own text, which is why act is passed: a condition passes NULL and
 * its oversized string is truncated the way every other name in the manifest
 * is. Only the first oversized string in an action has somewhere to live; a
 * second is reported and left NONE. */
static void mjTrigValue(const cJSON *v, ScnTrigValue *out, ScnTrigAct *act,
                        const char *where, ScnParseReport *rep) {
    memset(out, 0, sizeof(*out));

    if (cJSON_IsNumber(v)) {
        out->kind = SCN_TRIG_VAL_NUMBER;
        out->num  = v->valuedouble;
        return;
    }
    if (cJSON_IsBool(v)) {
        out->kind = SCN_TRIG_VAL_BOOL;
        out->num  = cJSON_IsTrue(v) ? 1.0 : 0.0;
        return;
    }
    if (cJSON_IsString(v) && v->valuestring != NULL) {
        out->kind = SCN_TRIG_VAL_STRING;
        if (strlen(v->valuestring) < SCN_TRIGGER_NAME_LEN) {
            mjCopyStr(out->text, sizeof(out->text), v->valuestring);
            return;
        }
        if (act == NULL) {
            mjCopyStr(out->text, sizeof(out->text), v->valuestring);
            mjReport(rep, where,
                     "scenario: %s is longer than %d bytes and is cut to "
                     "fit", where, SCN_TRIGGER_NAME_LEN - 1);
            return;
        }
        if (act->text[0] != '\0') {
            out->kind = SCN_TRIG_VAL_NONE;
            mjReport(rep, where,
                     "scenario: %s is the second long string on one action, "
                     "which carries one; dropped", where);
            return;
        }
        out->inText = true;
        /* The same fault as above at the other size: the action's text is
           wider than an argument slot, and a line past that is cut too. */
        if (strlen(v->valuestring) >= SCN_TRIGGER_TEXT_LEN) {
            mjReport(rep, where,
                     "scenario: %s is longer than %d bytes and is cut to "
                     "fit", where, SCN_TRIGGER_TEXT_LEN - 1);
        }
        mjCopyStr(act->text, sizeof(act->text), v->valuestring);
        return;
    }
    if (cJSON_IsObject(v)) {
        const cJSON *f = cJSON_GetObjectItemCaseSensitive(v, "field");
        if (cJSON_IsString(f) && f->valuestring != NULL) {
            out->kind = SCN_TRIG_VAL_FIELD;
            mjCopyStr(out->text, sizeof(out->text), f->valuestring);
            return;
        }
    }
    mjReport(rep, where,
             "scenario: %s is not a number, a string, a boolean or a field "
             "reference; dropped", where);
}

/* One where-row: [field, operator, value]. A row that is not an array of
 * three is reported and dropped, since there is no part of it to keep. */
static void mjTrigCond(const cJSON *row, ScnTrigCond *out,
                       const char *where, ScnParseReport *rep) {
    const cJSON *field;
    const cJSON *op;

    memset(out, 0, sizeof(*out));

    if (!cJSON_IsArray(row) || cJSON_GetArraySize(row) != 3) {
        mjReport(rep, where,
                 "scenario: %s is not a [field, operator, value] row", where);
        return;
    }
    field = cJSON_GetArrayItem(row, 0);
    op    = cJSON_GetArrayItem(row, 1);

    /* A name that is there but empty goes the same way as a slot holding no
       string at all: the editor writes one into a row it has just made room
       for, and a row naming nothing tests nothing. scnCheckTrigCond leaves
       both to this report. */
    if (cJSON_IsString(field) && field->valuestring != NULL &&
        field->valuestring[0] != '\0') {
        mjCopyStr(out->field, sizeof(out->field), field->valuestring);
    } else {
        mjReport(rep, where, "scenario: %s names no field", where);
    }
    if (cJSON_IsString(op) && op->valuestring != NULL) {
        out->op = scnManifestTrigOpFrom(op->valuestring);
    } else {
        mjReport(rep, where, "scenario: %s names no operator", where);
    }
    mjTrigValue(cJSON_GetArrayItem(row, 2), &out->value, NULL, where, rep);
}

/* One actions-row: [op, arg...]. The op is the first entry and the rest are
 * its positional arguments, in the order the file wrote them. */
static void mjTrigAct(const cJSON *row, ScnTrigAct *out, const char *where,
                      ScnParseReport *rep) {
    const cJSON *op;
    const cJSON *arg;
    int          i = 0;

    memset(out, 0, sizeof(*out));

    if (!cJSON_IsArray(row) || cJSON_GetArraySize(row) < 1) {
        mjReport(rep, where, "scenario: %s is not an [op, argument...] row",
                 where);
        return;
    }
    op = cJSON_GetArrayItem(row, 0);
    /* A name that is there but empty goes the same way as a slot holding no
       string at all: the editor writes one into an action it has just made
       room for, and an action naming nothing does nothing. scnCheckTrigAct
       leaves both to this report. */
    if (cJSON_IsString(op) && op->valuestring != NULL &&
        op->valuestring[0] != '\0') {
        mjCopyStr(out->op, sizeof(out->op), op->valuestring);
    } else {
        mjReport(rep, where, "scenario: %s names no op", where);
    }

    cJSON_ArrayForEach(arg, row) {
        char slot[SCN_VALIDATE_KEY_LEN];
        if (i++ == 0) {
            continue;                   /* the op, already taken */
        }
        if (out->numArgs >= SCN_TRIGGER_ARGS_MAX) {
            mjReport(rep, where,
                     "scenario: %s takes more than %d arguments; the rest "
                     "dropped", where, SCN_TRIGGER_ARGS_MAX);
            break;
        }
        snprintf(slot, sizeof(slot), "%s[%d]", where, (int)out->numArgs);
        mjTrigValue(arg, &out->args[out->numArgs], out, slot, rep);
        out->numArgs++;
    }
}

/* The triggers array into the struct.
 *
 * A trigger has no name to key a report on, so its position in the array is
 * the key: triggers[7], and triggers[7].where[2] for a row inside it. The
 * position counted is the one in the file, so a report names the row the
 * author wrote rather than the slot it landed in after an earlier drop.
 *
 * Every cap here drops what is past it and says so. Nothing is a refusal:
 * a manifest with more triggers than this build holds is still a manifest,
 * the way one with more regions is.
 *
 * A trigger whose where is there but is not an array goes the same way. A
 * trigger with no tests runs on every occurrence of its hook, so keeping
 * one whose tests could not be read would turn what the file states as a
 * conditional into an unconditional one. A row inside the array is the
 * other case: a row that cannot be read is kept zeroed, which holds on
 * nothing, so the tests the file does state still stand and the bad row is
 * inert beside them.
 *
 * A trigger naming no hook is dropped too, and for a plainer reason: there
 * is no hook to put it on, so it never runs however it is kept, and a slot
 * held by one is a slot the next trigger cannot have.
 *
 * scnReadTriggers reads the same shape out of a Lua table and drops on the
 * same terms, which is what lets scnManifestAgrees hold a package's two
 * forms against each other. */
static void mjDecodeTriggers(const cJSON *root, ScenarioManifest *m,
                             ScnParseReport *rep) {
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "triggers");
    const cJSON *entry;
    int          at = 0;

    if (arr == NULL) {
        return;
    }
    if (!cJSON_IsArray(arr)) {
        mjReport(rep, "triggers", "scenario: triggers is not an array");
        return;
    }
    cJSON_ArrayForEach(entry, arr) {
        char         where[32];      /* "triggers[%d]", and a row key
                                      * built on it always fits its own */
        ScnTrigger  *trig;
        const cJSON *when;
        const cJSON *list;
        const cJSON *row;
        int          n = at++;

        snprintf(where, sizeof(where), "triggers[%d]", n);
        if (!cJSON_IsObject(entry)) {
            mjReport(rep, where, "scenario: %s is not a trigger object",
                     where);
            continue;
        }
        /* Said once, under the first row past the cap, and the walk stops,
           the way lobby.teams stops above: a file decides how long this
           array is, and a line per excess row would fill the issue list
           with the one fact and hide every fault after it. The tests and
           actions below stop the same way. */
        if (m->numTriggers >= SCN_TRIGGERS_MAX) {
            mjReport(rep, where,
                     "scenario: more than %d triggers; the rest dropped",
                     SCN_TRIGGERS_MAX);
            break;
        }
        trig = &m->triggers[m->numTriggers];
        m->numTriggers++;
        memset(trig, 0, sizeof(*trig));

        /* A when that is there but empty names no hook the way a missing one
           does, and neither leaves anything to run on: the router never
           reaches the trigger, so keeping it spends a slot and rides through
           a pack and unpack as an entry that does nothing. It goes the way a
           bad where goes, and the issue is filed under the position. */
        when = cJSON_GetObjectItemCaseSensitive(entry, "when");
        if (cJSON_IsString(when) && when->valuestring != NULL &&
            when->valuestring[0] != '\0') {
            mjCopyStr(trig->when, sizeof(trig->when), when->valuestring);
        } else {
            mjReport(rep, where,
                     "scenario: %s names no hook to run on; %s dropped",
                     where, where);
            m->numTriggers--;           /* the slot claimed above, given back */
            continue;
        }

        list = cJSON_GetObjectItemCaseSensitive(entry, "where");
        if (list != NULL && !cJSON_IsArray(list)) {
            mjReport(rep, where,
                     "scenario: %s's where is not an array; %s dropped",
                     where, where);
            m->numTriggers--;           /* the slot claimed above, given back */
            continue;
        }
        if (list != NULL) {
            int w = 0;
            cJSON_ArrayForEach(row, list) {
                char slot[SCN_VALIDATE_KEY_LEN];
                snprintf(slot, sizeof(slot), "%s.where[%d]", where, w++);
                if (trig->numWhere >= SCN_TRIGGER_CONDS_MAX) {
                    mjReport(rep, slot,
                             "scenario: more than %d tests on one trigger; "
                             "the rest dropped", SCN_TRIGGER_CONDS_MAX);
                    break;
                }
                mjTrigCond(row, &trig->where[trig->numWhere], slot, rep);
                trig->numWhere++;
            }
        }

        list = cJSON_GetObjectItemCaseSensitive(entry, "actions");
        if (list != NULL && !cJSON_IsArray(list)) {
            mjReport(rep, where, "scenario: %s's actions is not an array",
                     where);
        } else if (list != NULL) {
            int a = 0;
            cJSON_ArrayForEach(row, list) {
                char slot[SCN_VALIDATE_KEY_LEN];
                snprintf(slot, sizeof(slot), "%s.actions[%d]", where, a++);
                if (trig->numActions >= SCN_TRIGGER_ACTIONS_MAX) {
                    mjReport(rep, slot,
                             "scenario: more than %d actions on one trigger; "
                             "the rest dropped", SCN_TRIGGER_ACTIONS_MAX);
                    break;
                }
                mjTrigAct(row, &trig->actions[trig->numActions], slot, rep);
                trig->numActions++;
            }
        }
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

/* Which kind of file the package says this is, and the twin of scnReadKind
 * in scenario_host.c: the same two words, the same default and the same
 * report for anything else.
 *
 * Absent reads as a scenario. A manifest written before this key existed
 * meant a file that ends its own round, because that is what every file
 * could do then, and reading it any other way would take that away from it
 * silently.
 *
 * A key that is there and is neither word is reported and read as a scenario
 * too. That is the side to fail towards: the mod kind is the one things are
 * held back from, so a word nobody could read leaves the file able to do what
 * it always could rather than quietly stripping it. */
static ScnManifestKind mjDecodeKind(const cJSON *root, ScnParseReport *rep) {
    const cJSON    *it = cJSON_GetObjectItemCaseSensitive(root, "kind");
    ScnManifestKind kind;

    if (it == NULL || cJSON_IsNull(it)) {
        return scnKindScenario;
    }
    if (!cJSON_IsString(it) || it->valuestring == NULL) {
        mjReport(rep, "kind",
                 "scenario: kind is not a word, and a kind is \"scenario\" "
                 "or \"mod\"");
        return scnKindScenario;
    }
    kind = scnManifestKindFrom(it->valuestring);
    if (kind == scnKindUnknown) {
        mjReport(rep, "kind",
                 "scenario: kind is '%s', and a kind is \"scenario\" or "
                 "\"mod\"", it->valuestring);
        return scnKindScenario;
    }
    return kind;
}

/* The api version the scenario was written against.
 *
 * Every other number in the manifest carries something a round can go on
 * without, so one out of range is held at the end of its range and reported.
 * This one is what decides whether this build understands the content at
 * all — the attach turns a scenario down when its api is newer than the
 * server's — so a value that cannot be read is a manifest that should not be
 * run rather than one to guess at. A negative api guessed at as 0 would pass
 * every server's check and run content written against nothing.
 *
 * Left out, the api is 1: version 1 of the schema is what this build reads,
 * and a manifest that names none was written for it.
 *
 * Answers false with err set, naming the field and the value as it was
 * read. */
static bool mjDecodeApi(const cJSON *root, int *out, char *err, size_t errLen) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, "api");
    double       v;

    *out = 1;
    if (!cJSON_IsNumber(it)) {
        return true;
    }
    v = it->valuedouble;
    /* NaN prints as nan and an infinity as inf, so one sentence names every
       value this turns down as the reader saw it. */
    if (v != v || v > DBL_MAX || v < -DBL_MAX ||
        v < 0.0 || v > 2147483647.0) {
        mjErr(err, errLen,
              "scenario: api is %g, and an api is a whole number from 0 up",
              v);
        return false;
    }
    *out = (int)v;
    return true;
}

/* The callbacks object into the struct, in the order the file writes it.
 *
 * Only the Lua reader can tell which callbacks a script defines, so this
 * keeps every string row that fits and leaves the check against the script
 * to the load, which reads the table back through scnReadManifest. A package
 * -pack wrote holds the rows that read already kept, so the two agree.
 *
 * A row that does not fit is dropped with a line to the operator and no
 * issue: the block describes, it does not decide anything, and a validator
 * issue would refuse a package over a sentence. */
static void mjDecodeCallbacks(const cJSON *root, ScenarioManifest *m) {
    const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "callbacks");
    const cJSON *it;
    char         line[MJ_LINE_LEN];
    size_t       used = 0;
    uint8_t      t;

    if (obj == NULL) {
        return;
    }
    if (!cJSON_IsObject(obj)) {
        serverSimConsoleMessage("scenario: callbacks is not an object of "
                                "callback: \"what it does\"");
        return;
    }
    cJSON_ArrayForEach(it, obj) {
        ScnManifestCallback *row;
        size_t               nameLen;
        size_t               n;
        size_t               at;

        if (it->string == NULL || !cJSON_IsString(it) ||
            it->valuestring == NULL || it->valuestring[0] == '\0') {
            continue;
        }
        nameLen = strlen(it->string);
        n  = scnCallbacksTextFit(it->valuestring, strlen(it->valuestring));
        at = (used == 0) ? 1u : used;
        if (nameLen == 0 || nameLen >= SCN_CALLBACK_NAME_LEN ||
            m->numCallbacks >= SCN_CALLBACKS_MAX ||
            at + scnCallbacksRowCost(nameLen, n) > SCN_CALLBACKS_BLOB_MAX) {
            snprintf(line, sizeof(line),
                     "scenario: callbacks.%.40s does not fit; dropped",
                     it->string);
            serverSimConsoleMessage(line);
            continue;
        }
        row = &m->callbacks[m->numCallbacks];
        m->numCallbacks++;
        used = at + scnCallbacksRowCost(nameLen, n);
        memcpy(row->name, it->string, nameLen);
        row->name[nameLen] = '\0';
        memcpy(row->text, it->valuestring, n);
        row->text[n] = '\0';
        row->byTrigger = false;
        for (t = 0; t < m->numTriggers && !row->byTrigger; t++) {
            row->byTrigger = strcmp(m->triggers[t].when, row->name) == 0;
        }
    }
}

/* A field of one settings row as a whole number inside an int32. False for
 * a field that is there and is not one; absent answers true with *had false. */
static bool mjSettingInt(const cJSON *row, const char *field, int32_t *out,
                         bool *had) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(row, field);
    double       v;

    *had = false;
    if (it == NULL) {
        return true;
    }
    if (!cJSON_IsNumber(it)) {
        return false;
    }
    v = it->valuedouble;
    if (v != v || v < -2147483648.0 || v > 2147483647.0 ||
        v != (double)(int64_t)v) {
        return false;
    }
    *out = (int32_t)(int64_t)v;
    *had = true;
    return true;
}

/* A string field of one settings row into dst. False for a field that is
 * there and is not a string, or does not fit. */
static bool mjSettingStr(const cJSON *row, const char *field, char *dst,
                         size_t dstLen) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(row, field);
    size_t       n;

    dst[0] = '\0';
    if (it == NULL) {
        return true;
    }
    if (!cJSON_IsString(it) || it->valuestring == NULL) {
        return false;
    }
    n = strlen(it->valuestring);
    if (n >= dstLen) {
        return false;
    }
    memcpy(dst, it->valuestring, n + 1);
    return true;
}

/* A choice row's "choices" and "default" into st, on the terms
 * scnSettingsChoices reads a Lua row on. NULL when they read, else the
 * reason for the report. */
static const char *mjSettingChoices(const cJSON *row, ScnSetting *st) {
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(row, "choices");
    const cJSON *def = cJSON_GetObjectItemCaseSensitive(row, "default");
    const cJSON *w;
    int          total;
    int          at;
    int          i = 0;

    if (!cJSON_IsArray(arr)) {
        return "needs a list of words for choices";
    }
    total = cJSON_GetArraySize(arr);
    if (total < SCN_SETTING_CHOICES_MIN ||
        total > SCN_SETTING_CHOICES_WORDS_MAX) {
        return "a choice setting needs 2 to 8 choices";
    }
    cJSON_ArrayForEach(w, arr) {
        size_t n;

        if (!cJSON_IsString(w) || w->valuestring == NULL) {
            return "every choice must be a string";
        }
        n = strlen(w->valuestring);
        if (n == 0 || n >= SCN_SETTING_CHOICE_LEN) {
            return "each choice must be 1 to 31 characters";
        }
        memcpy(st->choices[i++], w->valuestring, n + 1);
    }
    st->numChoices = (uint8_t)total;
    st->min        = 0;
    st->max        = total - 1;
    st->step       = 1;
    if (!cJSON_IsString(def) || def->valuestring == NULL) {
        return "needs one of its choices, as a string, for default";
    }
    at = scnSettingChoiceIndex(st, def->valuestring);
    if (at < 0) {
        return "default is not one of its choices";
    }
    st->def = at;
    return NULL;
}

/* The settings array into the struct, on the terms scenarioLuaReadSettings
 * reads the Lua table on: the same fields, the same defaults (type "int",
 * step 1), a bool row with a true or false default and no min, max or step,
 * a choice row with a list of words and one of them as its default,
 * the same scnSettingProblem check, duplicates and rows past
 * SCN_SETTINGS_MAX dropped. A dropped row is an issue, because the host
 * would otherwise be offered a dropdown the author did not mean. */
static void mjDecodeSettings(const cJSON *root, ScenarioManifest *m,
                             ScnParseReport *rep) {
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "settings");
    const cJSON *row;
    int          at = 0;

    if (arr == NULL) {
        return;
    }
    if (!cJSON_IsArray(arr)) {
        mjReport(rep, "settings",
                 "scenario: settings is not a list of settings");
        return;
    }
    cJSON_ArrayForEach(row, arr) {
        ScnSetting  st;
        char        key[48];
        char        type[16];
        bool        hadMin;
        bool        hadMax;
        bool        hadStep;
        bool        hadDef;
        const char *why;

        at++;
        snprintf(key, sizeof(key), "settings[%d]", at);
        if (!cJSON_IsObject(row)) {
            mjReport(rep, key, "scenario: %s is not an object; dropped", key);
            continue;
        }
        memset(&st, 0, sizeof(st));
        st.step = 1;
        if (!mjSettingStr(row, "id", st.id, sizeof(st.id)) ||
            st.id[0] == '\0') {
            mjReport(rep, key,
                     "scenario: %s has no id of up to %d characters; "
                     "dropped", key, SCN_SETTING_ID_LEN - 1);
            continue;
        }
        snprintf(key, sizeof(key), "settings.%s", st.id);
        if (!mjSettingStr(row, "label", st.label, sizeof(st.label)) ||
            st.label[0] == '\0') {
            mjReport(rep, key,
                     "scenario: %s has no label of up to %d characters; "
                     "dropped", key, SCN_SETTING_LABEL_LEN - 1);
            continue;
        }
        if (!mjSettingStr(row, "type", type, sizeof(type))) {
            snprintf(type, sizeof(type), "?");
        }
        st.type = SCN_SETTING_TYPE_INT;
        if (strcmp(type, "choice") == 0) {
            const char *bad;

            st.type = SCN_SETTING_TYPE_CHOICE;
            if (cJSON_GetObjectItemCaseSensitive(row, "min") != NULL ||
                cJSON_GetObjectItemCaseSensitive(row, "max") != NULL ||
                cJSON_GetObjectItemCaseSensitive(row, "step") != NULL) {
                bad = "is a choice setting and takes no min, max or step";
            } else {
                bad = mjSettingChoices(row, &st);
            }
            if (bad != NULL) {
                mjReport(rep, key, "scenario: %s %s; dropped", key, bad);
                continue;
            }
        } else if (strcmp(type, "bool") == 0) {
            const cJSON *def =
                cJSON_GetObjectItemCaseSensitive(row, "default");

            st.type = SCN_SETTING_TYPE_BOOL;
            if (cJSON_GetObjectItemCaseSensitive(row, "min") != NULL ||
                cJSON_GetObjectItemCaseSensitive(row, "max") != NULL ||
                cJSON_GetObjectItemCaseSensitive(row, "step") != NULL) {
                mjReport(rep, key,
                         "scenario: %s is a bool setting and takes no min, "
                         "max or step; dropped", key);
                continue;
            }
            if (!cJSON_IsBool(def)) {
                mjReport(rep, key,
                         "scenario: %s needs true or false for default; "
                         "dropped", key);
                continue;
            }
            st.min  = 0;
            st.max  = 1;
            st.step = 1;
            st.def  = cJSON_IsTrue(def) ? 1 : 0;
        } else if (type[0] != '\0' && strcmp(type, "int") != 0) {
            mjReport(rep, key,
                     "scenario: %s has type '%s'; only \"int\", \"bool\" "
                     "and \"choice\" are supported; dropped", key, type);
            continue;
        } else if (!mjSettingInt(row, "min", &st.min, &hadMin) ||
                   !mjSettingInt(row, "max", &st.max, &hadMax) ||
                   !mjSettingInt(row, "step", &st.step, &hadStep) ||
                   !mjSettingInt(row, "default", &st.def, &hadDef) ||
                   !hadMin || !hadMax || !hadDef) {
            mjReport(rep, key,
                     "scenario: %s needs whole numbers for min, max and "
                     "default (and step, if given); dropped", key);
            continue;
        }
        why = scnSettingProblem(&st);
        if (why != NULL) {
            mjReport(rep, key, "scenario: %s: %s; dropped", key, why);
            continue;
        }
        if (scnSettingFind(m->settings, (int)m->numSettings, st.id) != NULL) {
            mjReport(rep, key,
                     "scenario: %s is declared twice; the second is "
                     "dropped", key);
            continue;
        }
        if (m->numSettings >= SCN_SETTINGS_MAX) {
            mjReport(rep, key,
                     "scenario: more than %d settings; %s dropped",
                     SCN_SETTINGS_MAX, key);
            continue;
        }
        m->settings[m->numSettings++] = st;
    }
}

/* Everything the schema names, out of the tree and into the struct.
 *
 * triggers is read here and written back from the struct, so a key inside a
 * trigger that this build does not know is not kept the way an unknown key
 * elsewhere in the tree is.
 *
 * False for a manifest that cannot be decoded at all, which today is an api
 * this build cannot read. Everything else reports and carries on. */
static bool mjDecode(ScnManifestDoc *d, ScnParseReport *rep,
                     char *err, size_t errLen) {
    ScenarioManifest *m = &d->values;

    mjCopyStr(d->script, sizeof(d->script), SCN_MANIFEST_SCRIPT_DEFAULT);
    mjString(d->root, "script", d->script, sizeof(d->script));
    if (d->script[0] == '\0') {
        mjCopyStr(d->script, sizeof(d->script), SCN_MANIFEST_SCRIPT_DEFAULT);
    }

    mjString(d->root, "name", m->name, sizeof(m->name));
    mjString(d->root, "description", m->description, sizeof(m->description));
    m->kind = mjDecodeKind(d->root, rep);
    mjString(d->root, "game", m->game, sizeof(m->game));
    if (!mjDecodeApi(d->root, &m->api, err, errLen)) {
        return false;
    }
    m->bound      = mjBool(d->root, "bound", true);
    m->fillToCaps = mjBool(d->root, "fill_to_caps", false);
    m->needsBots  = mjBool(d->root, "needs_bots", false);
    m->workshopId     = mjDecodeId(d->root, "workshop_id", rep);
    m->workshopAuthor = mjDecodeId(d->root, "workshop_author", rep);
    mjDecodeIdentity(d->root, m, rep);

    mjDecodeLobby(d->root, &m->lobby, rep);
    mjDecodeRules(d->root, m, rep);
    mjDecodeTags(d->root, m, rep);
    mjDecodeRegions(d->root, m, rep);
    mjDecodeTriggers(d->root, m, rep);
    mjDecodeCallbacks(d->root, m);
    mjDecodeSettings(d->root, m, rep);
    mjDecodeBrains(d->root, d, rep);
    return true;
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
    if (!mjDecode(d, rep, err, errLen)) {
        scnManifestFree(d);
        return NULL;
    }
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

/* true, or no key at all for false. For a key added after manifests were
 * already written: a manifest that does not ask for it is the text it was
 * before the key existed, and turning it off takes the key away. */
static void mjPutTrue(cJSON *obj, const char *key, bool v) {
    if (!v) {
        cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
        return;
    }
    mjPutBool(obj, key, true);
}

/* An id as its decimal digits, or no key at all for 0. Left out rather than
 * written as "0", so a manifest that names no item is the text it was before
 * the key existed, and setting an id back to 0 takes the key away. */
static void mjPutId(cJSON *obj, const char *key, uint64_t v) {
    char digits[24];

    if (v == 0) {
        cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
        return;
    }
    snprintf(digits, sizeof(digits), "%llu", (unsigned long long)v);
    mjPutString(obj, key, digits);
}

/* A string, or no key at all for "": author and updated left unstated are
 * left out the way an id of 0 is. */
static void mjPutStated(cJSON *obj, const char *key, const char *v) {
    if (v == NULL || v[0] == '\0') {
        cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
        return;
    }
    mjPutString(obj, key, v);
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
        mjPutString(t, "mode", lob->teams[i].mode);
        mjPutString(t, "difficulty", lob->teams[i].difficulty);
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

/* One value or argument back out, in the kind it was read as. act is the
 * action the value belongs to, for a string held on its text, and is NULL
 * for a condition's value. NONE is a slot that did not read, and goes out as
 * a null so the row keeps the length the file gave it. */
static cJSON *mjTrigValueOut(const ScnTrigValue *v, const ScnTrigAct *act) {
    switch (v->kind) {
    case SCN_TRIG_VAL_NUMBER:
        return cJSON_CreateNumber(v->num);
    case SCN_TRIG_VAL_BOOL:
        return cJSON_CreateBool(v->num != 0.0 ? 1 : 0);
    case SCN_TRIG_VAL_STRING:
        if (v->inText && act != NULL) {
            return cJSON_CreateString(act->text);
        }
        return cJSON_CreateString(v->text);
    case SCN_TRIG_VAL_FIELD: {
        cJSON *obj = cJSON_CreateObject();
        if (obj == NULL) {
            return NULL;
        }
        cJSON_AddStringToObject(obj, "field", v->text);
        return obj;
    }
    case SCN_TRIG_VAL_NONE:
    default:
        return cJSON_CreateNull();
    }
}

/* The triggers array built fresh from the struct. Unlike the rest of the
 * emit, nothing of what was parsed is kept: a trigger is written whole or
 * not at all, so the array the file gets is the array the struct holds.
 *
 * The key goes out whether or not there are any, the way brains and regions
 * do, so a manifest that stated an empty array still has one after the
 * trip. */
static void mjEmitTriggers(cJSON *root, const ScenarioManifest *m) {
    cJSON *arr;
    int    i, j, k;

    arr = cJSON_CreateArray();
    if (arr == NULL) {
        return;
    }
    for (i = 0; i < (int)m->numTriggers; i++) {
        const ScnTrigger *t = &m->triggers[i];
        cJSON            *obj = cJSON_CreateObject();
        cJSON            *where;
        cJSON            *actions;

        if (obj == NULL) {
            break;
        }
        cJSON_AddItemToArray(arr, obj);
        cJSON_AddStringToObject(obj, "when", t->when);

        where = cJSON_AddArrayToObject(obj, "where");
        for (j = 0; where != NULL && j < (int)t->numWhere; j++) {
            const ScnTrigCond *c    = &t->where[j];
            const char        *name = scnManifestTrigOpName(c->op);
            cJSON             *row  = cJSON_CreateArray();

            if (row == NULL) {
                break;
            }
            cJSON_AddItemToArray(where, row);
            cJSON_AddItemToArray(row, cJSON_CreateString(c->field));
            /* An operator the table cannot name goes out as "", which is
               the one spelling that comes back as SCN_TRIG_CMP_UNKNOWN: it
               is a string, so the decode does not take the row for one that
               names no operator, and it matches none of the seven. A row
               the author got wrong is still wrong after a trip through a
               file rather than having settled into eq on the way. */
            cJSON_AddItemToArray(row,
                                 cJSON_CreateString(name != NULL ? name : ""));
            cJSON_AddItemToArray(row, mjTrigValueOut(&c->value, NULL));
        }

        actions = cJSON_AddArrayToObject(obj, "actions");
        for (j = 0; actions != NULL && j < (int)t->numActions; j++) {
            const ScnTrigAct *a   = &t->actions[j];
            cJSON            *row = cJSON_CreateArray();

            if (row == NULL) {
                break;
            }
            cJSON_AddItemToArray(actions, row);
            cJSON_AddItemToArray(row, cJSON_CreateString(a->op));
            for (k = 0; k < (int)a->numArgs; k++) {
                cJSON_AddItemToArray(row, mjTrigValueOut(&a->args[k], a));
            }
        }
    }
    mjPut(root, "triggers", arr);
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
    mjPutString(root, "kind", scnManifestKindName(m->kind));
    mjPutString(root, "game", m->game);
    mjPutBool(root, "bound", m->bound);
    mjPutBool(root, "fill_to_caps", m->fillToCaps);
    mjPutTrue(root, "needs_bots", m->needsBots);
    mjPutId(root, "workshop_id", m->workshopId);
    mjPutId(root, "workshop_author", m->workshopAuthor);
    mjPutStated(root, "author", m->author);
    mjPutStated(root, "updated", m->updated);

    lobby = mjObjectFor(root, "lobby");
    if (lobby != NULL) {
        mjPutNumber(lobby, "max_players", m->lobby.maxPlayers);
        mjPutBool(lobby, "extra_teams", m->lobby.extraTeams);
        mjEmitTeams(lobby, &m->lobby);
    }

    rules = mjObjectFor(root, "rules");
    if (rules != NULL) {
        for (i = 0; i < (int)m->numRules; i++) {
            mjPutNumber(rules, simRulesRuleName((int)m->rules[i].rule),
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

    mjEmitTriggers(root, m);

    /* Written fresh from the struct, and left out when it is empty: a script
       with no block gets a manifest with no key rather than an empty one. */
    cJSON_DeleteItemFromObjectCaseSensitive(root, "callbacks");
    if (m->numCallbacks > 0) {
        cJSON *cb = cJSON_AddObjectToObject(root, "callbacks");

        for (i = 0; cb != NULL && i < (int)m->numCallbacks &&
                    i < SCN_CALLBACKS_MAX;
             i++) {
            cJSON_AddStringToObject(cb, m->callbacks[i].name,
                                    m->callbacks[i].text);
        }
    }

    /* Written fresh from the struct like the callbacks, and left out when
       the script declares none. */
    cJSON_DeleteItemFromObjectCaseSensitive(root, "settings");
    if (m->numSettings > 0) {
        cJSON *arr = cJSON_AddArrayToObject(root, "settings");

        for (i = 0; arr != NULL && i < (int)m->numSettings &&
                    i < SCN_SETTINGS_MAX;
             i++) {
            const ScnSetting *st  = &m->settings[i];
            cJSON            *row = cJSON_CreateObject();

            if (row == NULL) {
                break;
            }
            mjPutString(row, "id", st->id);
            mjPutString(row, "label", st->label);
            if (st->type == SCN_SETTING_TYPE_BOOL) {
                mjPutString(row, "type", "bool");
                mjPutBool(row, "default", st->def != 0);
            } else if (st->type == SCN_SETTING_TYPE_CHOICE) {
                cJSON      *words = cJSON_CreateArray();
                const char *dw    = scnSettingChoiceText(st, st->def);
                int         w;

                mjPutString(row, "type", "choice");
                if (words != NULL) {
                    for (w = 0; w < (int)st->numChoices; w++) {
                        cJSON_AddItemToArray(
                            words, cJSON_CreateString(st->choices[w]));
                    }
                    cJSON_AddItemToObject(row, "choices", words);
                }
                mjPutString(row, "default", dw != NULL ? dw : "");
            } else {
                mjPutString(row, "type", "int");
                mjPutNumber(row, "min", (double)st->min);
                mjPutNumber(row, "max", (double)st->max);
                mjPutNumber(row, "step", (double)st->step);
                mjPutNumber(row, "default", (double)st->def);
            }
            cJSON_AddItemToArray(arr, row);
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

void scnManifestSetWorkshop(ScnManifestDoc *d, uint64_t id, uint64_t author) {
    if (d == NULL) {
        return;
    }
    d->values.workshopId     = id;
    d->values.workshopAuthor = author;
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

/* Two values, by the kind they were read as and then by the part that kind
 * uses: num for a number and for a boolean, text for a string and for a
 * field reference, and inText for a string whose bytes went to the action's
 * own text — that text is compared with the action it belongs to, and both
 * slots hold "" while the flag is set.
 *
 * A ScnTrigValue is not memcmp'd. It carries padding between kind and num,
 * and a difference there is not a disagreement an author could act on. */
static bool mjTrigValueSame(const ScnTrigValue *a, const ScnTrigValue *b) {
    if (a->kind != b->kind || a->inText != b->inText) {
        return false;
    }
    switch (a->kind) {
    case SCN_TRIG_VAL_NUMBER:
    case SCN_TRIG_VAL_BOOL:
        return a->num == b->num;
    case SCN_TRIG_VAL_STRING:
    case SCN_TRIG_VAL_FIELD:
        return strcmp(a->text, b->text) == 0;
    case SCN_TRIG_VAL_NONE:
    default:
        return true;
    }
}

/* An operator's name for a message, and "" for one the table cannot name:
 * SCN_TRIG_CMP_UNKNOWN, or a value outside the enum. */
static const char *mjOpText(ScnTrigCompare op) {
    const char *name = scnManifestTrigOpName(op);
    return name != NULL ? name : "";
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

/* The callbacks block is the one part of the manifest not held here. It
 * changes nothing about how a round plays, the Lua read drops rows the JSON
 * read cannot (a callback the script never defines), and a disagreement
 * would refuse a whole package over a sentence the lobby shows. The load
 * shows the rows the Lua read kept. */
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
    /* The kind is held to agreeing like every other field, and it matters
       more than most: it is what decides whether the ops that end a round
       raise when this file calls them. A package whose two forms disagreed
       about it would be one thing in the lobby and another in the round. */
    if (fromJson->kind != fromLua->kind) {
        return mjDiffer(key, keyLen, err, errLen, "kind",
                        "scenario: the manifest says kind '%s' and the "
                        "script's table says '%s'",
                        scnManifestKindName(fromJson->kind),
                        scnManifestKindName(fromLua->kind));
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
    if (fromJson->fillToCaps != fromLua->fillToCaps) {
        return mjDiffer(key, keyLen, err, errLen, "fill_to_caps",
                        "scenario: the manifest says fill_to_caps %s and the "
                        "script's table says %s",
                        fromJson->fillToCaps ? "true" : "false",
                        fromLua->fillToCaps ? "true" : "false");
    }
    if (fromJson->needsBots != fromLua->needsBots) {
        return mjDiffer(key, keyLen, err, errLen, "needs_bots",
                        "scenario: the manifest says needs_bots %s and the "
                        "script's table says %s",
                        fromJson->needsBots ? "true" : "false",
                        fromLua->needsBots ? "true" : "false");
    }
    /* The Workshop item and its author are held to agreeing only where the
       script's table states them. Both are written into manifest.json after
       the file is published, and nothing rewrites the script, so a script
       that assigns its own scenario table reads 0 for both and could never
       be expected to restate them. A table that does state one, and states
       a different one, is the disagreement. */
    if (fromLua->workshopId != 0 &&
        fromLua->workshopId != fromJson->workshopId) {
        return mjDiffer(key, keyLen, err, errLen, "workshop_id",
                        "scenario: the manifest says Workshop item %llu and "
                        "the script's table says %llu",
                        (unsigned long long)fromJson->workshopId,
                        (unsigned long long)fromLua->workshopId);
    }
    if (fromLua->workshopAuthor != 0 &&
        fromLua->workshopAuthor != fromJson->workshopAuthor) {
        return mjDiffer(key, keyLen, err, errLen, "workshop_author",
                        "scenario: the manifest says it was published by "
                        "%llu and the script's table says %llu",
                        (unsigned long long)fromJson->workshopAuthor,
                        (unsigned long long)fromLua->workshopAuthor);
    }
    /* The author and the updated time the way the workshop pair is: only
       when the table states one, since a script that leaves its table to
       the manifest reads back what the manifest pushed. */
    if (fromLua->author[0] != '\0' &&
        strcmp(fromLua->author, fromJson->author) != 0) {
        return mjDiffer(key, keyLen, err, errLen, "author",
                        "scenario: the manifest says the author is '%s' and "
                        "the script's table says '%s'", fromJson->author,
                        fromLua->author);
    }
    if (fromLua->updated[0] != '\0' &&
        strcmp(fromLua->updated, fromJson->updated) != 0) {
        return mjDiffer(key, keyLen, err, errLen, "updated",
                        "scenario: the manifest says it was updated %s and "
                        "the script's table says %s", fromJson->updated,
                        fromLua->updated);
    }
    /* The settings are held to agreeing, unlike the callbacks: they are
       what the host is offered and what game.setting answers, so a package
       whose two forms disagreed would offer one dropdown and play another. */
    if (fromJson->numSettings != fromLua->numSettings) {
        return mjDiffer(key, keyLen, err, errLen, "settings",
                        "scenario: the manifest declares %d settings and "
                        "the script's table declares %d",
                        (int)fromJson->numSettings,
                        (int)fromLua->numSettings);
    }
    for (i = 0; i < (int)fromJson->numSettings && i < SCN_SETTINGS_MAX;
         i++) {
        const ScnSetting *a = &fromJson->settings[i];
        const ScnSetting *b = &fromLua->settings[i];

        bool sameWords = a->numChoices == b->numChoices;
        int  w;

        for (w = 0; sameWords && w < (int)a->numChoices &&
                    w < SCN_SETTING_CHOICES_WORDS_MAX;
             w++) {
            sameWords = strcmp(a->choices[w], b->choices[w]) == 0;
        }
        if (strcmp(a->id, b->id) != 0 || strcmp(a->label, b->label) != 0 ||
            a->type != b->type || a->min != b->min || a->max != b->max ||
            a->step != b->step || a->def != b->def || !sameWords) {
            snprintf(where, sizeof(where), "settings[%d]", i + 1);
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the manifest and the script's table "
                            "declare setting %d differently", i + 1);
        }
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
        /* An empty key is a real answer here rather than a missing one — it
           says "leave the lobby's" — so "" against a key is a disagreement
           like any other and is reported with the empty side shown as such. */
        if (strcmp(a->mode, b->mode) != 0) {
            snprintf(where, sizeof(where), "lobby.teams[%d].mode", i);
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the manifest puts this team in mode "
                            "'%s' and the script's table puts it in '%s'",
                            a->mode, b->mode);
        }
        if (strcmp(a->difficulty, b->difficulty) != 0) {
            snprintf(where, sizeof(where), "lobby.teams[%d].difficulty", i);
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the manifest gives this team "
                            "difficulty '%s' and the script's table gives it "
                            "'%s'",
                            a->difficulty, b->difficulty);
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
                 simRulesRuleName((int)fromJson->rules[i].rule));
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
                     simRulesRuleName((int)fromLua->rules[i].rule));
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

    if (fromJson->numTriggers != fromLua->numTriggers) {
        return mjDiffer(key, keyLen, err, errLen, "triggers",
                        "scenario: the manifest states %d triggers and the "
                        "script's table states %d",
                        (int)fromJson->numTriggers, (int)fromLua->numTriggers);
    }
    /* A rule is matched by its index and a region by its name. A trigger has
     * neither, so the two forms are compared position against position and
     * the key is a subscript — the one key this function reports that names a
     * place rather than something the author gave a name to. Both readers
     * count that position the same way and report under it, so triggers[3]
     * means the same row in the manifest, in the script's table and here. */
    for (i = 0; i < (int)fromJson->numTriggers; i++) {
        const ScnTrigger *ta = &fromJson->triggers[i];
        const ScnTrigger *tb = &fromLua->triggers[i];
        int               j;

        snprintf(where, sizeof(where), "triggers[%d]", i);
        if (strcmp(ta->when, tb->when) != 0) {
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the manifest runs this on '%s' and the "
                            "script's table runs it on '%s'",
                            ta->when, tb->when);
        }
        if (ta->numWhere != tb->numWhere) {
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the manifest tests this %d times and "
                            "the script's table tests it %d times",
                            (int)ta->numWhere, (int)tb->numWhere);
        }
        for (j = 0; j < (int)ta->numWhere; j++) {
            const ScnTrigCond *ca = &ta->where[j];
            const ScnTrigCond *cb = &tb->where[j];

            snprintf(where, sizeof(where), "triggers[%d].where[%d]", i, j);
            if (strcmp(ca->field, cb->field) != 0) {
                return mjDiffer(key, keyLen, err, errLen, where,
                                "scenario: the manifest tests '%s' here and "
                                "the script's table tests '%s'",
                                ca->field, cb->field);
            }
            if (ca->op != cb->op) {
                return mjDiffer(key, keyLen, err, errLen, where,
                                "scenario: the manifest tests this with '%s' "
                                "and the script's table with '%s'",
                                mjOpText(ca->op), mjOpText(cb->op));
            }
            if (!mjTrigValueSame(&ca->value, &cb->value)) {
                return mjDiffer(key, keyLen, err, errLen, where,
                                "scenario: the manifest and the script's "
                                "table test this against different values");
            }
        }

        snprintf(where, sizeof(where), "triggers[%d]", i);
        if (ta->numActions != tb->numActions) {
            return mjDiffer(key, keyLen, err, errLen, where,
                            "scenario: the manifest gives this %d actions and "
                            "the script's table gives it %d",
                            (int)ta->numActions, (int)tb->numActions);
        }
        for (j = 0; j < (int)ta->numActions; j++) {
            const ScnTrigAct *aa = &ta->actions[j];
            const ScnTrigAct *ab = &tb->actions[j];
            int               k;

            snprintf(where, sizeof(where), "triggers[%d].actions[%d]", i, j);
            if (strcmp(aa->op, ab->op) != 0) {
                return mjDiffer(key, keyLen, err, errLen, where,
                                "scenario: the manifest does '%s' here and "
                                "the script's table does '%s'",
                                aa->op, ab->op);
            }
            if (aa->numArgs != ab->numArgs) {
                return mjDiffer(key, keyLen, err, errLen, where,
                                "scenario: the manifest gives this %d "
                                "arguments and the script's table gives it %d",
                                (int)aa->numArgs, (int)ab->numArgs);
            }
            for (k = 0; k < (int)aa->numArgs; k++) {
                if (!mjTrigValueSame(&aa->args[k], &ab->args[k])) {
                    snprintf(where, sizeof(where),
                             "triggers[%d].actions[%d][%d]", i, j, k);
                    return mjDiffer(key, keyLen, err, errLen, where,
                                    "scenario: the manifest and the script's "
                                    "table give this argument differently");
                }
            }
            snprintf(where, sizeof(where), "triggers[%d].actions[%d]", i, j);
            if (strcmp(aa->text, ab->text) != 0) {
                return mjDiffer(key, keyLen, err, errLen, where,
                                "scenario: the manifest's line here is '%s' "
                                "and the script's table's is '%s'",
                                aa->text, ab->text);
            }
        }
    }

    return true;
}
