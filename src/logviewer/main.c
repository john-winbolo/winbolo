/*
 * $Id$
 *
 * Copyright (c) 1998-2008 John Morrison.
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

#include <SDL3/SDL.h>
#include "logviewer.h"

int main(int argc, char *argv[]);

#ifdef _WIN32
int __stdcall WinMain(void *hInst, void *hPrev, char *lpCmd, int nShow) {
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;
    return main(__argc, __argv);
}
#endif

int main(int argc, char *argv[]) {
    const char *logPath = (argc > 1) ? argv[1] : NULL;
    /* NULL window/renderer signals standalone mode —
       logViewerRun() creates its own window */
    logViewerRun(NULL, NULL, logPath, false);
    return 0;
}
