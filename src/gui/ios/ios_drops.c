/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          iOS Drops
*Filename:      ios_drops.c
*Purpose:
*  The kept recording and game link; see ios_drops.h.
*********************************************************/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "../gamefront.h"
#include "../../common/wb_log.h"
#include "ios_drops.h"

/* The latest of each kind wins: a second file opened before the welcome
 * screen ran is the one the player asked for last. Written by the event
 * watch and read by the welcome and recordings screens; UIKit delivers drops
 * on the main thread, which is the thread those screens run on, but the lock
 * keeps that from mattering. */
static SDL_Mutex *s_dropLock = NULL;
static char s_dropLog[FILENAME_MAX];
static char s_dropLink[512];

/* True when name ends in ".wbv", in any case. */
static bool iosDropsIsWbv(const char *name) {
    size_t len = strlen(name);
    return len > 4 && SDL_strcasecmp(name + len - 4, ".wbv") == 0;
}

/* The watch: keeps a .wbv path or a winbolo:// link from a drop. Every other
 * event, and any other drop, passes untouched. Its answer is ignored. */
static bool SDLCALL iosDropsWatch(void *userdata, SDL_Event *ev) {
    (void)userdata;
    if (ev->type != SDL_EVENT_DROP_FILE || ev->drop.data == NULL) {
        return true;
    }
    SDL_LockMutex(s_dropLock);
    if (SDL_strncmp(ev->drop.data, "winbolo://", 10) == 0) {
        SDL_strlcpy(s_dropLink, ev->drop.data, sizeof(s_dropLink));
        WB_LOG_INFO(WB_LOG_CAT_GUI, "[URL] Kept winbolo:// link for the menu: %s",
                    ev->drop.data);
    } else if (iosDropsIsWbv(ev->drop.data)) {
        /* A newer file replaces one not yet read, which will now never be
         * read: end the access ios_open_url.m started for it. Not when it is
         * the same file again, whose access the new open has just started. */
        if (s_dropLog[0] != '\0' && strcmp(s_dropLog, ev->drop.data) != 0) {
            iosOpenUrlRelease(s_dropLog);
        }
        SDL_strlcpy(s_dropLog, ev->drop.data, sizeof(s_dropLog));
        WB_LOG_INFO(WB_LOG_CAT_GUI, "Kept recording to play: %s", ev->drop.data);
    }
    SDL_UnlockMutex(s_dropLock);
    return true;
}

void iosDropsInstall(void) {
    /* The watch list belongs to the events subsystem, which nothing has
     * brought up this early in main; this reference keeps it up for the
     * process. */
    SDL_InitSubSystem(SDL_INIT_EVENTS);
    s_dropLock = SDL_CreateMutex();
    if (!SDL_AddEventWatch(iosDropsWatch, NULL)) {
        WB_LOG_WARN(WB_LOG_CAT_GUI, "Could not watch for opened files: %s",
                    SDL_GetError());
    }
}

int iosDropsWelcomeResult(void) {
    char link[sizeof(s_dropLink)];
    bool haveLog;

    SDL_LockMutex(s_dropLock);
    SDL_strlcpy(link, s_dropLink, sizeof(link));
    s_dropLink[0] = '\0';
    haveLog = (s_dropLog[0] != '\0');
    SDL_UnlockMutex(s_dropLock);

    if (link[0] != '\0') {
        gameFrontHandleUrlOpen(link);
        return (int)openInternetManual;
    }
    if (haveLog) {
        return (int)openLogViewer;
    }
    return -1;
}

bool iosDropsTakeLog(char *out, size_t size) {
    bool have;
    SDL_LockMutex(s_dropLock);
    have = (s_dropLog[0] != '\0');
    if (have) {
        SDL_strlcpy(out, s_dropLog, size);
        s_dropLog[0] = '\0';
    }
    SDL_UnlockMutex(s_dropLock);
    return have;
}
