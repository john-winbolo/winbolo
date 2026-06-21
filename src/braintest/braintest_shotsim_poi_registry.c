/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "braintest_shotsim_poi_registry.h"
#include <string.h>
#include <stdio.h>

static ShotSimPoiEntry g_entries[SHOTSIM_POI_REG_MAX];
static int             g_count = 0;

static int findSlot(int bot_owner, const char *name) {
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].bot_owner == bot_owner
            && strncmp(g_entries[i].name, name,
                       SHOTSIM_POI_REG_NAME_MAX) == 0) {
            return i;
        }
    }
    return -1;
}

int shotSimPoiRegister(int bot_owner, const char *name,
                        const char *lua_expr) {
    if (!name || !name[0] || !lua_expr || !lua_expr[0]) return -1;
    int existing = findSlot(bot_owner, name);
    if (existing >= 0) {
        /* Overwrite — same name + same bot is treated as a refresh
         * so reloading the brain doesn't pile up duplicates. */
        snprintf(g_entries[existing].lua_expr,
                 SHOTSIM_POI_REG_EXPR_MAX, "%s", lua_expr);
        return existing;
    }
    if (g_count >= SHOTSIM_POI_REG_MAX) return -1;
    ShotSimPoiEntry *e = &g_entries[g_count];
    snprintf(e->name, SHOTSIM_POI_REG_NAME_MAX, "%s", name);
    snprintf(e->lua_expr, SHOTSIM_POI_REG_EXPR_MAX, "%s", lua_expr);
    e->bot_owner = bot_owner;
    return g_count++;
}

int shotSimPoiCount(void) { return g_count; }

const ShotSimPoiEntry *shotSimPoiGet(int idx) {
    if (idx < 0 || idx >= g_count) return NULL;
    return &g_entries[idx];
}

void shotSimPoiClearBot(int bot_owner) {
    int dst = 0;
    for (int src = 0; src < g_count; src++) {
        if (g_entries[src].bot_owner != bot_owner) {
            if (dst != src) g_entries[dst] = g_entries[src];
            dst++;
        }
    }
    g_count = dst;
}
