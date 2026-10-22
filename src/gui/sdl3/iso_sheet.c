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
 * Name:          iso_sheet.c
 * Purpose:
 *   Builds the isometric sprite sheet and answers where
 *   each sprite is on it.  iso_sheet.h has the layout.
 *********************************************************/

#include "iso_sheet.h"

#include "tileloader.h"
#include "../../common/wb_log.h"

#include <string.h>

/* The grid: this many cells across, standing cells in the rows above and
   the half-height faces and band on one row below them.  Twelve rather than
   sixteen so the sheet stays under 4096 texels on both axes at density 8,
   the finest a skin can serve, which is the smallest texture ceiling a GPU
   this client runs on still has. */
#define ISO_SHEET_COLS 12

#define ISO_TANK_FRAMES    16
#define ISO_PILL_FRAMES    16
#define ISO_LGM_FRAMES      3

/* The standing cells come first, so the half-height row has nothing under
   it to line up with. */
#define ISO_STANDING_COUNT (ISO_TANK_FRAMES * 3 * 2 + ISO_PILL_FRAMES * 2 + 3 + \
                            ISO_LGM_FRAMES + 1)
#define ISO_STANDING_ROWS  ((ISO_STANDING_COUNT + ISO_SHEET_COLS - 1) / ISO_SHEET_COLS)
#define ISO_HALF_COUNT     (ISO_SHEET_SLOT_COUNT - ISO_STANDING_COUNT)

/* How much of a classic sprite a generated face or band is cut from, in
   rows of the density 1 sprite, and how much darker it is drawn: the roof
   above it is the classic sprite at full brightness and the face is the
   side in shadow. */
#define ISO_FACE_SOURCE_ROWS   4
#define ISO_BAND_SOURCE_ROWS   8
#define ISO_FACE_SHADE_PERCENT 60

/* The cells, filled on first use from the loops below so the 140 names are
   written once, by the rule, rather than listed. */
static IsoSheetSlot s_slots[ISO_SHEET_SLOT_COUNT];
static char         s_names[ISO_SHEET_SLOT_COUNT][40];
static bool         s_slotsReady = false;

/* What the last build put on the sheet, by slot index. */
static bool s_present[ISO_SHEET_SLOT_COUNT];

static const char *teamName(IsoTeam team) {
    switch (team) {
        case ISO_TEAM_SELF:    return "self";
        case ISO_TEAM_GOOD:    return "good";
        case ISO_TEAM_EVIL:    return "evil";
        case ISO_TEAM_NEUTRAL: return "neutral";
        default:               return "";
    }
}

static void addSlot(int *n, const char *name, IsoSpriteKind kind, IsoTeam team,
                    int frame, int h) {
    IsoSheetSlot *s;

    if (*n >= ISO_SHEET_SLOT_COUNT) return;
    s = &s_slots[*n];
    SDL_strlcpy(s_names[*n], name, sizeof(s_names[*n]));
    s->name  = s_names[*n];
    s->kind  = kind;
    s->team  = team;
    s->frame = frame;
    s->w     = ISO_SHEET_CELL_W;
    s->h     = h;
    (*n)++;
}

static void ensureSlots(void) {
    char name[40];
    int n = 0;

    if (s_slotsReady) return;

    for (int boat = 0; boat < 2; boat++) {
        for (int team = ISO_TEAM_SELF; team <= ISO_TEAM_EVIL; team++) {
            for (int f = 0; f < ISO_TANK_FRAMES; f++) {
                SDL_snprintf(name, sizeof(name), "iso_tank_%s%s_%02d",
                             teamName((IsoTeam)team), boat ? "boat" : "", f);
                addSlot(&n, name, boat ? ISO_SPRITE_TANK_BOAT : ISO_SPRITE_TANK,
                        (IsoTeam)team, f, ISO_SHEET_CELL_H);
            }
        }
    }
    for (int team = ISO_TEAM_GOOD; team <= ISO_TEAM_EVIL; team++) {
        for (int f = 0; f < ISO_PILL_FRAMES; f++) {
            SDL_snprintf(name, sizeof(name), "iso_pillbox_%s_%02d",
                         teamName((IsoTeam)team), f);
            addSlot(&n, name, ISO_SPRITE_PILLBOX, (IsoTeam)team, f,
                    ISO_SHEET_CELL_H);
        }
    }
    for (int team = ISO_TEAM_GOOD; team <= ISO_TEAM_NEUTRAL; team++) {
        SDL_snprintf(name, sizeof(name), "iso_base_%s", teamName((IsoTeam)team));
        addSlot(&n, name, ISO_SPRITE_BASE, (IsoTeam)team, 0, ISO_SHEET_CELL_H);
    }
    for (int f = 0; f < ISO_LGM_FRAMES; f++) {
        SDL_snprintf(name, sizeof(name), "iso_lgm_frame%d", f);
        addSlot(&n, name, ISO_SPRITE_LGM, ISO_TEAM_SELF, f, ISO_SHEET_CELL_H);
    }
    addSlot(&n, "iso_lgm_helicopter", ISO_SPRITE_LGM_HELICOPTER, ISO_TEAM_SELF,
            0, ISO_SHEET_CELL_H);

    addSlot(&n, "iso_face_building",      ISO_SPRITE_FACE_BUILDING,
            ISO_TEAM_SELF, 0, ISO_SHEET_FACE_H);
    addSlot(&n, "iso_face_shot_building", ISO_SPRITE_FACE_SHOT_BUILDING,
            ISO_TEAM_SELF, 0, ISO_SHEET_FACE_H);
    addSlot(&n, "iso_face_pill",          ISO_SPRITE_FACE_PILL,
            ISO_TEAM_SELF, 0, ISO_SHEET_FACE_H);
    addSlot(&n, "iso_face_base",          ISO_SPRITE_FACE_BASE,
            ISO_TEAM_SELF, 0, ISO_SHEET_FACE_H);
    addSlot(&n, "iso_forest_band",        ISO_SPRITE_FOREST_BAND,
            ISO_TEAM_SELF, 0, ISO_SHEET_FACE_H);

    s_slotsReady = true;
}

int isoSheetSlotCount(void) {
    return ISO_SHEET_SLOT_COUNT;
}

const IsoSheetSlot *isoSheetSlotAt(int index) {
    if (index < 0 || index >= ISO_SHEET_SLOT_COUNT) return NULL;
    ensureSlots();
    return &s_slots[index];
}

int isoSheetSlotIndex(IsoSpriteKind kind, IsoTeam team, int frame) {
    int base;

    switch (kind) {
        case ISO_SPRITE_TANK:
        case ISO_SPRITE_TANK_BOAT:
            if (team < ISO_TEAM_SELF || team > ISO_TEAM_EVIL) return -1;
            if (frame < 0 || frame >= ISO_TANK_FRAMES) return -1;
            base = kind == ISO_SPRITE_TANK_BOAT ? ISO_TANK_FRAMES * 3 : 0;
            return base + (int)team * ISO_TANK_FRAMES + frame;
        case ISO_SPRITE_PILLBOX:
            if (team < ISO_TEAM_GOOD || team > ISO_TEAM_EVIL) return -1;
            if (frame < 0 || frame >= ISO_PILL_FRAMES) return -1;
            base = ISO_TANK_FRAMES * 3 * 2;
            return base + ((int)team - ISO_TEAM_GOOD) * ISO_PILL_FRAMES + frame;
        case ISO_SPRITE_BASE:
            if (team < ISO_TEAM_GOOD || team > ISO_TEAM_NEUTRAL) return -1;
            if (frame != 0) return -1;
            base = ISO_TANK_FRAMES * 3 * 2 + ISO_PILL_FRAMES * 2;
            return base + ((int)team - ISO_TEAM_GOOD);
        case ISO_SPRITE_LGM:
            if (frame < 0 || frame >= ISO_LGM_FRAMES) return -1;
            base = ISO_TANK_FRAMES * 3 * 2 + ISO_PILL_FRAMES * 2 + 3;
            return base + frame;
        case ISO_SPRITE_LGM_HELICOPTER:
            if (frame != 0) return -1;
            return ISO_STANDING_COUNT - 1;
        case ISO_SPRITE_FACE_BUILDING:
        case ISO_SPRITE_FACE_SHOT_BUILDING:
        case ISO_SPRITE_FACE_PILL:
        case ISO_SPRITE_FACE_BASE:
        case ISO_SPRITE_FOREST_BAND:
            if (frame != 0) return -1;
            return ISO_STANDING_COUNT + ((int)kind - ISO_SPRITE_FACE_BUILDING);
        default:
            return -1;
    }
}

/* The cell's rect at a density, by slot index, gutter excluded. */
static void slotRect(int index, int density, SDL_Rect *rect) {
    int pitch = ISO_SHEET_CELL_W * density + 2 * ISO_SHEET_GUTTER;

    if (index < ISO_STANDING_COUNT) {
        rect->x = (index % ISO_SHEET_COLS) * pitch + ISO_SHEET_GUTTER;
        rect->y = (index / ISO_SHEET_COLS) * pitch + ISO_SHEET_GUTTER;
        rect->w = ISO_SHEET_CELL_W * density;
        rect->h = ISO_SHEET_CELL_H * density;
    } else {
        rect->x = (index - ISO_STANDING_COUNT) * pitch + ISO_SHEET_GUTTER;
        rect->y = ISO_STANDING_ROWS * pitch + ISO_SHEET_GUTTER;
        rect->w = ISO_SHEET_CELL_W * density;
        rect->h = ISO_SHEET_FACE_H * density;
    }
}

void isoSheetSize(int density, int *w, int *h) {
    int pitch;

    if (density < 1) density = 1;
    pitch = ISO_SHEET_CELL_W * density + 2 * ISO_SHEET_GUTTER;
    if (w) *w = ISO_SHEET_COLS * pitch;
    if (h) *h = ISO_STANDING_ROWS * pitch +
                ISO_SHEET_FACE_H * density + 2 * ISO_SHEET_GUTTER;
}

bool isoSheetRect(IsoSpriteKind kind, IsoTeam team, int frame, int density,
                  SDL_Rect *rect, bool *present) {
    int index = isoSheetSlotIndex(kind, team, frame);

    if (index < 0) return false;
    if (density < 1) density = 1;
    if (rect) slotRect(index, density, rect);
    if (present) *present = s_present[index];
    return true;
}

/* ------------------------------------------------------------------ */

/* Copy w x h RGBA texels into the surface at (x, y). */
static void blitCell(SDL_Surface *s, int x, int y, int w, int h,
                     const unsigned char *px) {
    for (int row = 0; row < h; row++) {
        if (y + row < 0 || y + row >= s->h) continue;
        unsigned char *dst = (unsigned char *)s->pixels +
                             (size_t)(y + row) * (size_t)s->pitch + (size_t)x * 4;
        for (int col = 0; col < w; col++) {
            if (x + col < 0 || x + col >= s->w) continue;
            SDL_memcpy(dst + (size_t)col * 4, px + ((size_t)row * (size_t)w + (size_t)col) * 4, 4);
        }
    }
}

/* Fill the ring around a cell from the cell's own edge, so a sampler
   reaching past the rect finds the sprite again.  The corners take the
   corner texel. */
static void padCellEdges(SDL_Surface *s, const SDL_Rect *cell) {
    unsigned char *px = (unsigned char *)s->pixels;
    int g = ISO_SHEET_GUTTER;

    for (int y = cell->y - g; y < cell->y + cell->h + g; y++) {
        if (y < 0 || y >= s->h) continue;
        int ey = y < cell->y ? cell->y
                             : (y >= cell->y + cell->h ? cell->y + cell->h - 1 : y);
        for (int x = cell->x - g; x < cell->x + cell->w + g; x++) {
            if (x < 0 || x >= s->w) continue;
            if (x >= cell->x && x < cell->x + cell->w &&
                y >= cell->y && y < cell->y + cell->h) {
                continue;
            }
            int ex = x < cell->x ? cell->x
                                 : (x >= cell->x + cell->w ? cell->x + cell->w - 1 : x);
            SDL_memcpy(px + (size_t)y * (size_t)s->pitch + (size_t)x * 4,
                       px + (size_t)ey * (size_t)s->pitch + (size_t)ex * 4, 4);
        }
    }
}

/* The classic sprite a generated face or band is cut from, NULL for a kind
   that is never generated. */
static const char *fallbackSource(IsoSpriteKind kind) {
    switch (kind) {
        case ISO_SPRITE_FACE_BUILDING:      return "building_solid";
        case ISO_SPRITE_FACE_SHOT_BUILDING: return "shot_building";
        case ISO_SPRITE_FACE_PILL:          return "pillbox_good_15";
        case ISO_SPRITE_FACE_BASE:          return "base_neutral";
        case ISO_SPRITE_FOREST_BAND:        return "forest";
        default:                            return NULL;
    }
}

/* Make a face or band from a classic sprite: its bottom `srcRows` rows at
   density 1, stretched to the cell with the nearest texel and the colour
   taken down to ISO_FACE_SHADE_PERCENT.  `src` is the classic sprite at
   16 x 16 times `density`; `out` is the cell, w x h. */
static void makeFaceFromClassic(const unsigned char *src, int density,
                                int srcRows, unsigned char *out, int w, int h) {
    int srcSize = 16 * density;
    int rows    = srcRows * density;
    int y0      = srcSize - rows;

    for (int y = 0; y < h; y++) {
        int sy = y0 + y * rows / h;
        if (sy >= srcSize) sy = srcSize - 1;
        for (int x = 0; x < w; x++) {
            int sx = x * srcSize / w;
            if (sx >= srcSize) sx = srcSize - 1;
            const unsigned char *sp = src + ((size_t)sy * (size_t)srcSize + (size_t)sx) * 4;
            unsigned char *dp = out + ((size_t)y * (size_t)w + (size_t)x) * 4;
            dp[0] = (unsigned char)(sp[0] * ISO_FACE_SHADE_PERCENT / 100);
            dp[1] = (unsigned char)(sp[1] * ISO_FACE_SHADE_PERCENT / 100);
            dp[2] = (unsigned char)(sp[2] * ISO_FACE_SHADE_PERCENT / 100);
            dp[3] = sp[3];
        }
    }
}

SDL_Surface *isoSheetBuildFrom(struct SkinSource *skin, int density, int mode,
                               const char *basePath) {
    int sheetW, sheetH;
    SDL_Surface *sheet;
    unsigned char *cellBuf, *classicBuf;
    int want;
    int loadedCount = 0, madeCount = 0, absentCount = 0;

    ensureSlots();
    if (density < 1) density = 1;
    isoSheetSize(density, &sheetW, &sheetH);

    /* The table describes the sheet this call returns: a failed build
       leaves nothing present, so a reader never trusts the sheet before. */
    SDL_memset(s_present, 0, sizeof(s_present));

    sheet = SDL_CreateSurface(sheetW, sheetH, SDL_PIXELFORMAT_RGBA32);
    if (sheet == NULL) {
        WB_LOG_ERROR(WB_LOG_CAT_ASSET,
                     "isoSheetBuild: SDL_CreateSurface(%dx%d) failed: %s",
                     sheetW, sheetH, SDL_GetError());
        return NULL;
    }
    SDL_memset(sheet->pixels, 0, (size_t)sheet->pitch * (size_t)sheet->h);

    cellBuf    = (unsigned char *)SDL_malloc((size_t)(ISO_SHEET_CELL_W * density) *
                                             (size_t)(ISO_SHEET_CELL_H * density) * 4);
    classicBuf = (unsigned char *)SDL_malloc((size_t)(16 * density) *
                                             (size_t)(16 * density) * 4);
    if (cellBuf == NULL || classicBuf == NULL) {
        SDL_free(cellBuf);
        SDL_free(classicBuf);
        SDL_DestroySurface(sheet);
        return NULL;
    }

    /* The density each file is wanted at.  Classic is 1, as it is for the
       classic sheet; the other modes ask for the sheet's own density, and
       tileLoaderLoadSprite settles for the nearest the skin has.  The skin's
       density scan does not cover these names, so there is no fully covered
       density to fall back to as Match to Zoom does for the classic set. */
    want = mode == TILE_DETAIL_CLASSIC ? 1 : density;

    for (int i = 0; i < ISO_SHEET_SLOT_COUNT; i++) {
        const IsoSheetSlot *slot = &s_slots[i];
        SDL_Rect cell;
        int w, h;
        bool loaded;

        slotRect(i, density, &cell);
        w = cell.w;
        h = cell.h;

        loaded = tileLoaderLoadSprite(skin, slot->name, want, w, h, cellBuf, basePath);
        if (loaded) {
            loadedCount++;
        } else {
            const char *classic = fallbackSource(slot->kind);
            if (classic != NULL &&
                tileLoaderLoadSprite(skin, classic, want, 16 * density, 16 * density,
                                     classicBuf, basePath)) {
                makeFaceFromClassic(classicBuf, density,
                                    slot->kind == ISO_SPRITE_FOREST_BAND
                                        ? ISO_BAND_SOURCE_ROWS : ISO_FACE_SOURCE_ROWS,
                                    cellBuf, w, h);
                loaded = true;
                madeCount++;
            }
        }

        s_present[i] = loaded;
        if (!loaded) {
            absentCount++;
            continue;
        }

        blitCell(sheet, cell.x, cell.y, w, h, cellBuf);
        /* The colour under the cell's transparent texels first, so the ring
           copies an edge that already blends to itself. */
        tileLoaderBleedEdges(sheet, &cell);
        padCellEdges(sheet, &cell);
    }

    WB_LOG_INFO(WB_LOG_CAT_ASSET,
                "isoSheetBuild: density=%d, sheet=%dx%d, %d sprites loaded, "
                "%d made from classic sprites, %d absent",
                density, sheetW, sheetH, loadedCount, madeCount, absentCount);

    SDL_free(cellBuf);
    SDL_free(classicBuf);
    return sheet;
}

SDL_Surface *isoSheetBuild(struct SkinSource *skin, int density, int mode) {
    return isoSheetBuildFrom(skin, density, mode, NULL);
}
