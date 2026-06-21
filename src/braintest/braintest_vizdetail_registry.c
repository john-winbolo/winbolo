/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "braintest_vizdetail_registry.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static VizDetailEntry g_entries[VIZDETAIL_REG_MAX];
static int            g_count = 0;

/* Playback override. The active flag is what gates fallback to the
 * live registry; the entries pointer is allowed to be NULL when the
 * recorded frame had zero entries. Without the separate flag, a
 * NULL entries pointer would silently fall through to the live
 * registry, surfacing whatever was last emitted by a live brain
 * tick — which is exactly the wrong content during scrub. */
static bool                  g_pb_active  = false;
static const VizDetailEntry *g_pb_entries = NULL;
static int                   g_pb_count   = 0;

void vizDetailSetPlaybackView(const VizDetailEntry *entries, int count) {
    g_pb_active  = true;
    g_pb_entries = entries;
    g_pb_count   = (entries && count > 0) ? count : 0;
}
void vizDetailClearPlaybackView(void) {
    g_pb_active  = false;
    g_pb_entries = NULL;
    g_pb_count   = 0;
}

void vizDetailRegistryClear(void) {
    g_count = 0;
}

/* Active source — playback override if active, else the live
 * registry. Override active with NULL entries returns an empty
 * view (intended: recorded frame had zero entries). */
static inline const VizDetailEntry *active_arr(void) {
    if (g_pb_active) return g_pb_entries;   /* may be NULL — caller must check count */
    return g_entries;
}
static inline int active_count(void) {
    if (g_pb_active) return g_pb_count;
    return g_count;
}

int vizDetailFindByID(const char *id, int bot_owner) {
    if (!id) return -1;
    const VizDetailEntry *arr = active_arr();
    int n = active_count();
    for (int i = 0; i < n; i++) {
        if (bot_owner >= 0 && arr[i].bot_owner != bot_owner) continue;
        if (strncmp(arr[i].id, id, VIZDETAIL_ID_MAX) == 0) return i;
    }
    return -1;
}

int vizDetailRegister(int bot_owner, const char *id, VizDetailKind kind,
                       float x1, float y1, float x2, float y2,
                       const char *label) {
    if (!id || !id[0]) return -1;
    int idx = vizDetailFindByID(id, bot_owner);
    if (idx < 0) {
        if (g_count >= VIZDETAIL_REG_MAX) return -1;
        idx = g_count++;
        VizDetailEntry *e = &g_entries[idx];
        snprintf(e->id, VIZDETAIL_ID_MAX, "%s", id);
        e->body_line_count = 0;
        e->bot_owner = bot_owner;
    }
    VizDetailEntry *e = &g_entries[idx];
    e->kind = kind;
    e->x1 = x1; e->y1 = y1; e->x2 = x2; e->y2 = y2;
    snprintf(e->label, VIZDETAIL_LABEL_MAX, "%s", label ? label : "");
    return idx;
}

int vizDetailAppendBody(int bot_owner, const char *id, const char *line) {
    if (!id || !id[0]) return -1;
    int idx = vizDetailFindByID(id, bot_owner);
    if (idx < 0) {
        /* Body before geometry — create stub with no kind/geom so the
         * dialog still shows the body. label stays empty until a real
         * register call follows. */
        if (g_count >= VIZDETAIL_REG_MAX) return -1;
        idx = g_count++;
        VizDetailEntry *e = &g_entries[idx];
        snprintf(e->id, VIZDETAIL_ID_MAX, "%s", id);
        e->kind = VIZDETAIL_KIND_TEXT;
        e->x1 = 0; e->y1 = 0; e->x2 = 0; e->y2 = 0;
        e->label[0] = '\0';
        e->body_line_count = 0;
        e->bot_owner = bot_owner;
    }
    VizDetailEntry *e = &g_entries[idx];
    if (e->body_line_count >= VIZDETAIL_BODY_LINES_MAX) return idx;
    snprintf(e->body[e->body_line_count], VIZDETAIL_BODY_LINE_MAX,
             "%s", line ? line : "");
    e->body_line_count++;
    return idx;
}

int vizDetailCount(void) { return active_count(); }

const VizDetailEntry *vizDetailGet(int idx) {
    int n = active_count();
    if (idx < 0 || idx >= n) return NULL;
    return &active_arr()[idx];
}

static float entry_area(const VizDetailEntry *e) {
    switch (e->kind) {
    case VIZDETAIL_KIND_RECT: {
        float w = fabsf(e->x2 - e->x1);
        float h = fabsf(e->y2 - e->y1);
        return w * h;
    }
    case VIZDETAIL_KIND_CIRCLE: {
        float r = e->x2;
        return 3.14159265f * r * r;
    }
    case VIZDETAIL_KIND_TEXT:
    default:
        /* Treat text as a tiny point for hit-testing. */
        return 0.001f;
    }
}

static bool entry_contains(const VizDetailEntry *e, float tx, float ty) {
    switch (e->kind) {
    case VIZDETAIL_KIND_RECT: {
        float xmin = e->x1 < e->x2 ? e->x1 : e->x2;
        float xmax = e->x1 > e->x2 ? e->x1 : e->x2;
        float ymin = e->y1 < e->y2 ? e->y1 : e->y2;
        float ymax = e->y1 > e->y2 ? e->y1 : e->y2;
        return tx >= xmin && tx <= xmax && ty >= ymin && ty <= ymax;
    }
    case VIZDETAIL_KIND_CIRCLE: {
        float dx = tx - e->x1, dy = ty - e->y1;
        float r  = e->x2;
        return (dx * dx + dy * dy) <= (r * r);
    }
    case VIZDETAIL_KIND_TEXT:
    default: {
        /* Hit when the click is within ~0.5 tiles of the anchor. */
        float dx = tx - e->x1, dy = ty - e->y1;
        return (dx * dx + dy * dy) <= 0.25f;
    }
    }
}

int vizDetailHitTest(float tx, float ty, int bot_owner) {
    const VizDetailEntry *arr = active_arr();
    int n = active_count();
    int best = -1;
    float best_area = 1e30f;
    for (int i = 0; i < n; i++) {
        const VizDetailEntry *e = &arr[i];
        if (bot_owner >= 0 && e->bot_owner != bot_owner) continue;
        if (!entry_contains(e, tx, ty)) continue;
        float a = entry_area(e);
        if (a < best_area) {
            best_area = a;
            best = i;
        }
    }
    return best;
}
