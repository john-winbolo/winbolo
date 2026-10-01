/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Implementation of the starred-map persistence layer.
 *
 * Storage: SDL_GetPrefPath()/map_stars.txt, one entry per line, tab
 * separated: scope \t path \t isFolder \t name . Entries are kept in
 * insertion order so the "Starred" bar at the top of the chooser
 * lists most-recent-first when the caller reverses iteration.
 *
 * The in-memory model is a single std::vector — small N (typically
 * <50), linear scan for membership tests is plenty.
 */

#include "map_stars.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Entry {
    std::string scope;
    std::string path;
    std::string name;
    bool isFolder;
};

static std::vector<Entry> g_entries;
static bool g_loaded = false;

/* Build the absolute path to map_stars.txt under SDL_GetPrefPath.
 * Caches the result so we don't hit SDL_GetPrefPath each save. */
static const char *starFilePath() {
    static char path[512];
    static bool resolved = false;
    if (!resolved) {
        const char *dir = SDL_GetPrefPath("WinBolo", "WinBolo");
        if (dir && *dir) {
            SDL_snprintf(path, sizeof(path), "%smap_stars.txt", dir);
        } else {
            SDL_strlcpy(path, "map_stars.txt", sizeof(path));
        }
        resolved = true;
    }
    return path;
}

/* Linear scan — N is small. Returns iterator-style index or -1. */
static int findIndex(const char *scope, const char *path) {
    if (!scope) scope = "";
    if (!path) return -1;
    for (size_t i = 0; i < g_entries.size(); i++) {
        if (g_entries[i].scope == scope && g_entries[i].path == path) {
            return (int)i;
        }
    }
    return -1;
}

static void writeAll() {
    FILE *fp = fopen(starFilePath(), "wb");
    if (!fp) return;
    for (const Entry &e : g_entries) {
        /* Sanitise: strip tabs and newlines from each field so a
         * pathological entry can't corrupt the line-based format.
         * In practice none of the providers emit such characters,
         * but be defensive. */
        auto clean = [](const std::string &s) {
            std::string out;
            out.reserve(s.size());
            for (char c : s) {
                if (c == '\t' || c == '\n' || c == '\r') continue;
                out.push_back(c);
            }
            return out;
        };
        fprintf(fp, "%s\t%s\t%d\t%s\n",
                clean(e.scope).c_str(),
                clean(e.path).c_str(),
                e.isFolder ? 1 : 0,
                clean(e.name).c_str());
    }
    fclose(fp);
}

static void readAll() {
    g_entries.clear();
    FILE *fp = fopen(starFilePath(), "rb");
    if (!fp) return;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline / carriage-return. */
        size_t len = SDL_strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        /* Split on tabs in-place. */
        char *p = line;
        char *fields[4] = { nullptr, nullptr, nullptr, nullptr };
        int n = 0;
        fields[n++] = p;
        while (*p && n < 4) {
            if (*p == '\t') {
                *p = '\0';
                fields[n++] = p + 1;
            }
            p++;
        }
        if (n < 4) continue; /* malformed — skip silently */

        Entry e;
        e.scope    = fields[0];
        e.path     = fields[1];
        e.isFolder = (fields[2][0] == '1');
        e.name     = fields[3];
        g_entries.push_back(e);
    }
    fclose(fp);
}

} /* namespace */

extern "C" {

void mapStarsInit(void) {
    if (g_loaded) return;
    g_loaded = true;
    readAll();
}

bool mapStarsIsStarred(const char *scope, const char *path) {
    if (!g_loaded) mapStarsInit();
    return findIndex(scope, path) >= 0;
}

void mapStarsToggle(const char *scope, const char *path,
                    const char *name, bool isFolder) {
    if (!g_loaded) mapStarsInit();
    if (!scope) scope = "";
    if (!path || !*path) return;
    int idx = findIndex(scope, path);
    if (idx >= 0) {
        g_entries.erase(g_entries.begin() + idx);
    } else {
        Entry e;
        e.scope    = scope;
        e.path     = path;
        e.name     = (name && *name) ? name : path;
        e.isFolder = isFolder;
        g_entries.push_back(e);
    }
    writeAll();
}

size_t mapStarsVisitScope(const char *scope, MapStarsVisitor cb, void *ctx) {
    if (!g_loaded) mapStarsInit();
    if (!scope) scope = "";
    if (!cb) return 0;
    size_t visited = 0;
    for (const Entry &e : g_entries) {
        if (e.scope != scope) continue;
        visited++;
        if (!cb(e.path.c_str(), e.name.c_str(), e.isFolder, ctx)) break;
    }
    return visited;
}

} /* extern "C" */
