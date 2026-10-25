/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          android_mapeditor_files.c
 * Purpose:
 *  The Android half of the map editor's file picker
 *  (src/mapeditor/mapeditor_mobile_files.cpp, through
 *  mapeditor_mobile_files_platform.h).
 *
 *  The player's files are the Maps folder under SDL's
 *  preferences path (the app's files directory), the same
 *  folder the in-game Save Map writes into
 *  (android_frontend.c); it is made on first use. The
 *  bundled maps are data/maps under the working directory,
 *  where main_android.c extracts the APK's data tree and
 *  chdir's to, named by its full path so a chosen map's
 *  path does not depend on the working directory.
 *********************************************************/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "../mapeditor/mapeditor_mobile_files_platform.h"
#include "android_device.h"

bool meMobileFilesPlatformUserDir(char *out, size_t cap) {
  char *pref = SDL_GetPrefPath("WinBolo", "WinBolo");
  if (pref == NULL || pref[0] == '\0' || cap == 0) {
    SDL_free(pref);
    return false;
  }
  snprintf(out, cap, "%sMaps", pref);
  SDL_free(pref);
  if (!SDL_GetPathInfo(out, NULL) && !SDL_CreateDirectory(out)) {
    return false;
  }
  return true;
}

bool meMobileFilesPlatformBundledDir(char *out, size_t cap) {
  return androidDeviceAppPath("data/maps", out, cap);
}
