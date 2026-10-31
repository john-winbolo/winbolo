/*
 * Tests for bgMapNameAllowed() in src/gui/sdl3/tutorial_maps.h — the name
 * test the main menu's background game puts each map through before it
 * picks one (gamefront.c pickRandomMap, gamefront_wasm.c
 * wasmPickBackgroundMap).
 *
 * The contract checked here: a .map in data/maps itself passes; nothing in
 * a folder passes, so no tutorial in data/maps/tutorials (whatever its name
 * or the case of the folder's name) is ever picked; a file that is not a
 * .map does not pass; and the old tutorial map and Better Best Map Ever are
 * left out by name. The scenario test the callers add after it (a map with
 * a script beside it) needs the scenario library and is not part of this.
 */

#include <string.h>

#include "tutorial_maps.h"
#include "test_harness.h"

int run_bg_map_pick_skips_tutorials(void) {
    /* The folder and the menu's map are where the rest of the code looks. */
    UT_ASSERT(strcmp(TUTORIAL_MAPS_DIR, "data/maps/tutorials") == 0);
    UT_ASSERT(strcmp(TUTORIAL_MENU_MAP,
                     "data/maps/tutorials/Tutorial.map") == 0);

    /* Plain maps in data/maps itself. */
    UT_ASSERT(bgMapNameAllowed("Everard Island.map"));
    UT_ASSERT(bgMapNameAllowed("DH-Oil Rig.map"));
    UT_ASSERT(bgMapNameAllowed("B.C..map"));
    UT_ASSERT(bgMapNameAllowed("Shouting.MAP"));

    /* The tutorials folder, every tutorial in it, either separator, any
     * case. */
    UT_ASSERT(!bgMapNameAllowed("tutorials/Tutorial.map"));
    UT_ASSERT(!bgMapNameAllowed("tutorials/Classic Tutorial.map"));
    UT_ASSERT(!bgMapNameAllowed("tutorials\\Tutorial.map"));
    UT_ASSERT(!bgMapNameAllowed("Tutorials/Tutorial.map"));
    UT_ASSERT(!bgMapNameAllowed("TUTORIALS/Anything.map"));

    /* Any other folder under data/maps, and a path that starts with a
     * separator. */
    UT_ASSERT(!bgMapNameAllowed("Uploads/Foo.map"));
    UT_ASSERT(!bgMapNameAllowed("/Everard Island.map"));

    /* Left out by name, in any case. */
    UT_ASSERT(!bgMapNameAllowed("Inbuilt Tutorial.map"));
    UT_ASSERT(!bgMapNameAllowed("inbuilt tutorial.MAP"));
    UT_ASSERT(!bgMapNameAllowed("Better Best Map Ever.map"));

    /* Not a map file. */
    UT_ASSERT(!bgMapNameAllowed(NULL));
    UT_ASSERT(!bgMapNameAllowed(""));
    UT_ASSERT(!bgMapNameAllowed(".map"));
    UT_ASSERT(!bgMapNameAllowed("Tutorial.scenario.lua"));
    UT_ASSERT(!bgMapNameAllowed("Everard Island.map.bak"));
    UT_ASSERT(!bgMapNameAllowed("tutorials"));
    return 0;
}

/* The test the single-player frontend puts the running script through
 * before game.tutorial_done may hide the main menu's Tutorial button: the
 * file has to be in a folder named tutorials inside a folder named maps. */
int run_tutorial_maps_shipped_path(void) {
    UT_ASSERT(tutorialMapsIsShippedTutorial(
        "data/maps/tutorials/Tutorial.scenario.lua"));
    UT_ASSERT(tutorialMapsIsShippedTutorial(
        "data/maps/tutorials/Classic Tutorial.scenario.lua"));
    UT_ASSERT(tutorialMapsIsShippedTutorial(TUTORIAL_MENU_MAP));
    UT_ASSERT(tutorialMapsIsShippedTutorial(
        "C:\\Games\\WinBolo\\data\\maps\\tutorials\\Tutorial.scenario.lua"));
    UT_ASSERT(tutorialMapsIsShippedTutorial(
        "/var/app/data/Maps/Tutorials/Tutorial.scenario.lua"));
    UT_ASSERT(tutorialMapsIsShippedTutorial("maps/tutorials/x.lua"));

    /* Not in that folder. */
    UT_ASSERT(!tutorialMapsIsShippedTutorial(NULL));
    UT_ASSERT(!tutorialMapsIsShippedTutorial(""));
    UT_ASSERT(!tutorialMapsIsShippedTutorial(
        "data/maps/Survival.scenario.lua"));
    UT_ASSERT(!tutorialMapsIsShippedTutorial("tutorials/Tutorial.scenario.lua"));
    UT_ASSERT(!tutorialMapsIsShippedTutorial(
        "data/mods/tutorials/Tutorial.scenario.lua"));
    UT_ASSERT(!tutorialMapsIsShippedTutorial(
        "data/maps/tutorials2/Tutorial.scenario.lua"));
    UT_ASSERT(!tutorialMapsIsShippedTutorial(
        "data/maps/tutorials/sub/Tutorial.scenario.lua"));
    UT_ASSERT(!tutorialMapsIsShippedTutorial("data/maps/tutorials/"));
    UT_ASSERT(!tutorialMapsIsShippedTutorial("data/mymaps/tutorials/a.lua"));
    return 0;
}

/* Which games a scenario popup pauses: only a shipped tutorial played as a
 * single-player game with its server in this process. */
int run_tutorial_maps_popup_pauses(void) {
    const char *tut     = "data/maps/tutorials/Tutorial.scenario.lua";
    const char *classic = "data/maps/tutorials/Classic Tutorial.scenario.lua";
    const char *other   = "data/maps/Survival.scenario.lua";

    /* The tutorials, single player, server in this process. */
    UT_ASSERT(tutorialMapsPopupPauses(true, true, tut));
    UT_ASSERT(tutorialMapsPopupPauses(true, true, classic));

    /* Another single-player scenario, or no script at all. */
    UT_ASSERT(!tutorialMapsPopupPauses(true, true, other));
    UT_ASSERT(!tutorialMapsPopupPauses(true, true, ""));
    UT_ASSERT(!tutorialMapsPopupPauses(true, true, NULL));

    /* A multiplayer game or a listen-server host on the tutorial map. */
    UT_ASSERT(!tutorialMapsPopupPauses(false, true, tut));
    /* A joined game: the server is not in this process. */
    UT_ASSERT(!tutorialMapsPopupPauses(true, false, tut));
    UT_ASSERT(!tutorialMapsPopupPauses(false, false, tut));
    return 0;
}
