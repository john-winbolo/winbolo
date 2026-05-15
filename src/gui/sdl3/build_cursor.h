/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * build_cursor.h — gamepad-driven free build cursor.
 *
 * Mouse players click any tile to send the LGM there.  On gamepad the
 * X (build_confirm) action sends the LGM to the gunsight tile, which
 * is fine within gunsight range but doesn't give the same "click
 * anywhere" freedom.  This module is the gamepad equivalent: an
 * absolute-map-coord cursor toggled with R3, steered by the right
 * stick (taking it over from map scroll while active), confirmed by
 * X to dispatch the LGM build.
 *
 * Behaviour matches the mouse cursor reticle:
 *   - Cursor is pinned to its absolute map tile.  When the tank
 *     drives and the view scrolls, the cursor stays put — it can
 *     drift off-screen without being lost.
 *   - X dispatch leaves the cursor in place.  Repeated presses send
 *     more LGMs to the same tile until the player relocates.
 *   - If the player nudges the stick while the cursor is off-screen,
 *     the cursor snaps to the tank tile first so movement begins
 *     from a visible reference instead of an invisible reticle.
 *   - Camera-follow only kicks in when the player drives the cursor
 *     near a visible-area edge, never on tank-driven view scrolls.
 */
#ifndef WB_BUILD_CURSOR_H
#define WB_BUILD_CURSOR_H

#include <stdbool.h>
#include "global.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ClientSim;

/* Hard reset — turns off and clears accumulators.  Call at game
   teardown / new-game so leftover state can't survive into the next
   session. */
void buildCursorReset(void);

/* True while the cursor mode is active. */
bool buildCursorIsActive(void);

/* Toggle cursor on/off.  Entering snaps the cursor to the gunsight
   tile (or the tank tile if the gunsight isn't valid).  Idempotent. */
void buildCursorToggle(struct ClientSim *cs);

/* Force-exit cursor mode.  Used when X dispatches a build, or when
   gameplay context changes (entering pause/menu, gameover, etc.). */
void buildCursorExit(void);

/* Read the current cursor tile (absolute map coordinates).  Returns
   false when not active; outputs are then untouched. */
bool buildCursorGetTile(BYTE *mapX, BYTE *mapY);

/* Apply a per-frame movement delta in zoomed pixels.  Whole-tile
   crossings step the cursor; sub-tile remainder is kept for next
   frame.  The camera scrolls to keep the cursor on-screen with a
   small edge margin.  No-op when not active. */
void buildCursorTick(struct ClientSim *cs, int dxPx, int dyPx);

#ifdef __cplusplus
}
#endif

#endif /* WB_BUILD_CURSOR_H */
