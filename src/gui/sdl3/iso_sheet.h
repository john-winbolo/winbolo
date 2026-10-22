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
 * Name:          iso_sheet.h
 * Purpose:
 *   The isometric sprites on a sheet of their own: the
 *   standing tanks, boats, pillboxes, bases and builder,
 *   the four wall faces and the forest trunk band.  Each
 *   lands in a fixed cell with a ring of its own edge
 *   texels around it, and the lookup turns a sprite's kind,
 *   team and frame into that cell's rect.
 *
 *   A sheet of its own rather than rows on the classic
 *   496x176 layout because that layout is what skins are
 *   authored against and what the map editor and log
 *   viewer read; its addresses do not move.  The cells here
 *   are 32x32 at density 1 (32x16 for a face or the band),
 *   twice a classic tile, since a standing sprite covers the
 *   ground diamond and the height above it.
 *
 *   Every sprite is read by name through the chain the
 *   classic sheet uses, so a skin supplies isometric art the
 *   same way it supplies anything else.  A face or the band
 *   that no file supplies is made from the matching classic
 *   sprite; a tank, pillbox, base or builder that none
 *   supplies stays empty and reports absent, and the drawer
 *   draws the classic upright sprite in its place.
 *********************************************************/

#ifndef ISO_SHEET_H
#define ISO_SHEET_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SkinSource;

/* What a cell holds. The tanks, pillboxes, bases and builder are standing
   sprites: the ground diamond sits in the bottom half of the cell and the
   height rises into the top half. The faces are textures for a wall quad
   and the band a texture for the trunks under a forest's canopy. */
typedef enum IsoSpriteKind {
    ISO_SPRITE_TANK = 0,          /* 16 facings; self, good or evil */
    ISO_SPRITE_TANK_BOAT,         /* the same tank on a boat */
    ISO_SPRITE_PILLBOX,           /* 16 armour states, 15 intact and 0 empty;
                                     good or evil */
    ISO_SPRITE_BASE,              /* good, evil or neutral */
    ISO_SPRITE_LGM,               /* 3 walking frames */
    ISO_SPRITE_LGM_HELICOPTER,    /* the builder on his way back */
    ISO_SPRITE_FACE_BUILDING,
    ISO_SPRITE_FACE_SHOT_BUILDING,
    ISO_SPRITE_FACE_PILL,
    ISO_SPRITE_FACE_BASE,
    ISO_SPRITE_FOREST_BAND,
    ISO_SPRITE_KIND_COUNT
} IsoSpriteKind;

typedef enum IsoTeam {
    ISO_TEAM_SELF = 0,
    ISO_TEAM_GOOD,
    ISO_TEAM_EVIL,
    ISO_TEAM_NEUTRAL,
    ISO_TEAM_COUNT
} IsoTeam;

/* Cell sizes at density 1, matching what tools/gen_iso_sprites.py writes: a
   standing cell is 32x32 with the ground diamond in its bottom 16 rows; a
   face and the band are 32x16. */
#define ISO_SHEET_CELL_W   32
#define ISO_SHEET_CELL_H   32
#define ISO_SHEET_FACE_H   16

/* Texels of a cell's own edge repeated around it, for the reason
   sprite_atlas.h gives: a filtered sampler reads past the rect it is handed,
   and on a sheet packed edge to edge it finds the next sprite there. */
#define ISO_SHEET_GUTTER   1

/* How many cells the sheet holds: 96 tanks, 32 pillboxes, 3 bases, 4 of the
   builder, 4 faces and the band. */
#define ISO_SHEET_SLOT_COUNT 140

/* One cell of the sheet, as the build and the tests enumerate them. */
typedef struct IsoSheetSlot {
    const char   *name;    /* the file name without its extension */
    IsoSpriteKind kind;
    IsoTeam       team;    /* ISO_TEAM_SELF for a kind with no team */
    int           frame;   /* 0 for a kind with one frame */
    int           w, h;    /* the cell at density 1 */
} IsoSheetSlot;

/*********************************************************
 * NAME:          isoSheetSlotCount / isoSheetSlotAt
 * PURPOSE:
 *   The cells in sheet order, so a caller can walk every
 *   one of them. NULL for an index out of range.
 *********************************************************/
int                 isoSheetSlotCount(void);
const IsoSheetSlot *isoSheetSlotAt(int index);

/*********************************************************
 * NAME:          isoSheetSlotIndex
 * PURPOSE:
 *   The sheet index of a kind, team and frame, or -1 when
 *   the three do not name a cell: a neutral tank, a fourth
 *   builder frame, a team on a face.
 *********************************************************/
int isoSheetSlotIndex(IsoSpriteKind kind, IsoTeam team, int frame);

/*********************************************************
 * NAME:          isoSheetSize
 * PURPOSE:
 *   The surface isoSheetBuild makes at a density, in
 *   texels.  Arithmetic on the layout; no file is read.
 *********************************************************/
void isoSheetSize(int density, int *w, int *h);

/*********************************************************
 * NAME:          isoSheetRect
 * PURPOSE:
 *   The rect of a cell on a sheet built at `density`,
 *   gutter excluded, and whether the last isoSheetBuild
 *   put a sprite in it.  False, with neither written, when
 *   kind, team and frame name no cell.  The rect is
 *   arithmetic on the layout and holds for any build at
 *   that density; `present` describes the most recent one,
 *   which is the sheet the texture was made from.
 *********************************************************/
bool isoSheetRect(IsoSpriteKind kind, IsoTeam team, int frame, int density,
                  SDL_Rect *rect, bool *present);

/*********************************************************
 * NAME:          isoSheetBuild
 * PURPOSE:
 *   Builds the sheet at `density` (the multiple of the
 *   density 1 cell, the classic sheet's scale) under a
 *   TILE_DETAIL_* mode, reading every sprite through
 *   tileLoaderLoadSprite against `skin` and data/svg.  A
 *   face or the band with no file is made from the bottom
 *   rows of its classic sprite, stretched to the cell and
 *   darkened; every other empty cell is left transparent
 *   and reports absent from isoSheetRect.  Each filled cell
 *   has its transparent texels coloured by
 *   tileLoaderBleedEdges and its ring filled from its edge.
 *   Returns an RGBA32 surface the caller owns, NULL on
 *   failure.  A NULL skin builds the stock sheet.
 *********************************************************/
SDL_Surface *isoSheetBuild(struct SkinSource *skin, int density, int mode);

/*********************************************************
 * NAME:          isoSheetBuildFrom
 * PURPOSE:
 *   isoSheetBuild with data/ looked for under `basePath`
 *   instead of SDL_GetBasePath().  For a caller that keeps
 *   its art elsewhere, which the unit tests do to build a
 *   sheet with files missing.  NULL is the default.
 *********************************************************/
SDL_Surface *isoSheetBuildFrom(struct SkinSource *skin, int density, int mode,
                               const char *basePath);

#ifdef __cplusplus
}
#endif

#endif /* ISO_SHEET_H */
