/*
 * platform_config.c - Cross-platform configuration storage
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Thin wrapper around the Win32 INI APIs (GetPrivateProfileString /
 * WritePrivateProfileString). On non-Windows the implementation lives in
 * src/server/posix_stubs.c, shared with the SDL3 client and map editor so
 * all three binaries write to the same WinBolo.ini.
 *
 * The path resolves to SDL_GetPrefPath("WinBolo","WinBolo") + "WinBolo.ini"
 * on every OS, matching what gamefront.c (main game) and mapeditor.c use.
 */

#include "platform_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#ifdef _WIN32
#include <windows.h>
#else
/* Implementations in src/server/posix_stubs.c — header pulls in DWORD via
 * bolo/global.h so the prototype matches the definition exactly. */
#include "../../server/posix_stubs.h"
#endif

#define CONFIG_FILE_NAME "WinBolo.ini"

static char g_config_path[1024] = {0};
static int  g_initialized = 0;

int lv_platform_config_init(const char *app_name) {
    (void)app_name;
    if (g_initialized) return 1;

    const char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (prefDir) {
        snprintf(g_config_path, sizeof(g_config_path), "%s%s", prefDir, CONFIG_FILE_NAME);
    } else {
        /* Last-resort fallback — write next to the executable. */
        snprintf(g_config_path, sizeof(g_config_path), "%s", CONFIG_FILE_NAME);
    }

#ifndef _WIN32
    /* Pin posix_stubs (and therefore http.c::preferencesGetPreferenceFile)
     * to the same file. The map editor and main game do the same. */
    preferencesSetPreferenceFileOverride(g_config_path);
#endif

    g_initialized = 1;
    return 1;
}

void lv_platform_config_shutdown(void) {
    g_initialized = 0;
    g_config_path[0] = '\0';
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
#ifdef _WIN32
    GetPrivateProfileStringA(section, key, default_val, out,
                             (DWORD)(out_size > MAXDWORD ? MAXDWORD : out_size),
                             g_config_path);
#else
    GetPrivateProfileString(section, key, default_val, out,
                            (DWORD)out_size, g_config_path);
#endif
}

void lv_platform_config_set_string(const char *section, const char *key, const char *value) {
    if (!g_initialized) return;
#ifdef _WIN32
    WritePrivateProfileStringA(section, key, value, g_config_path);
#else
    WritePrivateProfileString(section, key, value, g_config_path);
#endif
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
    /* Win32 and posix_stubs both flush on every write — nothing to do. */
    return 1;
}

void lv_platform_config_get_path(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    strncpy(out, g_config_path, out_size - 1);
    out[out_size - 1] = '\0';
}
