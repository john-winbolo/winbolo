/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * mobile_recordings_platform.h - The platform half of the recordings screen
 *
 * What mobile_recordings.cpp asks of the platform: which directories hold
 * recordings, the recording the system handed over, the release of a file
 * another provider opened in place, and the export that takes a recording
 * off the phone. iOS answers from src/gui/ios/ios_recordings.cpp (the
 * preferences directory, Documents and Documents/Inbox; the in-place access
 * ios_open_url.m started; Copy to Files into Documents). Android answers from
 * src/android/android_recordings.c (the preferences directory, where the
 * round recorder writes, and its incoming subdirectory, where a recording
 * handed over as a content:// stream is copied; nothing to release; the
 * share sheet).
 */

#ifndef MOBILE_RECORDINGS_PLATFORM_H
#define MOBILE_RECORDINGS_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A directory the screen lists. path ends in a separator. inPrefs marks the
   preferences directory, where the recorder writes: the live single-player
   recording there is left out and the last finished single-player round is
   shown under a name of its own. */
typedef struct {
    char path[FILENAME_MAX];
    bool inPrefs;
} MobileRecordingsDir;

/* Fills out with the directories to list, at most max of them, and returns
   how many. A directory the platform cannot name is left out. */
int mobileRecordingsPlatformDirs(MobileRecordingsDir *out, int max);

/* Takes the path of a recording the system handed over (a file opened from
   another app), if one is kept. */
bool mobileRecordingsPlatformTakeDrop(char *out, size_t size);

/* Called once a recording has been read, or failed to open. iOS ends the
   access a file opened in place from another provider was given; Android
   has nothing to end. */
void mobileRecordingsPlatformRelease(const char *path);

/* Whether a listed recording gets the export button: iOS offers Copy to
   Files for the preferences directory's recordings only (the others are
   already in Documents); Android offers the share sheet for every one. */
bool mobileRecordingsPlatformExportShown(bool inPrefs);

/* The export button's text, from the language file. */
const char *mobileRecordingsPlatformExportLabel(void);

/* The export: iOS keeps a dated copy in Documents (modified dates the copy's
   name); Android hands the file to the share sheet. Returns false when the
   export could not be started. */
bool mobileRecordingsPlatformExport(const char *path, SDL_Time modified);

/* What the screen says under its title after an export, or NULL for
   nothing: iOS reports where the copy went or that it failed; Android's
   share sheet is its own feedback. */
const char *mobileRecordingsPlatformExportNotice(bool ok);

#ifdef __cplusplus
}
#endif

#endif /* MOBILE_RECORDINGS_PLATFORM_H */
