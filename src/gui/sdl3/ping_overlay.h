/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Ping Overlay
 *Filename:      ping_overlay.h
 *Purpose:
 *  The smart-ping pie menu and the markers for pings that
 *  have arrived.
 *
 *  Two halves, in two places:
 *
 *   * pingOverlayHandleEvent runs from the game's raw event
 *     handler (sdl3DrawHandleEvent). It owns the chord that
 *     opens the menu, the cursor while it is open, and the
 *     release that decides which ping to send. Events reach
 *     it only after the ImGui pump has already dropped
 *     anything ImGui wants, so a click on a panel over the
 *     game view never opens the pie.
 *
 *   * pingOverlayDraw runs inside the ImGui frame and paints
 *     the menu, the world markers and the off-screen edge
 *     markers into the foreground draw list — the same list
 *     the tablet controls draw on, so the ordering against
 *     the rest of the in-game overlay is already settled.
 *
 *  The geometry each half needs is in ping_pie.h and
 *  ping_edge.h; this file is the state machine and the
 *  drawing.
 *********************************************************/

#ifndef WINBOLO_PING_OVERLAY_H
#define WINBOLO_PING_OVERLAY_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ClientSim;

/* Rasterise the six ping icons out of data/ui/ping/ onto this renderer.
 * Safe to call again on a renderer change; the previous textures are freed.
 * An icon that will not load leaves its slot empty and the drawer falls back
 * to a plain disc, so a missing file costs the marker its glyph and nothing
 * else. */
void pingOverlayInit(SDL_Renderer *renderer);

/* Free the icon textures. */
void pingOverlayShutdown(void);

/* Offer one raw SDL event to the pie menu. Returns true when the menu
 * consumed it, so the caller stops treating it as a game input — a press
 * that opens the pie must not also drive a build or a shot.
 *
 * Called from the in-game event handler with events ImGui has already
 * declined. */
bool pingOverlayHandleEvent(struct ClientSim *cs, const SDL_Event *ev);

/* Is the pie menu open right now? For the in-game input that is polled or
 * read straight off an event rather than routed through
 * pingOverlayHandleEvent — the mouse wheel's gunsight adjust — so it stays
 * out of the way while the player is choosing a ping. */
bool pingOverlayIsMenuOpen(void);

/* Draw the menu and every ping still on its clock. Call once per frame from
 * inside the ImGui frame, while a game is running. */
void pingOverlayDraw(struct ClientSim *cs);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_PING_OVERLAY_H */
