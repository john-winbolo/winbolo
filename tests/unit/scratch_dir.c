/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Private scratch directory for any test that needs a file on disk.
 *
 * utCurrentTestName (test_harness.h) already keeps two tests in ONE run
 * apart. What it cannot do is keep two RUNS apart, because the name it
 * hands back is deliberately the same every time. Two ctest invocations —
 * a developer alongside an agent, or two worktrees on one machine — then
 * pick the same path for the same test and delete each other's files
 * between a write and the read that follows it.
 *
 * Measured on this tree before the change: `prefs_api_roundtrip` run 40
 * times alone failed 0 times; the same test run from two build directories
 * at once failed 40 of 80. The prefs tests were the worst of it because
 * their paths were absolute ("/tmp/winbolo_ut_*.json"), which put every
 * checkout on the machine on the same file, but nothing about the problem
 * was specific to them.
 *
 * So the layout is <build dir>/test-scratch/<pid>/<test name>/<leaf>:
 *
 *   the build dir  separates two worktrees,
 *   the pid        separates two runs sharing one build dir,
 *   the test name  separates two tests inside one run,
 *   and the leaf is whatever the test wants to call its file.
 *
 * The build-dir part is baked in at compile time (WB_TEST_SCRATCH_DIR)
 * rather than left relative to the working directory, so it holds however
 * the binary is invoked.
 *
 * CLEANUP. Each process removes its own <pid> subtree at exit, so a run
 * that finishes leaves nothing behind. It does NOT sweep the subtrees of
 * other pids, and nothing else should either: a blanket sweep at startup
 * would delete the files of a run already in flight, which is the very bug
 * this exists to fix. A crash therefore leaks one small pid directory —
 * that is the deliberate trade, and `rm -rf <build dir>/test-scratch` is
 * the cure if they ever pile up.
 */

#include "test_harness.h"

#include <SDL3/SDL.h>

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define UT_SCRATCH_PID() ((unsigned long)_getpid())
#else
#include <unistd.h>
#define UT_SCRATCH_PID() ((unsigned long)getpid())
#endif

/* Set by CMake to "${CMAKE_BINARY_DIR}/test-scratch". The fallback keeps a
 * hand-built translation unit compiling; it resolves against the working
 * directory, which for CTest is the build directory anyway. */
#ifndef WB_TEST_SCRATCH_DIR
#define WB_TEST_SCRATCH_DIR "test-scratch"
#endif

/* Cached directory for the test that asked last, and the name it was built
 * for. Running the binary bare walks every test in one process, so the name
 * changes under us and the cache has to notice. */
static char s_dir[1024];
static char s_dirTest[128];
static bool s_atexitArmed;

static SDL_EnumerationResult utScratchRemoveEntry(void *userdata,
                                                  const char *dirname,
                                                  const char *fname);

/* SDL_RemovePath refuses a directory with anything in it, and a test that
 * failed part way through has usually left something in it. */
static void utScratchRemoveTree(const char *path) {
    SDL_PathInfo info;

    if (!SDL_GetPathInfo(path, &info)) {
        return;
    }
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        SDL_EnumerateDirectory(path, utScratchRemoveEntry, NULL);
    }
    SDL_RemovePath(path);
}

static SDL_EnumerationResult utScratchRemoveEntry(void *userdata,
                                                  const char *dirname,
                                                  const char *fname) {
    char child[1024];

    (void)userdata;
    SDL_snprintf(child, sizeof(child), "%s/%s", dirname, fname);
    utScratchRemoveTree(child);
    return SDL_ENUM_CONTINUE;
}

static void utScratchAtExit(void) {
    char procDir[1024];

    SDL_snprintf(procDir, sizeof(procDir), "%s/%lu",
                 WB_TEST_SCRATCH_DIR, UT_SCRATCH_PID());
    utScratchRemoveTree(procDir);

    /* WB_TEST_SCRATCH_DIR itself is left alone on purpose — a concurrent
     * run owns a sibling <pid> directory under it, and an empty root costs
     * nothing. */
}

bool utScratchPath(char *out, size_t outSz, const char *leaf) {
    const char *test;

    if (out == NULL || outSz == 0) {
        return false;
    }
    out[0] = '\0';

    test = utCurrentTestName();
    if (test == NULL) {
        test = "none";
    }

    if (s_dir[0] == '\0' || strcmp(s_dirTest, test) != 0) {
        char procDir[1024];

        /* Each level is created separately: SDL_CreateDirectory does not
         * make parents, and it reports success when the directory is
         * already there, which is what the repeat calls rely on. */
        if (!SDL_CreateDirectory(WB_TEST_SCRATCH_DIR)) {
            return false;
        }
        SDL_snprintf(procDir, sizeof(procDir), "%s/%lu",
                     WB_TEST_SCRATCH_DIR, UT_SCRATCH_PID());
        if (!SDL_CreateDirectory(procDir)) {
            return false;
        }
        SDL_snprintf(s_dir, sizeof(s_dir), "%s/%s", procDir, test);
        if (!SDL_CreateDirectory(s_dir)) {
            s_dir[0] = '\0';
            return false;
        }
        SDL_strlcpy(s_dirTest, test, sizeof(s_dirTest));
    }

    if (!s_atexitArmed) {
        atexit(utScratchAtExit);
        s_atexitArmed = true;
    }

    if (leaf == NULL || leaf[0] == '\0') {
        SDL_snprintf(out, outSz, "%s", s_dir);
    } else {
        SDL_snprintf(out, outSz, "%s/%s", s_dir, leaf);
    }
    return true;
}
