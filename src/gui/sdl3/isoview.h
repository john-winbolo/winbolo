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
 * Name:          isoview.h
 * Purpose:       The isometric view's drawing half: renders
 *                the overview snapshot into an offscreen
 *                texture as a dimetric 2:1 picture, with the
 *                ground as textured diamonds, buildings,
 *                pills, bases and forest standing up, the
 *                tanks, men and shells on top and the
 *                overlay flat over the lot.
 *
 *                The same host shape as overview_view.h: the
 *                view creates only its own target. The
 *                renderer, the tile sheet and its scale, the
 *                sprite atlas, the isometric sheet and the
 *                crosshair all arrive from the host on every
 *                render, since a texture is bound to the
 *                renderer that made it. What is drawn is
 *                worked out by isoview_geom.c; this file turns
 *                its arrays into renderer calls.
 *********************************************************/

#ifndef ISOVIEW_H
#define ISOVIEW_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <stdbool.h>

#include "iso_project.h"
#include "sprite_atlas.h"
#include "tank_label.h"
#include "mapview_overlay.h"  /* ItemLabelCache */

#ifdef __cplusplus
extern "C" {
#endif

struct OverviewSnapshot;

typedef struct IsoView IsoView;

/* The caches and faces the labels are drawn with, all the host's: the
   textures in a cache belong to the renderer that made them, and the faces
   are the main window's, opened at its zoom. labelFont draws the tank names
   and the ping senders' names; pillFont and baseFont the item numbers.
   mainZoom is the zoom the faces were opened at, so a glyph can be scaled
   from that to the camera's scale. A NULL font or cache draws none of that
   kind. The ping names need a cache of their own, keyed on the player slot
   like the tank names' but holding different text for it. */
typedef struct IsoViewLabels {
    TankLabelCache *labelCache;
    TankLabelCache *pingNameCache;
    ItemLabelCache *itemLabelCache;
    TTF_Font       *labelFont;
    TTF_Font       *pillFont;
    TTF_Font       *baseFont;
    int             mainZoom;
} IsoViewLabels;

IsoView *isoViewCreate(void);
void     isoViewDestroy(IsoView *v);

/* Redraw into the view's own offscreen texture, reallocating it when the
   requested size changes. Must be called on the render thread.

   tilesTex and sheetScale are the host's tile sheet and the scale it was
   built at; spritesTex and sprites the padded sprite atlas, either NULL to
   draw the moving sprites from the sheet; isoTex the isometric sheet, built
   at sheetScale, NULL to draw every standing thing from the classic sprites;
   crosshair the gunsight sprite, NULL to leave it undrawn.

   Everything the render reads from the sim comes through `snap`, filled by
   the host under the client mutex and handed over here with the mutex
   released: nothing in this call touches the ClientSim. NULL draws the
   cleared frame and nothing on it.

   The camera: an item view centres it on the watched square, and draws the
   full screen map's yellow border round the picture; otherwise the camera
   follows the tank's position, snapped to its square's centre with Smooth
   Scrolling off, until the host moves it through isoViewCamera. The view's
   HUD insets shift the point followed so it lands in the clear part of the
   picture.

   Blackout: when the snapshot says the death is in its blackout the target
   is filled black over the finished frame. The death static the classic view
   draws is the host's to put over the texture; this view draws none. */
void isoViewRenderOffscreen(IsoView *v, SDL_Renderer *renderer,
                            SDL_Texture *tilesTex, int sheetScale,
                            SDL_Texture *spritesTex, const SpriteAtlas *sprites,
                            SDL_Texture *isoTex, SDL_Texture *crosshair,
                            int w, int h,
                            const struct OverviewSnapshot *snap,
                            const IsoViewLabels *labels);

/* The parts one render reads, for a host with no OverviewSnapshot: the Log
   Viewer fills these from its replayed state. Any list may be NULL to draw
   none of that kind; the item labels are already filtered to the squares
   that show a number. The camera is the view's own (isoViewCamera) and is
   not moved: a host that fills parts places it itself. */
typedef struct IsoViewParts {
    const struct OverviewMap   *map;
    const screenTanks          *tanks;
    const screenLgm            *lgms;
    const screenBullets        *bullets;
    const OverviewItemLabel    *itemLabels;
    int                         itemLabelCount;
    bool  cursorShown, cursorFaint;
    int   cursorMapX, cursorMapY;
    bool  gunsightShown;
    int   gsMapX, gsMapY, gsPixelX, gsPixelY;
    const ClientPing           *pings;
    int                         pingCount;
    bool  blackout;
    /* Whether an item view is on, for the border round the picture. The
       caption that names the view is the host's. */
    bool  itemViewBorder;
    /* Quarter turns the map was given before it got here, so the ground
       artwork is turned to match; 0 for the game's own view. */
    int   groundTurns;
} IsoViewParts;

/* isoViewRenderOffscreen's render from parts rather than a snapshot: the
   same renderer, textures and labels, the target cleared and filled, the
   camera left where the host put it. isoViewRenderOffscreen unpacks its
   snapshot into these and calls this, so there is one render path. */
void isoViewRenderParts(IsoView *v, SDL_Renderer *renderer,
                        SDL_Texture *tilesTex, int sheetScale,
                        SDL_Texture *spritesTex, const SpriteAtlas *sprites,
                        SDL_Texture *isoTex, SDL_Texture *crosshair,
                        int w, int h,
                        const IsoViewParts *parts,
                        const IsoViewLabels *labels);

SDL_Texture *isoViewGetTexture(const IsoView *v);
void         isoViewGetSize(const IsoView *v, int *outW, int *outH);

/* The camera, to read and move. The centre is in fractional world units
   (256 per square) and the scale in screen pixels per game pixel; see
   iso_project.h. */
IsoCamera   *isoViewCamera(IsoView *v);

/* Whether the render recentres the camera on the tank (or the item view's
   square) each frame. On by default; a host that pans the camera itself
   turns it off and puts it back when the player asks to be taken back to
   the tank. */
void         isoViewSetFollow(IsoView *v, bool follow);
bool         isoViewGetFollow(const IsoView *v);

/* Whether the host's pointer is over the picture with no panel under it.
   It decides how the build cursor is drawn: solid while cursor mode steers
   it or the pointer that put it there is live, faint for a target that is
   only locked in, the split the classic view and the full screen map make.
   Off until the host says otherwise. */
void         isoViewSetPointerOnMap(IsoView *v, bool onMap);

/* View pixels off each edge that the host draws its own panels over. The
   followed point is centred in what is left, so a tank is not followed to
   a spot under the status column. */
void         isoViewSetHudInsets(IsoView *v, float left, float top,
                                 float right, float bottom);

#ifdef __cplusplus
}
#endif

#endif /* ISOVIEW_H */
