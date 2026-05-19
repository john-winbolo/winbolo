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
#else
#  include <dirent.h>
#  include <unistd.h>
#endif

/* Walk one brain's directory and accumulate the maximum mtime over
 * any .lua file. Recurses into sub-directories (most brains have a
 * single level but the NewAutopilot tree has an opt/ folder). Result
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
static int brainListIndexCmp(const void *a, const void *b) {
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    return SDL_strcasecmp(g_brainListSortBase[ia].name,
                          g_brainListSortBase[ib].name);
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
        SDL_snprintf(paths[out->count], BRAIN_LIST_PATH_LEN,
                     "Brains/%s/init.lua", name);
    }
    out->count++;
}

/* Scan one parent directory (looking for child dirs that hold a brain). */
static void brainListScanParent(BrainList *out,
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

void brainListScan(BrainList *out, char (*paths)[BRAIN_LIST_PATH_LEN]) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (paths) memset(paths, 0, sizeof(paths[0]) * BRAIN_LIST_MAX);

    /* Working directory's brains/ — covers running from the repo. */
    brainListScanParent(out, paths, "brains");
    brainListScanParent(out, paths, "Brains");

    /* SDL_GetBasePath()/brains — covers installed builds where the
     * exe lives somewhere other than the brains tree. */
    const char *base = SDL_GetBasePath();
    if (base) {
        char p[1024];
        SDL_snprintf(p, sizeof(p), "%sbrains", base);
        brainListScanParent(out, paths, p);
        SDL_snprintf(p, sizeof(p), "%sBrains", base);
        brainListScanParent(out, paths, p);
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
