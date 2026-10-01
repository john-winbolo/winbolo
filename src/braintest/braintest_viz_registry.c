/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * braintest_viz_registry.c — see header for design.
 *********************************************************/

#include "braintest_viz_registry.h"

#include <stdio.h>
#include <string.h>

static VizRegistryEntry g_entries[VIZ_REG_MAX];
static int              g_count = 0;

static void copy_str(char *dst, size_t dst_sz, const char *src) {
    if (!dst || dst_sz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < dst_sz && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

void vizRegistryReset(void) {
    g_count = 0;
    memset(g_entries, 0, sizeof(g_entries));
}

int vizRegistryCount(void) { return g_count; }

const VizRegistryEntry *vizRegistryGet(int idx) {
    if (idx < 0 || idx >= g_count) return NULL;
    return &g_entries[idx];
}

VizRegistryEntry *vizRegistryGetMutable(int idx) {
    if (idx < 0 || idx >= g_count) return NULL;
    return &g_entries[idx];
}

int vizRegistryFind(const char *id) {
    if (!id || !id[0]) return -1;
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].id, id) == 0) return i;
    }
    return -1;
}

int vizRegistryAddBrain(const char *id,
                         const char *label,
                         const char *short_desc,
                         const char *long_desc,
                         bool default_on) {
    if (!id || !id[0]) return -1;
    int existing = vizRegistryFind(id);
    if (existing >= 0) return existing;
    if (g_count >= VIZ_REG_MAX) return -1;
    VizRegistryEntry *e = &g_entries[g_count];
    memset(e, 0, sizeof(*e));
    copy_str(e->id,         sizeof(e->id),         id);
    copy_str(e->label,      sizeof(e->label),      label && label[0] ? label : id);
    copy_str(e->short_desc, sizeof(e->short_desc), short_desc);
    copy_str(e->long_desc,  sizeof(e->long_desc),  long_desc);
    copy_str(e->key_hint,   sizeof(e->key_hint),   "-");
    e->default_on = default_on;
    e->is_on      = default_on;
    e->from_brain = true;
    g_count++;
    return g_count - 1;
}

int vizRegistryAddNative(const char *label,
                          const char *short_desc,
                          const char *long_desc,
                          const char *key_hint,
                          bool default_on) {
    if (g_count >= VIZ_REG_MAX) return -1;
    VizRegistryEntry *e = &g_entries[g_count];
    memset(e, 0, sizeof(*e));
    e->id[0] = '\0';
    copy_str(e->label,      sizeof(e->label),      label);
    copy_str(e->short_desc, sizeof(e->short_desc), short_desc);
    copy_str(e->long_desc,  sizeof(e->long_desc),  long_desc);
    copy_str(e->key_hint,   sizeof(e->key_hint),   key_hint && key_hint[0] ? key_hint : "-");
    e->default_on = default_on;
    e->is_on      = default_on;
    e->from_brain = false;
    g_count++;
    return g_count - 1;
}

/* INI format: one "key=on|off" line per entry. Key is the id
 * for brain rows, or "@<label>" for native rows (label may
 * contain spaces; the @ prefix avoids any chance of clashing
 * with a brain id). */
static const char *entry_ini_key(const VizRegistryEntry *e, char *buf, size_t bufSz) {
    if (e->id[0]) {
        copy_str(buf, bufSz, e->id);
    } else {
        if (bufSz > 0) buf[0] = '@';
        copy_str(buf + 1, bufSz - 1, e->label);
    }
    return buf;
}

void vizRegistrySaveIni(const char *path) {
    if (!path) return;

    /* Brain overlays register LAZILY — an overlay only enters the
     * registry the first time its code path runs (e.g. the shoot_pill
     * HUD doesn't register until a bot enters that substate). A plain
     * truncate-and-write would therefore drop the saved on/off for every
     * overlay that hasn't registered THIS session, so toggles for
     * conditional overlays get silently lost on the (always-fires) exit
     * save. Fix: read the existing INI first and pass through any keys we
     * don't currently have a registry entry for, so their saved state
     * survives until their overlay registers again and loads it. */

    /* Snapshot current registry keys — authoritative this session. */
    static char regKeys[VIZ_REG_MAX][128];
    for (int i = 0; i < g_count; i++) {
        entry_ini_key(&g_entries[i], regKeys[i], sizeof(regKeys[i]));
    }

    /* Gather passthrough lines for keys NOT in the current registry. */
    static char passthrough[VIZ_REG_MAX][256];
    int passCount = 0;
    FILE *in = fopen(path, "r");
    if (in) {
        char line[512];
        while (passCount < VIZ_REG_MAX && fgets(line, sizeof(line), in)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
                line[--len] = '\0';
            }
            if (line[0] == '\0' || line[0] == '#') continue;
            const char *eq = strchr(line, '=');
            if (!eq) continue;
            char key[128];
            size_t klen = (size_t)(eq - line);
            if (klen >= sizeof(key)) klen = sizeof(key) - 1;
            memcpy(key, line, klen);
            key[klen] = '\0';
            bool in_reg = false;
            for (int i = 0; i < g_count; i++) {
                if (strcmp(regKeys[i], key) == 0) { in_reg = true; break; }
            }
            if (in_reg) continue;  /* written authoritatively below */
            copy_str(passthrough[passCount], sizeof(passthrough[passCount]), line);
            passCount++;
        }
        fclose(in);
    }

    FILE *f = fopen(path, "w");
    if (!f) return;
    char key[128];
    for (int i = 0; i < g_count; i++) {
        VizRegistryEntry *e = &g_entries[i];
        entry_ini_key(e, key, sizeof(key));
        fprintf(f, "%s=%s\n", key, e->is_on ? "on" : "off");
    }
    /* Preserve saved state for overlays not registered this session. */
    for (int i = 0; i < passCount; i++) {
        fprintf(f, "%s\n", passthrough[i]);
    }
    fclose(f);
}

void vizRegistryLoadIni(const char *path) {
    if (!path) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    char savedKey[256];
    while (fgets(line, sizeof(line), f)) {
        /* strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        if (line[0] == '\0' || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *val = eq + 1;
        bool on = (strcmp(val, "on") == 0 || strcmp(val, "1") == 0
                   || strcmp(val, "true") == 0);
        /* Find the matching entry. */
        for (int i = 0; i < g_count; i++) {
            VizRegistryEntry *e = &g_entries[i];
            entry_ini_key(e, savedKey, sizeof(savedKey));
            if (strcmp(savedKey, line) == 0) {
                e->is_on = on;
                break;
            }
        }
    }
    fclose(f);
}
