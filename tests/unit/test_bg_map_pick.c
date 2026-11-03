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

/* Which popups open the popup window. Off (TUTORIAL_MAPS_SCRIPT_POPUPS 0):
 * only a built-in tutorial's, and any other script's goes to the Scenario
 * Text lines. On: every script's. The pause rule is separate
 * (run_tutorial_maps_popup_pauses): only a tutorial in a local game pauses. */
int run_tutorial_maps_popup_opens_window(void) {
    const char *tut     = "data/maps/tutorials/Tutorial.scenario.lua";
    const char *classic = "data/maps/tutorials/Classic Tutorial.scenario.lua";
    const char *other   = "data/maps/Survival.scenario.lua";
    const char *mod     = "data/mods/MyMod/MyMod.scenario.lua";

    /* The flag as it is built. */
    UT_ASSERT(TUTORIAL_MAPS_SCRIPT_POPUPS == 0);

    /* Off: the tutorials open the window, and in a local game they pause. */
    UT_ASSERT(tutorialMapsPopupOpensWindow(false, tut));
    UT_ASSERT(tutorialMapsPopupOpensWindow(false, classic));
    UT_ASSERT(tutorialMapsPopupPauses(true, true, tut));
    /* Off: another scenario, a mod, or no script opens no window, and so
       pauses nothing. */
    UT_ASSERT(!tutorialMapsPopupOpensWindow(false, other));
    UT_ASSERT(!tutorialMapsPopupOpensWindow(false, mod));
    UT_ASSERT(!tutorialMapsPopupOpensWindow(false, ""));
    UT_ASSERT(!tutorialMapsPopupOpensWindow(false, NULL));
    UT_ASSERT(!tutorialMapsPopupPauses(true, true, other));

    /* On: every script's popup opens the window. Only a tutorial pauses. */
    UT_ASSERT(tutorialMapsPopupOpensWindow(true, tut));
    UT_ASSERT(tutorialMapsPopupOpensWindow(true, other));
    UT_ASSERT(tutorialMapsPopupOpensWindow(true, mod));
    UT_ASSERT(tutorialMapsPopupOpensWindow(true, ""));
    UT_ASSERT(!tutorialMapsPopupPauses(true, true, mod));
    return 0;
}

/* A popup with no window, split into Scenario Text lines: line breaks are
 * spaces, words are packed in order and broken only at a space, a word
 * longer than a line is cut at a UTF-8 character's start, and text past the
 * last line is reported dropped. */
int run_tutorial_maps_popup_fallback_lines(void) {
    char out[4][11];   /* 10 bytes a line */
    bool dropped = true;
    int  n;

    /* One short line, with its line breaks and extra spaces as one space. */
    n = tutorialMapsPopupFallbackLines("Hi\n\n  {FIRE}", &out[0][0],
                                       sizeof(out[0]), 4, &dropped);
    UT_ASSERT(n == 1);
    UT_ASSERT(strcmp(out[0], "Hi {FIRE}") == 0);
    UT_ASSERT(!dropped);

    /* Packed into lines of up to 10 bytes, broken at spaces. */
    n = tutorialMapsPopupFallbackLines("one two three four five", &out[0][0],
                                       sizeof(out[0]), 4, &dropped);
    UT_ASSERT(n == 3);
    UT_ASSERT(strcmp(out[0], "one two") == 0);
    UT_ASSERT(strcmp(out[1], "three four") == 0);
    UT_ASSERT(strcmp(out[2], "five") == 0);
    UT_ASSERT(!dropped);

    /* A word longer than a line is cut. */
    n = tutorialMapsPopupFallbackLines("abcdefghijklm", &out[0][0],
                                       sizeof(out[0]), 4, &dropped);
    UT_ASSERT(n == 2);
    UT_ASSERT(strcmp(out[0], "abcdefghij") == 0);
    UT_ASSERT(strcmp(out[1], "klm") == 0);

    /* Never inside a UTF-8 character: "\xC3\xA9" is one two-byte letter,
       and byte 10 would split the fifth one. */
    n = tutorialMapsPopupFallbackLines(
        "a\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", &out[0][0],
        sizeof(out[0]), 4, &dropped);
    UT_ASSERT(n == 2);
    UT_ASSERT(strcmp(out[0], "a\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9") == 0);
    UT_ASSERT(strcmp(out[1], "\xC3\xA9") == 0);

    /* More text than the lines hold: the first lines, and dropped. */
    n = tutorialMapsPopupFallbackLines("aaaa bbbb cccc dddd eeee ffff gggg "
                                       "hhhh iiii", &out[0][0],
                                       sizeof(out[0]), 4, &dropped);
    UT_ASSERT(n == 4);
    UT_ASSERT(strcmp(out[3], "gggg hhhh") == 0);
    UT_ASSERT(dropped);

    /* Nothing to show. */
    n = tutorialMapsPopupFallbackLines(" \n ", &out[0][0], sizeof(out[0]), 4,
                                       &dropped);
    UT_ASSERT(n == 0);
    UT_ASSERT(tutorialMapsPopupFallbackLines(NULL, &out[0][0],
                                             sizeof(out[0]), 4, NULL) == 0);

    /* How long the lines stay up: 6 s, 1 more for each 20 bytes, at most
       30. */
    UT_ASSERT(tutorialMapsPopupFallbackSeconds(0) == 6);
    UT_ASSERT(tutorialMapsPopupFallbackSeconds(100) == 11);
    UT_ASSERT(tutorialMapsPopupFallbackSeconds(511) == 30);
    return 0;
}
