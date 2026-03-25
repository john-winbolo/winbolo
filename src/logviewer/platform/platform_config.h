/*
 * platform_config.h - Cross-platform configuration storage
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
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
int platform_config_init(const char* app_name);

/* Shutdown the configuration system */
void platform_config_shutdown(void);

/* Get a string value from configuration
 * section - Section name (e.g., "LOGVIEWER")
 * key - Key name (e.g., "ScreenSizeX")
 * default_val - Default value if key not found
 * out - Buffer to store result
 * out_size - Size of output buffer */
void platform_config_get_string(const char* section, const char* key, 
                                const char* default_val, char* out, size_t out_size);

/* Set a string value in configuration
 * section - Section name
 * key - Key name
 * value - Value to set */
void platform_config_set_string(const char* section, const char* key, const char* value);

/* Get an integer value from configuration
 * section - Section name
 * key - Key name
 * default_val - Default value if key not found
 * Returns the integer value */
int platform_config_get_int(const char* section, const char* key, int default_val);

/* Set an integer value in configuration
 * section - Section name
 * key - Key name
 * value - Value to set */
void platform_config_set_int(const char* section, const char* key, int value);

/* Get a boolean value from configuration
 * section - Section name
 * key - Key name
 * default_val - Default value if key not found
 * Returns 1 for true, 0 for false */
int platform_config_get_bool(const char* section, const char* key, int default_val);

/* Set a boolean value in configuration
 * section - Section name
 * key - Key name
 * value - 1 for true, 0 for false */
void platform_config_set_bool(const char* section, const char* key, int value);

/* Save configuration to disk
 * Returns 1 on success, 0 on failure */
int platform_config_save(void);

/* Get the configuration file path
 * out - Buffer to store path
 * out_size - Size of output buffer */
void platform_config_get_path(char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_CONFIG_H */