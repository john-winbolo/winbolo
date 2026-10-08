/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * build_cursor.h — the build target selection, shared by every input
 * device.
 *
 * There is ONE build cursor.  It holds an absolute map tile, and it is
 * the single answer to "which square does a build go to?":
 *
 *   - The mouse sets it when the player moves the pointer over the
 *     view; a click dispatches the LGM to it.
 *   - A tablet tap sets it and dispatches in the same gesture.
 *   - On gamepad it is a free cursor toggled with R3 and steered by
 *     the right stick (taking it over from map scroll while active),
 *     with X dispatching the build.  The gunsight-tile build the pad
 *     otherwise gets is fine within gunsight range but doesn't give
 *     the same "click anywhere" freedom.
 *
 * The renderer paints its reticle from the same tile
 * (buildCursorResolveReticle), so what the player sees and what a
 * build dispatches to are the same square by construction — deriving
 * either one from the live pointer position instead lets a view scroll
 * or a pointer resync silently move the target out from under the
 * player.
 *
 * Behaviour:
 *   - Cursor is pinned to its absolute map tile.  When the tank
 *     drives and the view scrolls, the cursor stays put — it can
 *     drift off-screen without being lost.  Only the player moving
 *     it (mouse, tap, stick) relocates it.
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

/* Controller build-behaviour options (persisted in prefs, set from the
   Controller tab of Set Keys). */
extern bool g_buildExitExecutes;   /* exiting build mode dispatches the build */
extern bool g_buildExitExecutesMomentaryOnly; /* sub-option: only execute-on-exit
                                                  when leaving press-and-hold
                                                  (momentary) mode, not a tap-off */
extern bool g_buildDoubleTapRoad;  /* double-tap the toggle builds a road under the tank */
extern bool g_buildHoldMomentary;  /* hold the toggle = momentary mode (off on release) */
extern bool g_buildAutoCloseOnExecute; /* executing a build (Execute Build action)
                                          auto-exits build cursor mode */

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

/* Set the cursor's absolute map tile directly.  Used by the mouse path so
   moving the mouse repositions the shared build cursor (it follows the
   reticle), keeping mouse and gamepad placement in sync.  Marks the cursor
   as positioned; works whether or not cursor mode is currently active. */
void buildCursorSetTile(BYTE mapX, BYTE mapY);

/* Read the cursor's stored target tile regardless of whether cursor mode is
   active.  Returns false only if no target has been set yet (then the caller
   should fall back to the gunsight).  This is what "Build Now" dispatches to,
   so a target locked in with cursor mode then turned OFF still builds — even
   if the tank has since driven it off-screen / out of range. */
bool buildCursorGetTargetTile(BYTE *mapX, BYTE *mapY);

/* The mouse pointer was carried off the view with this tile: forget it as
   the target, so a click or Build Now no longer sends the builder there.
   Only when it is still the target the mouse set — not while cursor mode is
   steering, nor once the stick or a tap has moved it elsewhere. */
void buildCursorDropTarget(BYTE mapX, BYTE mapY);

/* Clamp the cursor inside the visible area — only while cursor mode is ON.
   Call once per frame so that as the tank drives and the view scrolls, the
   cursor is dragged along the edge and never leaves the screen.  No-op when
   cursor mode is OFF (the target stays pinned to its absolute tile). */
void buildCursorClampToView(struct ClientSim *cs);

/* Apply a per-frame movement delta in zoomed pixels.  Whole-tile
   crossings step the cursor; sub-tile remainder is kept for next
   frame.  The camera scrolls to keep the cursor on-screen with a
   small edge margin.  No-op when not active. */
void buildCursorTick(struct ClientSim *cs, int dxPx, int dyPx);

/* Resolve the build reticle the renderer should paint this frame.
   This is the single source of truth shared by every frontend that
   draws the reticle — desktop, wasm, tablet — so what the player sees
   and what a build click dispatches to can never disagree.

   The reticle tracks the *latched* build target (an absolute map tile
   set by the mouse, a tap, or the gamepad stick), not the live pointer
   position, so it stays pinned to its world square while the tank
   drives and the view scrolls underneath it.

   pointerScreenX/Y are the frontend's own pointer/touch selection
   (1-based screen tiles), used only as the fallback before anything
   has been latched; pointerLive says whether that selection is
   currently showing.

   Outputs 1-based screen tile coords.  *outFaint is set when the
   target should be drawn dimmed (a locked target with no live pointer
   driving it — the gamepad "Build Now goes here" hint).  Returns false
   when no reticle should be drawn at all, e.g. the latched target has
   scrolled off-screen. */
bool buildCursorResolveReticle(struct ClientSim *cs, bool pointerLive,
                               BYTE pointerScreenX, BYTE pointerScreenY,
                               BYTE *outScreenX, BYTE *outScreenY,
                               bool *outFaint);

#ifdef __cplusplus
}
#endif

#endif /* WB_BUILD_CURSOR_H */
