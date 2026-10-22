/*
 * platform_dialogs_ios.c - Log viewer dialogs for the iOS client
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * The iOS driver for platform_dialogs.h, in place of
 * platform/platform_dialogs.c, whose non-Windows file pickers run osascript,
 * zenity or kdialog through system() and popen(), which iOS does not have.
 * The iOS client only embeds the reel (lv_embed.c), which never asks for a
 * file: it is handed the round's bytes. So the two file pickers answer
 * "cancelled", and the message boxes go through SDL, which shows them as
 * native alerts on iOS. Same shape as src/logviewer/wasm/
 * platform_dialogs_wasm.c, the web client's driver.
 */

#include "../platform/platform_dialogs.h"
#include "../../gui/lang.h"

#include <SDL3/SDL.h>

static SDL_Window *g_window = NULL;

int lv_platform_dialogs_init(void) {
    return 1;
}

void lv_platform_dialogs_shutdown(void) {
    g_window = NULL;
}

void lv_platform_dialogs_set_window(void *window) {
    g_window = (SDL_Window *)window;
}

/* No file picker: nothing in the embedded reel opens one. */
int lv_platform_dialog_open_file(const char *title,
                                 const char *filter_name,
                                 const char *filter_ext,
                                 const char *default_ext,
                                 char *out_path,
                                 size_t out_size,
                                 const char *initial_dir) {
    (void)title; (void)filter_name; (void)filter_ext; (void)default_ext;
    (void)initial_dir;
    if (out_path != NULL && out_size > 0) out_path[0] = '\0';
    return PLATFORM_DIALOG_CANCEL;
}

/* No file picker: nothing in the embedded reel saves one. */
int lv_platform_dialog_save_file(const char *title,
                                 const char *filter_name,
                                 const char *filter_ext,
                                 const char *default_ext,
                                 const char *default_name,
                                 char *out_path,
                                 size_t out_size,
                                 const char *initial_dir) {
    (void)title; (void)filter_name; (void)filter_ext; (void)default_ext;
    (void)default_name; (void)initial_dir;
    if (out_path != NULL && out_size > 0) out_path[0] = '\0';
    return PLATFORM_DIALOG_CANCEL;
}

int lv_platform_dialog_message(const char *title, const char *message) {
    return SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, message,
                                    g_window) ? 1 : 0;
}

int lv_platform_dialog_error(const char *title, const char *message) {
    return SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message,
                                    g_window) ? 1 : 0;
}

int lv_platform_dialog_question(const char *title, const char *message) {
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, langGetText(STR_YES) },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, langGetText(STR_NO)  },
    };
    const SDL_MessageBoxData data = {
        SDL_MESSAGEBOX_INFORMATION, g_window,
        title, message,
        2, buttons, NULL
    };
    int buttonId = -1;
    if (!SDL_ShowMessageBox(&data, &buttonId)) return -1;
    return buttonId;
}
