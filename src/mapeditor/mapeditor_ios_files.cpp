/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_ios_files.cpp
 * Purpose:       The iOS half of the map editor's file picker
 *                (mapeditor_mobile_files.cpp, through
 *                mapeditor_mobile_files_platform.h): the
 *                player's files are the app's Documents
 *                folder, which the Files app shows under
 *                WinBolo, and the bundled maps are data/maps
 *                in the app bundle.
 *********************************************************/

#include <SDL3/SDL.h>
#include <string.h>

#include "mapeditor_mobile_files_platform.h"

bool meMobileFilesPlatformUserDir(char *out, size_t cap) {
    const char *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    if (!docs || !docs[0] || cap == 0) return false;
    SDL_strlcpy(out, docs, cap);
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = '\0';
    return true;
}

bool meMobileFilesPlatformBundledDir(char *out, size_t cap) {
    const char *base = SDL_GetBasePath();
    if (cap == 0) return false;
    SDL_snprintf(out, cap, "%sdata/maps", base ? base : "");
    return true;
}
