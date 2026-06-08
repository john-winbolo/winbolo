/*
 * platform_config.c - Cross-platform configuration storage
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Thin wrapper over the process-global preferences document
 * (common/prefs.h, WinBolo.json), shared with the SDL3 client, map
 * editor and braintest so every binary reads and writes the same file.
 * The standalone log-viewer main loads the document via prefsInit at
 * startup; the in-game log viewer reuses the document the game already
 * loaded — neither path re-inits here.
 */

#include "platform_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "../../common/prefs.h"

static int g_initialized = 0;

int lv_platform_config_init(const char *app_name) {
    (void)app_name;
    g_initialized = 1;
    return 1;
}

void lv_platform_config_shutdown(void) {
    g_initialized = 0;
}

void lv_platform_config_get_string(const char *section, const char *key,
                                   const char *default_val, char *out, size_t out_size) {
    if (!g_initialized || out_size == 0) {
        if (out && out_size > 0) {
            strncpy(out, default_val ? default_val : "", out_size - 1);
            out[out_size - 1] = '\0';
        }
        return;
    }
    prefsGetString(section, key, default_val, out, out_size);
}

void lv_platform_config_set_string(const char *section, const char *key, const char *value) {
    if (!g_initialized) return;
    prefsSetString(section, key, value);
}

int lv_platform_config_get_int(const char *section, const char *key, int default_val) {
    char str[64];
    char default_str[32];
    snprintf(default_str, sizeof(default_str), "%d", default_val);
    lv_platform_config_get_string(section, key, default_str, str, sizeof(str));
    return atoi(str);
}

void lv_platform_config_set_int(const char *section, const char *key, int value) {
    char str[32];
    snprintf(str, sizeof(str), "%d", value);
    lv_platform_config_set_string(section, key, str);
}

int lv_platform_config_get_bool(const char *section, const char *key, int default_val) {
    char str[64];
    lv_platform_config_get_string(section, key, default_val ? "Yes" : "No", str, sizeof(str));
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
    /* prefsSetString flushes the document on every write — nothing to do. */
    return 1;
}

void lv_platform_config_get_path(char *out, size_t out_size) {
    /* The configuration now lives in the process-global preferences
     * document (common/prefs.h); platform_config no longer owns a path. */
    if (!out || out_size == 0) return;
    out[0] = '\0';
}
