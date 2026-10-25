/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          iOS Recordings
*Filename:      ios_recordings.cpp
*Purpose:
*  The iOS half of the recordings screen
*  (src/gui/mobile/mobile_recordings.cpp, through
*  mobile_recordings_platform.h).
*
*  Where recordings are looked for:
*  - the preferences directory, where gamefront.c writes
*    singleplayer.wbv and the round recorder writes the
*    rounds of a hosted game.
*  - Documents, which the Files app and Finder file sharing
*    show as the WinBolo folder (UIFileSharingEnabled and
*    LSSupportsOpeningDocumentsInPlace in Info.plist), and
*    Documents/Inbox, where iOS puts a file another app such
*    as Mail sends over.
*
*  The recorder keeps writing to the preferences directory.
*  Moving it to Documents would put the live recording and
*  the one-round singleplayer.wbv, which the next round
*  overwrites, in the Files app; instead a recording from
*  the preferences directory has a Copy to Files button that
*  keeps a dated copy in Documents, where the player can
*  take it off the phone.
*********************************************************/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <string>

extern "C" {
#include "../lang.h"
}

#include "ios_drops.h"
#include "../mobile/mobile_recordings_platform.h"

/* The Documents directory with a trailing separator, or "" when SDL cannot
 * name it. */
static std::string iosRecordingsDocumentsDir(void) {
    const char *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    if (docs == NULL) return std::string();
    std::string dir(docs);
    if (!dir.empty() && dir[dir.size() - 1] != '/') dir += '/';
    return dir;
}

int mobileRecordingsPlatformDirs(MobileRecordingsDir *out, int max) {
    int n = 0;
    char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (prefDir != NULL) {
        if (n < max) {
            SDL_strlcpy(out[n].path, prefDir, sizeof(out[n].path));
            out[n].inPrefs = true;
            n++;
        }
        SDL_free(prefDir);
    }
    std::string docs = iosRecordingsDocumentsDir();
    if (!docs.empty()) {
        if (n < max) {
            SDL_strlcpy(out[n].path, docs.c_str(), sizeof(out[n].path));
            out[n].inPrefs = false;
            n++;
        }
        if (n < max) {
            SDL_strlcpy(out[n].path, (docs + "Inbox/").c_str(), sizeof(out[n].path));
            out[n].inPrefs = false;
            n++;
        }
    }
    return n;
}

bool mobileRecordingsPlatformTakeDrop(char *out, size_t size) {
    return iosDropsTakeLog(out, size);
}

/* A file opened in place from another provider was readable only for the
 * read; see ios_open_url.m. */
void mobileRecordingsPlatformRelease(const char *path) {
    iosOpenUrlRelease(path);
}

/* The copy is offered for the preferences directory's recordings; the
 * others are already in Documents. */
bool mobileRecordingsPlatformExportShown(bool inPrefs) {
    return inPrefs;
}

const char *mobileRecordingsPlatformExportLabel(void) {
    return langGetText(STR_IOSREC_COPY);
}

/* Keeps a dated copy of a recording from the preferences directory in
 * Documents: "<name>-YYYYMMDD-HHMMSS.wbv", dated by the recording itself so a
 * second copy of the same round lands on the same name. */
bool mobileRecordingsPlatformExport(const char *path, SDL_Time modified) {
    std::string docs = iosRecordingsDocumentsDir();
    if (docs.empty()) return false;
    const char *base = strrchr(path, '/');
    base = (base != NULL) ? base + 1 : path;
    std::string stem(base);
    if (stem.size() > 4) stem.resize(stem.size() - 4);   /* drop ".wbv" */
    SDL_DateTime dt;
    char stamp[32] = "";
    if (SDL_TimeToDateTime(modified, &dt, true)) {
        snprintf(stamp, sizeof(stamp), "-%04d%02d%02d-%02d%02d%02d",
                 dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    }
    std::string dest = docs + stem + stamp + ".wbv";
    return SDL_CopyFile(path, dest.c_str());
}

const char *mobileRecordingsPlatformExportNotice(bool ok) {
    return ok ? langGetText(STR_IOSREC_COPIED)
              : langGetText(STR_IOSREC_COPY_FAILED);
}
