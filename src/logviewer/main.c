/*
 * $Id$
 *
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
#include <string.h>
#include <SDL3/SDL.h>
#include "logviewer.h"
#include "lv_dump.h"
#include "../common/wb_log.h"
#include "../common/prefs.h"
#include "../winbolonet/http.h"
#include "../winbolonet/winbolonet_core.h"
#include "platform/platform_config.h"
#include "lv_fullscreen_game.h"

int main(int argc, char *argv[]);

/* There is no game in this program: the viewer changes its own window. The
 * body for WinBolo is in lv_fullscreen_game.c. */
bool lvGameFullScreenChoose(bool on) {
    (void)on;
    return false;
}

#ifdef _WIN32
/* SDL_MAIN_HANDLED above: this declares SDL_RunApp and adds no entry point. */
#include <SDL3/SDL_main.h>

/* SDL_RunApp with no argv reads the command line as UTF-16 and gives main
 * UTF-8, the form every path in the viewer is opened as (lv_utf8_path.h).
 * __argv is in the system's code page instead, so a log path with a letter
 * outside it — the game's "Spawn in log viewer" passes one under the user's
 * own folder — did not open. */
int __stdcall WinMain(void *hInst, void *hPrev, char *lpCmd, int nShow) {
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;
    return SDL_RunApp(0, NULL, main, NULL);
}
#endif

int main(int argc, char *argv[]) {
    const char *logPath = NULL;
    int i;
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
        /* The game may be running beside the viewer and keeps the same
         * file. Write only the keys the viewer changed, onto a fresh read
         * of the file. The game does the same (gameFrontStart in
         * src/gui/sdl3/gamefront.c), so neither program wipes the other's
         * changes. */
        prefsSetMergeOnSave(true);
        lv_platform_config_set_own_writes(1);
    }

    /* "-dump <log> ..." writes the recording out as text and ends: no window,
       no renderer and no network (lv_dump.h). */
    if (lvDumpWanted(argc, argv)) {
        int dumpResult = lvDumpMain(argc, argv);
        if (!prefsFlush()) {
            prefsFlush();
        }
        return dumpResult;
    }

    /* The command line: the log to open, and "-size WxH" for a window of a
       fixed size in pixels. The viewer reads the size and warns about a text
       that is not one once its window is up (logViewerSetWindowSizeText). */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-size") == 0) {
            logViewerSetWindowSizeText(i + 1 < argc ? argv[i + 1] : NULL);
            i++;
        } else if (logPath == NULL) {
            logPath = argv[i];
        }
    }

    httpCreate();

    /* NULL window/renderer signals standalone mode —
       logViewerRun() creates its own window */
    logViewerRun(NULL, NULL, logPath, false);

    httpDestroy();
    /* Anything set after the viewer's last save. A flush that found no
     * file waits for one more flush (prefs.h); this is the last, so it is
     * tried twice. */
    if (!prefsFlush()) {
        prefsFlush();
    }
    return 0;
}
