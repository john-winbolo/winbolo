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
static int         gEnabled = 0;
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
    /* Unbuffered — we explicitly fflush() after every write, so any
     * stdio buffer is just overhead.  Crucially, MSVC's debug ucrt
     * asserts (via __debugbreak/0x80000003) when setvbuf gets
     * _IOLBF/_IOFBF with size=0; POSIX accepts it as "use default".
     * _IONBF doesn't take a size hint and works everywhere. */
    setvbuf(gFile, NULL, _IONBF, 0);
    gStartTicks = SDL_GetTicks();
    atexit(mpDiagLogClose);
    gInitDone = 1;
    /* First line names ourselves so multi-process tails are unambiguous. */
    fprintf(gFile, "[     0.000] mp_diag_log: pid=%d file='%s'\n",
            mpDiagLogGetPid(), path);
    fflush(gFile);
}

void mpDiagLogEnable(int enable) {
    gEnabled = enable ? 1 : 0;
    /* Lazy-init on the first enable so the file isn't created at all
     * if the user never starts an MP host this session. */
    if (gEnabled && !gInitDone) {
        mpDiagLogInit();
    }
    if (gEnabled && gInitDone) {
        Uint64 elapsed = SDL_GetTicks() - gStartTicks;
        SDL_LockMutex(gMutex);
        fprintf(gFile, "[%6llu.%03llu] mp_diag_log: ENABLE\n",
                (unsigned long long)(elapsed / 1000),
                (unsigned long long)(elapsed % 1000));
        fflush(gFile);
        SDL_UnlockMutex(gMutex);
    } else if (!gEnabled && gInitDone) {
        Uint64 elapsed = SDL_GetTicks() - gStartTicks;
        SDL_LockMutex(gMutex);
        fprintf(gFile, "[%6llu.%03llu] mp_diag_log: DISABLE\n",
                (unsigned long long)(elapsed / 1000),
                (unsigned long long)(elapsed % 1000));
        fflush(gFile);
        SDL_UnlockMutex(gMutex);
    }
}

int mpDiagLogIsEnabled(void) {
    return gEnabled;
}

void mpDiagLog(const char *fmt, ...) {
    va_list ap;
    Uint64 elapsed;
    if (!gEnabled) return;
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
    fflush(gFile);
    SDL_UnlockMutex(gMutex);
}
