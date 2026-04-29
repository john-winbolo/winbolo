/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * wb_log.h — WinBolo structured logging
 *
 * Wraps SDL3's logging facility with project-specific categories, an
 * optional persistent log file, and compile-time level gating so release
 * builds can drop verbose calls entirely.
 *
 * USAGE
 *   #include "common/wb_log.h"
 *   WB_LOG_INFO(WB_LOG_CAT_NET, "client %u joined from %s", id, addr);
 *
 * CATEGORIES
 *   Use WB_LOG_CAT_* identifiers (defined as SDL log category integers).
 *   These map to filterable subsystems: NET, SERVER, SIM, CLIENT, GUI,
 *   AUDIO, ASSET, LUA, MAP, LOGVIEWER, PLATFORM.
 *
 * RUNTIME CONTROL
 *   Env var WINBOLO_LOG=net=trace,server=debug,*=info  (parsed by
 *   wb_log_init). Or call SDL_SetLogPriority() / SDL_SetLogPriorities()
 *   directly at any time.
 *
 * COMPILE-TIME LEVEL
 *   Define WB_LOG_LEVEL to one of WB_LOG_LEVEL_TRACE/DEBUG/INFO/WARN/
 *   ERROR/OFF. Calls below the threshold expand to ((void)0) — args are
 *   not evaluated, so do NOT put side effects in log argument lists.
 *
 * THREAD SAFETY
 *   wb_log_init / wb_log_shutdown must be called from the main thread.
 *   The log macros themselves are safe to call from any thread; SDL
 *   serializes output and our file sink takes its own mutex.
 */

#ifndef _WB_LOG_H
#define _WB_LOG_H

#include <SDL3/SDL_log.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Compile-time level                                                  */
/* ------------------------------------------------------------------ */
#define WB_LOG_LEVEL_TRACE 1
#define WB_LOG_LEVEL_DEBUG 2
#define WB_LOG_LEVEL_INFO  3
#define WB_LOG_LEVEL_WARN  4
#define WB_LOG_LEVEL_ERROR 5
#define WB_LOG_LEVEL_OFF   6

#ifndef WB_LOG_LEVEL
#  ifdef NDEBUG
#    define WB_LOG_LEVEL WB_LOG_LEVEL_INFO
#  else
#    define WB_LOG_LEVEL WB_LOG_LEVEL_DEBUG
#  endif
#endif

/* ------------------------------------------------------------------ */
/* Categories                                                          */
/* ------------------------------------------------------------------ */
/* Use SDL_LOG_CATEGORY_CUSTOM as the base. SDL reserves values below
   it for its own subsystems (VIDEO, AUDIO, RENDER, etc.). */
enum {
    WB_LOG_CAT_NET = SDL_LOG_CATEGORY_CUSTOM,
    WB_LOG_CAT_SERVER,
    WB_LOG_CAT_SIM,
    WB_LOG_CAT_CLIENT,
    WB_LOG_CAT_GUI,
    WB_LOG_CAT_AUDIO,
    WB_LOG_CAT_ASSET,
    WB_LOG_CAT_LUA,
    WB_LOG_CAT_MAP,
    WB_LOG_CAT_LOGVIEWER,
    WB_LOG_CAT_PLATFORM,
    WB_LOG_CAT__COUNT  /* must remain last */
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */
/*
 * Initialize logging.
 *
 * prefOrgName / prefAppName: passed to SDL_GetPrefPath to locate a
 *   user-writable directory. If either is NULL, the log is written next
 *   to the executable (SDL_GetBasePath).
 * logFileBaseName: e.g. "winbolo.log". Existing files are rotated
 *   (.log -> .log.1 -> .log.2 -> .log.3, oldest dropped).
 *
 * Honours the WINBOLO_LOG env var on entry (per-category priorities).
 * Safe to call after SDL_Init(0); does not require any subsystem.
 *
 * Returns true on success. On failure, logging continues to stderr
 * via SDL's default sink.
 */
bool wb_log_init(const char *prefOrgName,
                 const char *prefAppName,
                 const char *logFileBaseName);

/* Flush and close the file sink. Safe to call even if init failed. */
void wb_log_shutdown(void);

/* Returns the absolute path of the active log file, or NULL if no file
   sink is open. Pointer is owned by wb_log; valid until shutdown. */
const char *wb_log_path(void);

/* Map a string ("trace"/"debug"/"info"/"warn"/"error"/"off") to an SDL
   priority. Returns SDL_LOG_PRIORITY_INVALID on unknown input.
   Exposed mainly for ImGui debug panels and tests. */
SDL_LogPriority wb_log_parse_priority(const char *s);

/* Map a string ("net"/"server"/...) to a WB_LOG_CAT_*. Returns -1 on
   unknown, -2 for "*"/"all". Exposed for ImGui debug panels. */
int wb_log_parse_category(const char *s);

/* Human-readable name of a category integer (any SDL category, not just
   ours). Returned pointer is a static string. */
const char *wb_log_category_name(int category);

/* ------------------------------------------------------------------ */
/* Macros                                                              */
/* ------------------------------------------------------------------ */
#if WB_LOG_LEVEL <= WB_LOG_LEVEL_TRACE
#  define WB_LOG_TRACE(cat, ...) SDL_LogTrace((int)(cat), __VA_ARGS__)
#else
#  define WB_LOG_TRACE(cat, ...) ((void)0)
#endif

#if WB_LOG_LEVEL <= WB_LOG_LEVEL_DEBUG
#  define WB_LOG_DEBUG(cat, ...) SDL_LogDebug((int)(cat), __VA_ARGS__)
#else
#  define WB_LOG_DEBUG(cat, ...) ((void)0)
#endif

#if WB_LOG_LEVEL <= WB_LOG_LEVEL_INFO
#  define WB_LOG_INFO(cat, ...)  SDL_LogInfo((int)(cat), __VA_ARGS__)
#else
#  define WB_LOG_INFO(cat, ...)  ((void)0)
#endif

#if WB_LOG_LEVEL <= WB_LOG_LEVEL_WARN
#  define WB_LOG_WARN(cat, ...)  SDL_LogWarn((int)(cat), __VA_ARGS__)
#else
#  define WB_LOG_WARN(cat, ...)  ((void)0)
#endif

#if WB_LOG_LEVEL <= WB_LOG_LEVEL_ERROR
#  define WB_LOG_ERROR(cat, ...) SDL_LogError((int)(cat), __VA_ARGS__)
#else
#  define WB_LOG_ERROR(cat, ...) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* _WB_LOG_H */
