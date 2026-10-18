/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * logviewer_spawn.h - the parts of "Spawn in log viewer" that need no SDL
 *
 * The round recap's "Spawn in log viewer" button starts the separate Log
 * Viewer program on a copy of the round's log (lobby_reel.cpp). The parts of
 * that which are only text are here, header only, so the unit tests can use
 * them (tests/unit/test_logviewer_spawn.c):
 *
 *   lvSpawnViewerCandidates - the places the Log Viewer program can be,
 *       from the folder WinBolo runs from (SDL_GetBasePath), in the order
 *       to try them.
 *   lvSpawnCopyName - the file name of the copy of the log the viewer
 *       opens: the time, then the map name with only safe letters.
 *   lvSpawnIsCopyName - whether a file name is one lvSpawnCopyName makes,
 *       so that only those are deleted when old copies are removed.
 *   lvSpawnArgs - the argument list for the new process: the program, the
 *       log and the end. No shell reads it, so spaces and letters outside
 *       ASCII in the two paths need no quotes.
 */

#ifndef LOGVIEWER_SPAWN_H
#define LOGVIEWER_SPAWN_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The longest path these helpers make, with its end. */
#define LV_SPAWN_PATH_MAX 1024
/* The most places lvSpawnViewerCandidates gives. */
#define LV_SPAWN_CANDIDATES_MAX 3
/* The longest map part of a copy's name, in bytes. */
#define LV_SPAWN_MAP_PART_MAX 40
/* How many copies stay in the folder. Older ones are deleted when a new
 * copy is made. */
#define LV_SPAWN_KEEP_COPIES 8

/* The kinds of system, for the program's name and place. */
#define LV_SPAWN_OS_WINDOWS 0
#define LV_SPAWN_OS_MACOS   1
#define LV_SPAWN_OS_OTHER   2

#if defined(_WIN32)
#define LV_SPAWN_OS_THIS LV_SPAWN_OS_WINDOWS
#elif defined(__APPLE__)
#define LV_SPAWN_OS_THIS LV_SPAWN_OS_MACOS
#else
#define LV_SPAWN_OS_THIS LV_SPAWN_OS_OTHER
#endif

/* The Log Viewer's output name (CMakeLists.txt: OUTPUT_NAME "Log Viewer"). */
#define LV_SPAWN_EXE_WINDOWS "Log Viewer.exe"
#define LV_SPAWN_EXE_OTHER   "Log Viewer"
#define LV_SPAWN_APP_MACOS   "Log Viewer.app/Contents/MacOS/Log Viewer"
/* What SDL_GetBasePath gives inside a macOS bundle ends with this. */
#define LV_SPAWN_BUNDLE_TAIL ".app/Contents/Resources/"

/* dir, then name, into out. dir gets a separator at its end when it has
 * none. False, with out empty, when the two do not go in. */
static inline int lvSpawnJoin(char *out, size_t outSize, const char *dir,
                              size_t dirLen, const char *name, int os) {
    const char sep = (os == LV_SPAWN_OS_WINDOWS) ? '\\' : '/';
    int        needSep;
    size_t     nameLen;
    if (out == NULL || outSize == 0) return 0;
    out[0] = '\0';
    if (dir == NULL || name == NULL) return 0;
    needSep = dirLen > 0 && dir[dirLen - 1] != '/' && dir[dirLen - 1] != '\\';
    nameLen = strlen(name);
    if (dirLen + (size_t)needSep + nameLen + 1 > outSize) return 0;
    memcpy(out, dir, dirLen);
    if (needSep) out[dirLen] = sep;
    memcpy(out + dirLen + needSep, name, nameLen + 1);
    return 1;
}

/* The places the Log Viewer can be, in the order to try them, into out.
 * basePath is the folder WinBolo runs from, as SDL_GetBasePath gives it.
 * Gives how many went into out (none when basePath is NULL or empty).
 *
 *   Windows - "Log Viewer.exe" in the same folder as WinBolo.exe.
 *   macOS   - in a bundle (basePath ends in ".app/Contents/Resources/"):
 *             "Log Viewer.app" in the folder that holds WinBolo.app, as the
 *             disk image has them, then a "Log Viewer.app" in the bundle's
 *             Resources. Out of a bundle: "Log Viewer.app" in the folder,
 *             then a plain "Log Viewer" program there.
 *   Other   - "Log Viewer" in the same folder as the game. */
static inline int lvSpawnViewerCandidates(const char *basePath, int os,
                                          char out[][LV_SPAWN_PATH_MAX],
                                          int maxOut) {
    int    n = 0;
    size_t baseLen;
    if (basePath == NULL || out == NULL || maxOut <= 0) return 0;
    baseLen = strlen(basePath);
    if (baseLen == 0) return 0;

    if (os == LV_SPAWN_OS_WINDOWS) {
        if (lvSpawnJoin(out[n], LV_SPAWN_PATH_MAX, basePath, baseLen,
                        LV_SPAWN_EXE_WINDOWS, os)) {
            n++;
        }
        return n;
    }
    if (os == LV_SPAWN_OS_MACOS) {
        const size_t tailLen = strlen(LV_SPAWN_BUNDLE_TAIL);
        if (baseLen > tailLen &&
            strcmp(basePath + baseLen - tailLen, LV_SPAWN_BUNDLE_TAIL) == 0) {
            /* ".../WinBolo.app/Contents/Resources/": back to the '/' before
             * the bundle's own name, which ends the folder that holds it. */
            size_t appEnd = baseLen - strlen("Contents/Resources/");
            size_t cut    = appEnd - 1; /* the '/' after ".app" */
            while (cut > 0 && basePath[cut - 1] != '/') cut--;
            if (cut > 0 && n < maxOut &&
                lvSpawnJoin(out[n], LV_SPAWN_PATH_MAX, basePath, cut,
                            LV_SPAWN_APP_MACOS, os)) {
                n++;
            }
            if (n < maxOut &&
                lvSpawnJoin(out[n], LV_SPAWN_PATH_MAX, basePath, baseLen,
                            LV_SPAWN_APP_MACOS, os)) {
                n++;
            }
            return n;
        }
        if (n < maxOut &&
            lvSpawnJoin(out[n], LV_SPAWN_PATH_MAX, basePath, baseLen,
                        LV_SPAWN_APP_MACOS, os)) {
            n++;
        }
        if (n < maxOut &&
            lvSpawnJoin(out[n], LV_SPAWN_PATH_MAX, basePath, baseLen,
                        LV_SPAWN_EXE_OTHER, os)) {
            n++;
        }
        return n;
    }
    if (lvSpawnJoin(out[n], LV_SPAWN_PATH_MAX, basePath, baseLen,
                    LV_SPAWN_EXE_OTHER, os)) {
        n++;
    }
    return n;
}

/* The name of a copy of a round's log, into out:
 * "YYYYMMDDtHHMMSS_<map>.wbv", or "YYYYMMDDtHHMMSS_<map>-<seq>.wbv" when seq
 * is not 0 (a second copy in the same second). In the map part only ASCII
 * letters, digits, '-' and '_' stay; every other byte, a space and the bytes
 * of letters outside ASCII too, becomes '_', and the part is cut at
 * LV_SPAWN_MAP_PART_MAX bytes. No map name gives "round". False, with out
 * empty, when the name does not go in. */
static inline int lvSpawnCopyName(char *out, size_t outSize, int year, int month,
                                  int day, int hour, int minute, int second,
                                  unsigned seq, const char *mapName) {
    char   map[LV_SPAWN_MAP_PART_MAX + 1];
    size_t m = 0;
    int    len;
    if (out == NULL || outSize == 0) return 0;
    out[0] = '\0';
    if (mapName != NULL) {
        const unsigned char *p = (const unsigned char *)mapName;
        for (; *p != '\0' && m < LV_SPAWN_MAP_PART_MAX; p++) {
            const unsigned char c = *p;
            const int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '-' || c == '_';
            map[m++] = keep ? (char)c : '_';
        }
    }
    map[m] = '\0';
    if (m == 0) {
        memcpy(map, "round", sizeof("round"));
    }
    if (year < 0 || year > 9999 || month < 0 || month > 99 || day < 0 ||
        day > 99 || hour < 0 || hour > 99 || minute < 0 || minute > 99 ||
        second < 0 || second > 99) {
        return 0;
    }
    if (seq == 0) {
        len = snprintf(out, outSize, "%04d%02d%02dt%02d%02d%02d_%s.wbv", year,
                       month, day, hour, minute, second, map);
    } else {
        len = snprintf(out, outSize, "%04d%02d%02dt%02d%02d%02d_%s-%u.wbv", year,
                       month, day, hour, minute, second, map, seq);
    }
    if (len < 0 || (size_t)len >= outSize) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

/* Whether name is one lvSpawnCopyName makes: eight digits, 't', six digits,
 * '_', at least one more byte, and ".wbv" at the end. */
static inline int lvSpawnIsCopyName(const char *name) {
    size_t len;
    int    i;
    if (name == NULL) return 0;
    len = strlen(name);
    /* 15 for the time, 1 for '_', at least 1 for the map, 4 for ".wbv". */
    if (len < 15 + 1 + 1 + 4) return 0;
    for (i = 0; i < 15; i++) {
        if (i == 8) {
            if (name[i] != 't') return 0;
        } else if (name[i] < '0' || name[i] > '9') {
            return 0;
        }
    }
    if (name[15] != '_') return 0;
    return strcmp(name + len - 4, ".wbv") == 0;
}

/* The argument list for the new process into out: the program, the log,
 * then NULL. */
static inline void lvSpawnArgs(const char *exePath, const char *logPath,
                               const char *out[3]) {
    out[0] = exePath;
    out[1] = logPath;
    out[2] = NULL;
}

#endif /* LOGVIEWER_SPAWN_H */
