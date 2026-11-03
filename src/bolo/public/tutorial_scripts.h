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
 * Name:          tutorial_scripts.h
 * Purpose:
 *   Which scripts are the shipped tutorials, and which
 *   scripts get the popup call (game.popup). Read by the
 *   scenario library (the host and -validate), which the
 *   dedicated server and every client link, and by the
 *   client's menus through src/gui/sdl3/tutorial_maps.h.
 *   Pure: no SDL, no file access. The part that reads the
 *   disk (the data folder and the real path of a script)
 *   is scenarioHostScriptIsShippedTutorial in
 *   src/scenario/scenario_host.c.
 *********************************************************/

#ifndef TUTORIAL_SCRIPTS_H
#define TUTORIAL_SCRIPTS_H

#include <stdbool.h>
#include <stddef.h>

/* The folder under data/maps that holds every shipped tutorial. Each
 * tutorial is a map with its scenario script beside it, as any scripted
 * map is, so the map chooser, the lobby's map list and the packaging treat
 * the folder as they treat any other folder of maps. */
#define TUTORIAL_MAPS_DIR_NAME "tutorials"
#define TUTORIAL_MAPS_DIR      "data/maps/" TUTORIAL_MAPS_DIR_NAME

/* The same folder below the data folder itself. The scenario host puts it
 * after the data folder it was given (scenarioHostSetDataRoot) to find the
 * one folder whose scripts are the shipped tutorials. */
#define TUTORIAL_MAPS_UNDER_DATA "maps/" TUTORIAL_MAPS_DIR_NAME

/* ASCII case folding for the path tests, so they do not need SDL. */
static inline char tutorialMapsFold(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* One character of a path for the comparison below: both slashes the same,
 * and the case folded where foldCase is true. */
static inline char tutorialMapsPathChar(char c, bool foldCase) {
    if (c == '\\') c = '/';
    return foldCase ? tutorialMapsFold(c) : c;
}

/* Whether path names a file directly in the folder dir: dir, one slash,
 * then a file name with no slash in it. Both are full paths that the caller
 * has already made canonical (no "." or ".." parts, links followed), so
 * this only compares text. Either slash matches either slash. foldCase is
 * true where the file system ignores case (Windows, macOS). A trailing
 * slash on dir is allowed. A file in a folder below dir does not pass, and
 * neither does dir itself or a folder whose name only starts with dir's
 * last name ("tutorials2"). */
static inline bool tutorialMapsPathInDir(const char *dir, const char *path,
                                         bool foldCase) {
    size_t dl;
    size_t i;

    if (dir == NULL || path == NULL || dir[0] == '\0') return false;
    dl = 0;
    while (dir[dl] != '\0') dl++;
    while (dl > 1 && (dir[dl - 1] == '/' || dir[dl - 1] == '\\')) dl--;
    for (i = 0; i < dl; i++) {
        if (path[i] == '\0' ||
            tutorialMapsPathChar(path[i], foldCase) !=
                tutorialMapsPathChar(dir[i], foldCase)) {
            return false;
        }
    }
    if (path[dl] != '/' && path[dl] != '\\') return false;
    if (path[dl + 1] == '\0') return false;          /* the folder itself */
    for (i = dl + 1; path[i] != '\0'; i++) {
        if (path[i] == '/' || path[i] == '\\') return false;   /* deeper */
    }
    return true;
}

/* Whether a scenario or mod that is not a built-in tutorial has the popup
 * call, game.popup.
 *   1 (on):  every scenario and mod script has game.popup, and each popup
 *            opens the popup window, as a built-in tutorial's does. The
 *            pause rule (tutorialMapsPopupPauses in
 *            src/gui/sdl3/tutorial_maps.h) still applies: only the built-in
 *            tutorials pause.
 *   0 (off): only a built-in tutorial's own map script has game.popup
 *            (tutorialMapsScriptHasPopup). In every other script game.popup
 *            is nil, so a call is the usual Lua error "attempt to call a nil
 *            value", and `if game.popup then` finds nothing.
 * The scenario host reads it where it builds each script's globals
 * (scnScriptLoad in src/scenario/scenario_host.c), and -validate reads it for
 * the stub game table. Off for now. */
#define TUTORIAL_MAPS_SCRIPT_POPUPS 0

/* Whether one script gets game.popup in its globals. scriptPopupsOn is
 * TUTORIAL_MAPS_SCRIPT_POPUPS, a parameter so a test can try both values.
 * isMapScript is true for the committed map's own script and false for a
 * mod. shippedTutorial is whether that script's file is directly in the
 * shipped tutorials folder under the data folder
 * (scenarioHostScriptIsShippedTutorial). With script popups off, only a
 * map's own script there gets the call: a mod never does, wherever its file
 * is. */
static inline bool tutorialMapsScriptHasPopup(bool scriptPopupsOn,
                                              bool isMapScript,
                                              bool shippedTutorial) {
    return scriptPopupsOn || (isMapScript && shippedTutorial);
}

#endif /* TUTORIAL_SCRIPTS_H */
