/*
 * Copyright (c) 1998-2008 John Morrison.
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
 * Name:          map_preview_view.h
 * Purpose:       Reusable zoomable / pannable map preview
 *                widget — renders tile-based map content
 *                into an offscreen SDL_Texture that the
 *                caller displays with ImGui::Image (or
 *                anywhere else).
 *
 *                Two intended call sites today:
 *                  (a) the full-screen modal popup
 *                      (src/gui/sdl3/map_preview_popup.cpp)
 *                  (b) the lobby's ##MapPanel inline view
 *                      (src/gui/sdl3/dialogs/imgui_lobby.cpp)
 *********************************************************/

#ifndef MAP_PREVIEW_VIEW_H
#define MAP_PREVIEW_VIEW_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include "global.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MapPreviewView MapPreviewView;

/* Allocate a fresh widget instance. Returns NULL on OOM. */
MapPreviewView *mapPreviewViewCreate(void);

/* Free every resource held by the widget (textures, parsed map, source
 * data). The pointer is invalid after this returns. */
void mapPreviewViewDestroy(MapPreviewView *v);

/* Load source map data. Either of these triggers a deferred
 * preview-build that happens on the next mapPreviewViewRenderOffscreen
 * call (so they're cheap to invoke from outside a render frame). The
 * data is copied internally — the caller's buffer can be freed
 * immediately. */
bool mapPreviewViewLoadCompressed(MapPreviewView *v,
                                  const BYTE *data, int len);
bool mapPreviewViewLoadFile(MapPreviewView *v, const char *path);

/* Set initial camera centre + zoom from minimap-derived bounds (used
 * by the popup-style "click to open" path). The widget will auto-fit
 * once the underlying map data has parsed; this call is a hint for
 * the frames before that completes. */
void mapPreviewViewSetInitialBounds(MapPreviewView *v,
                                    int boundsMinX, int boundsMinY,
                                    int boundsMaxX, int boundsMaxY);

/* Render the visible tiles into the widget's offscreen texture. Safe
 * to call every frame; only rebuilds the offscreen when the viewport
 * size changes or the source data was reloaded.  Call BEFORE
 * ImGui::NewFrame (changes the SDL render target). */
void mapPreviewViewRenderOffscreen(MapPreviewView *v,
                                   SDL_Renderer *renderer,
                                   int viewW, int viewH);

/* Return the texture last filled by RenderOffscreen — feed to
 * ImGui::Image. NULL until the first successful render. */
SDL_Texture *mapPreviewViewGetTexture(MapPreviewView *v);

/* Read back the offscreen texture dimensions (= the resolution the
 * caller should display the Image at; usually the same as the viewW/H
 * passed to RenderOffscreen, but may differ at sub-1x zoom where the
 * widget oversamples). */
void mapPreviewViewGetTextureSize(const MapPreviewView *v,
                                  int *outW, int *outH);

/* Drive interactions for the frame: pan via mouse drag, zoom via
 * wheel / pinch, arrow-key pan. Caller indicates whether the view's
 * Image is currently hovered. */
typedef struct {
    bool dragPan;      /* allow left-drag to pan */
    bool wheelZoom;    /* allow wheel to zoom (cursor-anchored) */
    bool pinchZoom;    /* allow macOS pinch to zoom */
    bool arrowPan;     /* allow arrow keys to pan */
} MapPreviewInputOpts;

void mapPreviewViewHandleInput(MapPreviewView *v, bool hovered,
                               const MapPreviewInputOpts *opts);

/* True iff source map data has fully parsed and the widget can show
 * tiles. Useful for "loading..." placeholders. */
bool mapPreviewViewIsReady(const MapPreviewView *v);

/* Current zoom level (1.0f = native tile size). */
float mapPreviewViewGetZoom(const MapPreviewView *v);

/* Bump zoom by one step in the widget's preset table. Returns the new
 * level. Used by an "expand"/"zoom in" icon button. */
float mapPreviewViewZoomIn(MapPreviewView *v);
float mapPreviewViewZoomOut(MapPreviewView *v);

#ifdef __cplusplus
}
#endif

#endif /* MAP_PREVIEW_VIEW_H */
