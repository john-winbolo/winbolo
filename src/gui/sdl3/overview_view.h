/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
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

   `ownsWindow` means the same thing it does in overviewViewHandleInput: this
   view has replaced the classic one rather than sitting beside it. Then the
   camera keeps the tank on screen the way the classic view's scroll always
   has — see overviewCameraKeepTankOnScreen. */
void          overviewViewRenderOffscreen(OverviewView *v, SDL_Renderer *r,
                                          SDL_Texture *tiles, int sheetScale,
                                          SDL_Texture *crosshair,
                                          int w, int h, ClientSim *cs,
                                          bool ownsWindow);
SDL_Texture  *overviewViewGetTexture(const OverviewView *v);
void          overviewViewGetSize(const OverviewView *v, int *outW, int *outH);
OverviewCamera *overviewViewCamera(OverviewView *v);

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

#ifdef __cplusplus
}
#endif

#endif /* OVERVIEW_VIEW_H */
