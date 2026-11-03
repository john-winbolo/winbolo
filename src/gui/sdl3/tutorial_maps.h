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
 * Name:          tutorial_maps.h
 * Purpose:
 *   The test the menu's background game puts each map name
 *   through, so that it never shows a tutorial, and the
 *   test for whether a scenario popup pauses the game. The
 *   desktop, mobile and web menus share both. The tutorials
 *   folder, the popup flag and the popup rule are in
 *   src/bolo/public/tutorial_scripts.h, which the scenario
 *   library and the dedicated server read too. Pure, so the
 *   unit tests check them without a menu
 *   (test_bg_map_pick.c).
 *********************************************************/

#ifndef TUTORIAL_MAPS_H
#define TUTORIAL_MAPS_H

#include <stdbool.h>
#include <string.h>

/* TUTORIAL_MAPS_DIR, TUTORIAL_MAPS_SCRIPT_POPUPS, tutorialMapsFold and the
 * popup rule. */
#include "tutorial_scripts.h"

/* The tutorial the main menu's Tutorial entry starts. */
#define TUTORIAL_MENU_MAP      TUTORIAL_MAPS_DIR "/Tutorial.map"

/* Whether a and b are the same name, ignoring ASCII case. */
static inline bool tutorialMapsSameName(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (tutorialMapsFold(*a) != tutorialMapsFold(*b)) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Whether the menu's background game may pick the map at relPath, a path
 * relative to data/maps, by its name and place alone:
 *
 *   - a map in a folder under data/maps is never picked. The tutorials are
 *     in one (TUTORIAL_MAPS_DIR_NAME), and so are maps a player put in a
 *     folder of their own. The background game has only ever picked from
 *     data/maps itself, and this keeps it so even if the listing that feeds
 *     it starts to walk folders;
 *   - the file has to be a .map;
 *   - the old built-in tutorial's map and Better Best Map Ever are left out
 *     by name, as they always have been.
 *
 * A map that passes still has to be a map with no scenario, which the
 * caller asks the scenario library (scenarioHostMapCarriesScript): this
 * test does not open the file. */
static inline bool bgMapNameAllowed(const char *relPath) {
    const char *p;
    size_t      n;

    if (relPath == NULL || relPath[0] == '\0') return false;
    for (p = relPath; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') return false;
    }
    n = strlen(relPath);
    if (n <= 4 || !tutorialMapsSameName(relPath + n - 4, ".map")) {
        return false;
    }
    if (tutorialMapsSameName(relPath, "Inbuilt Tutorial.map") ||
        tutorialMapsSameName(relPath, "Better Best Map Ever.map")) {
        return false;
    }
    return true;
}

/* Whether a scenario popup (game.popup) pauses the game while it is open.
 * Only the shipped tutorials pause: a single-player game, with its server in
 * this process, whose running map's own script is directly in the shipped
 * tutorials folder under the data folder. mapScriptShipped is that test's
 * answer for the running map's own script
 * (scenarioHostScriptIsShippedTutorial), false for none. It is the same test
 * that gives a script game.popup while TUTORIAL_MAPS_SCRIPT_POPUPS is 0.
 * Popups in any other game, single player or not, leave the game running. */
static inline bool tutorialMapsPopupPauses(bool singlePlayer,
                                           bool serverInProcess,
                                           bool mapScriptShipped) {
    return singlePlayer && serverInProcess && mapScriptShipped;
}

#endif /* TUTORIAL_MAPS_H */
