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
 * Name:          test_gfx_view_projection.c
 * Purpose:       The view projection setting in gfx_settings.c:
 *                a fresh install draws the classic view, both
 *                values survive a set and a get, anything
 *                outside the enum lands on classic, and the
 *                reset to defaults puts an isometric pick back.
 *                The prefs loader writes through this store, so
 *                this is what keeps a hand-edited WinBolo.json
 *                from leaving the game in a view nothing draws.
 *********************************************************/

#include "gfx_settings.h"
#include "test_harness.h"

int run_gfx_view_projection(void) {
    /* Back to a fresh install first: the store is process-wide, and what a
       player who has never picked gets can only be asked from a known
       state. */
    gfxSettingsResetDefaults();

    UT_ASSERT_MSG(gfxGetViewProjection() == GFX_VIEW_CLASSIC,
                  "a fresh install starts at %d, expected classic (%d)",
                  (int)gfxGetViewProjection(), (int)GFX_VIEW_CLASSIC);

    gfxSetViewProjection(GFX_VIEW_ISOMETRIC);
    UT_ASSERT_MSG(gfxGetViewProjection() == GFX_VIEW_ISOMETRIC,
                  "set isometric, read back %d", (int)gfxGetViewProjection());

    gfxSetViewProjection(GFX_VIEW_CLASSIC);
    UT_ASSERT_MSG(gfxGetViewProjection() == GFX_VIEW_CLASSIC,
                  "set classic, read back %d", (int)gfxGetViewProjection());

    /* Past the end, and a nonsense number: both read as classic. */
    gfxSetViewProjection(GFX_VIEW_ISOMETRIC);
    gfxSetViewProjection((GfxViewProjection)((int)GFX_VIEW_ISOMETRIC + 1));
    UT_ASSERT_MSG(gfxGetViewProjection() == GFX_VIEW_CLASSIC,
                  "a value past the end left %d, expected classic",
                  (int)gfxGetViewProjection());

    gfxSetViewProjection(GFX_VIEW_ISOMETRIC);
    gfxSetViewProjection((GfxViewProjection)99);
    UT_ASSERT_MSG(gfxGetViewProjection() == GFX_VIEW_CLASSIC,
                  "a nonsense value left %d, expected classic",
                  (int)gfxGetViewProjection());

    /* Below the enum as well: atoi on a prefs value that is not a number can
       hand a negative in. */
    gfxSetViewProjection(GFX_VIEW_ISOMETRIC);
    gfxSetViewProjection((GfxViewProjection)-1);
    UT_ASSERT_MSG(gfxGetViewProjection() == GFX_VIEW_CLASSIC,
                  "a negative value left %d, expected classic",
                  (int)gfxGetViewProjection());

    /* The reset covers this setting too. */
    gfxSetViewProjection(GFX_VIEW_ISOMETRIC);
    gfxSettingsResetDefaults();
    UT_ASSERT_MSG(gfxGetViewProjection() == GFX_VIEW_CLASSIC,
                  "reset to defaults left %d, expected classic",
                  (int)gfxGetViewProjection());

    return 0;
}
