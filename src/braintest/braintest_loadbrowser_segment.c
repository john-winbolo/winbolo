/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * BrainTest "Load Session" browser — next/prev segment.
 *
 * A long recording arrives on disk in two nested pieces:
 *
 *   BLOCKS   the recorder rolls to a fresh dir every 15 wall-clock
 *            minutes (server_lifecycle.c), giving
 *            "<YYYYMMDD_HHMMSS>_<B>[_<label>]" — the timestamp is the
 *            GAME's start, only <B> moves;
 *   PARTS    brainrec_split turns one such dir into
 *            "<block name>_part<X>of<N>".
 *
 * Watching a 20-minute game end to end therefore means walking
 * part1of4 .. part4of4 and then hopping to block 2's part1of<M> — which
 * is exactly what this file computes, so the UI (and the [ / ] keys) can
 * offer "the next segment" without the user reading dir names.
 *
 * Kept plain C, free of ImGui, so tests/unit can link it
 * (test_loadbrowser_segment.c). The name grammar it walks
 * (loadBrowserSplitName / loadBrowserNameEq) lives in
 * braintest_loadbrowser_rename.c, which is the single place a session dir
 * name is parsed; the rendering lives in braintest_loadbrowser.cpp.
 *********************************************************/
#include <stdio.h>
#include <string.h>

#include "braintest_loadbrowser.h"

void loadBrowserBaseName(const char *path, char *out, size_t outSz) {
    if (!out || outSz == 0) return;
    out[0] = '\0';
    if (!path) return;

    /* Ignore any trailing separators first ("debug_sessions/foo/" and
     * "debug_sessions\\foo" are the same session as "foo"). */
    size_t end = strlen(path);
    while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\')) end--;
    size_t start = end;
    while (start > 0 && path[start - 1] != '/' && path[start - 1] != '\\') start--;

    size_t n = end - start;
    if (n > outSz - 1) n = outSz - 1;
    memcpy(out, path + start, n);
    out[n] = '\0';
}

bool loadBrowserSameSession(const char *a, const char *b) {
    char ba[LOADBROWSER_MAX_NAME], bb[LOADBROWSER_MAX_NAME];
    if (!a || !b || !a[0] || !b[0]) return false;
    loadBrowserBaseName(a, ba, sizeof ba);
    loadBrowserBaseName(b, bb, sizeof bb);
    if (!ba[0] || !bb[0]) return false;
    return loadBrowserNameEq(ba, bb);
}

/* The BLOCK dir a session name belongs to — "<ts>_<B>[_<label>]" with any
 * "_part<X>of<N>" stripped off into *x / *n (0/0 when there is none). False
 * for anything that is not a blocked recording dir (a hand-named dir, a
 * pre-block bare-timestamp recording), which the walk treats as "no
 * neighbours". */
static bool lbStem(const char *name, char *stem, size_t stemSz, int *x, int *n) {
    char ts[16], label[LOADBROWSER_MAX_NAME];
    int block = 0;
    if (!loadBrowserSplitName(name, ts, sizeof ts, &block,
                              label, sizeof label, x, n)) return false;
    if (block < 1) return false;
    if (label[0]) snprintf(stem, stemSz, "%s_%d_%s", ts, block, label);
    else          snprintf(stem, stemSz, "%s_%d",    ts, block);
    return true;
}

/* Index of the entry named exactly `name`, or -1. */
static int lbFindByName(const LoadSessionEntry *list, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (loadBrowserNameEq(list[i].name, name)) return i;
    }
    return -1;
}

/* Index of `stem`'s first part (direction +1) or last part (direction -1),
 * for whatever N that block happened to be split into, or -1. */
static int lbFindEdgePart(const LoadSessionEntry *list, int count,
                          const char *stem, int direction) {
    for (int i = 0; i < count; i++) {
        char es[LOADBROWSER_MAX_NAME];
        int x = 0, n = 0;
        if (!lbStem(list[i].name, es, sizeof es, &x, &n)) continue;
        if (n < 1) continue;                     /* whole block dir, not a part */
        if (!loadBrowserNameEq(es, stem)) continue;
        if (direction > 0 ? (x == 1) : (x == n)) return i;
    }
    return -1;
}

/* The neighbouring BLOCK of `stem`, preferring its split parts over the whole
 * block dir: stepping forward off the last part should land on the next
 * block's part1, not on a 15-minute file the splitter already superseded. */
static int lbFindNeighbourBlock(const LoadSessionEntry *list, int count,
                                const char *stem, int direction) {
    char ts[16], label[LOADBROWSER_MAX_NAME], want[LOADBROWSER_MAX_NAME];
    int block = 0, x = 0, n = 0;
    if (!loadBrowserSplitName(stem, ts, sizeof ts, &block,
                              label, sizeof label, &x, &n)) return -1;
    if (block < 1) return -1;

    int nb = block + direction;
    if (nb < 1) return -1;
    if (label[0]) snprintf(want, sizeof want, "%s_%d_%s", ts, nb, label);
    else          snprintf(want, sizeof want, "%s_%d",    ts, nb);

    int idx = lbFindEdgePart(list, count, want, direction);
    if (idx >= 0) return idx;
    return lbFindByName(list, count, want);
}

int loadBrowserFindSegment(const LoadSessionEntry *list, int count,
                           const char *loadedDir, int direction) {
    char base[LOADBROWSER_MAX_NAME], stem[LOADBROWSER_MAX_NAME];
    int x = 0, n = 0;

    if (!list || count <= 0 || !loadedDir || !loadedDir[0]) return -1;
    if (direction != 1 && direction != -1) return -1;
    loadBrowserBaseName(loadedDir, base, sizeof base);
    if (!base[0]) return -1;
    if (!lbStem(base, stem, sizeof stem, &x, &n)) return -1;

    if (n > 0) {
        int nx = x + direction;
        if (nx >= 1 && nx <= n) {
            char want[LOADBROWSER_MAX_NAME];
            snprintf(want, sizeof want, "%s_part%dof%d", stem, nx, n);
            return lbFindByName(list, count, want);
        }
        /* Off the end of this block's parts — carry on into the next block. */
    }
    /* From an unsplit block dir (or off either end of the parts), the
     * neighbouring block. `stem` is the block dir name either way. */
    return lbFindNeighbourBlock(list, count, stem, direction);
}
