/*
 * platform_dialogs_android.c - Log viewer dialogs for the Android client
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * The Android driver for platform_dialogs.h, in place of
 * platform/platform_dialogs.c, whose non-Windows file pickers run osascript,
 * zenity or kdialog through system() and popen(), which Android does not
 * have. The Android client only embeds the reel (lv_embed.c), which never
 * asks for a file: it is handed the round's bytes. So the two file pickers
 * answer "cancelled", and the message boxes go through the client's ImGui
 * message box, whose Yes and No come from the language file. Same shape as
 * src/logviewer/ios/platform_dialogs_ios.c, the iOS driver.
 */

#include "../platform/platform_dialogs.h"
#include "../../gui/sdl3/dialogs/imgui_messagebox.h"

#include <SDL3/SDL.h>

static SDL_Window *g_window = NULL;

int lv_platform_dialogs_init(void) {
    return 1;
}

void lv_platform_dialogs_shutdown(void) {
    g_window = NULL;
}

/* The ImGui message box draws into the client's one window, so the parent
 * is kept only for the record. */
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
    imguiMessageBoxEx(title, message, IMGUI_MSG_INFO, IMGUI_MSG_OK);
    return 1;
}

int lv_platform_dialog_error(const char *title, const char *message) {
    imguiMessageBoxEx(title, message, IMGUI_MSG_ERROR, IMGUI_MSG_OK);
    return 1;
}

/* 1 for Yes, 0 for No, as the header says; the box has no third button. */
int lv_platform_dialog_question(const char *title, const char *message) {
    int r = imguiMessageBoxEx(title, message, IMGUI_MSG_INFO, IMGUI_MSG_YES_NO);
    return (r == IMGUI_MSG_RESULT_YES) ? 1 : 0;
}
