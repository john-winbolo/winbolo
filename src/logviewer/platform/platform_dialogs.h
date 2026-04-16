/*
 * platform_dialogs.h - Cross-platform file dialogs
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This provides a cross-platform abstraction for file open/save dialogs.
 */

#ifndef PLATFORM_DIALOGS_H
#define PLATFORM_DIALOGS_H

#include <stddef.h>  /* For size_t */

/* File dialog result codes */
#define PLATFORM_DIALOG_OK      1
#define PLATFORM_DIALOG_CANCEL  0
#define PLATFORM_DIALOG_ERROR   -1

/* Initialize the dialog system
 * Returns 1 on success, 0 on failure */
int lv_platform_dialogs_init(void);

/* Shutdown the dialog system */
void lv_platform_dialogs_shutdown(void);

/* Set the SDL_Window to use as parent for dialogs (pass NULL to clear).
 * Call this after creating the window so file dialogs associate correctly.
 * Takes void* to avoid pulling SDL headers into every caller. */
void lv_platform_dialogs_set_window(void *window);

/* Show an open file dialog
 * title - Dialog title
 * filter_name - Filter name (e.g., "Log Files")
 * filter_ext - Filter extension (e.g., "*.wbv")
 * default_ext - Default extension if user doesn't type one
 * out_path - Buffer to store selected path
 * out_size - Size of output buffer
 * initial_dir - Initial directory (can be NULL)
 * Returns PLATFORM_DIALOG_OK, PLATFORM_DIALOG_CANCEL, or PLATFORM_DIALOG_ERROR */
int lv_platform_dialog_open_file(const char* title, 
                              const char* filter_name,
                              const char* filter_ext,
                              const char* default_ext,
                              char* out_path, 
                              size_t out_size,
                              const char* initial_dir);

/* Show a save file dialog
 * title - Dialog title
 * filter_name - Filter name (e.g., "Map Files")
 * filter_ext - Filter extension (e.g., "*.map")
 * default_ext - Default extension if user doesn't type one
 * default_name - Default file name (can be NULL)
 * out_path - Buffer to store selected path
 * out_size - Size of output buffer
 * initial_dir - Initial directory (can be NULL)
 * Returns PLATFORM_DIALOG_OK, PLATFORM_DIALOG_CANCEL, or PLATFORM_DIALOG_ERROR */
int lv_platform_dialog_save_file(const char* title,
                              const char* filter_name,
                              const char* filter_ext,
                              const char* default_ext,
                              const char* default_name,
                              char* out_path,
                              size_t out_size,
                              const char* initial_dir);

/* Show a simple message box
 * title - Message box title
 * message - Message to display
 * Returns 1 on success, 0 on failure */
int lv_platform_dialog_message(const char* title, const char* message);

/* Show an error message box
 * title - Message box title
 * message - Error message to display
 * Returns 1 on success, 0 on failure */
int lv_platform_dialog_error(const char* title, const char* message);

/* Show a yes/no question dialog
 * title - Dialog title
 * message - Question to display
 * Returns 1 for Yes, 0 for No, -1 for error/cancel */
int lv_platform_dialog_question(const char* title, const char* message);

#endif /* PLATFORM_DIALOGS_H */