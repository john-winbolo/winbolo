/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * BrainTest "Load Session" browser — session-dir name grammar
 * and the Rename feature built on it.
 *
 * The recorder names a session dir "<YYYYMMDD_HHMMSS>_<B>[_<label>]"
 * (server_lifecycle.c): the timestamp is the GAME's start, <B> is the
 * 15-minute block, and <label> comes from WINBOLO_BRAINDBG_LABEL — so it
 * is whatever the label happened to be when the game was STARTED. Coming
 * back a week later to "20260903_011013_2_john" and wanting it to say
 * "oilrig_6bots" is the whole point of this file.
 *
 * One rename is a FAMILY rename. A single game reaches disk as several
 * dirs — every 15-minute block, and every "_part<X>of<N>" brainrec_split
 * cut out of a block — so renaming just the row the user clicked would
 * leave the rest of the game under the old name AND break the segment
 * walk (loadBrowserFindSegment matches blocks by ts + label). Every dir
 * sharing the member's timestamp and label therefore moves together.
 *
 * Split in two so the decision is testable without a filesystem:
 *   loadBrowserRenameFamily  works out (old,new) pairs from the in-memory
 *                            listing and refuses bad renames;
 *   loadBrowserApplyRenames  is the only part that touches disk.
 *
 * Plain C and ImGui-free so tests/unit can link it; the UI is in
 * braintest_loadbrowser.cpp.
 *********************************************************/
#include <SDL3/SDL.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "braintest_loadbrowser.h"

/* ---- Shared name primitives -------------------------------------------
 * Session dirs come from the filesystem, so on Windows two spellings that
 * differ only in case are the same session. Elsewhere they are not.
 * braintest_loadbrowser_segment.c uses these through the public
 * loadBrowserNameEq / loadBrowserSplitName rather than keeping a second
 * copy of the grammar. */

static int lbCharEq(char a, char b) {
#ifdef _WIN32
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
#endif
    return a == b;
}

bool loadBrowserNameEq(const char *a, const char *b) {
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

static bool lbIsDigit(char c) { return c >= '0' && c <= '9'; }

/* Strip a trailing "_part<X>of<N>" off `name` into stem + X + N. Returns false
 * (leaving the outputs alone) when there is no part suffix. Only a suffix that
 * runs to the end of the name counts, so a label containing "part" is safe. */
static bool lbSplitPart(const char *name, char *stem, size_t stemSz,
                        int *x, int *n) {
    const char *p = NULL;
    for (const char *c = name; *c; c++) {
        if (lbStartsWith(c, "_part")) p = c;   /* last one wins */
    }
    if (!p) return false;

    const char *d = p + 5;
    int xv = 0, nv = 0, digits = 0;
    while (lbIsDigit(*d)) { xv = xv * 10 + (*d - '0'); d++; digits++; }
    if (digits == 0 || !lbStartsWith(d, "of")) return false;
    d += 2;
    digits = 0;
    while (lbIsDigit(*d)) { nv = nv * 10 + (*d - '0'); d++; digits++; }
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

bool loadBrowserSplitName(const char *name, char *ts, size_t tsSz, int *block,
                          char *label, size_t labelSz, int *partX, int *partN) {
    char stem[LOADBROWSER_MAX_NAME];
    int px = 0, pn = 0, bv = 0;
    int i;

    if (ts && tsSz)       ts[0] = '\0';
    if (label && labelSz) label[0] = '\0';
    if (block) *block = 0;
    if (partX) *partX = 0;
    if (partN) *partN = 0;

    if (!name || !name[0]) return false;
    if (strlen(name) >= sizeof stem) return false;   /* not a name we wrote */

    if (!lbSplitPart(name, stem, sizeof stem, &px, &pn)) {
        memcpy(stem, name, strlen(name) + 1);
        px = pn = 0;
    }

    /* The timestamp shape is pinned (8 digits, '_', 6 digits) the same way
     * btParseDirTime() pins it, so a numeric LABEL can never be mistaken for
     * the block number. A short stem stops the loops at its NUL. */
    for (i = 0; i < 8; i++)  if (!lbIsDigit(stem[i])) return false;
    if (stem[8] != '_') return false;
    for (i = 9; i < 15; i++) if (!lbIsDigit(stem[i])) return false;

    const char *d = stem + 15;
    if (*d == '_') {
        int digits = 0;
        d++;
        while (lbIsDigit(*d)) { bv = bv * 10 + (*d - '0'); d++; digits++; }
        /* "<ts>_foo" (no block digits) and "<ts>_0_x" are not recorder
         * names; refusing them keeps a label out of the block slot. */
        if (digits == 0 || bv < 1) return false;
        if (*d != '\0' && *d != '_') return false;
        if (*d == '_') d++;                    /* skip the label separator */
    } else if (*d != '\0') {
        return false;                          /* "<ts>junk" */
    }
    /* Otherwise a bare "<ts>": block 0, and `d` already sits on the NUL, so
     * the label copy below yields "". */

    if (ts) {
        if (tsSz < 16) return false;
        memcpy(ts, stem, 15);
        ts[15] = '\0';
    }
    if (block) *block = bv;
    if (label && labelSz > 0) {
        size_t len = strlen(d);
        if (len > labelSz - 1) len = labelSz - 1;
        memcpy(label, d, len);
        label[len] = '\0';
    }
    if (partX) *partX = px;
    if (partN) *partN = pn;
    return true;
}

/* ---- Rename planning --------------------------------------------------- */

static void lbErr(char *err, size_t errSz, const char *fmt, ...) {
    va_list ap;
    if (!err || errSz == 0) return;
    va_start(ap, fmt);
    SDL_vsnprintf(err, errSz, fmt, ap);
    va_end(ap);
}

/* Labels end up in dir names, so keep them to the same set
 * WINBOLO_BRAINDBG_LABEL is sanitized to. An empty label is legal — it means
 * "drop the label". */
static bool lbLabelOk(const char *label, char *bad) {
    for (const char *c = label; *c; c++) {
        if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
            lbIsDigit(*c) || *c == '_' || *c == '-') continue;
        *bad = *c;
        return false;
    }
    return true;
}

/* "debug_sessions/foo" -> "debug_sessions/" (empty when the dir is bare). */
static void lbDirPrefix(const char *dir, char *out, size_t outSz) {
    size_t cut = 0;
    if (!out || outSz == 0) return;
    out[0] = '\0';
    if (!dir) return;
    for (size_t i = 0; dir[i]; i++) {
        if (dir[i] == '/' || dir[i] == '\\') cut = i + 1;
    }
    if (cut > outSz - 1) cut = outSz - 1;
    memcpy(out, dir, cut);
    out[cut] = '\0';
}

/* "<ts>_<B>[_<label>][_part<X>of<N>]" — the one place a session name is
 * spelled, so the plan can never invent a shape the parser rejects. */
static void lbBuildName(char *out, size_t outSz, const char *ts, int block,
                        const char *label, int partX, int partN) {
    if (label && label[0]) SDL_snprintf(out, outSz, "%s_%d_%s", ts, block, label);
    else                   SDL_snprintf(out, outSz, "%s_%d",    ts, block);
    if (partN > 0) {
        size_t n = SDL_strlen(out);
        SDL_snprintf(out + n, outSz - n, "_part%dof%d", partX, partN);
    }
}

/* Is `name` one of the dirs this plan is moving OUT of the way? A target that
 * collides with a family member's OLD name is not a collision — that member is
 * being renamed too. */
static bool lbInPlanOld(char (*plan)[2][300], int n, const char *name) {
    char base[LOADBROWSER_MAX_NAME];
    for (int i = 0; i < n; i++) {
        loadBrowserBaseName(plan[i][0], base, sizeof base);
        if (loadBrowserNameEq(base, name)) return true;
    }
    return false;
}

int loadBrowserRenameFamily(const LoadSessionEntry *list, int count,
                            const char *anyMemberDir, const char *newLabel,
                            const char *loadedDir,
                            char (*plan)[2][300], int planMax,
                            char *err, size_t errSz) {
    char memberName[LOADBROWSER_MAX_NAME];
    char ts[16], label[LOADBROWSER_MAX_NAME];
    int block = 0, px = 0, pn = 0;
    int n = 0;
    char badChar = 0;

    if (err && errSz) err[0] = '\0';
    if (!list || count <= 0 || !plan || planMax <= 0) {
        lbErr(err, errSz, "nothing to rename");
        return -1;
    }
    if (!newLabel) newLabel = "";

    loadBrowserBaseName(anyMemberDir, memberName, sizeof memberName);
    if (!memberName[0]) {
        lbErr(err, errSz, "no session selected");
        return -1;
    }
    if (!loadBrowserSplitName(memberName, ts, sizeof ts, &block,
                              label, sizeof label, &px, &pn) || block < 1) {
        lbErr(err, errSz,
              "\"%s\" is not a <timestamp>_<block> recording dir, so it has "
              "no label slot to rename", memberName);
        return -1;
    }

    if (!lbLabelOk(newLabel, &badChar)) {
        lbErr(err, errSz,
              "'%c' is not allowed in a label — use letters, digits, '_' or '-'",
              badChar);
        return -1;
    }
    if (SDL_strlen(newLabel) > (size_t)LOADBROWSER_LABEL_MAX) {
        lbErr(err, errSz, "label is longer than %d characters",
              LOADBROWSER_LABEL_MAX);
        return -1;
    }
    if (loadBrowserNameEq(newLabel, label)) {
        if (label[0]) lbErr(err, errSz, "already labelled \"%s\"", label);
        else          lbErr(err, errSz, "this session has no label already");
        return -1;
    }

    /* Two passes so PARTS are renamed before the BLOCK dirs they were cut
     * from — every target is distinct either way, but "part before block"
     * keeps the on-disk order the splitter itself would produce. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < count; i++) {
            char ets[16], elabel[LOADBROWSER_MAX_NAME], newName[LOADBROWSER_MAX_NAME];
            char prefix[300];
            int eblock = 0, epx = 0, epn = 0;

            if (!loadBrowserSplitName(list[i].name, ets, sizeof ets, &eblock,
                                      elabel, sizeof elabel, &epx, &epn))
                continue;
            if (eblock < 1) continue;
            if (strcmp(ets, ts) != 0) continue;               /* digits: exact */
            if (!loadBrowserNameEq(elabel, label)) continue;  /* "" == "" */
            if ((pass == 0) != (epn > 0)) continue;           /* parts first */

            if (loadedDir && loadedDir[0] &&
                loadBrowserSameSession(list[i].dir, loadedDir)) {
                lbErr(err, errSz,
                      "\"%s\" is the recording this BrainTest is replaying — "
                      "quit or load another session first", list[i].name);
                return -1;
            }

            lbBuildName(newName, sizeof newName, ts, eblock, newLabel, epx, epn);
            if (SDL_strlen(newName) >= (size_t)(LOADBROWSER_MAX_NAME - 1)) {
                lbErr(err, errSz, "\"%s\" would be too long a dir name", newName);
                return -1;
            }
            if (n >= planMax) {
                lbErr(err, errSz, "more than %d dirs in this session", planMax);
                return -1;
            }

            lbDirPrefix(list[i].dir, prefix, sizeof prefix);
            SDL_snprintf(plan[n][0], sizeof plan[n][0], "%s", list[i].dir);
            SDL_snprintf(plan[n][1], sizeof plan[n][1], "%s%s", prefix, newName);
            n++;
        }
    }

    if (n == 0) {
        lbErr(err, errSz, "\"%s\" is not in the list", memberName);
        return -1;
    }

    /* Collision: some OTHER listed session already owns a target name. (A
     * name belonging to the family itself is fine — it is moving too, though
     * with distinct block/part suffixes that cannot actually happen.) */
    for (int i = 0; i < n; i++) {
        char want[LOADBROWSER_MAX_NAME];
        loadBrowserBaseName(plan[i][1], want, sizeof want);
        for (int j = 0; j < count; j++) {
            if (!loadBrowserNameEq(list[j].name, want)) continue;
            if (lbInPlanOld(plan, n, list[j].name)) continue;
            lbErr(err, errSz, "\"%s\" already exists", want);
            return -1;
        }
    }
    return n;
}

int loadBrowserApplyRenames(const char (*plan)[2][300], int n,
                            char *err, size_t errSz) {
    if (err && errSz) err[0] = '\0';
    if (!plan || n <= 0) {
        lbErr(err, errSz, "nothing to rename");
        return -1;
    }

    /* Pre-pass: a dir the scan skipped (no brainrec.btr, wrong version) never
     * reaches the listing but still owns its name on disk. Finding that out
     * BEFORE the first rename is the difference between a refusal and a
     * half-renamed session. */
    for (int i = 0; i < n; i++) {
        SDL_PathInfo pi;
        if (SDL_GetPathInfo(plan[i][1], &pi)) {
            lbErr(err, errSz, "\"%s\" already exists on disk", plan[i][1]);
            return -1;
        }
    }

    for (int i = 0; i < n; i++) {
        if (!SDL_RenamePath(plan[i][0], plan[i][1])) {
            lbErr(err, errSz,
                  "renamed %d of %d, then %s -> %s failed: %s "
                  "(the %d already renamed stay renamed)",
                  i, n, plan[i][0], plan[i][1], SDL_GetError(), i);
            return -1;
        }
    }
    return n;
}
