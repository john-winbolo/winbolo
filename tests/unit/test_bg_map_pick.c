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

/* The text half of the shipped-tutorial test (tutorialMapsPathInDir in
 * src/bolo/public/tutorial_scripts.h): a file directly in the canonical
 * tutorials folder passes; the folder itself, a file in a folder below it, a
 * folder whose name only starts the same, and the same names under another
 * folder do not. The half that reads the disk is
 * scenario_host_shipped_tutorial_rooted. */
int run_tutorial_maps_shipped_path(void) {
    const char *win = "C:\\Games\\WinBolo\\data\\maps\\tutorials";
    const char *nix = "/opt/winbolo/data/maps/tutorials";

    UT_ASSERT(tutorialMapsPathInDir(
        win, "C:\\Games\\WinBolo\\data\\maps\\tutorials\\Tutorial.scenario.lua",
        true));
    /* Either slash, and the case where the file system ignores it. */
    UT_ASSERT(tutorialMapsPathInDir(
        win, "c:/games/winbolo/DATA/Maps/Tutorials/Classic Tutorial.scenario.lua",
        true));
    UT_ASSERT(!tutorialMapsPathInDir(
        nix, "/opt/winbolo/data/Maps/tutorials/Tutorial.scenario.lua", false));
    UT_ASSERT(tutorialMapsPathInDir(
        nix, "/opt/winbolo/data/maps/tutorials/Tutorial.scenario.lua", false));
    /* A trailing slash on the folder. */
    UT_ASSERT(tutorialMapsPathInDir(
        "/opt/winbolo/data/maps/tutorials/",
        "/opt/winbolo/data/maps/tutorials/Tutorial.scenario.lua", false));

    /* Not directly in that folder. */
    UT_ASSERT(!tutorialMapsPathInDir(nix, NULL, false));
    UT_ASSERT(!tutorialMapsPathInDir(NULL, "/a", false));
    UT_ASSERT(!tutorialMapsPathInDir("", "/a", false));
    UT_ASSERT(!tutorialMapsPathInDir(nix, "", false));
    UT_ASSERT(!tutorialMapsPathInDir(nix, nix, false));
    UT_ASSERT(!tutorialMapsPathInDir(
        nix, "/opt/winbolo/data/maps/tutorials/", false));
    UT_ASSERT(!tutorialMapsPathInDir(
        nix, "/opt/winbolo/data/maps/tutorials/sub/Tutorial.scenario.lua",
        false));
    UT_ASSERT(!tutorialMapsPathInDir(
        nix, "/opt/winbolo/data/maps/tutorials2/Tutorial.scenario.lua",
        false));
    /* A maps/tutorials folder of the same name somewhere else. */
    UT_ASSERT(!tutorialMapsPathInDir(
        nix, "/home/me/maps/tutorials/Tutorial.scenario.lua", false));
    UT_ASSERT(!tutorialMapsPathInDir(
        nix, "/opt/winbolo/maps/tutorials/Tutorial.scenario.lua", false));
    UT_ASSERT(!tutorialMapsPathInDir(
        win, "D:\\Packs\\data\\maps\\tutorials\\Tutorial.scenario.lua", true));
    UT_ASSERT(!tutorialMapsPathInDir(
        win, "data/maps/tutorials/Tutorial.scenario.lua", true));
    return 0;
}

/* Which games a scenario popup pauses: only a shipped tutorial played as a
 * single-player game with its server in this process. The third value is
 * the shipped-tutorial test's answer for the running map's own script. */
int run_tutorial_maps_popup_pauses(void) {
    /* A shipped tutorial, single player, server in this process. */
    UT_ASSERT(tutorialMapsPopupPauses(true, true, true));

    /* Another single-player scenario, or no script at all. */
    UT_ASSERT(!tutorialMapsPopupPauses(true, true, false));

    /* A multiplayer game or a listen-server host on the tutorial map. */
    UT_ASSERT(!tutorialMapsPopupPauses(false, true, true));
    /* A joined game: the server is not in this process. */
    UT_ASSERT(!tutorialMapsPopupPauses(true, false, true));
    UT_ASSERT(!tutorialMapsPopupPauses(false, false, true));
    return 0;
}

/* Which scripts have game.popup. Off (TUTORIAL_MAPS_SCRIPT_POPUPS 0): only
 * a map's own script that the shipped-tutorial test passes; another map's
 * script and any mod (even one the test would pass) do not. On: every
 * script. The pause rule is separate (run_tutorial_maps_popup_pauses): only a
 * tutorial in a local game pauses. */
int run_tutorial_maps_script_has_popup(void) {
    /* The flag as it is built. */
    UT_ASSERT(TUTORIAL_MAPS_SCRIPT_POPUPS == 0);

    /* Off: a shipped tutorial's map script has the call, and in a local game
       its popups pause. */
    UT_ASSERT(tutorialMapsScriptHasPopup(false, true, true));
    UT_ASSERT(tutorialMapsPopupPauses(true, true, true));
    /* Off: another map's script, or a mod, has no call. */
    UT_ASSERT(!tutorialMapsScriptHasPopup(false, true, false));
    UT_ASSERT(!tutorialMapsScriptHasPopup(false, false, false));
    /* Off: a mod is never a tutorial, even where the test passes. */
    UT_ASSERT(!tutorialMapsScriptHasPopup(false, false, true));

    /* On: every script has the call. Only a tutorial pauses. */
    UT_ASSERT(tutorialMapsScriptHasPopup(true, true, true));
    UT_ASSERT(tutorialMapsScriptHasPopup(true, true, false));
    UT_ASSERT(tutorialMapsScriptHasPopup(true, false, false));
    UT_ASSERT(!tutorialMapsPopupPauses(true, true, false));
    return 0;
}
