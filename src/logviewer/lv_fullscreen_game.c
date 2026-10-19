/*
 * lv_fullscreen_game.c - full screen for WinBolo's window, asked for by the
 *                        Log Viewer that runs in it
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Built into WinBolo only (see lv_fullscreen_game.h). The Log Viewer's own
 * program has the other body of this function, in main.c.
 */

#include "lv_fullscreen_game.h"

/* The game's command for full screen outside a game (src/gui/sdl3/winbolo.c):
 * it changes the window, keeps the game's own flag and saves the setting.
 * Declared here, as the game's dialogs declare it: the game's headers and
 * the viewer's do not go in one file. */
void windowFullScreenChoose(bool on);

bool lvGameFullScreenChoose(bool on) {
    windowFullScreenChoose(on);
    return true;
}
