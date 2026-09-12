/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Table
 *Filename:      scenario_table.c
 *Author:        John Morrison
 *Purpose:
 *  The flat key/value table a bot is handed. Storage is a
 *  fixed array of pairs, so a table is copied by
 *  assignment and owns nothing — every caller from the
 *  command line to the brain VM holds its own.
 *********************************************************/

#include <string.h>

#include "scenario_table.h"

/* Copy into a field whose capacity the caller has already checked
 * against the source length. */
static void scnCopyField(char *dst, const char *src, size_t cap) {
    size_t len = strlen(src);
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static bool scnIsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void scnTableClear(ScnTable *t) {
    if (t == NULL) return;
    memset(t, 0, sizeof(*t));
}

bool scnTableSet(ScnTable *t, const char *key, const char *value) {
    uint8_t i;

    if (t == NULL || key == NULL || key[0] == '\0') return false;
    if (value == NULL) value = "";
    if (strlen(key) >= SCN_TABLE_KEY_LEN) return false;
    if (strlen(value) >= SCN_TABLE_VALUE_LEN) return false;

    for (i = 0; i < t->count && i < SCN_TABLE_MAX; i++) {
        if (strcmp(t->kv[i].key, key) == 0) {
            scnCopyField(t->kv[i].value, value, SCN_TABLE_VALUE_LEN);
            return true;
        }
    }
    if (t->count >= SCN_TABLE_MAX) return false;
    scnCopyField(t->kv[t->count].key, key, SCN_TABLE_KEY_LEN);
    scnCopyField(t->kv[t->count].value, value, SCN_TABLE_VALUE_LEN);
    t->count++;
    return true;
}

const char *scnTableGet(const ScnTable *t, const char *key) {
    uint8_t i;

    if (t == NULL || key == NULL) return NULL;
    for (i = 0; i < t->count && i < SCN_TABLE_MAX; i++) {
        if (strcmp(t->kv[i].key, key) == 0) return t->kv[i].value;
    }
    return NULL;
}

bool scnTableFromArgText(const char *text, ScnTable *out) {
    const char *p;
    bool ok = true;

    if (out == NULL) return false;
    scnTableClear(out);
    if (text == NULL) return true;

    p = text;
    while (*p != '\0') {
        /* One token, with every space dropped as it is copied, so
         * "deprive = 100" and "deprive=100" are the same pair. */
        char token[SCN_TABLE_KEY_LEN + SCN_TABLE_VALUE_LEN];
        size_t n = 0;
        bool tooLong = false;
        char *eq;

        while (*p != '\0' && *p != ',' && *p != ';') {
            if (!scnIsSpace(*p)) {
                if (n + 1 < sizeof(token)) {
                    token[n++] = *p;
                } else {
                    tooLong = true;
                }
            }
            p++;
        }
        if (*p != '\0') p++;      /* step over the separator */
        token[n] = '\0';
        if (n == 0) continue;     /* nothing between two separators */
        if (tooLong) { ok = false; continue; }

        eq = strchr(token, '=');
        if (eq != NULL) {
            *eq = '\0';
            if (!scnTableSet(out, token, eq + 1)) ok = false;
        } else if (!scnTableSet(out, token, "1")) {
            /* A bare flag is the value "1", which is truthy in Lua. */
            ok = false;
        }
    }
    return ok;
}

void scnTableFormat(const ScnTable *t, char *out, size_t outCap) {
    uint8_t i;
    size_t at = 0;

    if (out == NULL || outCap == 0) return;
    out[0] = '\0';
    if (t == NULL) return;

    for (i = 0; i < t->count && i < SCN_TABLE_MAX; i++) {
        size_t klen = strlen(t->kv[i].key);
        size_t vlen = strlen(t->kv[i].value);
        size_t need = (at > 0 ? 1u : 0u) + klen + 1u + vlen;
        if (at + need + 1u > outCap) break;   /* truncate at a pair boundary */
        if (at > 0) out[at++] = ';';
        memcpy(out + at, t->kv[i].key, klen);
        at += klen;
        out[at++] = '=';
        memcpy(out + at, t->kv[i].value, vlen);
        at += vlen;
        out[at] = '\0';
    }
}
