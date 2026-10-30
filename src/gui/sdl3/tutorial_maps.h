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
 *   Where the shipped tutorials live, and the test the
 *   menu's background game puts each map name through, so
 *   that it never shows a tutorial. The desktop, mobile and
 *   web menus share both, and the test for whether a
 *   scenario popup pauses the game. Pure, so the unit tests
 *   check them without a menu (test_bg_map_pick.c).
 *********************************************************/

#ifndef TUTORIAL_MAPS_H
#define TUTORIAL_MAPS_H

#include <stdbool.h>
#include <string.h>

/* The folder under data/maps that holds every shipped tutorial. Each
 * tutorial is a map with its scenario script beside it, as any scripted
 * map is, so the map chooser, the lobby's map list and the packaging treat
 * the folder as they treat any other folder of maps. */
#define TUTORIAL_MAPS_DIR_NAME "tutorials"
#define TUTORIAL_MAPS_DIR      "data/maps/" TUTORIAL_MAPS_DIR_NAME

/* The tutorial the main menu's Tutorial entry starts. */
#define TUTORIAL_MENU_MAP      TUTORIAL_MAPS_DIR "/Tutorial.map"

/* ASCII case folding for the name test below, so it does not need SDL. */
static inline char tutorialMapsFold(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

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

/* Whether path names a file in the shipped tutorials folder: its folder is
 * TUTORIAL_MAPS_DIR_NAME and that folder's own folder is "maps". Either
 * slash, any case, and anything in front, so a relative path, an absolute
 * one and an app bundle's path all pass. The single-player frontend asks it
 * of the running map's own script before a script's game.tutorial_done may
 * hide the main menu's Tutorial button. */
static inline bool tutorialMapsIsShippedTutorial(const char *path) {
    const char *seg[3];   /* starts of the file, its folder, that folder's */
    size_t      len[3];
    const char *end;
    const char *p;
    int         i;

    if (path == NULL || path[0] == '\0') return false;
    end = path + strlen(path);
    for (i = 0; i < 3; i++) {
        p = end;
        while (p > path && p[-1] != '/' && p[-1] != '\\') p--;
        seg[i] = p;
        len[i] = (size_t)(end - p);
        if (len[i] == 0) return false;
        if (i < 2) {
            if (p == path) return false;   /* no folder in front of it */
            end = p - 1;
        }
    }
    {
        const char *names[2] = { TUTORIAL_MAPS_DIR_NAME, "maps" };
        for (i = 0; i < 2; i++) {
            size_t k;
            if (len[i + 1] != strlen(names[i])) return false;
            for (k = 0; k < len[i + 1]; k++) {
                if (tutorialMapsFold(seg[i + 1][k]) != names[i][k]) {
                    return false;
                }
            }
        }
    }
    return true;
}

/* Whether a scenario popup (game.popup) pauses the game while it is open.
 * Only the shipped tutorials pause: a single-player game, with its server in
 * this process, whose running map's own script is in the tutorials folder.
 * Popups in any other game, single player or not, leave the game running.
 * mapScriptPath is the running map's own script, "" or NULL for none. */
static inline bool tutorialMapsPopupPauses(bool singlePlayer,
                                           bool serverInProcess,
                                           const char *mapScriptPath) {
    return singlePlayer && serverInProcess &&
           tutorialMapsIsShippedTutorial(mapScriptPath);
}

#endif /* TUTORIAL_MAPS_H */
