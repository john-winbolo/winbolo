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
 * Name:          overview_view.h
 * Purpose:       The map overview's drawing half: renders a
 *                client's fog memory into an offscreen
 *                texture and feeds mouse / keyboard input to
 *                an OverviewCamera.
 *
 *                Host-independent. SDL textures are bound to
 *                the renderer that created them, so the
 *                renderer, the tile sheet and the scale that
 *                sheet was built at all arrive from the host
 *                on every render — the view creates only its
 *                own target.
 *********************************************************/

#ifndef OVERVIEW_VIEW_H
#define OVERVIEW_VIEW_H

#include <SDL3/SDL.h>

#include "overview_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/* keyItems, the player's in-game bindings. Included after overview_camera.h
   so the WinSock2 guard there has already been seen at the default struct
   packing, and from inside the extern "C" so the input declarations it
   carries keep C linkage in a C++ host that has not included it yet. */
#include "../input.h"

#ifndef CLIENTSIM_TYPEDEF
#define CLIENTSIM_TYPEDEF
typedef struct ClientSim ClientSim;
#endif

typedef struct OverviewView OverviewView;

OverviewView *overviewViewCreate(void);
void          overviewViewDestroy(OverviewView *v);

/* Redraw into the view's own offscreen texture, reallocating it when the
   requested size changes. tiles/sheetScale belong to the host, not the view,
   and so does crosshair — the local player's gunsight sprite, on the same
   renderer as the tile sheet for the same reason. NULL just leaves the
   crosshair undrawn.

   Everything the render reads from the sim comes through `snap`, which the
   host fills with clientSimFillOverviewSnapshot while it holds the client
   mutex and hands over here with the mutex released: nothing in this call
   touches the ClientSim. NULL draws the cleared frame and nothing on it.

   `ownsWindow` means the same thing it does in overviewViewHandleInput: this
   view has replaced the classic one rather than sitting beside it. Here it
   decides whether the picture gets a border while an item view is on: the
   pop-out does not, because the classic view is still on screen beside it with
   its own corner label. */
void          overviewViewRenderOffscreen(OverviewView *v, SDL_Renderer *r,
                                          SDL_Texture *tiles, int sheetScale,
                                          SDL_Texture *crosshair,
                                          int w, int h,
                                          const struct OverviewSnapshot *snap,
                                          bool ownsWindow);
SDL_Texture  *overviewViewGetTexture(const OverviewView *v);
void          overviewViewGetSize(const OverviewView *v, int *outW, int *outH);
OverviewCamera *overviewViewCamera(OverviewView *v);

/* View pixels off each edge that the host draws its own panels over — the
   status column, the build strip and the newswire in full screen. The view
   treats those strips as covered when it brings something into the picture,
   so a tank respawning behind a panel is scrolled into the clear rather than
   left under it. A host with nothing over the map never calls this, and the
   pop-out is such a host. */
void          overviewViewSetHudInsets(OverviewView *v, float left, float top,
                                       float right, float bottom);

/* Wheel / drag / click / keyboard, read from the current ImGui context — so
   the host calls this inside its own frame, right after submitting the pan
   InvisibleButton over the image. Pointer motion over the map moves the shared
   build cursor and a left-click builds at the square under it, so the pan item
   has to claim the right mouse button and leave the left one free. `cs`
   supplies the tank square that the centre-on-tank key needs and takes the
   build request and the gunsight bump; NULL disables all three. `keys` is the
   player's in-game bindings: the view's own keys are checked against them so
   it never shadows a game action, and NULL means no bindings are known, in
   which case every key is the view's.

   The wheel belongs to the gunsight, as it does over the main view, and zooms
   only while kiOverviewZoom is held — cleared, it zooms unconditionally.

   Zooming from the keyboard and turning following on and off are bindings of
   their own — kiOverviewZoomIn, kiOverviewZoomOut and kiOverviewFollow — read
   off the keyboard rather than through the shadow test, since the test counts
   them like any other binding. One rung, or one flip, per press.

   `ownsWindow` says this view has replaced the classic one rather than sitting
   beside it in a pop-out. The scroll keys then have no other map to scroll, so
   they pan this one; beside the classic view they are left to it. */
void          overviewViewHandleInput(OverviewView *v, bool hovered,
                                      int viewW, int viewH, ClientSim *cs,
                                      const keyItems *keys, bool ownsWindow);

/* Hand the OS pointer back if this view switched it to the crosshair. The
   host calls it whenever the view stops being shown: nothing else will put
   the system cursor back. Safe on a NULL view. */
void          overviewViewReleaseCursor(OverviewView *v);

/* The camera readout — the zoom and whether the camera follows the tank — is
   something the host draws over the map for a moment, not all the time. The
   view starts it itself when the player changes either through
   overviewViewHandleInput; the host starts it with overviewViewShowCameraNotice
   when the view comes back on screen, so the player sees the state they are
   picking up. A freshly created view starts with one. Safe on a NULL view.

   overviewViewCameraNotice fills `buf` with what to show and `outAlpha` with
   how solid to draw it (1 held, falling to 0 through the fade), and returns
   false once there is nothing to draw. `keys` supplies the follow binding the
   free-camera text names; NULL leaves it unnamed. */
void          overviewViewShowCameraNotice(OverviewView *v);
bool          overviewViewCameraNotice(OverviewView *v, const keyItems *keys,
                                       char *buf, size_t cap, float *outAlpha);

#ifdef __cplusplus
}
#endif

#endif /* OVERVIEW_VIEW_H */
