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
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOADBROWSER_MAX_SESSIONS 512
#define LOADBROWSER_MAX_NAME     64   /* LoadSessionEntry::name capacity */

typedef struct {
    char   dir[300];        /* path, e.g. "debug_sessions/20260624_163153" */
    char   name[LOADBROWSER_MAX_NAME];  /* basename, for display */
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

/* ---- Segment walking (braintest_loadbrowser_segment.c, plain C) --------
 * A recording reaches disk as 15-minute BLOCKS ("<ts>_<B>[_<label>]", the
 * recorder) that brainrec_split may further cut into PARTS
 * ("<block>_part<X>of<N>"). These let the browser offer "next / previous
 * segment" instead of making the user read dir names. */

/* Basename of a session path: the text after the last '/' or '\', with any
 * trailing separators ignored. Always NUL-terminates (empty on bad input). */
void loadBrowserBaseName(const char *path, char *out, size_t outSz);

/* True when two paths name the same session — the browser stores
 * "debug_sessions/<name>" while -loadsession may carry any prefix or
 * separator style, so only the basenames are compared (case-insensitively
 * on Windows). */
bool loadBrowserSameSession(const char *a, const char *b);

/* Index in `list` of the segment that follows (direction +1) or precedes
 * (direction -1) the session `loadedDir`, or -1 when there is none.
 *   - inside a split block, the neighbouring _part<X>of<N>;
 *   - off either end of the parts (or from an unsplit block dir), the
 *     neighbouring block <B±1> — preferring that block's _part1of<M> going
 *     forward / _part<M>of<M> going back, else the block dir itself. */
int loadBrowserFindSegment(const LoadSessionEntry *list, int count,
                           const char *loadedDir, int direction);

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_LOADBROWSER_H */
