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
 *   * pingOverlayHandleEvent runs at the very top of the main
 *     window's event pump, before ImGui is offered the event
 *     at all. It owns the chord that opens the menu, the
 *     cursor while it is open, and the release that decides
 *     which ping to send. It has to run first because the
 *     map overview lays an InvisibleButton over its picture
 *     to catch a right-drag as a pan: give ImGui the press
 *     and the pan starts, and the swallow block in the pump
 *     then drops the event before the game ever sees it.
 *
 *     Running before ImGui means io.WantCaptureMouse cannot
 *     be asked whether the press was over a panel, so the
 *     answer comes from the frame just drawn instead: each
 *     view records where its map is on screen and whether the
 *     pointer was over it (pingOverlaySetClassicSurface /
 *     pingOverlaySetOverviewSurface), and a press opens the
 *     pie only inside that.
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
struct OverviewCamera;

/* ------------------------------------------------------------------
 * The map surface, recorded once per frame by whichever view drew it
 * ------------------------------------------------------------------
 *
 * The event hook runs before ImGui, so it cannot ask ImGui anything about
 * the pointer. These say where the map was last frame and whether the
 * pointer was on it and not on a panel; a chord press outside that opens
 * nothing. They also tell the drawer which coordinate system a ping's world
 * position converts through — the classic view's offset/sub-position pair,
 * or the overview's camera.
 *
 * Call exactly one of them per frame while a game is up, and
 * pingOverlayClearSurface whenever neither map is on screen.
 */

/* The classic 15x15 view. Its rectangle comes from sdl3draw, so the caller
 * passes only whether the pointer was over it with no ImGui window on top. */
void pingOverlaySetClassicSurface(bool hoverable);

/* The map overview filling the game window. (x, y, w, h) is where its image
 * was blitted, in the same pixels ImGui reports the mouse in; `cam` is the
 * camera it was drawn with (copied, so the caller may go on moving it) and
 * viewW/viewH the offscreen's size, which is what the camera's own
 * conversions are measured against. */
void pingOverlaySetOverviewSurface(float x, float y, float w, float h,
                                   bool hoverable,
                                   const struct OverviewCamera *cam,
                                   int viewW, int viewH);

/* No map on screen: the lobby, or a frame that could not draw one. A chord
 * press then opens nothing. */
void pingOverlayClearSurface(void);

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
 * Called from the top of the main window's event pump, before ImGui: an
 * event this takes is not offered to ImGui either, so the overview's pan
 * item never activates under a chord and never sees the release. */
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
