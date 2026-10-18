/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The parts of the round recap's "Spawn in log viewer" button that need no
 * SDL (logviewer_spawn.h).
 *
 *   logviewer_spawn_paths - where the Log Viewer program is looked for on
 *       Windows, on macOS in and out of a bundle, and on other systems; a
 *       base path with no separator at its end; no base path; a path that
 *       does not go in.
 *   logviewer_spawn_names - the name of the copy of the log: the time, the
 *       map with only safe letters, a second copy in the same second, no
 *       map, a long map; which names are copies; the argument list.
 */

#include <stddef.h>
#include <string.h>

#include "logviewer_spawn.h"
#include "test_harness.h"

int run_logviewer_spawn_paths(void) {
    char out[LV_SPAWN_CANDIDATES_MAX][LV_SPAWN_PATH_MAX];
    int  n;

    /* Windows: the program beside WinBolo.exe, in a folder with a space and
     * letters outside ASCII ("Zoë"). */
    n = lvSpawnViewerCandidates("C:\\Games\\Zo\xC3\xAB WinBolo\\", LV_SPAWN_OS_WINDOWS,
                                out, LV_SPAWN_CANDIDATES_MAX);
    UT_ASSERT_MSG(n == 1, "windows gave %d places", n);
    UT_ASSERT_MSG(strcmp(out[0], "C:\\Games\\Zo\xC3\xAB WinBolo\\Log Viewer.exe") == 0,
                  "windows: \"%s\"", out[0]);

    /* A base path with no separator at its end gets one. */
    n = lvSpawnViewerCandidates("C:\\WinBolo", LV_SPAWN_OS_WINDOWS, out,
                                LV_SPAWN_CANDIDATES_MAX);
    UT_ASSERT(n == 1);
    UT_ASSERT_MSG(strcmp(out[0], "C:\\WinBolo\\Log Viewer.exe") == 0, "no end: \"%s\"",
                  out[0]);
    n = lvSpawnViewerCandidates("/opt/winbolo", LV_SPAWN_OS_OTHER, out,
                                LV_SPAWN_CANDIDATES_MAX);
    UT_ASSERT(n == 1);
    UT_ASSERT_MSG(strcmp(out[0], "/opt/winbolo/Log Viewer") == 0, "no end: \"%s\"",
                  out[0]);

    /* Linux and other systems: the program beside the game. */
    n = lvSpawnViewerCandidates("/home/a/winbolo/", LV_SPAWN_OS_OTHER, out,
                                LV_SPAWN_CANDIDATES_MAX);
    UT_ASSERT(n == 1);
    UT_ASSERT_MSG(strcmp(out[0], "/home/a/winbolo/Log Viewer") == 0, "other: \"%s\"",
                  out[0]);

    /* macOS in a bundle: "Log Viewer.app" beside WinBolo.app first, then one
     * in the bundle's Resources. */
    n = lvSpawnViewerCandidates("/Applications/WinBolo/WinBolo.app/Contents/Resources/",
                                LV_SPAWN_OS_MACOS, out, LV_SPAWN_CANDIDATES_MAX);
    UT_ASSERT_MSG(n == 2, "bundle gave %d places", n);
    UT_ASSERT_MSG(strcmp(out[0], "/Applications/WinBolo/Log Viewer.app/Contents/MacOS/"
                                 "Log Viewer") == 0,
                  "bundle 1: \"%s\"", out[0]);
    UT_ASSERT_MSG(strcmp(out[1], "/Applications/WinBolo/WinBolo.app/Contents/Resources/"
                                 "Log Viewer.app/Contents/MacOS/Log Viewer") == 0,
                  "bundle 2: \"%s\"", out[1]);
    /* A bundle at the top of the disk. */
    n = lvSpawnViewerCandidates("/WinBolo.app/Contents/Resources/", LV_SPAWN_OS_MACOS,
                                out, LV_SPAWN_CANDIDATES_MAX);
    UT_ASSERT(n == 2);
    UT_ASSERT_MSG(strcmp(out[0], "/Log Viewer.app/Contents/MacOS/Log Viewer") == 0,
                  "top: \"%s\"", out[0]);
    /* Only as many as the caller has room for. */
    n = lvSpawnViewerCandidates("/Applications/WinBolo.app/Contents/Resources/",
                                LV_SPAWN_OS_MACOS, out, 1);
    UT_ASSERT(n == 1);
    UT_ASSERT(strcmp(out[0], "/Applications/Log Viewer.app/Contents/MacOS/Log Viewer") == 0);

    /* macOS out of a bundle (a build folder): the app there, then a plain
     * program there. */
    n = lvSpawnViewerCandidates("/Users/a/winbolo/build/", LV_SPAWN_OS_MACOS, out,
                                LV_SPAWN_CANDIDATES_MAX);
    UT_ASSERT_MSG(n == 2, "no bundle gave %d places", n);
    UT_ASSERT(strcmp(out[0],
                     "/Users/a/winbolo/build/Log Viewer.app/Contents/MacOS/Log Viewer") == 0);
    UT_ASSERT(strcmp(out[1], "/Users/a/winbolo/build/Log Viewer") == 0);

    /* No base path: nowhere to look. */
    UT_ASSERT(lvSpawnViewerCandidates(NULL, LV_SPAWN_OS_WINDOWS, out,
                                      LV_SPAWN_CANDIDATES_MAX) == 0);
    UT_ASSERT(lvSpawnViewerCandidates("", LV_SPAWN_OS_OTHER, out,
                                      LV_SPAWN_CANDIDATES_MAX) == 0);
    UT_ASSERT(lvSpawnViewerCandidates("/a/", LV_SPAWN_OS_OTHER, out, 0) == 0);

    /* A path that does not go in is left out, not cut short. */
    {
        static char longBase[LV_SPAWN_PATH_MAX];
        memset(longBase, 'a', sizeof(longBase) - 8);
        longBase[0]                  = '/';
        longBase[sizeof(longBase) - 8] = '/';
        longBase[sizeof(longBase) - 7] = '\0';
        UT_ASSERT(lvSpawnViewerCandidates(longBase, LV_SPAWN_OS_OTHER, out,
                                          LV_SPAWN_CANDIDATES_MAX) == 0);
    }
    return 0;
}

int run_logviewer_spawn_names(void) {
    char        name[128];
    const char *args[LV_SPAWN_ARGS_MAX];
    char        bundle[LV_SPAWN_PATH_MAX];

    /* The time and the map. */
    UT_ASSERT(lvSpawnCopyName(name, sizeof(name), 2026, 10, 18, 9, 5, 7, 0, "Everard"));
    UT_ASSERT_MSG(strcmp(name, "20261018t090507_Everard.wbv") == 0, "plain: \"%s\"", name);
    UT_ASSERT(lvSpawnIsCopyName(name));

    /* Spaces, punctuation and letters outside ASCII become '_'; '-' and '_'
     * stay. "Zoë" is three letters and four bytes, so two '_'. */
    UT_ASSERT(lvSpawnCopyName(name, sizeof(name), 2026, 1, 2, 3, 4, 5, 0,
                              "Oil Rig: Zo\xC3\xAB's-map_2"));
    UT_ASSERT_MSG(strcmp(name, "20260102t030405_Oil_Rig__Zo___s-map_2.wbv") == 0,
                  "letters: \"%s\"", name);
    UT_ASSERT(lvSpawnIsCopyName(name));

    /* A second copy in the same second. */
    UT_ASSERT(lvSpawnCopyName(name, sizeof(name), 2026, 10, 18, 9, 5, 7, 3, "Everard"));
    UT_ASSERT_MSG(strcmp(name, "20261018t090507_Everard-3.wbv") == 0, "seq: \"%s\"", name);
    UT_ASSERT(lvSpawnIsCopyName(name));

    /* No map name. */
    UT_ASSERT(lvSpawnCopyName(name, sizeof(name), 2026, 10, 18, 9, 5, 7, 0, NULL));
    UT_ASSERT_MSG(strcmp(name, "20261018t090507_round.wbv") == 0, "null map: \"%s\"", name);
    UT_ASSERT(lvSpawnCopyName(name, sizeof(name), 2026, 10, 18, 9, 5, 7, 0, ""));
    UT_ASSERT_MSG(strcmp(name, "20261018t090507_round.wbv") == 0, "empty map: \"%s\"", name);

    /* A long map name is cut at LV_SPAWN_MAP_PART_MAX bytes. */
    {
        char longMap[100];
        memset(longMap, 'm', sizeof(longMap) - 1);
        longMap[sizeof(longMap) - 1] = '\0';
        UT_ASSERT(lvSpawnCopyName(name, sizeof(name), 2026, 10, 18, 9, 5, 7, 0, longMap));
        UT_ASSERT_MSG(strlen(name) == 15 + 1 + LV_SPAWN_MAP_PART_MAX + 4,
                      "long map name is %u bytes", (unsigned)strlen(name));
    }

    /* A name that does not go in, and a time out of range. */
    UT_ASSERT(!lvSpawnCopyName(name, 20, 2026, 10, 18, 9, 5, 7, 0, "Everard"));
    UT_ASSERT(name[0] == '\0');
    UT_ASSERT(!lvSpawnCopyName(name, sizeof(name), 2026, 10, 18, 100, 5, 7, 0, "E"));
    UT_ASSERT(!lvSpawnCopyName(name, sizeof(name), -1, 10, 18, 9, 5, 7, 0, "E"));

    /* Only the names the copies have are copies: not the game's own logs
     * nor anything else a person keeps in the folder. */
    UT_ASSERT(!lvSpawnIsCopyName("singleplayer.wbv"));
    UT_ASSERT(!lvSpawnIsCopyName("20261018t090507_.wbv"));
    UT_ASSERT(!lvSpawnIsCopyName("20261018x090507_Everard.wbv"));
    UT_ASSERT(!lvSpawnIsCopyName("20261018t090507_Everard.txt"));
    UT_ASSERT(!lvSpawnIsCopyName("2026101t8090507_Everard.wbv"));
    UT_ASSERT(!lvSpawnIsCopyName(NULL));

    /* The argument list: the program, the log, the end. Paths with spaces
     * go in as they are, with no quotes. */
    UT_ASSERT(lvSpawnArgs(LV_SPAWN_OS_WINDOWS, "C:\\Program Files\\WinBolo\\Log Viewer.exe",
                          "C:\\Users\\Zo\xC3\xAB\\a b.wbv", bundle, sizeof(bundle), args) == 2);
    UT_ASSERT(strcmp(args[0], "C:\\Program Files\\WinBolo\\Log Viewer.exe") == 0);
    UT_ASSERT(strcmp(args[1], "C:\\Users\\Zo\xC3\xAB\\a b.wbv") == 0);
    UT_ASSERT(args[2] == NULL);
    UT_ASSERT(lvSpawnArgs(LV_SPAWN_OS_OTHER, "/opt/winbolo/Log Viewer", "/tmp/a b.wbv",
                          bundle, sizeof(bundle), args) == 2);
    UT_ASSERT(strcmp(args[0], "/opt/winbolo/Log Viewer") == 0);
    UT_ASSERT(args[2] == NULL);

    /* macOS, the program inside its bundle: open starts the bundle, with -n
     * for a new copy, and the log after --args. The bundle is the path up
     * to the last "/Contents/MacOS/", so a folder of that name higher up is
     * left alone. */
    UT_ASSERT_MSG(lvSpawnArgs(LV_SPAWN_OS_MACOS,
                              "/Users/Zo\xC3\xAB/Contents/MacOS/Games/Log Viewer.app/Contents/MacOS/Log Viewer",
                              "/Users/Zo\xC3\xAB/a b.wbv", bundle, sizeof(bundle), args) == 6,
                  "macOS bundle gave the wrong count");
    UT_ASSERT(strcmp(args[0], "/usr/bin/open") == 0);
    UT_ASSERT(strcmp(args[1], "-n") == 0);
    UT_ASSERT(strcmp(args[2], "-a") == 0);
    UT_ASSERT_MSG(strcmp(args[3], "/Users/Zo\xC3\xAB/Contents/MacOS/Games/Log Viewer.app") == 0,
                  "bundle: \"%s\"", args[3]);
    UT_ASSERT(strcmp(args[4], "--args") == 0);
    UT_ASSERT(strcmp(args[5], "/Users/Zo\xC3\xAB/a b.wbv") == 0);
    UT_ASSERT(args[6] == NULL);

    /* macOS, a plain program in no bundle: run as it is. */
    UT_ASSERT(lvSpawnArgs(LV_SPAWN_OS_MACOS, "/opt/winbolo/Log Viewer", "/tmp/a.wbv",
                          bundle, sizeof(bundle), args) == 2);
    UT_ASSERT(strcmp(args[0], "/opt/winbolo/Log Viewer") == 0);
    UT_ASSERT(args[2] == NULL);

    /* A bundle path that does not fit: run the program as it is rather than
     * give open a cut path. */
    {
        char small[8];
        UT_ASSERT(lvSpawnArgs(LV_SPAWN_OS_MACOS, "/Applications/Log Viewer.app/Contents/MacOS/Log Viewer",
                              "/tmp/a.wbv", small, sizeof(small), args) == 2);
        UT_ASSERT(strcmp(args[0], "/Applications/Log Viewer.app/Contents/MacOS/Log Viewer") == 0);
    }
    UT_ASSERT(lvSpawnArgs(LV_SPAWN_OS_MACOS, NULL, "/tmp/a.wbv", bundle, sizeof(bundle), args) == 0);
    UT_ASSERT(args[0] == NULL);
    return 0;
}
