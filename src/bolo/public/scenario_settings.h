/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Script Settings
 *Filename:      scenario_settings.h
 *Author:        John Morrison
 *Purpose:
 *  The settings a script lets the host choose in the
 *  lobby, as its manifest declares them, and the blob one
 *  script's declarations travel to a lobby in.
 *
 *  A script declares them in its scenario table:
 *
 *    settings = {
 *      { id = "rounds", label = "Rounds", type = "int",
 *        min = 1, max = 5, step = 1, default = 5 },
 *    }
 *
 *  and reads the host's choice back with game.setting(id),
 *  which answers the declared default for any id the host
 *  has not chosen a value for.
 *
 *  Only "int" exists today and is drawn as a dropdown. The
 *  type is a byte on the wire and every row carries its own
 *  length, so a reader skips a type it does not know and a
 *  "bool" or a "choice" can be added later without breaking
 *  the readers already out there.
 *
 *  The blob is one script's whole block:
 *
 *    [count 1] then count rows of
 *    [type 1][bodyLen 1][body]
 *
 *  and for SCN_SETTING_TYPE_INT the body is
 *
 *    [idLen 1][id][labelLen 1][label]
 *    [min 4][max 4][step 4][default 4]
 *
 *  with each number a signed 32-bit value, big-endian. A
 *  script that declares nothing is no bytes at all.
 *
 *  Here in public/ for the reason scenario_callbacks.h is:
 *  the manifest readers, the scenario directory, the server
 *  sim, the client sim and the lobby's details dialog all
 *  read these rows and share no other header. Inline, so a
 *  reader links nothing for it.
 *********************************************************/

#ifndef SCENARIO_SETTINGS_H
#define SCENARIO_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform_types.h" /* BOLO_STATIC_ASSERT */

/* The most settings one script may declare. A row past this is dropped with
 * a report, so the host's dialog never grows past one screen of dropdowns. */
#define SCN_SETTINGS_MAX 16

/* An id and its terminator. Letters, digits and '_' only, the characters a
 * Lua name is made of, so an id is always a word an author can type. */
#define SCN_SETTING_ID_LEN 32

/* A label and its terminator: the words the dialog shows beside the
 * dropdown. The script's own text, not a translated string. */
#define SCN_SETTING_LABEL_LEN 64

/* The most entries one dropdown may offer: (max - min) / step + 1. A range
 * past this is a declaration the dialog cannot draw usefully, so it is
 * refused rather than drawn as a list nobody can scroll. */
#define SCN_SETTING_CHOICES_MAX 100

/* What kind of value a row holds. A byte on the wire, so the numbers are
 * fixed. SCN_SETTING_TYPE_COUNT is how many this build can read. */
#define SCN_SETTING_TYPE_INT   0
#define SCN_SETTING_TYPE_COUNT 1

/* One declared setting. def is the default: "default" is a C keyword. */
typedef struct {
    char    id[SCN_SETTING_ID_LEN];
    char    label[SCN_SETTING_LABEL_LEN];
    uint8_t type;   /* SCN_SETTING_TYPE_* */
    int32_t min;
    int32_t max;
    int32_t step;
    int32_t def;
} ScnSetting;

/* The body of one int row at its widest, and the whole blob at its largest. */
#define SCN_SETTING_INT_BODY_MAX \
    (1 + (SCN_SETTING_ID_LEN - 1) + 1 + (SCN_SETTING_LABEL_LEN - 1) + 16)
#define SCN_SETTINGS_BLOB_MAX \
    (1 + SCN_SETTINGS_MAX * (2 + SCN_SETTING_INT_BODY_MAX))

/* A row's body length travels as one byte. */
BOLO_STATIC_ASSERT(SCN_SETTING_INT_BODY_MAX <= 255,
                   scn_setting_int_body_fits_a_byte);

/* Whether an id is one a script may declare: 1 to 31 letters, digits and
 * underscores. */
static inline bool scnSettingIdOk(const char *id) {
    size_t i;
    size_t n;

    if (id == NULL) return false;
    n = strlen(id);
    if (n == 0 || n >= SCN_SETTING_ID_LEN) return false;
    for (i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

/* How many entries the dropdown for s offers, or 0 for a row whose range
 * or step is not usable. Worked in 64 bits so no range of int32s overflows. */
static inline int64_t scnSettingChoices(const ScnSetting *s) {
    if (s == NULL || s->step <= 0 || s->min > s->max) return 0;
    return ((int64_t)s->max - (int64_t)s->min) / (int64_t)s->step + 1;
}

/* Whether v is a value s can hold: inside the range and on the step. */
static inline bool scnSettingValueOk(const ScnSetting *s, int64_t v) {
    if (s == NULL || s->step <= 0) return false;
    if (v < (int64_t)s->min || v > (int64_t)s->max) return false;
    return ((v - (int64_t)s->min) % (int64_t)s->step) == 0;
}

/* NULL when s is a declaration this build can use, else the reason in a few
 * words for a report. Both manifest readers ask this, so a Lua table and a
 * package's manifest.json are held to the same rules. */
static inline const char *scnSettingProblem(const ScnSetting *s) {
    if (s == NULL) return "missing";
    if (!scnSettingIdOk(s->id)) {
        return "id must be 1 to 31 letters, digits or '_'";
    }
    if (s->label[0] == '\0') return "label is empty";
    if (s->type >= SCN_SETTING_TYPE_COUNT) return "type is not \"int\"";
    if (s->step <= 0) return "step must be greater than 0";
    if (s->min > s->max) return "min is greater than max";
    if (s->def < s->min || s->def > s->max) {
        return "default is outside min..max";
    }
    if (!scnSettingValueOk(s, s->def)) return "default is not on the step";
    if (scnSettingChoices(s) > SCN_SETTING_CHOICES_MAX) {
        return "the range has more than 100 entries";
    }
    return NULL;
}

/* The value a script plays with: v when the host chose it and s can hold
 * it, else the declared default. */
static inline int32_t scnSettingResolve(const ScnSetting *s, bool chosen,
                                        int64_t v) {
    if (s == NULL) return 0;
    if (chosen && scnSettingValueOk(s, v)) return (int32_t)v;
    return s->def;
}

/* What the server keeps when the host sends v: a value below the range is
 * the lowest entry, one above it the highest entry (the last one on the
 * step), and one inside the range but off the step is the default, because
 * no entry is nearer to it than another in any way a host would expect. */
static inline int32_t scnSettingClamp(const ScnSetting *s, int64_t v) {
    int64_t top;

    if (s == NULL) return 0;
    if (s->step <= 0 || s->min > s->max) return s->def;
    top = (int64_t)s->min +
          (((int64_t)s->max - (int64_t)s->min) / (int64_t)s->step) *
              (int64_t)s->step;
    if (v < (int64_t)s->min) return s->min;
    if (v > top) return (int32_t)top;
    return scnSettingResolve(s, true, v);
}

/* The row named id among n, or NULL. */
static inline const ScnSetting *scnSettingFind(const ScnSetting *rows, int n,
                                               const char *id) {
    int i;

    if (rows == NULL || id == NULL) return NULL;
    for (i = 0; i < n; i++) {
        if (strcmp(rows[i].id, id) == 0) return &rows[i];
    }
    return NULL;
}

static inline void scnSettingsPutI32(uint8_t *at, int32_t v) {
    uint32_t u = (uint32_t)v;
    at[0] = (uint8_t)(u >> 24);
    at[1] = (uint8_t)(u >> 16);
    at[2] = (uint8_t)(u >> 8);
    at[3] = (uint8_t)u;
}

static inline int32_t scnSettingsGetI32(const uint8_t *at) {
    uint32_t u = ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) |
                 ((uint32_t)at[2] << 8) | (uint32_t)at[3];
    return (int32_t)u;
}

/* One row onto the end of a blob of *len bytes. False, and the blob left as
 * it was, for a row that is not usable (scnSettingProblem) or would take the
 * blob past cap. A blob starts at *len == 0. */
static inline bool scnSettingsBlobAppend(uint8_t *blob, size_t cap,
                                         size_t *len, const ScnSetting *s) {
    size_t idLen;
    size_t labelLen;
    size_t body;
    size_t at;

    if (blob == NULL || len == NULL || scnSettingProblem(s) != NULL) {
        return false;
    }
    idLen    = strlen(s->id);
    labelLen = strlen(s->label);
    if (labelLen >= SCN_SETTING_LABEL_LEN) return false;
    body = 1 + idLen + 1 + labelLen + 16;
    at   = (*len == 0) ? 1u : *len;
    if (at + 2 + body > cap || (*len > 0 && blob[0] >= SCN_SETTINGS_MAX)) {
        return false;
    }
    if (*len == 0) blob[0] = 0;
    blob[at++] = s->type;
    blob[at++] = (uint8_t)body;
    blob[at++] = (uint8_t)idLen;
    memcpy(blob + at, s->id, idLen);
    at += idLen;
    blob[at++] = (uint8_t)labelLen;
    memcpy(blob + at, s->label, labelLen);
    at += labelLen;
    scnSettingsPutI32(blob + at, s->min);
    scnSettingsPutI32(blob + at + 4, s->max);
    scnSettingsPutI32(blob + at + 8, s->step);
    scnSettingsPutI32(blob + at + 12, s->def);
    at += 16;
    blob[0]++;
    *len = at;
    return true;
}

/* One int row's body into *out. False for a body that is not one. */
static inline bool scnSettingsReadIntBody(const uint8_t *b, size_t n,
                                          ScnSetting *out) {
    size_t  pos = 0;
    uint8_t idLen;
    uint8_t labelLen;

    memset(out, 0, sizeof(*out));
    if (n < 1) return false;
    idLen = b[pos++];
    if (idLen == 0 || idLen >= SCN_SETTING_ID_LEN || pos + idLen + 1 > n) {
        return false;
    }
    memcpy(out->id, b + pos, idLen);
    out->id[idLen] = '\0';
    pos += idLen;
    labelLen = b[pos++];
    if (labelLen == 0 || labelLen >= SCN_SETTING_LABEL_LEN ||
        pos + labelLen + 16 != n) {
        return false;
    }
    memcpy(out->label, b + pos, labelLen);
    out->label[labelLen] = '\0';
    pos += labelLen;
    out->type = SCN_SETTING_TYPE_INT;
    out->min  = scnSettingsGetI32(b + pos);
    out->max  = scnSettingsGetI32(b + pos + 4);
    out->step = scnSettingsGetI32(b + pos + 8);
    out->def  = scnSettingsGetI32(b + pos + 12);
    /* A NUL inside either string, or numbers that do not make a usable row,
       and the row is not one. */
    return strlen(out->id) == idLen && strlen(out->label) == labelLen &&
           scnSettingProblem(out) == NULL;
}

/* Every row of a blob this build can read, into out (max of them), in the
 * blob's order. A row of a type this build does not know is skipped by its
 * length. -1 for bytes that are not a blob: a length past what is there,
 * bytes left over, a known row that does not read, more rows than
 * SCN_SETTINGS_MAX or more bytes than SCN_SETTINGS_BLOB_MAX. Zero bytes is a
 * blob of no rows. out may be NULL to only check the blob. */
static inline int scnSettingsBlobRead(const uint8_t *blob, size_t len,
                                      ScnSetting *out, int max) {
    size_t  pos = 1;
    uint8_t rows;
    uint8_t r;
    int     n = 0;

    if (len == 0) return 0;
    if (blob == NULL || len > SCN_SETTINGS_BLOB_MAX) return -1;
    rows = blob[0];
    if (rows > SCN_SETTINGS_MAX) return -1;
    for (r = 0; r < rows; r++) {
        uint8_t type;
        uint8_t body;

        if (pos + 2 > len) return -1;
        type = blob[pos];
        body = blob[pos + 1];
        pos += 2;
        if (pos + body > len) return -1;
        if (type == SCN_SETTING_TYPE_INT) {
            ScnSetting row;
            if (!scnSettingsReadIntBody(blob + pos, body, &row)) return -1;
            if (out != NULL && n < max) out[n] = row;
            n++;
        }
        pos += body;
    }
    if (pos != len) return -1;
    return n;
}

#endif /* SCENARIO_SETTINGS_H */
