/*
 * $Id$
 *
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

/*********************************************************
 * Name:          main.c
 * Purpose:
 *   Standalone entry point for the Log Viewer. Delegates
 *   to logViewerRun() which creates its own SDL window
 *   and owns the event loop.
 *********************************************************/

#ifdef _WIN32
#define SDL_MAIN_HANDLED
#endif

#include <stdlib.h>
#include <SDL3/SDL.h>
#include "logviewer.h"
#include "../common/wb_log.h"
#include "../common/prefs.h"
#include "../winbolonet/http.h"
#include "../winbolonet/winbolonet_core.h"
#include "platform/platform_config.h"

int main(int argc, char *argv[]);

#ifdef _WIN32
int __stdcall WinMain(void *hInst, void *hPrev, char *lpCmd, int nShow) {
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;
    return main(__argc, __argv);
}
#endif

int main(int argc, char *argv[]) {
    const char *logPath = (argc > 1) ? argv[1] : NULL;
    SDL_Init(0);
    wb_log_init("WinBolo", "LogViewer", "logviewer.log");
    atexit(wb_log_shutdown);

    lv_platform_config_init("LogViewer");

    /* Load the process-global preferences document the shared winbolonet code
     * reads through. Mirror platform_config's SDL_GetPrefPath WinBolo location
     * with the .json document. */
    {
        char prefsJsonPath[1024];
        const char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
        if (prefDir) {
            SDL_snprintf(prefsJsonPath, sizeof(prefsJsonPath), "%sWinBolo.json", prefDir);
        } else {
            SDL_snprintf(prefsJsonPath, sizeof(prefsJsonPath), "%s", "WinBolo.json");
        }
        prefsInit(prefsJsonPath);
    }

    httpCreate();

    /* NULL window/renderer signals standalone mode —
       logViewerRun() creates its own window */
    logViewerRun(NULL, NULL, logPath, false);

    httpDestroy();
    return 0;
}
