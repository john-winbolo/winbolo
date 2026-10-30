/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * platform_config_wasm.c - Browser localStorage config for WASM build
 *
 * Replaces platform_config.c — uses browser localStorage via EM_ASM
 * to persist settings across sessions without touching the filesystem.
 *
 * Each value is a localStorage item named winbolo_config[SECTION]KEY. The
 * name is split at the first ']', so a section name may hold any character
 * but ']', the same rule as a section of the desktop's INI file; the key may
 * hold any character. The values are kept in memory at any length: the
 * layout of the sidebars (Dock.Ini) is ImGui's settings text, many kilobytes
 * long. A save writes only the values that changed since the last one, so
 * the page can save once a frame, as the desktop viewer does, at no cost
 * when nothing changed.
 *
 * Earlier builds named the items winbolo_config_SECTION_KEY, split at the
 * first '_'. That is not safe for a section with '_' in its name, but no
 * section in use has one (LOGVIEWER, SETTINGS, WINBOLO.NET), so such an
 * item is still read the old way. The next save writes it under the new
 * name and removes the old item.
 */

#include "platform/platform_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>

static int g_initialized = 0;

typedef struct {
    char *section;
    char *key;
    char *value;
    int   dirty;   /* changed since localStorage last had it */
    int   legacy;  /* also in localStorage under the old name, to remove */
} ConfigEntry;

static ConfigEntry *g_entries = NULL;
static int g_entry_count = 0;
static int g_entry_cap = 0;

static char *dup_string(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d != NULL) memcpy(d, s, n);
    return d;
}

/* A new entry at the end of the list, or NULL when there is no memory. */
static ConfigEntry *add_entry(const char *section, const char *key, const char *value) {
    ConfigEntry *e;
    if (g_entry_count == g_entry_cap) {
        int cap = (g_entry_cap > 0) ? g_entry_cap * 2 : 64;
        ConfigEntry *grown = (ConfigEntry *)realloc(g_entries, (size_t)cap * sizeof(ConfigEntry));
        if (grown == NULL) return NULL;
        g_entries = grown;
        g_entry_cap = cap;
    }
    e = &g_entries[g_entry_count];
    e->section = dup_string(section);
    e->key = dup_string(key);
    e->value = dup_string(value);
    e->dirty = 0;
    e->legacy = 0;
    if (e->section == NULL || e->key == NULL || e->value == NULL) {
        free(e->section);
        free(e->key);
        free(e->value);
        return NULL;
    }
    g_entry_count++;
    return e;
}

static ConfigEntry *find_entry(const char *section, const char *key) {
    for (int i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].section, section) == 0 &&
            strcmp(g_entries[i].key, key) == 0) {
            return &g_entries[i];
        }
    }
    return NULL;
}

int lv_platform_config_init(const char *app_name) {
    int count, i;
    (void)app_name;
    if (g_initialized) return 1;

    g_entry_count = 0;

    /* The names of this program's items, kept on the page while they are
     * read one by one below. localStorage can throw (a private window, or
     * storage turned off); there is then nothing to read. */
    count = EM_ASM_INT({
        var keys = [];
        try {
            for (var i = 0; i < localStorage.length; i++) {
                var k = localStorage.key(i);
                if (k && (k.startsWith("winbolo_config[") || k.startsWith("winbolo_config_"))) {
                    keys.push(k);
                }
            }
        } catch (e) {
            keys = [];
        }
        Module.wbConfigKeys = keys;
        return keys.length;
    });

    for (i = 0; i < count; i++) {
        /* The item's name with the prefix's last character left on: '[' for
         * the new form, '_' for the old one. Both strings are made by the
         * page with malloc and freed here. */
        char *name = (char *)EM_ASM_INT({
            return stringToNewUTF8(Module.wbConfigKeys[$0].substring("winbolo_config".length));
        }, i);
        char *val = (char *)EM_ASM_INT({
            var v = null;
            try { v = localStorage.getItem(Module.wbConfigKeys[$0]); } catch (e) { v = null; }
            return (v === null) ? 0 : stringToNewUTF8(v);
        }, i);
        if (name != NULL && val != NULL) {
            int legacy = (name[0] == '_');
            /* New form: SECTION]KEY, split at the first ']'. Old form:
             * SECTION_KEY, split at the first '_'. */
            char *sep = strchr(name + 1, legacy ? '_' : ']');
            if (sep != NULL) {
                ConfigEntry *e;
                *sep = '\0';
                e = find_entry(name + 1, sep + 1);
                if (e == NULL) {
                    e = add_entry(name + 1, sep + 1, val);
                } else if (legacy) {
                    /* Both names are there. An old item is removed when its
                     * value is written under the new name, so one still
                     * there was written after that, by an earlier build:
                     * its value is the newer one. */
                    char *copy = dup_string(val);
                    if (copy != NULL) {
                        free(e->value);
                        e->value = copy;
                    }
                }
                if (e != NULL && legacy) {
                    /* Written under the new name at the next save, which
                     * removes the old item. */
                    e->dirty = 1;
                    e->legacy = 1;
                }
            }
        }
        free(name);
        free(val);
    }
    EM_ASM({ delete Module.wbConfigKeys; });

    g_initialized = 1;
    return 1;
}

void lv_platform_config_shutdown(void) {
    int i;
    for (i = 0; i < g_entry_count; i++) {
        free(g_entries[i].section);
        free(g_entries[i].key);
        free(g_entries[i].value);
    }
    free(g_entries);
    g_entries = NULL;
    g_entry_count = 0;
    g_entry_cap = 0;
    g_initialized = 0;
}

static void set_value(const char *section, const char *key, const char *value) {
    ConfigEntry *e = find_entry(section, key);
    if (e != NULL) {
        char *copy;
        if (strcmp(e->value, value) == 0) return;
        copy = dup_string(value);
        if (copy == NULL) return;
        free(e->value);
        e->value = copy;
        e->dirty = 1;
        return;
    }
    e = add_entry(section, key, value);
    if (e != NULL) e->dirty = 1;
}

void lv_platform_config_get_string(const char *section, const char *key,
                                const char *default_val, char *out, size_t out_size) {
    ConfigEntry *e;
    if (!g_initialized) {
        strncpy(out, default_val, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    e = find_entry(section, key);
    if (e != NULL) {
        strncpy(out, e->value, out_size - 1);
    } else {
        strncpy(out, default_val, out_size - 1);
    }
    out[out_size - 1] = '\0';
}

void lv_platform_config_set_string(const char *section, const char *key, const char *value) {
    if (!g_initialized || value == NULL) return;
    set_value(section, key, value);
}

int lv_platform_config_get_int(const char *section, const char *key, int default_val) {
    char str[64], def[32];
    snprintf(def, sizeof(def), "%d", default_val);
    lv_platform_config_get_string(section, key, def, str, sizeof(str));
    return atoi(str);
}

void lv_platform_config_set_int(const char *section, const char *key, int value) {
    char str[32];
    snprintf(str, sizeof(str), "%d", value);
    lv_platform_config_set_string(section, key, str);
}

int lv_platform_config_get_bool(const char *section, const char *key, int default_val) {
    char str[64], def[8];
    snprintf(def, sizeof(def), "%s", default_val ? "Yes" : "No");
    lv_platform_config_get_string(section, key, def, str, sizeof(str));
    if (str[0] == 'Y' || str[0] == 'y' || str[0] == '1' ||
        str[0] == 'T' || str[0] == 't') {
        return 1;
    }
    return 0;
}

void lv_platform_config_set_bool(const char *section, const char *key, int value) {
    lv_platform_config_set_string(section, key, value ? "Yes" : "No");
}

/* When a save that failed may be tried again, in ms (emscripten_get_now). */
static double g_retry_at = 0.0;

int lv_platform_config_retry_waiting(void) {
    if (g_retry_at > 0.0 && emscripten_get_now() < g_retry_at) return 1;
    g_retry_at = 0.0;
    return 0;
}

int lv_platform_config_save(void) {
    int ok = 1;
    /* Every call tries the writes, so the save when the page is closed or
     * hidden always tries once. After a refused write the page's per-frame
     * save waits two seconds (lv_platform_config_retry_waiting), as the
     * desktop does: it does not try once a frame. */
    g_retry_at = 0.0;
    /* Write to localStorage the values that changed since the last save. A
     * value the browser refused (storage full or turned off) stays marked,
     * so the next save tries it again. */
    for (int i = 0; i < g_entry_count; i++) {
        int written;
        if (!g_entries[i].dirty) continue;
        written = EM_ASM_INT({
            try {
                var section = UTF8ToString($0);
                var key = UTF8ToString($1);
                localStorage.setItem("winbolo_config[" + section + "]" + key, UTF8ToString($2));
                /* The value is now under the new name: the old item, named
                 * as earlier builds named it, goes. */
                if ($3) localStorage.removeItem("winbolo_config_" + section + "_" + key);
                return 1;
            } catch (e) {
                return 0;
            }
        }, g_entries[i].section, g_entries[i].key, g_entries[i].value, g_entries[i].legacy);
        if (written) {
            g_entries[i].dirty = 0;
            g_entries[i].legacy = 0;
        } else {
            ok = 0;
        }
    }
    if (!ok) {
        g_retry_at = emscripten_get_now() + 2000.0;
    }
    return ok;
}

void lv_platform_config_get_path(char *out, size_t out_size) {
    strncpy(out, "localStorage", out_size - 1);
    out[out_size - 1] = '\0';
}
