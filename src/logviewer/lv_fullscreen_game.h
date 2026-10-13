/*
 * lv_fullscreen_game.h - full screen for the window of the game that runs
 *                        the Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * When WinBolo runs the viewer in its own window (Watch a Log, a live game
 * watched from the game), the window and its full screen setting are the
 * game's. Alt+Enter in the viewer must then change full screen the way the
 * game does, so the game's setting and its saved preference stay right.
 *
 * The game's front end knows nothing of this. The viewer's side of it is one
 * function with two bodies, one for each program:
 *   WinBolo         lv_fullscreen_game.c, which calls the game's own command
 *                   (windowFullScreenChoose in src/gui/sdl3/winbolo.c)
 *   the Log Viewer  main.c, where there is no game
 */

#ifndef LV_FULLSCREEN_GAME_H
#define LV_FULLSCREEN_GAME_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ask the game to turn its window's full screen on or off. True when the
 * game did it. False when there is no game in this program; the caller then
 * changes the window itself. */
bool lvGameFullScreenChoose(bool on);

#ifdef __cplusplus
}
#endif

#endif /* LV_FULLSCREEN_GAME_H */
