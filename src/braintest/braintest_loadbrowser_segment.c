/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
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
 * Kept plain C, free of ImGui and SDL, so tests/unit can link it
 * (test_loadbrowser_segment.c). The rendering lives in
 * braintest_loadbrowser.cpp.
 *********************************************************/
#include <stdio.h>
#include <string.h>

#include "braintest_loadbrowser.h"

/* Session dirs come from the filesystem, so on Windows two spellings that
 * differ only in case are the same session. Elsewhere they are not. */
static int lbCharEq(char a, char b) {
#ifdef _WIN32
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
#endif
    return a == b;
}

static bool lbNameEq(const char *a, const char *b) {
    if (!a || !b) return false;
    for (; *a && *b; a++, b++) if (!lbCharEq(*a, *b)) return false;
    return *a == '\0' && *b == '\0';
}

/* Does `s` start with the (ASCII) literal `lit`? Case-folded the same way, so
 * a "_PART1OF4" dir off a case-insensitive filesystem still parses. */
static bool lbStartsWith(const char *s, const char *lit) {
    for (; *lit; s++, lit++) {
        if (!*s || !lbCharEq(*s, *lit)) return false;
    }
    return true;
}

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
    return lbNameEq(ba, bb);
}

/* Split "<stem>_part<X>of<N>" into stem + X + N. Returns false (leaving the
 * outputs alone) when `name` carries no part suffix. Only a suffix that runs
 * to the end of the name counts, so a label containing "part" is safe. */
static bool lbSplitPart(const char *name, char *stem, size_t stemSz,
                        int *x, int *n) {
    const char *p = NULL;
    for (const char *c = name; *c; c++) {
        if (lbStartsWith(c, "_part")) p = c;   /* last one wins */
    }
    if (!p) return false;

    const char *d = p + 5;
    int xv = 0, nv = 0, digits = 0;
    while (*d >= '0' && *d <= '9') { xv = xv * 10 + (*d - '0'); d++; digits++; }
    if (digits == 0 || !lbStartsWith(d, "of")) return false;
    d += 2;
    digits = 0;
    while (*d >= '0' && *d <= '9') { nv = nv * 10 + (*d - '0'); d++; digits++; }
    if (digits == 0 || *d != '\0') return false;   /* must end the name */
    if (xv < 1 || nv < 1 || xv > nv) return false;

    size_t len = (size_t)(p - name);
    if (len > stemSz - 1) len = stemSz - 1;
    memcpy(stem, name, len);
    stem[len] = '\0';
    *x = xv;
    *n = nv;
    return true;
}

/* Split a block dir name "<YYYYMMDD_HHMMSS>_<B>[_<label>]" into its three
 * pieces. The timestamp shape is pinned (8 digits, '_', 6 digits, '_') the
 * same way btParseDirTime() pins it, so a numeric label can't be mistaken for
 * the block number. Returns false for anything else (a hand-named dir, a
 * pre-block recording). */
static bool lbSplitBlock(const char *name, char *ts, size_t tsSz,
                         int *block, char *label, size_t labelSz) {
    int i;
    for (i = 0; i < 8; i++)  if (name[i] < '0' || name[i] > '9') return false;
    if (name[8] != '_') return false;
    for (i = 9; i < 15; i++) if (name[i] < '0' || name[i] > '9') return false;
    if (name[15] != '_') return false;

    const char *d = name + 16;
    int bv = 0, digits = 0;
    while (*d >= '0' && *d <= '9') { bv = bv * 10 + (*d - '0'); d++; digits++; }
    if (digits == 0 || (*d != '\0' && *d != '_')) return false;
    if (bv < 1) return false;

    if (tsSz < 16) return false;
    memcpy(ts, name, 15);
    ts[15] = '\0';
    *block = bv;
    if (*d == '_') d++;                       /* skip the label separator */
    if (labelSz > 0) {
        size_t len = strlen(d);
        if (len > labelSz - 1) len = labelSz - 1;
        memcpy(label, d, len);
        label[len] = '\0';
    }
    return true;
}

/* Index of the entry named exactly `name`, or -1. */
static int lbFindByName(const LoadSessionEntry *list, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (lbNameEq(list[i].name, name)) return i;
    }
    return -1;
}

/* Index of `stem`'s first part (direction +1) or last part (direction -1),
 * for whatever N that block happened to be split into, or -1. */
static int lbFindEdgePart(const LoadSessionEntry *list, int count,
                          const char *stem, int direction) {
    size_t stemLen = strlen(stem);
    for (int i = 0; i < count; i++) {
        char es[LOADBROWSER_MAX_NAME];
        int x = 0, n = 0;
        if (!lbSplitPart(list[i].name, es, sizeof es, &x, &n)) continue;
        if (strlen(es) != stemLen || !lbNameEq(es, stem)) continue;
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
    int block = 0;
    if (!lbSplitBlock(stem, ts, sizeof ts, &block, label, sizeof label)) return -1;

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

    if (lbSplitPart(base, stem, sizeof stem, &x, &n)) {
        int nx = x + direction;
        if (nx >= 1 && nx <= n) {
            char want[LOADBROWSER_MAX_NAME];
            snprintf(want, sizeof want, "%s_part%dof%d", stem, nx, n);
            return lbFindByName(list, count, want);
        }
        /* Off the end of this block's parts — carry on into the next block. */
        return lbFindNeighbourBlock(list, count, stem, direction);
    }
    return lbFindNeighbourBlock(list, count, base, direction);
}
