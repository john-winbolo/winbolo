/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * mp_diag_log.c — see header.  Temporary diagnostic logger; written to
 * be removable in one grep when the regression is fixed.
 */

#include "mp_diag_log.h"

#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_timer.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#  include <process.h>
#  define mpDiagLogGetPid() ((int)_getpid())
#else
#  include <unistd.h>
#  define mpDiagLogGetPid() ((int)getpid())
#endif

static FILE       *gFile = NULL;
static SDL_Mutex  *gMutex = NULL;
static int         gInitDone = 0;
static int         gInitFailed = 0;
static int         gDiagLogEnabled = 0;
static Uint64      gStartTicks = 0;

static void mpDiagLogClose(void) {
    if (gFile != NULL) {
        fclose(gFile);
        gFile = NULL;
    }
}

static void mpDiagLogInit(void) {
    const char *override;
    char path[256];
    if (gInitDone || gInitFailed) return;
    gMutex = SDL_CreateMutex();
    if (gMutex == NULL) {
        gInitFailed = 1;
        return;
    }
    override = getenv("MP_DIAG_LOG_FILE");
    if (override != NULL && override[0] != '\0') {
        snprintf(path, sizeof(path), "%s", override);
    } else {
        snprintf(path, sizeof(path), "mp-logging-%d.txt", mpDiagLogGetPid());
    }
    gFile = fopen(path, "w");
    if (gFile == NULL) {
        SDL_DestroyMutex(gMutex);
        gMutex = NULL;
        gInitFailed = 1;
        return;
    }
    /* Line-buffered with a real buffer: stdio flushes on newline (which
     * every mpDiagLog call writes), but the per-line fwrite no longer
     * hits the OS directly. The previous _IONBF + explicit fflush per
     * call collapsed the bot brain budget once the in-process control
     * bus started fanning CTRL_CHAT out to ~12 ClientSim subscribers per
     * publish. Worst-case loss on a hard crash is one buffer's worth of
     * tail, which is acceptable for a diagnostic log. */
    setvbuf(gFile, NULL, _IOLBF, 8192);
    gStartTicks = SDL_GetTicks();
    atexit(mpDiagLogClose);
    gInitDone = 1;
    /* First line names ourselves so multi-process tails are unambiguous. */
    fprintf(gFile, "[     0.000] mp_diag_log: pid=%d file='%s'\n",
            mpDiagLogGetPid(), path);
}

void mpDiagLogEnable(int enable) {
    /* Hardcoded OFF: flip this to `gDiagLogEnabled = enable ? 1 : 0;` to re-enable
     * the diag log. Disabled by default so no file is written and the call
     * sites short-circuit at the gDiagLogEnabled check in mpDiagLog. */
    (void)enable;
    gDiagLogEnabled = 0;
}

int mpDiagLogIsEnabled(void) {
    return gDiagLogEnabled;
}

void mpDiagLog(const char *fmt, ...) {
    va_list ap;
    Uint64 elapsed;
    if (!gDiagLogEnabled) return;
    if (!gInitDone) {
        mpDiagLogInit();
        if (!gInitDone) return;
    }
    SDL_LockMutex(gMutex);
    elapsed = SDL_GetTicks() - gStartTicks;
    fprintf(gFile, "[%6llu.%03llu] ",
            (unsigned long long)(elapsed / 1000),
            (unsigned long long)(elapsed % 1000));
    va_start(ap, fmt);
    vfprintf(gFile, fmt, ap);
    va_end(ap);
    fputc('\n', gFile);
    SDL_UnlockMutex(gMutex);
}
