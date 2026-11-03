/*
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
 * Name:          fullscreen_key.h
 * Purpose:       What the full screen key (Alt+Enter, and the
 *                macOS Window menu item) does.
 *
 *   Outside a game it is the app full screen flag. In a game
 *   it is the full screen map, which takes the window full
 *   screen with it. A server in classic mode (or with the
 *   overview set to None) withholds that map, and then the
 *   key goes back to the plain window full screen: a player
 *   who came into the game from a full screen lobby has a full
 *   screen window and no map to leave, and without this the
 *   key did nothing and the window stayed full screen for the
 *   whole game.
 *
 *   Where the window does not own the flag (the Deck, the
 *   tablet layouts, iOS, the web canvas) the withheld map
 *   leaves the key with nothing to do.
 *
 *   Logic only, no SDL, so it compiles into the unit test
 *   binary.
 *********************************************************/

#ifndef FULLSCREEN_KEY_H
#define FULLSCREEN_KEY_H

#include <stdbool.h>

typedef enum {
  FULLSCREEN_KEY_NOTHING = 0,
  FULLSCREEN_KEY_MAP,    /* toggle the full screen map */
  FULLSCREEN_KEY_WINDOW  /* toggle the app full screen flag and the window */
} FullScreenKeyAction;

/* gameRunning: a game is running in this client.
   mapWithheld: the server is withholding the full screen map.
   windowOwnsFlag: the app full screen flag is the desktop window's own
   full screen state. */
static inline FullScreenKeyAction fullScreenKeyAction(bool gameRunning,
                                                      bool mapWithheld,
                                                      bool windowOwnsFlag) {
  if (!gameRunning) return FULLSCREEN_KEY_WINDOW;
  if (!mapWithheld) return FULLSCREEN_KEY_MAP;
  return windowOwnsFlag ? FULLSCREEN_KEY_WINDOW : FULLSCREEN_KEY_NOTHING;
}

#endif /* FULLSCREEN_KEY_H */
