/*
 * platform_config.h - Cross-platform configuration storage
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This provides a cross-platform abstraction for reading/writing
 * configuration settings. On Windows, it maintains compatibility with
 * the existing WinBolo.ini format.
 */

#ifndef PLATFORM_CONFIG_H
#define PLATFORM_CONFIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the configuration system
 * app_name - Application name for config file location
 * Returns 1 on success, 0 on failure */
int lv_platform_config_init(const char* app_name);

/* Shutdown the configuration system */
void lv_platform_config_shutdown(void);

/* Get a string value from configuration
 * section - Section name (e.g., "LOGVIEWER")
 * key - Key name (e.g., "ScreenSizeX")
 * default_val - Default value if key not found
 * out - Buffer to store result
 * out_size - Size of output buffer */
void lv_platform_config_get_string(const char* section, const char* key, 
                                const char* default_val, char* out, size_t out_size);

/* Set a string value in configuration
 * section - Section name
 * key - Key name
 * value - Value to set */
void lv_platform_config_set_string(const char* section, const char* key, const char* value);

/* Get an integer value from configuration
 * section - Section name
 * key - Key name
 * default_val - Default value if key not found
 * Returns the integer value */
int lv_platform_config_get_int(const char* section, const char* key, int default_val);

/* Set an integer value in configuration
 * section - Section name
 * key - Key name
 * value - Value to set */
void lv_platform_config_set_int(const char* section, const char* key, int value);

/* Get a boolean value from configuration
 * section - Section name
 * key - Key name
 * default_val - Default value if key not found
 * Returns 1 for true, 0 for false */
int lv_platform_config_get_bool(const char* section, const char* key, int default_val);

/* Set a boolean value in configuration
 * section - Section name
 * key - Key name
 * value - 1 for true, 0 for false */
void lv_platform_config_set_bool(const char* section, const char* key, int value);

/* Save configuration to disk, when this program writes its own preferences
 * (lv_platform_config_set_own_writes) and something changed. A failed write
 * is tried again two seconds later at the earliest.
 * Returns 1 on success or nothing to do, 0 on failure or a write not yet
 * tried again */
int lv_platform_config_save(void);

#ifdef __EMSCRIPTEN__
/* The web build only. Its lv_platform_config_save tries the writes on every
 * call, so the save when the page is closed or hidden always tries once.
 * Not 0: a write was refused less than two seconds ago, and the page's
 * per-frame save waits. */
int lv_platform_config_retry_waiting(void);
#endif

/* Not 0: lv_platform_config_save writes the preferences. The standalone
 * viewer sets this; inside the game it stays 0, and the game writes. */
void lv_platform_config_set_own_writes(int on);

/* Get the configuration file path
 * out - Buffer to store path
 * out_size - Size of output buffer */
void lv_platform_config_get_path(char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_CONFIG_H */