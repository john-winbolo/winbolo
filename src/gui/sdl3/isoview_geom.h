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
 * Name:          isoview_geom.h
 * Purpose:
 *   What the isometric view draws, worked out as plain
 *   geometry: the ground diamond of every square a target
 *   shows, the roof, faces and shadow of every tile that
 *   stands up, the tanks, men and shells on top, and where
 *   the overlay's marks go. Every quad comes back as four
 *   screen corners, a source rect and a vertex colour, in
 *   the order to draw them, and nothing here touches a
 *   renderer or the sim: the inputs are the overview
 *   snapshot's map memory and lists, an IsoCamera and a
 *   target size, so the unit tests link this file and check
 *   the picture square by square.
 *
 *   isoview.c is the other half. It owns the offscreen
 *   texture and turns these arrays into SDL_RenderGeometry
 *   and SDL_RenderTexture calls.
 *********************************************************/

#ifndef ISOVIEW_GEOM_H
#define ISOVIEW_GEOM_H

#include <stdbool.h>

#include "iso_project.h"
#include "iso_sheet.h"       /* IsoSpriteKind, IsoTeam */
#include "overview_types.h"  /* OverviewMap */
#include "screentank.h"
#include "screenlgm.h"
#include "screenbullet.h"
#include "client_sim.h"      /* OverviewItemLabel */

#ifdef __cplusplus
extern "C" {
#endif

/* A rect in texels of whichever texture the quad names. For the classic
   sheet it is at scale 1, as tiles.h gives it, and the drawer multiplies by
   the sheet scale; for the isometric sheet it is already at the density the
   sheet was built at, as the lookup handed it over. */
typedef struct IsoGeomRect {
    float x, y, w, h;
} IsoGeomRect;

/* Which texture a quad samples. */
typedef enum IsoGeomTex {
    ISO_GEOM_TEX_NONE = 0,   /* a flat colour */
    ISO_GEOM_TEX_CLASSIC,    /* the classic tile sheet, or the sprite atlas
                                built from it */
    ISO_GEOM_TEX_ISO         /* the isometric sheet */
} IsoGeomTex;

/* One quad on the target. A ground or roof diamond lists its corners top,
   right, bottom, left; a face, a shadow or an upright sprite lists them
   top-left, top-right, bottom-right, bottom-left. The source rect's corners
   map to the same four in the same order, so a tile's top-left texel lands
   on a diamond's top vertex; with the ground turned (IsoGeomInput.groundTurns)
   a ground or roof diamond's corners are listed from the vertex the texel
   moved to instead. The colour multiplies the texels: white draws
   the sprite as it is, and the fog and face shading are darker values. */
typedef struct IsoGeomQuad {
    float         px[4], py[4];
    IsoGeomRect   src;
    unsigned char tex;             /* IsoGeomTex */
    unsigned char r, g, b, a;
    short         mapX, mapY;      /* the square it belongs to */
} IsoGeomQuad;

/* The tiles that stand above the ground, and their heights in game pixels.
   Everything else is flat. */
typedef enum IsoTallKind {
    ISO_TALL_NONE = 0,
    ISO_TALL_BUILDING,
    ISO_TALL_SHOT_BUILDING,
    ISO_TALL_PILL,
    ISO_TALL_BASE,
    ISO_TALL_FOREST,
    ISO_TALL_KIND_COUNT
} IsoTallKind;

#define ISO_HEIGHT_BUILDING       10
#define ISO_HEIGHT_SHOT_BUILDING  5
#define ISO_HEIGHT_PILL           6
#define ISO_HEIGHT_BASE           3
#define ISO_HEIGHT_FOREST         5
/* The tallest of them, which is how far below the target's bottom edge a
   roof can still reach into the picture. */
#define ISO_HEIGHT_TALLEST        ISO_HEIGHT_BUILDING

/* The two faces a tall tile can show, as a mask. The south-west face is the
   square's south edge, which projects from the left vertex to the bottom
   one; the south-east face is its east edge, right vertex to bottom. */
#define ISO_FACE_SW 0x1
#define ISO_FACE_SE 0x2

/* How bright each face is drawn, out of 255: the light is from the upper
   left of the screen. */
#define ISO_FACE_SHADE_SW 191
#define ISO_FACE_SHADE_SE 140

/* The ground shadow of a tall tile and the drop shadow of a sprite, both
   black at these alphas. */
#define ISO_SHADOW_TALL_ALPHA   77    /* about 30 percent */
#define ISO_SHADOW_SPRITE_ALPHA 102   /* about 40 percent */
/* How far the drop shadow sits from its sprite, in game pixels, on both
   axes. */
#define ISO_SHADOW_SPRITE_OFFSET 3

/* What one entry of the sorted pass is. */
typedef enum IsoGeomItemKind {
    ISO_GEOM_ITEM_TALL = 0,
    ISO_GEOM_ITEM_TANK,
    ISO_GEOM_ITEM_LGM,
    ISO_GEOM_ITEM_SHELL
} IsoGeomItemKind;

/* One thing that stands on the ground, with the quads that draw it. The
   quads sit in IsoGeomScene.stand at quadFirst for quadCount, in the order to
   draw them: shadow, then faces, then roof or sprite. The items are sorted
   by depth, so walking them in order and drawing each one's quads draws the
   scene. */
typedef struct IsoGeomItem {
    int           depth;         /* isoDepth of the square it is keyed on */
    unsigned char kind;          /* IsoGeomItemKind */
    unsigned char tallKind;      /* IsoTallKind, for a tall tile */
    unsigned char faces;         /* ISO_FACE_* drawn, for a tall tile */
    bool          isoSprite;     /* drawn from the isometric sheet */
    int           frame;         /* the 16-step facing drawn, for a tank or
                                    man; the tile for a tall tile */
    int           listIndex;     /* 1-based index into the snapshot list, for
                                    a tank, man or shell */
    int           mapX, mapY;
    int           quadFirst, quadCount;
} IsoGeomItem;

/* Where the overlay puts a mark. */
typedef struct IsoGeomAnchor {
    int   index;    /* the tank's 1-based list index, or the item label's
                       index in the array handed in */
    float x, y;
} IsoGeomAnchor;

typedef struct IsoGeomOverlay {
    bool  cursorShown;
    bool  cursorFaint;
    float cursorX[4], cursorY[4];        /* the cursor square's diamond */
    bool  gunsightShown;
    float gunsightX, gunsightY;          /* the aim point */
    int           tankLabelCount;
    IsoGeomAnchor tankLabels[MAX_TANKS]; /* top-left of each name */
    int           itemLabelCount;
    IsoGeomAnchor itemLabels[MAX_PILLS + MAX_BASES]; /* centre of each number */
} IsoGeomOverlay;

/* The isometric sheet's answer for one sprite: whether the sheet holds it
   and, when it does, the cell's rect in texels. The drawer answers from
   isoSheetRect at the sheet's density; a test answers from a table. */
typedef bool (*IsoGeomSheetFn)(void *user, IsoSpriteKind kind, IsoTeam team,
                               int frame, IsoGeomRect *out);

/* Everything a build reads. The lists and the map are the snapshot's, taken
   with the client mutex held and read here after it is released, as the
   full screen map does. Any of them may be NULL: a NULL map draws no ground
   and no tall tiles, a NULL list draws none of that kind. */
typedef struct IsoGeomInput {
    IsoCamera           cam;
    int                 targetW, targetH;

    const OverviewMap   *map;
    const screenTanks   *tanks;
    const screenLgm     *lgms;
    const screenBullets *bullets;

    /* The player's animation smoothness (GfxAnimSmoothness) and the sheet
       scale its Match pixelation step snaps to, as spritePositionOffset takes
       them; smoothShells is the setting that puts shells in Smooth whatever
       the mode. */
    int  animMode;
    int  sheetScale;
    bool smoothShells;

    /* What fog looks like: fogLookColour's answer. washes false draws every
       square at full brightness. */
    bool          fogWashes;
    unsigned char fogR, fogG, fogB;

    IsoGeomSheetFn sheet;
    void          *sheetUser;

    /* Quarter turns clockwise the map memory was turned by before it got
       here, 0..3. The tiles' artwork is turned to match: each ground and
       roof diamond's corners are listed from the vertex the tile's top-left
       texel lands on after the turn, one vertex clockwise per quarter, so a
       road or a river bank faces the way its turned square does. 0 (the
       memset default) draws the artwork as the sheet has it. */
    int groundTurns;

    /* The overlay's inputs. The item labels are already filtered to the
       squares that show a number. */
    bool  cursorShown, cursorFaint;
    int   cursorMapX, cursorMapY;
    bool  gunsightShown;
    int   gsMapX, gsMapY, gsPixelX, gsPixelY;
    const OverviewItemLabel *itemLabels;
    int                      itemLabelCount;
} IsoGeomInput;

/* The built picture. The arrays grow as a build needs and are kept between
   builds; isoGeomSceneFree gives them back. */
typedef struct IsoGeomScene {
    IsoGeomQuad *ground;      /* every ground diamond and mine, map order */
    int          groundCount, groundCap;
    IsoGeomQuad *stand;       /* the quads the items below index */
    int          standCount, standCap;
    IsoGeomItem *items;       /* sorted by depth, then by build order */
    int          itemCount, itemCap;
    IsoGeomOverlay overlay;
    /* The square range the ground pass walked, for a caller that draws
       something else over the same squares. */
    int minX, minY, maxX, maxY;
} IsoGeomScene;

void isoGeomSceneInit(IsoGeomScene *s);
void isoGeomSceneFree(IsoGeomScene *s);

/*********************************************************
 * NAME:          isoGeomBuild
 * PURPOSE:
 *   Fills the scene from the input: the ground pass over
 *   isoGeomVisibleRange, every tall tile and sprite in it
 *   as items sorted by isoDepth, and the overlay anchors.
 *   Returns false when the arrays could not be grown, with
 *   the scene emptied.
 *********************************************************/
bool isoGeomBuild(IsoGeomScene *s, const IsoGeomInput *in);

/*********************************************************
 * NAME:          isoGeomVisibleRange
 * PURPOSE:
 *   isoVisibleRange widened at its far end for the tallest
 *   roof, as iso_project.h describes: a roof of height h
 *   reaches h / 8 squares along each ground axis past the
 *   bottom edge, so ceil(ISO_HEIGHT_TALLEST / 8) is added
 *   to both maxima, clamped to the map.
 *********************************************************/
bool isoGeomVisibleRange(const IsoCamera *cam, int targetW, int targetH,
                         int *outMinX, int *outMinY, int *outMaxX, int *outMaxY);

/*********************************************************
 * NAME:          isoGeomTallKind / isoGeomTallHeight
 * PURPOSE:
 *   What stands on a square drawn with this tile, and how
 *   high it is in game pixels; ISO_TALL_NONE and 0 for flat
 *   ground.
 *********************************************************/
IsoTallKind isoGeomTallKind(BYTE tile);
int         isoGeomTallHeight(IsoTallKind kind);

/*********************************************************
 * NAME:          isoGeomExposedFaces
 * PURPOSE:
 *   Which of the two faces square (mx, my) shows: a face is
 *   drawn where the neighbour on that side, south for the
 *   south-west face and east for the south-east, is not a
 *   tile of the same kind. Off the map counts as a different
 *   kind. 0 for a square that is not tall.
 *********************************************************/
unsigned isoGeomExposedFaces(const OverviewMap *map, int mx, int my);

/*********************************************************
 * NAME:          isoGeomTankTeam
 * PURPOSE:
 *   The team and facing a classic tank frame carries, and
 *   whether it is on a boat. The frame is TANK_SELF_0 to
 *   TANK_EVILBOAT_15; false for anything else.
 *********************************************************/
bool isoGeomTankTeam(int frame, IsoTeam *outTeam, bool *outBoat,
                     int *outFacing);

/*********************************************************
 * NAME:          isoGeomSquareCentre
 * PURPOSE:
 *   The screen position of the centre of square (mx, my)
 *   raised `height` game pixels, under the input's camera.
 *   For the marks a host places itself, the pings among
 *   them.
 *********************************************************/
void isoGeomSquareCentre(const IsoGeomInput *in, int mx, int my, float height,
                         float *outX, float *outY);

/*********************************************************
 * NAME:          isoGeomGroundTurns
 * PURPOSE:
 *   The input's quarter-turn count brought into 0..3, so a
 *   negative or a whole number of full turns reads as the
 *   turn it amounts to.
 *********************************************************/
int  isoGeomGroundTurns(const IsoGeomInput *in);

#ifdef __cplusplus
}
#endif

#endif /* ISOVIEW_GEOM_H */
