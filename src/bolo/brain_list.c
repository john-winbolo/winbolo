/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "brain_list.h"
#include "brain_list_internal.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

#if defined(_WIN32)
#  include <windows.h>
#  define BRAIN_LIST_SEP '\\'
#else
#  include <dirent.h>
#  include <unistd.h>
#  define BRAIN_LIST_SEP '/'
#endif

/* Walk one brain's directory and accumulate the maximum mtime over
 * any .lua file. Recurses into sub-directories (most brains have a
 * single level but the GoalHunter tree has an opt/ folder). Result
 * is 0 when nothing was found. */
static time_t brainListMaxMtime(const char *brainDir) {
    time_t best = 0;

#if defined(_WIN32)
    char pattern[1024];
    SDL_snprintf(pattern, sizeof(pattern), "%s\\*", brainDir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' ||
             (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0'))) continue;
        char full[1024];
        SDL_snprintf(full, sizeof(full), "%s\\%s", brainDir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            time_t sub = brainListMaxMtime(full);
            if (sub > best) best = sub;
        } else {
            size_t n = strlen(fd.cFileName);
            if (n > 4 && SDL_strcasecmp(fd.cFileName + n - 4, ".lua") == 0) {
                struct stat st;
                if (stat(full, &st) == 0 && st.st_mtime > best) {
                    best = st.st_mtime;
                }
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(brainDir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == '\0' ||
             (e->d_name[1] == '.' && e->d_name[2] == '\0'))) continue;
        char full[1024];
        SDL_snprintf(full, sizeof(full), "%s/%s", brainDir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            time_t sub = brainListMaxMtime(full);
            if (sub > best) best = sub;
        } else {
            size_t n = strlen(e->d_name);
            if (n > 4 && strcasecmp(e->d_name + n - 4, ".lua") == 0) {
                if (st.st_mtime > best) best = st.st_mtime;
            }
        }
    }
    closedir(d);
#endif
    return best;
}

/* Return TRUE iff `dir`/init.lua exists. */
static bool brainListHasInit(const char *dir) {
    char p[1024];
    SDL_snprintf(p, sizeof(p), "%s%cinit.lua", dir,
#if defined(_WIN32)
                 '\\'
#else
                 '/'
#endif
                 );
    struct stat st;
    return (stat(p, &st) == 0);
}

/* Indexed sort: we sort an int[] permutation of entry indices by
 * name order, then reorder entries and paths in lockstep so
 * paths[i] keeps tracking entries[i]. The qsort comparator reads
 * the base pointer out of a module-static — not thread-safe, but
 * brainListScan is a one-shot startup call (per-ServerSim init,
 * never concurrent), so that's fine. */
static const BrainListEntry *g_brainListSortBase = NULL;

double brainListSplitVersion(const char *name, char *base, size_t baseSz) {
    const char *us = strrchr(name, '_');
    if (us && us[1] >= '0' && us[1] <= '9') {
        size_t blen = (size_t)(us - name);
        if (blen >= baseSz) blen = baseSz - 1;
        memcpy(base, name, blen);
        base[blen] = '\0';
        return atof(us + 1);
    }
    SDL_strlcpy(base, name, baseSz);
    return 0.0;
}

static int brainListIndexCmp(const void *a, const void *b) {
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    const char *na = g_brainListSortBase[ia].name;
    const char *nb = g_brainListSortBase[ib].name;
    char ba[BRAIN_LIST_NAME_LEN], bb[BRAIN_LIST_NAME_LEN];
    double va = brainListSplitVersion(na, ba, sizeof ba);
    double vb = brainListSplitVersion(nb, bb, sizeof bb);
    int c = SDL_strcasecmp(ba, bb);
    if (c != 0) return c;             /* different brain family → alphabetical */
    if (va > vb) return -1;           /* same family → newest version first   */
    if (va < vb) return 1;
    return SDL_strcasecmp(na, nb);    /* stable tiebreak */
}

/* Add one entry for the directory at `brainDir` with display name `name`.
 * Skips when the brain has no init.lua, when the list is full, or when
 * an entry with the same name already exists. */
static void brainListMaybeAdd(BrainList *out,
                              char (*paths)[BRAIN_LIST_PATH_LEN],
                              const char *brainDir, const char *name) {
    if (out->count >= BRAIN_LIST_MAX) return;
    if (!brainListHasInit(brainDir)) return;
    for (int i = 0; i < out->count; i++) {
        if (SDL_strcasecmp(out->entries[i].name, name) == 0) return;
    }
    BrainListEntry *e = &out->entries[out->count];
    SDL_strlcpy(e->name, name, sizeof(e->name));
    time_t mt = brainListMaxMtime(brainDir);
    if (mt > 0) {
        struct tm tmv;
#if defined(_WIN32)
        localtime_s(&tmv, &mt);
#else
        localtime_r(&mt, &tmv);
#endif
        strftime(e->version, sizeof(e->version), "%Y-%m-%d %H:%M", &tmv);
    } else {
        e->version[0] = '\0';
    }
    if (paths) {
        /* Store the directory the brain was actually found in — NOT a
         * hardcoded "Brains/<name>". Brains discovered under the prefs dir
         * (or SDL_GetBasePath) live outside the cwd, and the host chdir's to
         * SDL_GetBasePath at startup, so a relative "Brains/..." path would
         * resolve against the bundle Resources dir and fail to load. brainDir
         * already carries the real (absolute, for prefs/base) path the
         * scanner walked, so the lobby can load exactly what it listed. */
        SDL_snprintf(paths[out->count], BRAIN_LIST_PATH_LEN,
                     "%s%cinit.lua", brainDir,
#if defined(_WIN32)
                     '\\'
#else
                     '/'
#endif
                     );
    }
    out->count++;
}

/* Scan one parent directory (looking for child dirs that hold a brain).
 * Declared in brain_list_internal.h (non-static) so the unit test can drive
 * it against an arbitrary directory. */
void brainListScanParent(BrainList *out,
                         char (*paths)[BRAIN_LIST_PATH_LEN],
                         const char *parent) {
#if defined(_WIN32)
    char pattern[1024];
    SDL_snprintf(pattern, sizeof(pattern), "%s\\*", parent);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        char full[1024];
        SDL_snprintf(full, sizeof(full), "%s\\%s", parent, fd.cFileName);
        brainListMaybeAdd(out, paths, full, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(parent);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char full[1024];
        SDL_snprintf(full, sizeof(full), "%s/%s", parent, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) continue;
        brainListMaybeAdd(out, paths, full, e->d_name);
    }
    closedir(d);
#endif
}

/* Parse an already-loaded about.txt blob into tagline (first non-empty line)
 * + description (the remainder, leading blank lines trimmed). */
static void brainListSplitMeta(const char *blob,
                               char *tagline, size_t taglineSz,
                               char *desc, size_t descSz) {
    const char *p = blob;
    /* Skip leading whitespace/blank lines to find the tagline. */
    while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') p++;
    const char *tagEnd = p;
    while (*tagEnd && *tagEnd != '\n' && *tagEnd != '\r') tagEnd++;
    if (tagline && taglineSz > 0) {
        size_t n = (size_t)(tagEnd - p);
        if (n >= taglineSz) n = taglineSz - 1;
        memcpy(tagline, p, n);
        tagline[n] = '\0';
    }
    /* Description = everything after the tagline line, blank lines trimmed. */
    const char *d = tagEnd;
    while (*d == '\n' || *d == '\r' || *d == ' ' || *d == '\t') d++;
    if (desc && descSz > 0) {
        size_t n = strlen(d);
        if (n >= descSz) n = descSz - 1;
        memcpy(desc, d, n);
        desc[n] = '\0';
        /* Trim trailing whitespace. */
        while (n > 0 && (desc[n - 1] == '\n' || desc[n - 1] == '\r' ||
                         desc[n - 1] == ' '  || desc[n - 1] == '\t')) {
            desc[--n] = '\0';
        }
    }
}

/* Try to read "<parent>/<name>/<file>" into buf; returns bytes read (0 if
 * absent/empty). buf is always NUL-terminated. */
static size_t brainListReadSidecar(const char *parent, const char *name,
                                   const char *file,
                                   char *buf, size_t bufSz) {
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%s%c%s%c%s", parent,
#if defined(_WIN32)
                 '\\', name, '\\', file
#else
                 '/', name, '/', file
#endif
                 );
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, bufSz - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n;
}

/* The directories a brain is looked for under, in search order: the working
 * directory's brains/ and Brains/, then the same two beside the executable,
 * which covers an installed build whose exe lives somewhere other than the
 * brains tree, and last the writable prefs path's Brains/, which is where a
 * player drops brains of their own on a platform whose app directory is
 * read-only.
 *
 * One list, read by the per-brain file reads below, by brainListResolve and
 * by brainListScan, so a brain one of them finds is a brain the others find.
 * The prefs path used to be the scan's alone, which listed a brain installed
 * only there in the lobby and then failed to resolve it for a scenario.
 * Fills out[] and returns how many parents it wrote. */
#define BRAIN_LIST_PARENT_MAX 5
#define BRAIN_LIST_PARENT_LEN 1024

static int brainListParents(char (*out)[BRAIN_LIST_PARENT_LEN]) {
    const char *base;
    char       *pref;
    int         n = 0;

    SDL_strlcpy(out[n++], "brains", BRAIN_LIST_PARENT_LEN);
    SDL_strlcpy(out[n++], "Brains", BRAIN_LIST_PARENT_LEN);
    base = SDL_GetBasePath();
    if (base) {
        SDL_snprintf(out[n++], BRAIN_LIST_PARENT_LEN, "%sbrains", base);
        SDL_snprintf(out[n++], BRAIN_LIST_PARENT_LEN, "%sBrains", base);
    }
    /* SDL_GetPrefPath returns a trailing separator and a string the caller
       frees. ~/Library/Application Support/WinBolo/WinBolo/Brains on macOS,
       where the app bundle is read-only and code-signed. */
    pref = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (pref) {
        SDL_snprintf(out[n++], BRAIN_LIST_PARENT_LEN, "%sBrains", pref);
        SDL_free(pref);
    }
    return n;
}

/* Read "<name>/<file>" from the first brains parent that has it. Every brain
 * ships with its own about.txt / modes.txt, so this is a local read on each
 * machine — none of it goes over the wire. */
static size_t brainListReadSidecarAny(const char *name, const char *file,
                                      char *blob, size_t blobSz) {
    char   parents[BRAIN_LIST_PARENT_MAX][BRAIN_LIST_PARENT_LEN];
    int    count = brainListParents(parents);
    size_t got   = 0;
    int    i;

    for (i = 0; i < count && got == 0; i++) {
        got = brainListReadSidecar(parents[i], name, file, blob, blobSz);
    }
    return got;
}

bool brainListResolve(const char *name, char *outPath, size_t outLen) {
    char parents[BRAIN_LIST_PARENT_MAX][BRAIN_LIST_PARENT_LEN];
    int  count;
    int  i;

    if (outPath == NULL || outLen == 0) return false;
    outPath[0] = '\0';
    if (name == NULL || name[0] == '\0') return false;
    /* One directory under a brains parent, and never the parent itself: a
       value that paths anywhere, and one that opens with a dot, are not names
       the scan would have catalogued either. */
    if (strpbrk(name, "/\\") != NULL || name[0] == '.') return false;

    count = brainListParents(parents);
    for (i = 0; i < count; i++) {
        char dir[BRAIN_LIST_PARENT_LEN];
        int  n;

        SDL_snprintf(dir, sizeof(dir), "%s%c%s", parents[i], BRAIN_LIST_SEP,
                     name);
        if (!brainListHasInit(dir)) continue;
        n = SDL_snprintf(outPath, outLen, "%s%cinit.lua", dir,
                         BRAIN_LIST_SEP);
        /* A path that did not fit is no answer: half of one opens nothing,
           and a caller told false falls back to the server's own brain. */
        if (n < 0 || (size_t)n >= outLen) {
            outPath[0] = '\0';
            return false;
        }
        return true;
    }
    return false;
}

static size_t brainListReadAboutAny(const char *name, char *blob, size_t blobSz) {
    return brainListReadSidecarAny(name, "about.txt", blob, blobSz);
}

/* Is this line of about.txt a "color:" key line? Points *value past the
 * colon and any spaces when it is. Case-insensitive, "colour:" accepted. */
static bool brainListColorLine(const char *line, const char **value) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    size_t n = 0;
    if (SDL_strncasecmp(p, "colour", 6) == 0) n = 6;
    else if (SDL_strncasecmp(p, "color", 5) == 0) n = 5;
    if (!n) return false;
    p += n;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':' && *p != '=') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (value) *value = p;
    return true;
}

/* Drop the "color:" key lines from an about.txt blob in place, so the
 * tagline/description split below never shows one to the player. */
static void brainListStripColorLines(char *blob) {
    char *src = blob, *dst = blob;
    while (*src) {
        char *eol = src;
        while (*eol && *eol != '\n') eol++;
        size_t len = (size_t)(eol - src) + (*eol ? 1u : 0u);
        if (!brainListColorLine(src, NULL)) {
            if (dst != src) memmove(dst, src, len);
            dst += len;
        }
        src += len;
    }
    *dst = '\0';
}

bool brainListLoadMeta(const char *name,
                       char *tagline, size_t taglineSz,
                       char *desc, size_t descSz) {
    if (tagline && taglineSz) tagline[0] = '\0';
    if (desc && descSz) desc[0] = '\0';
    if (!name || !name[0]) return false;

    char blob[2048];
    if (!brainListReadAboutAny(name, blob, sizeof(blob))) return false;
    brainListStripColorLines(blob);
    brainListSplitMeta(blob, tagline, taglineSz, desc, descSz);
    return true;
}

bool brainListLoadColor(const char *name, uint32_t *rgb) {
    if (!name || !name[0] || !rgb) return false;
    char blob[2048];
    if (!brainListReadAboutAny(name, blob, sizeof(blob))) return false;
    const char *line = blob;
    while (*line) {
        const char *eol = line;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        const char *val = NULL;
        if (brainListColorLine(line, &val) && val < eol) {
            /* "#RRGGBB" or "RRGGBB"; anything else is ignored so a typo in a
             * brain's about.txt falls back to the derived colour. */
            if (*val == '#') val++;
            if (eol - val >= 6) {
                char hex[7];
                memcpy(hex, val, 6);
                hex[6] = '\0';
                char *end = NULL;
                unsigned long v = strtoul(hex, &end, 16);
                if (end == hex + 6) {
                    *rgb = (uint32_t)v & 0xFFFFFFu;
                    return true;
                }
            }
        }
        while (*eol == '\n' || *eol == '\r') eol++;
        line = eol;
    }
    return false;
}

/* ── modes.txt: a brain's own list of modes and difficulty levels ──── */

/* Trim spaces and tabs off both ends of `s`, in place. Returns the first
 * kept character (which may be the terminating NUL for an all-blank line). */
static char *brainModesTrim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
    return s;
}

/* Fold a key to lower case in place and say whether it is a legal one:
 * 1..BRAIN_MODE_KEY_LEN-1 characters of [a-z0-9_]. Anything else (a space,
 * a dot, an empty header) is a malformed line the caller skips. */
static bool brainModesKeyOk(char *k) {
    size_t n = strlen(k);
    if (n == 0 || n >= BRAIN_MODE_KEY_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        char c = (char)SDL_tolower((unsigned char)k[i]);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
        k[i] = c;
    }
    return true;
}

/* The mode list a brain with no manifest gets: one "default" mode with the
 * three levels the game shipped with, starting at hard. Keeping this in one
 * place means "no modes.txt" and "[default] easy/medium/hard, default hard"
 * are literally the same thing to every caller. */
static void brainModesSynthesizeDefault(BrainModes *out) {
    static const struct { const char *key, *label; int chips; } kLevels[3] = {
        { "easy",   "Easy",   1 },
        { "medium", "Medium", 2 },
        { "hard",   "Hard",   3 },
    };
    memset(out, 0, sizeof(*out));
    out->modeCount = 1;
    BrainMode *m = &out->modes[0];
    SDL_strlcpy(m->key,   "default", sizeof(m->key));
    SDL_strlcpy(m->label, "Default", sizeof(m->label));
    m->levelCount = 3;
    for (int i = 0; i < 3; i++) {
        SDL_strlcpy(m->levels[i].key,   kLevels[i].key,   sizeof(m->levels[i].key));
        SDL_strlcpy(m->levels[i].label, kLevels[i].label, sizeof(m->levels[i].label));
        m->levels[i].chips = kLevels[i].chips;
    }
    m->defaultLevel = 2;   /* hard — what a new bot has always started at */
}

/* Parse one "levels = k1:L1, k2:L2" value into a mode. Entries past
 * BRAIN_LEVELS_MAX are dropped; an entry with no ':' , an empty label or an
 * illegal key is skipped and the rest of the line still counts. */
static void brainModesParseLevels(BrainMode *m, char *value) {
    m->levelCount = 0;
    char *p = value;
    while (*p != '\0') {
        char *comma = strchr(p, ',');
        if (comma) *comma = '\0';
        char *entry = brainModesTrim(p);
        char *colon = strchr(entry, ':');
        if (colon) {
            *colon = '\0';
            char *key  = brainModesTrim(entry);
            char *rest = colon + 1;
            /* The chip count is an OPTIONAL third field: "key:Label:N", where
             * N is 1, 2 or 3 — how many of the lobby's three difficulty chips
             * the level lights.
             *
             * Absent: the level takes its POSITION on the scale, so the first
             * kept level is 1, the second 2, and the third and any after it 3.
             * That is what every modes.txt written before chips existed means
             * anyway, so those keep working untouched — including third-party
             * brains, which is the trap that bit us once before when a change
             * here assumed every brain would be updated with it.
             *
             * Present but not 1, 2 or 3: the level is DROPPED. A brain that
             * declares something is taken at its word, and a value the lobby
             * cannot draw is an error worth losing the level over. */
            char *colon2 = strchr(rest, ':');
            int   chips  = 0;
            bool  chipsDeclared = false;
            if (colon2) {
                *colon2 = '\0';
                chipsDeclared = true;
                char *chipTxt = brainModesTrim(colon2 + 1);
                if (chipTxt[0] >= '1' && chipTxt[0] <= '3' && chipTxt[1] == '\0') {
                    chips = chipTxt[0] - '0';
                }
            }
            char *label = brainModesTrim(rest);
            if (!chipsDeclared) {
                /* Counted over the levels KEPT so far, so a malformed entry
                 * earlier in the line does not leave a gap in the scale. */
                int pos = m->levelCount + 1;
                chips = (pos > 3) ? 3 : pos;
            }
            if (chips >= 1 && chips <= 3 &&
                brainModesKeyOk(key) && label[0] != '\0' &&
                m->levelCount < BRAIN_LEVELS_MAX) {
                BrainLevel *lv = &m->levels[m->levelCount++];
                SDL_strlcpy(lv->key,   key,   sizeof(lv->key));
                SDL_strlcpy(lv->label, label, sizeof(lv->label));
                lv->chips = chips;
            }
        }
        if (!comma) break;
        p = comma + 1;
    }
}

int brainModesFindMode(const BrainModes *modes, const char *key) {
    if (!modes || !key || !key[0]) return -1;
    for (int i = 0; i < modes->modeCount; i++) {
        if (SDL_strcasecmp(modes->modes[i].key, key) == 0) return i;
    }
    return -1;
}

int brainModeFindLevel(const BrainMode *mode, const char *key) {
    if (!mode || !key || !key[0]) return -1;
    for (int i = 0; i < mode->levelCount; i++) {
        if (SDL_strcasecmp(mode->levels[i].key, key) == 0) return i;
    }
    return -1;
}

bool brainListLoadModes(const char *name, BrainModes *out) {
    if (!out) return false;
    brainModesSynthesizeDefault(out);
    if (!name || !name[0]) return false;

    char blob[4096];
    if (!brainListReadSidecarAny(name, "modes.txt", blob, sizeof(blob))) {
        return false;
    }

    /* Parsed into a scratch list first: a manifest that turns out to hold
     * no usable mode leaves the caller with the synthesized default rather
     * than an empty list nothing can render. */
    BrainModes parsed;
    memset(&parsed, 0, sizeof(parsed));
    /* Each mode's `default` names a level KEY, and may appear before the
     * `levels` line, so it is remembered here and resolved at the end. */
    char defaultKeys[BRAIN_MODES_MAX][BRAIN_MODE_KEY_LEN];
    memset(defaultKeys, 0, sizeof(defaultKeys));

    BrainMode *cur = NULL;       /* mode the current section writes into */
    int        curIdx = -1;
    char      *line = blob;
    while (line != NULL && *line != '\0') {
        char *eol = strchr(line, '\n');
        char *next = NULL;
        if (eol) { *eol = '\0'; next = eol + 1; }
        /* Strip a CR (CRLF files) and anything from a '#' comment on. */
        char *cr = strchr(line, '\r');
        if (cr) *cr = '\0';
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char *s = brainModesTrim(line);
        if (s[0] == '[') {
            /* New section: "[key]". */
            cur = NULL;
            char *close = strchr(s, ']');
            if (close) {
                *close = '\0';
                char *key = brainModesTrim(s + 1);
                if (brainModesKeyOk(key) &&
                    parsed.modeCount < BRAIN_MODES_MAX &&
                    brainModesFindMode(&parsed, key) < 0) {
                    curIdx = parsed.modeCount++;
                    cur = &parsed.modes[curIdx];
                    SDL_strlcpy(cur->key, key, sizeof(cur->key));
                    /* Until a label line says otherwise the key IS the
                     * label, so a terse manifest still renders. */
                    SDL_strlcpy(cur->label, key, sizeof(cur->label));
                }
            }
        } else if (s[0] != '\0' && cur != NULL) {
            char *eq = strchr(s, '=');
            if (eq) {
                *eq = '\0';
                char *field = brainModesTrim(s);
                char *value = brainModesTrim(eq + 1);
                if (SDL_strcasecmp(field, "label") == 0) {
                    if (value[0] != '\0') {
                        SDL_strlcpy(cur->label, value, sizeof(cur->label));
                    }
                } else if (SDL_strcasecmp(field, "levels") == 0) {
                    brainModesParseLevels(cur, value);
                } else if (SDL_strcasecmp(field, "default") == 0) {
                    if (brainModesKeyOk(value)) {
                        SDL_strlcpy(defaultKeys[curIdx], value,
                                    sizeof(defaultKeys[curIdx]));
                    }
                }
                /* An unknown field is ignored: a newer manifest can carry
                 * keys this build has never heard of and still load. */
            }
        }
        line = next;
    }

    /* Resolve each mode's default level. A mode with NO levels is kept, not
     * dropped: a brain is allowed to say it has one way of playing and no
     * difficulty at all (Andrew: "allow bots to also just not give any
     * difficulty levels and then we don't need the 3-chips control"). The
     * lobby then shows no difficulty tag and no difficulty dropdown for it.
     *
     * The cost of allowing this: a `levels` line whose every entry is
     * malformed also lands on zero levels, and now reads as "no difficulty"
     * instead of sending the brain back to the synthesized default. A brain
     * author who meant to declare levels sees them silently absent rather
     * than replaced. That is the trade for making "no levels" expressible. */
    BrainModes kept;
    memset(&kept, 0, sizeof(kept));
    for (int i = 0; i < parsed.modeCount; i++) {
        BrainMode *m = &kept.modes[kept.modeCount++];
        *m = parsed.modes[i];
        int d = brainModeFindLevel(m, defaultKeys[i]);
        /* No (or an unknown) default key falls back to the LAST level,
         * which is the hardest by convention and matches the pre-manifest
         * "new bots start on Hard" rule. With no levels at all there is
         * nothing to point at, so the index is 0 and nothing reads it. */
        m->defaultLevel = (d >= 0) ? d
                        : (m->levelCount > 0 ? m->levelCount - 1 : 0);
    }
    if (kept.modeCount <= 0) return false;   /* out keeps the fallback */
    *out = kept;
    return true;
}

/* Recover a brain's directory name from its init.lua path — the same
 * "Brains/<DirName>/init.lua" convention bot_manager uses — so callers
 * holding a path don't each re-implement the split. */
bool brainListLoadModesForPath(const char *brainPath, BrainModes *out) {
    if (!out) return false;
    char name[BRAIN_LIST_NAME_LEN];
    name[0] = '\0';
    if (brainPath && brainPath[0]) {
        const char *end = brainPath + strlen(brainPath);
        /* Back over the file name ... */
        const char *fname = end;
        while (fname > brainPath && fname[-1] != '/' && fname[-1] != '\\') fname--;
        /* ... then over the directory that holds it. */
        if (fname > brainPath) {
            const char *dirEnd   = fname - 1;
            const char *dirStart = dirEnd;
            while (dirStart > brainPath &&
                   dirStart[-1] != '/' && dirStart[-1] != '\\') dirStart--;
            size_t dirLen = (size_t)(dirEnd - dirStart);
            if (dirLen >= sizeof(name)) dirLen = sizeof(name) - 1;
            memcpy(name, dirStart, dirLen);
            name[dirLen] = '\0';
        }
    }
    return brainListLoadModes(name, out);
}

void brainListScan(BrainList *out, char (*paths)[BRAIN_LIST_PATH_LEN]) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (paths) memset(paths, 0, sizeof(paths[0]) * BRAIN_LIST_MAX);

    /* The same parents brainListResolve and the per-brain file reads walk, in
     * the same order, so a brain this listing shows is a brain a scenario
     * that names it can resolve. The scan used to keep its own copy of the
     * list with the prefs path on the end of it, which listed a brain
     * installed only there and then could not resolve it. */
    {
        char parents[BRAIN_LIST_PARENT_MAX][BRAIN_LIST_PARENT_LEN];
        int  count = brainListParents(parents);
        int  i;

        for (i = 0; i < count; i++) {
            brainListScanParent(out, paths, parents[i]);
        }
    }

    if (out->count > 1) {
        /* Permutation sort: index[i] starts at i, comparator orders by
         * entries[index[i]].name. After qsort, apply the permutation
         * to entries (and paths in lockstep) using a temp buffer. */
        int idx[BRAIN_LIST_MAX];
        for (int i = 0; i < out->count; i++) idx[i] = i;
        g_brainListSortBase = out->entries;
        qsort(idx, (size_t)out->count, sizeof(idx[0]), brainListIndexCmp);
        g_brainListSortBase = NULL;

        BrainListEntry tmpEntries[BRAIN_LIST_MAX];
        char           tmpPaths[BRAIN_LIST_MAX][BRAIN_LIST_PATH_LEN];
        for (int i = 0; i < out->count; i++) {
            tmpEntries[i] = out->entries[idx[i]];
            if (paths) {
                SDL_strlcpy(tmpPaths[i], paths[idx[i]], BRAIN_LIST_PATH_LEN);
            }
        }
        for (int i = 0; i < out->count; i++) {
            out->entries[i] = tmpEntries[i];
            if (paths) {
                SDL_strlcpy(paths[i], tmpPaths[i], BRAIN_LIST_PATH_LEN);
            }
        }
    }
}
