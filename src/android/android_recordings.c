/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          android_recordings.c
 * Purpose:
 *  The Android half of the recordings screen
 *  (src/gui/mobile/mobile_recordings.cpp, through
 *  mobile_recordings_platform.h).
 *
 *  Where recordings are looked for:
 *  - the preferences directory (SDL's pref path, the app's
 *    files directory), where gamefront.c writes
 *    singleplayer.wbv and the round recorder writes
 *    <stamp>_<Map>.wbv for every round it records.
 *  - the incoming directory under it, where WinBoloActivity
 *    copies a recording another app hands over
 *    (incoming/incoming.wbv).
 *  A recording leaves the phone through the share sheet
 *  (android_share.c), offered for every listed one.
 *********************************************************/

#include <SDL3/SDL.h>
#include <stdio.h>

#include "../gui/lang.h"
#include "../gui/mobile/mobile_recordings_platform.h"
#include "android_drops.h"
#include "android_share.h"

/* The directory WinBoloActivity copies a handed-over recording into, under
   the preferences directory (the files directory). */
#define ANDROID_INCOMING_DIR "incoming/"

int mobileRecordingsPlatformDirs(MobileRecordingsDir *out, int max) {
  int n = 0;
  char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
  if (prefDir != NULL) {
    if (n < max) {
      SDL_strlcpy(out[n].path, prefDir, sizeof(out[n].path));
      out[n].inPrefs = true;
      n++;
    }
    if (n < max) {
      snprintf(out[n].path, sizeof(out[n].path), "%s%s", prefDir,
               ANDROID_INCOMING_DIR);
      out[n].inPrefs = false;
      n++;
    }
    SDL_free(prefDir);
  }
  return n;
}

bool mobileRecordingsPlatformTakeDrop(char *out, size_t size) {
  return androidDropsTakeLog(out, size);
}

/* Every file the screen opens is the app's own (a handed-over recording was
   copied into the incoming directory), so there is no access to end. */
void mobileRecordingsPlatformRelease(const char *path) {
  (void)path;
}

bool mobileRecordingsPlatformExportShown(bool inPrefs) {
  (void)inPrefs;
  return true;
}

const char *mobileRecordingsPlatformExportLabel(void) {
  return langGetText(STR_ANDROIDREC_SHARE);
}

bool mobileRecordingsPlatformExport(const char *path, SDL_Time modified) {
  (void)modified;
  return androidShareFile(path, "application/octet-stream");
}

/* The share sheet is its own feedback; a failure to start it is logged by
   androidShareFile. */
const char *mobileRecordingsPlatformExportNotice(bool ok) {
  (void)ok;
  return NULL;
}
