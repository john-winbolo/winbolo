/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * wb_log.c — implementation. See wb_log.h for the API contract.
 */

#include "wb_log.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static FILE                *g_log_file = NULL;
static char                 g_log_path[1024];
static SDL_LogOutputFunction g_default_sink = NULL;
static void                *g_default_sink_userdata = NULL;
static bool                 g_installed = false;

/* ------------------------------------------------------------------ */
/* String <-> enum helpers                                             */
/* ------------------------------------------------------------------ */
SDL_LogPriority wb_log_parse_priority(const char *s) {
    if (!s || !*s) return SDL_LOG_PRIORITY_INVALID;
    if (SDL_strcasecmp(s, "trace")    == 0) return SDL_LOG_PRIORITY_TRACE;
    if (SDL_strcasecmp(s, "verbose")  == 0) return SDL_LOG_PRIORITY_VERBOSE;
    if (SDL_strcasecmp(s, "debug")    == 0) return SDL_LOG_PRIORITY_DEBUG;
    if (SDL_strcasecmp(s, "info")     == 0) return SDL_LOG_PRIORITY_INFO;
    if (SDL_strcasecmp(s, "warn")     == 0 ||
        SDL_strcasecmp(s, "warning")  == 0) return SDL_LOG_PRIORITY_WARN;
    if (SDL_strcasecmp(s, "error")    == 0) return SDL_LOG_PRIORITY_ERROR;
    if (SDL_strcasecmp(s, "critical") == 0) return SDL_LOG_PRIORITY_CRITICAL;
    /* "off" -> COUNT (above CRITICAL) effectively silences the category. */
    if (SDL_strcasecmp(s, "off")      == 0) return SDL_LOG_PRIORITY_COUNT;
    return SDL_LOG_PRIORITY_INVALID;
}

int wb_log_parse_category(const char *s) {
    if (!s || !*s) return -1;
    if (SDL_strcasecmp(s, "*") == 0 || SDL_strcasecmp(s, "all") == 0) return -2;
    if (SDL_strcasecmp(s, "net")       == 0) return WB_LOG_CAT_NET;
    if (SDL_strcasecmp(s, "server")    == 0) return WB_LOG_CAT_SERVER;
    if (SDL_strcasecmp(s, "sim")       == 0) return WB_LOG_CAT_SIM;
    if (SDL_strcasecmp(s, "client")    == 0) return WB_LOG_CAT_CLIENT;
    if (SDL_strcasecmp(s, "gui")       == 0) return WB_LOG_CAT_GUI;
    if (SDL_strcasecmp(s, "audio")     == 0) return WB_LOG_CAT_AUDIO;
    if (SDL_strcasecmp(s, "asset")     == 0) return WB_LOG_CAT_ASSET;
    if (SDL_strcasecmp(s, "lua")       == 0) return WB_LOG_CAT_LUA;
    if (SDL_strcasecmp(s, "map")       == 0) return WB_LOG_CAT_MAP;
    if (SDL_strcasecmp(s, "logviewer") == 0) return WB_LOG_CAT_LOGVIEWER;
    if (SDL_strcasecmp(s, "platform")  == 0) return WB_LOG_CAT_PLATFORM;
    return -1;
}

const char *wb_log_category_name(int category) {
    switch (category) {
        case WB_LOG_CAT_NET:       return "NET";
        case WB_LOG_CAT_SERVER:    return "SERVER";
        case WB_LOG_CAT_SIM:       return "SIM";
        case WB_LOG_CAT_CLIENT:    return "CLIENT";
        case WB_LOG_CAT_GUI:       return "GUI";
        case WB_LOG_CAT_AUDIO:     return "AUDIO";
        case WB_LOG_CAT_ASSET:     return "ASSET";
        case WB_LOG_CAT_LUA:       return "LUA";
        case WB_LOG_CAT_MAP:       return "MAP";
        case WB_LOG_CAT_LOGVIEWER: return "LOGVIEW";
        case WB_LOG_CAT_PLATFORM:  return "PLATFORM";
        case SDL_LOG_CATEGORY_APPLICATION: return "APP";
        case SDL_LOG_CATEGORY_ERROR:       return "ERR";
        case SDL_LOG_CATEGORY_ASSERT:      return "ASSERT";
        case SDL_LOG_CATEGORY_SYSTEM:      return "SYSTEM";
        case SDL_LOG_CATEGORY_AUDIO:       return "SDLAUDIO";
        case SDL_LOG_CATEGORY_VIDEO:       return "SDLVIDEO";
        case SDL_LOG_CATEGORY_RENDER:      return "RENDER";
        case SDL_LOG_CATEGORY_INPUT:       return "INPUT";
        case SDL_LOG_CATEGORY_TEST:        return "TEST";
        case SDL_LOG_CATEGORY_GPU:         return "GPU";
        default:                           return "?";
    }
}

static const char *prio_name(SDL_LogPriority p) {
    switch (p) {
        case SDL_LOG_PRIORITY_TRACE:    return "TRACE";
        case SDL_LOG_PRIORITY_VERBOSE:  return "VERB ";
        case SDL_LOG_PRIORITY_DEBUG:    return "DEBUG";
        case SDL_LOG_PRIORITY_INFO:     return "INFO ";
        case SDL_LOG_PRIORITY_WARN:     return "WARN ";
        case SDL_LOG_PRIORITY_ERROR:    return "ERROR";
        case SDL_LOG_PRIORITY_CRITICAL: return "CRIT ";
        default:                        return "?    ";
    }
}

/* ------------------------------------------------------------------ */
/* WINBOLO_LOG env var parsing                                         */
/* ------------------------------------------------------------------ */
/* spec: "net=trace,server=debug,*=info" — applied left to right, so
   later entries override earlier ones for the same category. */
static void apply_env_log_spec(const char *spec) {
    if (!spec || !*spec) return;
    const char *p = spec;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t segLen = comma ? (size_t)(comma - p) : strlen(p);
        const char *eq = (const char *)memchr(p, '=', segLen);
        if (eq) {
            char catName[32], prioName[32];
            size_t catLen  = (size_t)(eq - p);
            size_t prioLen = segLen - catLen - 1;
            if (catLen > 0 && catLen < sizeof catName &&
                prioLen > 0 && prioLen < sizeof prioName) {
                memcpy(catName, p, catLen);   catName[catLen] = '\0';
                memcpy(prioName, eq + 1, prioLen); prioName[prioLen] = '\0';
                int cat = wb_log_parse_category(catName);
                SDL_LogPriority prio = wb_log_parse_priority(prioName);
                if (prio != SDL_LOG_PRIORITY_INVALID) {
                    if (cat == -2) {
                        SDL_SetLogPriorities(prio);
                    } else if (cat >= 0) {
                        SDL_SetLogPriority(cat, prio);
                    }
                }
            }
        }
        if (!comma) break;
        p = comma + 1;
    }
}

/* ------------------------------------------------------------------ */
/* File rotation                                                       */
/* ------------------------------------------------------------------ */
/* path -> path.1 -> path.2 -> path.3 (oldest dropped). Failures are
   silent — rotation is best-effort. */
static void rotate_existing(const char *path) {
    char cur[1100], nxt[1100];

    /* Drop oldest. */
    SDL_snprintf(nxt, sizeof nxt, "%s.3", path);
    remove(nxt);

    /* path.2 -> path.3 ; path.1 -> path.2 ; path -> path.1 */
    for (int i = 2; i >= 0; --i) {
        if (i == 0) {
            SDL_snprintf(cur, sizeof cur, "%s", path);
        } else {
            SDL_snprintf(cur, sizeof cur, "%s.%d", path, i);
        }
        SDL_snprintf(nxt, sizeof nxt, "%s.%d", path, i + 1);
        rename(cur, nxt);
    }
}

/* ------------------------------------------------------------------ */
/* Output sink                                                         */
/* ------------------------------------------------------------------ */
/* SDL guarantees this is called serialized — its own internal mutex is
   held — so we don't need our own lock. */
static void file_sink(void *userdata,
                      int category,
                      SDL_LogPriority priority,
                      const char *message) {
    /* Chain to the default sink first so console/stderr/OutputDebugString
       behave exactly as before this module was installed. */
    if (g_default_sink) {
        g_default_sink(g_default_sink_userdata, category, priority, message);
    }

    if (!g_log_file) return;

    /* Wall-clock timestamp + monotonic ms for ordering. */
    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    Uint64 ms = SDL_GetTicks();

    fprintf(g_log_file,
            "[%s.%03u] [tid %010llu] [%-8s] [%s] %s\n",
            ts,
            (unsigned)(ms % 1000u),
            (unsigned long long)SDL_GetCurrentThreadID(),
            wb_log_category_name(category),
            prio_name(priority),
            message ? message : "");
    fflush(g_log_file);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
bool wb_log_init(const char *prefOrgName,
                 const char *prefAppName,
                 const char *logFileBaseName) {
    if (g_installed) return true;

    /* Logging is opt-in. With no WINBOLO_LOG spec we install a silent sink,
       open no log file, and emit nothing to console or disk. Set
       WINBOLO_LOG (e.g. "*=info" or "net=trace,server=debug") to turn
       output back on; the spec selects which categories are written. */
    const char *logSpec = SDL_getenv("WINBOLO_LOG");
    const bool enabled = (logSpec && *logSpec);

    g_log_path[0] = '\0';
    if (enabled && logFileBaseName && *logFileBaseName) {
        /* Resolve directory: prefer SDL_GetPrefPath when both org+app given;
           otherwise fall back to base path; otherwise current working dir. */
        char *dir = NULL;  /* must be SDL_free()d if non-null */
        if (prefOrgName && prefAppName) {
            dir = SDL_GetPrefPath(prefOrgName, prefAppName);
        }
        if (!dir) {
            const char *base = SDL_GetBasePath();
            if (base) {
                size_t n = strlen(base) + 1;
                dir = (char *)SDL_malloc(n);
                if (dir) memcpy(dir, base, n);
            }
        }

        if (dir) {
            SDL_snprintf(g_log_path, sizeof g_log_path, "%s%s", dir, logFileBaseName);
        } else {
            SDL_snprintf(g_log_path, sizeof g_log_path, "%s", logFileBaseName);
        }
        rotate_existing(g_log_path);
        g_log_file = fopen(g_log_path, "wb");
        if (!g_log_file) {
            /* Couldn't open — clear path so wb_log_path() returns NULL. */
            g_log_path[0] = '\0';
        }

        if (dir) SDL_free(dir);
    }

    /* Install our sink, capturing the previous one (always the SDL
       default unless someone else installed first). */
    SDL_GetLogOutputFunction(&g_default_sink, &g_default_sink_userdata);
    if (!g_default_sink) {
        g_default_sink = SDL_GetDefaultLogOutputFunction();
        g_default_sink_userdata = NULL;
    }
    SDL_SetLogOutputFunction(file_sink, NULL);
    g_installed = true;

    /* Silence every category by default, then let the env spec re-enable
       what it names. Unset/empty spec leaves everything off, so no message
       reaches the sink (no console echo, no file writes). A later
       SDL_SetLogPriority() at runtime can still re-enable console output,
       but file logging requires WINBOLO_LOG to be set at startup. */
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_COUNT);
    if (enabled) {
        apply_env_log_spec(logSpec);

        /* Session banner — useful when reading old logs cold. */
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "wb_log: session start, file=%s, compile-level=%d",
                    g_log_path[0] ? g_log_path : "(none)",
                    (int)WB_LOG_LEVEL);
    }

    return g_log_file != NULL;
}

void wb_log_shutdown(void) {
    if (!g_installed) return;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "wb_log: session end");
    /* Restore previous sink before closing the file so any late SDL
       internal logs during teardown still go somewhere sensible. */
    SDL_SetLogOutputFunction(g_default_sink, g_default_sink_userdata);
    if (g_log_file) {
        fflush(g_log_file);
        fclose(g_log_file);
        g_log_file = NULL;
    }
    g_log_path[0] = '\0';
    g_installed = false;
}

const char *wb_log_path(void) {
    return (g_installed && g_log_path[0]) ? g_log_path : NULL;
}
