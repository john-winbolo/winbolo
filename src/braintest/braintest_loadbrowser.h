/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * BrainTest "Load Session" browser — lists debug_sessions/
 * recordings (winbolods brainrec.btr) and lets the user pick
 * one to replay. Loading relaunches BrainTest with
 * -loadsession <dir> and exits the current game.
 *
 * The ImGui rendering lives in the .cpp (C++); the scan +
 * relaunch logic is C in braintest_main.c. This header is the
 * C/C++ bridge.
 *********************************************************/
#ifndef BRAINTEST_LOADBROWSER_H
#define BRAINTEST_LOADBROWSER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOADBROWSER_MAX_SESSIONS 512

typedef struct {
    char   dir[300];        /* path, e.g. "debug_sessions/20260624_163153" */
    char   name[64];        /* basename, for display */
    char   map[64];         /* recorded map name */
    int    bots;            /* recorded player/bot count */
    int    durationSec;     /* wall-clock game length, -1 if unknown */
    double sizeMB;          /* brainrec.btr size */
    bool   loadable;        /* valid, version-matching brainrec.btr present */
    char   note[64];        /* reason shown when not loadable */
} LoadSessionEntry;

/* Render the browser window. *open toggles visibility (Cancel clears it).
 * Returns the index in `list` the user clicked "Load" on this frame, else -1.
 * loadedDir: the -loadsession dir this instance is replaying ("" / NULL when
 * none) — its row is highlighted and scrolled into view when the window
 * opens, so stepping to the next part of a split session is one glance. */
int loadBrowserRender(bool *open, const LoadSessionEntry *list, int count,
                      const char *loadedDir);

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_LOADBROWSER_H */
