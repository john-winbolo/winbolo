/*
 * platform_config.c - Cross-platform configuration storage
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Windows implementation uses Win32 INI APIs for backward compatibility
 * with existing WinBolo.ini files.
 */

#include "platform_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>
#include <dirent.h>
#endif

/* Configuration file name */
#define CONFIG_FILE_NAME "WinBolo.ini"
#define MAX_LINE_LENGTH 512

static char g_config_path[1024] = {0};
static int g_initialized = 0;

/* Forward declarations for non-Windows platforms */
#ifndef _WIN32
static int parse_ini_file(const char* path);
static int write_ini_file(const char* path);
static char* find_value(const char* section, const char* key);
static void set_value(const char* section, const char* key, const char* value);
#endif

int platform_config_init(const char* app_name) {
    (void)app_name; /* Unused on Windows, may be used on other platforms */
    
    if (g_initialized) {
        return 1;
    }
    
#ifdef _WIN32
    /* On Windows, use the Windows directory for WinBolo.ini
     * This matches the original behavior where GetPrivateProfileString
     * looks in %WINDIR%\WinBolo.ini */
    GetWindowsDirectoryA(g_config_path, sizeof(g_config_path));
    strncat(g_config_path, "\\", sizeof(g_config_path) - strlen(g_config_path) - 1);
    strncat(g_config_path, CONFIG_FILE_NAME, sizeof(g_config_path) - strlen(g_config_path) - 1);
#elif defined(__APPLE__)
    /* On macOS, use ~/Library/Application Support/WinBolo/WinBolo.ini */
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) {
            home = pw->pw_dir;
        }
    }
    if (!home) {
        home = ".";
    }
    if (strlen(home) > sizeof(g_config_path) - 64) {
        return 0;
    }

    snprintf(g_config_path, sizeof(g_config_path),
             "%s/Library/Application Support/WinBolo", home);
    mkdir(g_config_path, 0700);
    strncat(g_config_path, "/", sizeof(g_config_path) - strlen(g_config_path) - 1);
    strncat(g_config_path, CONFIG_FILE_NAME, sizeof(g_config_path) - strlen(g_config_path) - 1);
#else
    /* On Linux, use ~/.config/winbolo/WinBolo.ini */
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) {
            home = pw->pw_dir;
        }
    }
    if (!home) {
        home = ".";
    }
    if (strlen(home) > sizeof(g_config_path) - 64) {
        return 0;
    }

    snprintf(g_config_path, sizeof(g_config_path), "%s/.config/winbolo", home);

    /* Create directory if it doesn't exist */
    mkdir(g_config_path, 0700);

    strncat(g_config_path, "/", sizeof(g_config_path) - strlen(g_config_path) - 1);
    strncat(g_config_path, CONFIG_FILE_NAME, sizeof(g_config_path) - strlen(g_config_path) - 1);
#endif
    
    g_initialized = 1;
    return 1;
}

void platform_config_shutdown(void) {
    g_initialized = 0;
    g_config_path[0] = '\0';
}

void platform_config_get_string(const char* section, const char* key, 
                                const char* default_val, char* out, size_t out_size) {
    if (!g_initialized) {
        strncpy(out, default_val, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    
#ifdef _WIN32
    /* Use Win32 API for backward compatibility */
    GetPrivateProfileStringA(section, key, default_val, out, (DWORD)min(out_size, (size_t)MAXDWORD), g_config_path);
#else
    /* Parse INI file and find value */
    char* value = find_value(section, key);
    if (value) {
        strncpy(out, value, out_size - 1);
        out[out_size - 1] = '\0';
    } else {
        strncpy(out, default_val, out_size - 1);
        out[out_size - 1] = '\0';
    }
#endif
}

void platform_config_set_string(const char* section, const char* key, const char* value) {
    if (!g_initialized) return;
    
#ifdef _WIN32
    WritePrivateProfileStringA(section, key, value, g_config_path);
#else
    set_value(section, key, value);
#endif
}

int platform_config_get_int(const char* section, const char* key, int default_val) {
    char str[64];
    char default_str[32];
    
    snprintf(default_str, sizeof(default_str), "%d", default_val);
    platform_config_get_string(section, key, default_str, str, sizeof(str));
    
    return atoi(str);
}

void platform_config_set_int(const char* section, const char* key, int value) {
    char str[32];
    snprintf(str, sizeof(str), "%d", value);
    platform_config_set_string(section, key, str);
}

int platform_config_get_bool(const char* section, const char* key, int default_val) {
    char str[64];
    char default_str[8];
    
    snprintf(default_str, sizeof(default_str), "%s", default_val ? "Yes" : "No");
    platform_config_get_string(section, key, default_str, str, sizeof(str));
    
    /* Check for "Yes", "yes", "Y", "y", "1", "True", "true" */
    if (str[0] == 'Y' || str[0] == 'y' || str[0] == '1' || 
        (str[0] == 'T' || str[0] == 't')) {
        return 1;
    }
    return 0;
}

void platform_config_set_bool(const char* section, const char* key, int value) {
    platform_config_set_string(section, key, value ? "Yes" : "No");
}

int platform_config_save(void) {
    /* On Windows, WritePrivateProfileString flushes automatically */
    /* On other platforms, we'd need to write the file here */
#ifndef _WIN32
    return write_ini_file(g_config_path);
#endif
    return 1;
}

void platform_config_get_path(char* out, size_t out_size) {
    strncpy(out, g_config_path, out_size - 1);
    out[out_size - 1] = '\0';
}

#ifndef _WIN32
/* Simple INI file handling for non-Windows platforms */

#define MAX_ENTRIES 256

typedef struct {
    char section[64];
    char key[64];
    char value[256];
} ConfigEntry;

static ConfigEntry g_entries[MAX_ENTRIES];
static int g_entry_count = 0;

static int parse_ini_file(const char* path) {
    FILE* fp = fopen(path, "r");
    if (!fp) return 0;
    
    char line[MAX_LINE_LENGTH];
    char current_section[64] = "";
    
    g_entry_count = 0;
    
    while (fgets(line, sizeof(line), fp) && g_entry_count < MAX_ENTRIES) {
        /* Remove trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        
        /* Skip empty lines and comments */
        if (len == 0 || line[0] == ';' || line[0] == '#') continue;
        
        /* Check for section header */
        if (line[0] == '[') {
            char* end = strchr(line, ']');
            if (end) {
                *end = '\0';
                strncpy(current_section, line + 1, sizeof(current_section) - 1);
            }
            continue;
        }
        
        /* Parse key=value */
        char* eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            char* key = line;
            char* value = eq + 1;
            
            /* Trim whitespace */
            while (*key == ' ') key++;
            while (*value == ' ') value++;
            
            strncpy(g_entries[g_entry_count].section, current_section, sizeof(g_entries[0].section) - 1);
            strncpy(g_entries[g_entry_count].key, key, sizeof(g_entries[0].key) - 1);
            strncpy(g_entries[g_entry_count].value, value, sizeof(g_entries[0].value) - 1);
            g_entry_count++;
        }
    }
    
    fclose(fp);
    return 1;
}

static char* find_value(const char* section, const char* key) {
    /* Parse file first */
    parse_ini_file(g_config_path);
    
    for (int i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].section, section) == 0 &&
            strcmp(g_entries[i].key, key) == 0) {
            return g_entries[i].value;
        }
    }
    return NULL;
}

static void set_value(const char* section, const char* key, const char* value) {
    /* Check if entry exists */
    for (int i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].section, section) == 0 &&
            strcmp(g_entries[i].key, key) == 0) {
            strncpy(g_entries[i].value, value, sizeof(g_entries[0].value) - 1);
            return;
        }
    }
    
    /* Add new entry */
    if (g_entry_count < MAX_ENTRIES) {
        strncpy(g_entries[g_entry_count].section, section, sizeof(g_entries[0].section) - 1);
        strncpy(g_entries[g_entry_count].key, key, sizeof(g_entries[0].key) - 1);
        strncpy(g_entries[g_entry_count].value, value, sizeof(g_entries[0].value) - 1);
        g_entry_count++;
    }
}

static int write_ini_file(const char* path) {
    FILE* fp = fopen(path, "w");
    if (!fp) return 0;
    
    char last_section[64] = "";
    
    for (int i = 0; i < g_entry_count; i++) {
        /* Write section header if changed */
        if (strcmp(last_section, g_entries[i].section) != 0) {
            fprintf(fp, "\n[%s]\n", g_entries[i].section);
            strncpy(last_section, g_entries[i].section, sizeof(last_section) - 1);
            last_section[sizeof(last_section) - 1] = '\0';
        }
        
        fprintf(fp, "%s=%s\n", g_entries[i].key, g_entries[i].value);
    }
    
    fclose(fp);
    return 1;
}
#endif /* !_WIN32 */