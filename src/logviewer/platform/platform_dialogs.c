/*
 * platform_dialogs.c - Cross-platform file dialogs
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Windows implementation uses Win32 common dialogs.
 * Other platforms can use SDL dialogs or zenity/kdialog.
 */

#include "platform_dialogs.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#else
#include <SDL3/SDL.h>
#endif

static int g_initialized = 0;

#ifndef _WIN32
static SDL_Window *g_dialog_window = NULL;
#endif

int platform_dialogs_init(void) {
    g_initialized = 1;
    return 1;
}

void platform_dialogs_shutdown(void) {
    g_initialized = 0;
#ifndef _WIN32
    g_dialog_window = NULL;
#endif
}

void platform_dialogs_set_window(void *window) {
#ifndef _WIN32
    g_dialog_window = (SDL_Window *)window;
#else
    (void)window;
#endif
}

#ifdef _WIN32

/* Helper to build filter string for Windows dialogs */
static void build_filter_string(char* filter, size_t filter_size,
                                const char* name, const char* ext) {
    /* Windows filter format: "Name\0*.ext\0" */
    memset(filter, 0, filter_size);
    snprintf(filter, filter_size - 2, "%s", name);
    size_t name_len = strlen(name);
    filter[name_len] = '\0';
    snprintf(filter + name_len + 1, filter_size - name_len - 2, "%s", ext);
}

int platform_dialog_open_file(const char* title, 
                              const char* filter_name,
                              const char* filter_ext,
                              const char* default_ext,
                              char* out_path, 
                              size_t out_size,
                              const char* initial_dir) {
    OPENFILENAMEA ofn;
    char filter[256];
    
    if (!g_initialized) return PLATFORM_DIALOG_ERROR;
    
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;  /* No parent window for now */
    ofn.hInstance = NULL;
    
    build_filter_string(filter, sizeof(filter), filter_name, filter_ext);
    ofn.lpstrFilter = filter;
    ofn.lpstrCustomFilter = NULL;
    ofn.nFilterIndex = 1;
    
    ofn.lpstrFile = out_path;
    out_path[0] = '\0';
    ofn.nMaxFile = (DWORD)min(out_size, (size_t)MAXDWORD);
    
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = initial_dir;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    ofn.lpstrDefExt = default_ext;
    
    if (GetOpenFileNameA(&ofn)) {
        return PLATFORM_DIALOG_OK;
    }
    return PLATFORM_DIALOG_CANCEL;
}

int platform_dialog_save_file(const char* title,
                              const char* filter_name,
                              const char* filter_ext,
                              const char* default_ext,
                              const char* default_name,
                              char* out_path,
                              size_t out_size,
                              const char* initial_dir) {
    OPENFILENAMEA ofn;
    char filter[256];
    
    if (!g_initialized) return PLATFORM_DIALOG_ERROR;
    
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.hInstance = NULL;
    
    build_filter_string(filter, sizeof(filter), filter_name, filter_ext);
    ofn.lpstrFilter = filter;
    ofn.lpstrCustomFilter = NULL;
    ofn.nFilterIndex = 1;
    
    ofn.lpstrFile = out_path;
    if (default_name) {
        strncpy(out_path, default_name, out_size - 1);
        out_path[out_size - 1] = '\0';
    } else {
        out_path[0] = '\0';
    }
    ofn.nMaxFile = (DWORD)min(out_size, (size_t)MAXDWORD);
    
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = initial_dir;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = default_ext;
    
    if (GetSaveFileNameA(&ofn)) {
        return PLATFORM_DIALOG_OK;
    }
    return PLATFORM_DIALOG_CANCEL;
}

int platform_dialog_message(const char* title, const char* message) {
    MessageBoxA(NULL, message, title, MB_OK | MB_ICONINFORMATION);
    return 1;
}

int platform_dialog_error(const char* title, const char* message) {
    MessageBoxA(NULL, message, title, MB_OK | MB_ICONERROR);
    return 1;
}

int platform_dialog_question(const char* title, const char* message) {
    int result = MessageBoxA(NULL, message, title, MB_YESNO | MB_ICONQUESTION);
    if (result == IDYES) return 1;
    if (result == IDNO) return 0;
    return -1;
}

#else /* Non-Windows platforms */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* Try to run a command via popen and read one line of output.
 * Returns 1 if a non-empty line was read, 0 otherwise. */
static int run_popen_dialog(const char *cmd, char *out_path, size_t out_size) {
    FILE *f = popen(cmd, "r");
    if (!f) return 0;
    char *r = fgets(out_path, (int)out_size, f);
    int status = pclose(f);
    if (!r || status != 0) {
        out_path[0] = '\0';
        return 0;
    }
    /* Strip trailing newline */
    size_t len = strlen(out_path);
    if (len > 0 && out_path[len - 1] == '\n') out_path[len - 1] = '\0';
    return out_path[0] != '\0';
}

/* Check if a command is available on PATH */
static int command_exists(const char *cmd) {
    char buf[256];
    snprintf(buf, sizeof(buf), "command -v %s >/dev/null 2>&1", cmd);
    return system(buf) == 0;
}

/* Build a zenity open-file command */
static void build_zenity_open_cmd(char *buf, size_t buf_size,
                                  const char *title, const char *filter_ext) {
    /* filter_ext comes in as "*.WBV" — zenity wants the bare pattern */
    snprintf(buf, buf_size,
             "zenity --file-selection --title=\"%s\" --file-filter=\"%s\" 2>/dev/null",
             title, filter_ext);
}

/* Build a zenity save-file command */
static void build_zenity_save_cmd(char *buf, size_t buf_size,
                                  const char *title, const char *default_name) {
    if (default_name && default_name[0]) {
        snprintf(buf, buf_size,
                 "zenity --file-selection --save --confirm-overwrite"
                 " --title=\"%s\" --filename=\"%s\" 2>/dev/null",
                 title, default_name);
    } else {
        snprintf(buf, buf_size,
                 "zenity --file-selection --save --confirm-overwrite"
                 " --title=\"%s\" 2>/dev/null",
                 title);
    }
}

/* Build a kdialog open-file command */
static void build_kdialog_open_cmd(char *buf, size_t buf_size,
                                   const char *title, const char *filter_ext) {
    snprintf(buf, buf_size,
             "kdialog --getopenfilename . \"%s\" --title \"%s\" 2>/dev/null",
             filter_ext, title);
}

/* Build a kdialog save-file command */
static void build_kdialog_save_cmd(char *buf, size_t buf_size,
                                   const char *title, const char *filter_ext,
                                   const char *default_name) {
    if (default_name && default_name[0]) {
        snprintf(buf, buf_size,
                 "kdialog --getsavefilename \"%s\" \"%s\" --title \"%s\" 2>/dev/null",
                 default_name, filter_ext, title);
    } else {
        snprintf(buf, buf_size,
                 "kdialog --getsavefilename . \"%s\" --title \"%s\" 2>/dev/null",
                 filter_ext, title);
    }
}

#ifdef __APPLE__
/* Strip "*.ext" glob prefix and lowercase — osascript wants bare extension e.g. "wbv" */
static void strip_glob_ext(const char *filter_ext, char *out, size_t size) {
    const char *p = filter_ext;
    while (*p == '*' || *p == '.') p++;
    size_t i = 0;
    while (*p && i < size - 1) {
        out[i++] = (char)tolower((unsigned char)*p++);
    }
    out[i] = '\0';
}

static void build_osascript_open_cmd(char *buf, size_t buf_size,
                                     const char *title, const char *filter_ext) {
    char ext[32];
    strip_glob_ext(filter_ext, ext, sizeof(ext));
    snprintf(buf, buf_size,
             "osascript -e 'POSIX path of"
             " (choose file of type {\"%s\"} with prompt \"%s\")' 2>/dev/null",
             ext, title);
}

static void build_osascript_save_cmd(char *buf, size_t buf_size,
                                     const char *title, const char *default_name) {
    if (default_name && default_name[0]) {
        snprintf(buf, buf_size,
                 "osascript -e 'POSIX path of"
                 " (choose file name default name \"%s\" with prompt \"%s\")' 2>/dev/null",
                 default_name, title);
    } else {
        snprintf(buf, buf_size,
                 "osascript -e 'POSIX path of"
                 " (choose file name with prompt \"%s\")' 2>/dev/null",
                 title);
    }
}
#endif /* __APPLE__ */

/* SDL3 async dialog fallback */
typedef struct {
    char *out_path;
    size_t out_size;
    int result;
    volatile int done;
} DialogState;

static void SDLCALL open_file_callback(void *userdata,
                                       const char * const *filelist,
                                       int filter_index) {
    (void)filter_index;
    DialogState *state = (DialogState *)userdata;
    if (filelist && filelist[0]) {
        strncpy(state->out_path, filelist[0], state->out_size - 1);
        state->out_path[state->out_size - 1] = '\0';
        state->result = PLATFORM_DIALOG_OK;
    } else {
        state->out_path[0] = '\0';
        state->result = PLATFORM_DIALOG_CANCEL;
    }
    state->done = 1;
}

static void SDLCALL save_file_callback(void *userdata,
                                       const char * const *filelist,
                                       int filter_index) {
    (void)filter_index;
    DialogState *state = (DialogState *)userdata;
    if (filelist && filelist[0]) {
        strncpy(state->out_path, filelist[0], state->out_size - 1);
        state->out_path[state->out_size - 1] = '\0';
        state->result = PLATFORM_DIALOG_OK;
    } else {
        state->out_path[0] = '\0';
        state->result = PLATFORM_DIALOG_CANCEL;
    }
    state->done = 1;
}

static int sdl_open_file(const char *filter_name, const char *filter_ext,
                         char *out_path, size_t out_size) {
    DialogState state = { out_path, out_size, PLATFORM_DIALOG_CANCEL, 0 };
    SDL_DialogFileFilter filters[] = {
        { filter_name, filter_ext },
        { NULL, NULL }
    };
    SDL_ShowOpenFileDialog(open_file_callback, &state, g_dialog_window, filters, 1, NULL, false);
    while (!state.done) {
        SDL_Event e;
        SDL_WaitEventTimeout(&e, 100);
    }
    return state.result;
}

static int sdl_save_file(const char *filter_name, const char *filter_ext,
                         char *out_path, size_t out_size) {
    DialogState state = { out_path, out_size, PLATFORM_DIALOG_CANCEL, 0 };
    SDL_DialogFileFilter filters[] = {
        { filter_name, filter_ext },
        { NULL, NULL }
    };
    SDL_ShowSaveFileDialog(save_file_callback, &state, g_dialog_window, filters, 1, NULL);
    while (!state.done) {
        SDL_Event e;
        SDL_WaitEventTimeout(&e, 100);
    }
    return state.result;
}

int platform_dialog_open_file(const char* title,
                              const char* filter_name,
                              const char* filter_ext,
                              const char* default_ext,
                              char* out_path,
                              size_t out_size,
                              const char* initial_dir) {
    (void)default_ext; (void)initial_dir;
    if (!g_initialized) return PLATFORM_DIALOG_ERROR;

    char cmd[1024];

#ifdef __APPLE__
    build_osascript_open_cmd(cmd, sizeof(cmd), title, filter_ext);
    if (run_popen_dialog(cmd, out_path, out_size))
        return PLATFORM_DIALOG_OK;
    return PLATFORM_DIALOG_CANCEL;
#else
    if (command_exists("zenity")) {
        build_zenity_open_cmd(cmd, sizeof(cmd), title, filter_ext);
        if (run_popen_dialog(cmd, out_path, out_size))
            return PLATFORM_DIALOG_OK;
        /* zenity ran but user cancelled */
        return PLATFORM_DIALOG_CANCEL;
    }

    if (command_exists("kdialog")) {
        build_kdialog_open_cmd(cmd, sizeof(cmd), title, filter_ext);
        if (run_popen_dialog(cmd, out_path, out_size))
            return PLATFORM_DIALOG_OK;
        return PLATFORM_DIALOG_CANCEL;
    }

    /* Last resort: SDL3 portal-based dialog */
    return sdl_open_file(filter_name, filter_ext, out_path, out_size);
#endif
}

int platform_dialog_save_file(const char* title,
                              const char* filter_name,
                              const char* filter_ext,
                              const char* default_ext,
                              const char* default_name,
                              char* out_path,
                              size_t out_size,
                              const char* initial_dir) {
    (void)default_ext; (void)initial_dir;
    if (!g_initialized) return PLATFORM_DIALOG_ERROR;

    if (default_name) {
        strncpy(out_path, default_name, out_size - 1);
        out_path[out_size - 1] = '\0';
    } else {
        out_path[0] = '\0';
    }

    char cmd[1024];

#ifdef __APPLE__
    build_osascript_save_cmd(cmd, sizeof(cmd), title, out_path[0] ? out_path : default_name);
    if (run_popen_dialog(cmd, out_path, out_size))
        return PLATFORM_DIALOG_OK;
    return PLATFORM_DIALOG_CANCEL;
#else
    if (command_exists("zenity")) {
        build_zenity_save_cmd(cmd, sizeof(cmd), title, out_path[0] ? out_path : default_name);
        if (run_popen_dialog(cmd, out_path, out_size))
            return PLATFORM_DIALOG_OK;
        return PLATFORM_DIALOG_CANCEL;
    }

    if (command_exists("kdialog")) {
        build_kdialog_save_cmd(cmd, sizeof(cmd), title, filter_ext,
                               out_path[0] ? out_path : default_name);
        if (run_popen_dialog(cmd, out_path, out_size))
            return PLATFORM_DIALOG_OK;
        return PLATFORM_DIALOG_CANCEL;
    }

    /* Last resort: SDL3 portal-based dialog */
    return sdl_save_file(filter_name, filter_ext, out_path, out_size);
#endif
}

int platform_dialog_message(const char* title, const char* message) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, message, NULL);
    return 1;
}

int platform_dialog_error(const char* title, const char* message) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, NULL);
    return 1;
}

int platform_dialog_question(const char* title, const char* message) {
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes" },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No"  },
    };
    const SDL_MessageBoxData data = {
        SDL_MESSAGEBOX_INFORMATION, NULL,
        title, message,
        2, buttons, NULL
    };
    int button_id = -1;
    SDL_ShowMessageBox(&data, &button_id);
    return button_id;
}

#endif /* _WIN32 */