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
