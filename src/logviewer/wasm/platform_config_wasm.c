/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * platform_config_wasm.c - Browser localStorage config for WASM build
 *
 * Replaces platform_config.c — uses browser localStorage via EM_ASM
 * to persist settings across sessions without touching the filesystem.
 */

#include "platform/platform_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>

static int g_initialized = 0;

/* In-memory store — same approach as the Linux INI parser */
#define MAX_ENTRIES 256

typedef struct {
    char section[64];
    char key[64];
    char value[256];
} ConfigEntry;

static ConfigEntry g_entries[MAX_ENTRIES];
static int g_entry_count = 0;

int lv_platform_config_init(const char *app_name) {
    (void)app_name;
    if (g_initialized) return 1;

    g_entry_count = 0;

    /* Load all entries from localStorage */
    int count = EM_ASM_INT({
        var prefix = "winbolo_config_";
        var count = 0;
        for (var i = 0; i < localStorage.length; i++) {
            var k = localStorage.key(i);
            if (k && k.startsWith(prefix)) {
                count++;
            }
        }
        return count;
    });

    if (count > 0) {
        EM_ASM({
            var prefix = "winbolo_config_";
            var idx = 0;
            for (var i = 0; i < localStorage.length && idx < 256; i++) {
                var k = localStorage.key(i);
                if (!k || !k.startsWith(prefix)) continue;
                var val = localStorage.getItem(k);
                if (val === null) continue;

                /* Key format: winbolo_config_SECTION_KEY */
                var rest = k.substring(prefix.length);
                var sep = rest.indexOf("_");
                if (sep < 0) continue;
                var section = rest.substring(0, sep);
                var key = rest.substring(sep + 1);

                /* Write to C memory via provided buffer pointers */
                stringToUTF8(section, $0 + idx * (64+64+256), 64);
                stringToUTF8(key, $0 + idx * (64+64+256) + 64, 64);
                stringToUTF8(val, $0 + idx * (64+64+256) + 128, 256);
                idx++;
            }
            HEAP32[$1 >> 2] = idx;
        }, g_entries, &g_entry_count);
    }

    g_initialized = 1;
    return 1;
}

void lv_platform_config_shutdown(void) {
    g_initialized = 0;
    g_entry_count = 0;
}

static char *find_value(const char *section, const char *key) {
    for (int i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].section, section) == 0 &&
            strcmp(g_entries[i].key, key) == 0) {
            return g_entries[i].value;
        }
    }
    return NULL;
}

static void set_value(const char *section, const char *key, const char *value) {
    for (int i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].section, section) == 0 &&
            strcmp(g_entries[i].key, key) == 0) {
            strncpy(g_entries[i].value, value, sizeof(g_entries[0].value) - 1);
            g_entries[i].value[sizeof(g_entries[0].value) - 1] = '\0';
            return;
        }
    }
    if (g_entry_count < MAX_ENTRIES) {
        strncpy(g_entries[g_entry_count].section, section, sizeof(g_entries[0].section) - 1);
        g_entries[g_entry_count].section[sizeof(g_entries[0].section) - 1] = '\0';
        strncpy(g_entries[g_entry_count].key, key, sizeof(g_entries[0].key) - 1);
        g_entries[g_entry_count].key[sizeof(g_entries[0].key) - 1] = '\0';
        strncpy(g_entries[g_entry_count].value, value, sizeof(g_entries[0].value) - 1);
        g_entries[g_entry_count].value[sizeof(g_entries[0].value) - 1] = '\0';
        g_entry_count++;
    }
}

void lv_platform_config_get_string(const char *section, const char *key,
                                const char *default_val, char *out, size_t out_size) {
    if (!g_initialized) {
        strncpy(out, default_val, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    char *val = find_value(section, key);
    if (val) {
        strncpy(out, val, out_size - 1);
    } else {
        strncpy(out, default_val, out_size - 1);
    }
    out[out_size - 1] = '\0';
}

void lv_platform_config_set_string(const char *section, const char *key, const char *value) {
    if (!g_initialized) return;
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

int lv_platform_config_save(void) {
    /* Flush all entries to localStorage */
    for (int i = 0; i < g_entry_count; i++) {
        EM_ASM({
            var prefix = "winbolo_config_";
            var section = UTF8ToString($0);
            var key = UTF8ToString($1);
            var val = UTF8ToString($2);
            localStorage.setItem(prefix + section + "_" + key, val);
        }, g_entries[i].section, g_entries[i].key, g_entries[i].value);
    }
    return 1;
}

void lv_platform_config_get_path(char *out, size_t out_size) {
    strncpy(out, "localStorage", out_size - 1);
    out[out_size - 1] = '\0';
}
