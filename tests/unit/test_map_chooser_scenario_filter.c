/*
 * Tests for mapChooserEntryPassesScenarioFilter() in
 * src/gui/sdl3/dialogs/imgui_mapchooser.h — the row test behind the map
 * chooser's "Scenarios only" checkbox.
 *
 * The contract checked here: with the tick off every row stays; with it on,
 * folders and the ".." row stay so the user can still walk to scripted maps,
 * a map row stays only when it has a script beside it, and the inbuilt
 * Everard row (no path, no script) goes.
 */

#include <string.h>

#include "imgui_mapchooser.h"
#include "test_harness.h"

int run_mapchooser_scenario_filter(void) {
    MapChooserEntry plain;
    MapChooserEntry scripted;
    MapChooserEntry folder;
    MapChooserEntry parentUp;
    MapChooserEntry everard;
    memset(&plain, 0, sizeof(plain));
    memset(&scripted, 0, sizeof(scripted));
    memset(&folder, 0, sizeof(folder));
    memset(&parentUp, 0, sizeof(parentUp));
    memset(&everard, 0, sizeof(everard));
    strcpy(plain.path, "data/maps/Plain.map");
    strcpy(scripted.path, "data/maps/Soccer.map");
    scripted.scripted = true;
    strcpy(folder.path, "data/maps/Uploads");
    folder.isFolder = true;
    parentUp.isFolder = true;
    parentUp.isParentUp = true;
    strcpy(everard.name, "Everard Island (Inbuilt)");

    /* Tick off: today's list, unchanged. */
    UT_ASSERT(mapChooserEntryPassesScenarioFilter(&plain, false));
    UT_ASSERT(mapChooserEntryPassesScenarioFilter(&scripted, false));
    UT_ASSERT(mapChooserEntryPassesScenarioFilter(&folder, false));
    UT_ASSERT(mapChooserEntryPassesScenarioFilter(&parentUp, false));
    UT_ASSERT(mapChooserEntryPassesScenarioFilter(&everard, false));

    /* Tick on: only scripted maps, plus the rows used to navigate. */
    UT_ASSERT(!mapChooserEntryPassesScenarioFilter(&plain, true));
    UT_ASSERT(mapChooserEntryPassesScenarioFilter(&scripted, true));
    UT_ASSERT(mapChooserEntryPassesScenarioFilter(&folder, true));
    UT_ASSERT(mapChooserEntryPassesScenarioFilter(&parentUp, true));
    UT_ASSERT(!mapChooserEntryPassesScenarioFilter(&everard, true));

    /* A parent-up row that somehow lacks isFolder still stays: it is how
     * the user leaves a folder with nothing scripted in it. */
    parentUp.isFolder = false;
    UT_ASSERT(mapChooserEntryPassesScenarioFilter(&parentUp, true));
    return 0;
}
