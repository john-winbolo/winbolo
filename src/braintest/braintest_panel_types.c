/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "braintest_panel_types.h"
#include <string.h>

#define PANEL_TYPE_MAX       32
#define PANEL_TYPE_NAME_MAX  48

typedef struct {
    char          name[PANEL_TYPE_NAME_MAX];
    PanelRenderFn fn;
} PanelTypeEntry;

/* File-static array. BSS-zeroed before any C++ static
 * initializer fires, so per-bot modules that register
 * themselves at static-init time always see a clean slot
 * to write into regardless of TU init order. */
static PanelTypeEntry g_types[PANEL_TYPE_MAX];
static int            g_count = 0;

void panelTypeRegister(const char *type_name, PanelRenderFn fn) {
    if (!type_name || !type_name[0]) return;
    /* Replace existing entry if present. */
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_types[i].name, type_name) == 0) {
            g_types[i].fn = fn;
            return;
        }
    }
    if (g_count >= PANEL_TYPE_MAX) return;
    strncpy(g_types[g_count].name, type_name, PANEL_TYPE_NAME_MAX - 1);
    g_types[g_count].name[PANEL_TYPE_NAME_MAX - 1] = '\0';
    g_types[g_count].fn = fn;
    g_count++;
}

PanelRenderFn panelTypeFind(const char *type_name) {
    if (!type_name || !type_name[0]) return NULL;
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_types[i].name, type_name) == 0) {
            return g_types[i].fn;
        }
    }
    return NULL;
}

int panelTypeCount(void) { return g_count; }

const char *panelTypeNameAt(int idx) {
    if (idx < 0 || idx >= g_count) return NULL;
    return g_types[idx].name;
}
