/*
 * What the full screen key does (fullscreen_key.h).
 *
 * Outside a game it toggles the app full screen flag. In a game it toggles
 * the full screen map. When the server withholds the map (classic mode, or
 * the overview set to None), it goes back to the app full screen flag where
 * the desktop window owns that flag, and does nothing elsewhere. Before this,
 * a classic game started from a full screen lobby kept the window full
 * screen, and Alt+Enter had no way to take it back to windowed.
 */

#include "test_harness.h"

#include "fullscreen_key.h"

int run_fullscreen_key_action(void) {
    /* Outside a game: always the app full screen flag, whatever the rest. */
    UT_ASSERT(fullScreenKeyAction(false, false, true)  == FULLSCREEN_KEY_WINDOW);
    UT_ASSERT(fullScreenKeyAction(false, true,  true)  == FULLSCREEN_KEY_WINDOW);
    UT_ASSERT(fullScreenKeyAction(false, false, false) == FULLSCREEN_KEY_WINDOW);
    UT_ASSERT(fullScreenKeyAction(false, true,  false) == FULLSCREEN_KEY_WINDOW);

    /* In a game with the map offered: the full screen map. */
    UT_ASSERT(fullScreenKeyAction(true, false, true)  == FULLSCREEN_KEY_MAP);
    UT_ASSERT(fullScreenKeyAction(true, false, false) == FULLSCREEN_KEY_MAP);

    /* In a classic game on the desktop: the window, so a full screen lobby
       does not leave the player full screen for the whole game. */
    UT_ASSERT(fullScreenKeyAction(true, true, true) == FULLSCREEN_KEY_WINDOW);

    /* In a classic game where the window does not own the flag (Deck,
       tablet, iOS, web canvas): nothing to toggle. */
    UT_ASSERT(fullScreenKeyAction(true, true, false) == FULLSCREEN_KEY_NOTHING);
    return 0;
}
