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
   crosshair undrawn. */
void          overviewViewRenderOffscreen(OverviewView *v, SDL_Renderer *r,
                                          SDL_Texture *tiles, int sheetScale,
                                          SDL_Texture *crosshair,
                                          int w, int h, ClientSim *cs);
SDL_Texture  *overviewViewGetTexture(const OverviewView *v);
void          overviewViewGetSize(const OverviewView *v, int *outW, int *outH);
OverviewCamera *overviewViewCamera(OverviewView *v);

/* Wheel / drag / keyboard, read from the current ImGui context — so the host
   calls this inside its own frame, right after submitting the pan
   InvisibleButton over the image. `cs` supplies the tank square that the
   centre-on-tank key needs; NULL just disables that key. */
void          overviewViewHandleInput(OverviewView *v, bool hovered,
                                      int viewW, int viewH, ClientSim *cs);

#ifdef __cplusplus
}
#endif

#endif /* OVERVIEW_VIEW_H */
