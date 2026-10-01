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

/* Same as LoadCompressed but snapshot/restore the camera so the
 * caller can swap in regenerated map bytes without the view
 * snapping back to auto-fit. Use for live-preview rebuilds where
 * the user's current zoom/pan is the point. Returns false (and
 * leaves the camera alone) if the underlying load fails. */
bool mapPreviewViewLoadCompressedKeepCamera(MapPreviewView *v,
                                            const BYTE *data, int len);

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

/* Map-square (mapSqX,mapSqY) -> texture-local pixel of that tile's
 * TOP-LEFT (same point viewRenderStarts uses as a start's dest origin;
 * add half a scaled tile to center). Operates in the displayed
 * texture's pixel space (mapPreviewViewGetTextureSize). Returns false
 * if no valid sprite-mode transform is cached (e.g. minimap zoom). */
bool mapPreviewViewWorldToScreen(const MapPreviewView *v,
                                 int mapSqX, int mapSqY,
                                 float *outX, float *outY);

/* Texture-local pixel (sx,sy) -> map-square under it. Inverse of the
 * above. Returns false if no transform is cached or the result falls
 * outside the 0..255 map-square range. */
bool mapPreviewViewScreenToWorld(const MapPreviewView *v,
                                 float sx, float sy,
                                 int *outMapSqX, int *outMapSqY);

/* Reserved-start position accessors over the parsed preview. Return 0 /
 * false when no map has parsed. i is 1-based, matching the rest of the
 * start-index conventions. */
BYTE mapPreviewViewGetStartCount(const MapPreviewView *v);
bool mapPreviewViewGetStart(const MapPreviewView *v, BYTE i,
                            BYTE *outX, BYTE *outY);

/* Drive interactions for the frame: pan via mouse drag, zoom via
 * wheel / pinch, arrow-key pan. Caller indicates whether the view's
 * Image is currently hovered.
 *
 * Call this immediately after submitting the pan InvisibleButton over the
 * displayed Image (and its IsItemHovered): wheel zoom anchors the map point
 * under the cursor, and it reads the current ImGui item rect to know where
 * the Image is on screen. Submitting another item in between degrades wheel
 * zoom to centre-anchored; it does not misbehave. */
typedef struct {
    bool dragPan;      /* allow left-drag to pan (sub-unit exact) */
    bool wheelZoom;    /* allow wheel to zoom (cursor-anchored) */
    bool pinchZoom;    /* allow macOS pinch to zoom */
    bool arrowPan;     /* allow arrow keys to pan */
} MapPreviewInputOpts;

void mapPreviewViewHandleInput(MapPreviewView *v, bool hovered,
                               const MapPreviewInputOpts *opts);

/* True iff source map data has fully parsed and the widget can show
 * tiles. Useful for "loading..." placeholders. */
bool mapPreviewViewIsReady(const MapPreviewView *v);

/* Set per-start ownership for marker colouring. owners is 0-based, parallel
 * to start order (start index i+1): 0=unclaimed, 1=self, 2=ally, 3=enemy.
 * Selects the boat sprite at sprite zoom and the dot colour at minimap zoom.
 * MINIMAP_OWNER_OFFSIDE (minimap_render.h) may be set on top of a code: the
 * boat then draws at half its alpha and the dot half-blended into the
 * terrain, keeping the code's colour, for a start the viewer's team side
 * rejects. Pass owners=NULL / count=0 to clear (everything renders as the
 * default). */
void mapPreviewViewSetStartOwners(MapPreviewView *v,
                                  const uint8_t *owners, int count);

/* Put the camera on a map square, for "jump to here" navigation. minZoom
 * (0 to leave zoom alone) raises the zoom to at least that level so the
 * target is actually legible when the view was fitted to the whole map.
 * Suppresses the pending auto-fit, so this survives being called right
 * after a Load*. */
void mapPreviewViewCenterOnMapSquare(MapPreviewView *v,
                                     int mapSqX, int mapSqY, float minZoom);

/* Whether the texture should be point-sampled where it is drawn. True in
 * every mode that builds the offscreen at the display size (so the draw is
 * ~1:1 and bilinear only smears crisp art across a sub-pixel offset);
 * false on the sub-1x fallback that oversamples on purpose for the drawing
 * side to downscale. Note that SDL_SetTextureScaleMode on the returned
 * texture does nothing under the ImGui SDL_Renderer backend — it rewrites
 * the mode per draw — so an ImGui caller has to act on this via the
 * backend's sampler draw callbacks (imguiPush/PopNearestSampling). */
bool mapPreviewViewWantsNearestSampling(const MapPreviewView *v);

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
