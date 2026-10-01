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
 * Name:          skin_preview.c
 * Purpose:
 *   Renders a sample of a skin's art to a PNG, at the size
 *   a Steam Workshop item's preview image wants.
 *********************************************************/

#include "skin_preview.h"

#include <SDL3/SDL.h>

#include "tileloader.h"
#include "../tiles.h"

#include "../../common/wb_log.h"

/* stb_image_write_impl.c carries the implementation for every target that
   links this file, so only the declarations are wanted here. */
#include "../../third_party/stb/stb_image_write.h"

/* One 64-pixel cell of the preview.  Four across, five down: the sixteen
   tank facings on the first four rows, then a base, a pillbox and two
   terrain tiles on the last. */
#define PREVIEW_TILE  64
#define PREVIEW_COLS  4
#define PREVIEW_ROWS  5
#define PREVIEW_W     (PREVIEW_COLS * PREVIEW_TILE)
#define PREVIEW_H     (PREVIEW_ROWS * PREVIEW_TILE)

/* A 64-pixel tile is four times the 16-pixel base the sheet layout is stated
   in, so a sprite's tiles.h position multiplies straight through. */
#define PREVIEW_SCALE (PREVIEW_TILE / TILE_SIZE_X)

/* Where each cell's art sits in a 1x sheet.  tiles.h gives these as absolute
   pixel offsets, not column and row numbers. */
typedef struct PreviewCell {
    int x;
    int y;
} PreviewCell;

static const PreviewCell kPreviewCells[PREVIEW_COLS * PREVIEW_ROWS] = {
    { TANK_SELF_0_X,  TANK_SELF_0_Y  },
    { TANK_SELF_1_X,  TANK_SELF_1_Y  },
    { TANK_SELF_2_X,  TANK_SELF_2_Y  },
    { TANK_SELF_3_X,  TANK_SELF_3_Y  },
    { TANK_SELF_4_X,  TANK_SELF_4_Y  },
    { TANK_SELF_5_X,  TANK_SELF_5_Y  },
    { TANK_SELF_6_X,  TANK_SELF_6_Y  },
    { TANK_SELF_7_X,  TANK_SELF_7_Y  },
    { TANK_SELF_8_X,  TANK_SELF_8_Y  },
    { TANK_SELF_9_X,  TANK_SELF_9_Y  },
    { TANK_SELF_10_X, TANK_SELF_10_Y },
    { TANK_SELF_11_X, TANK_SELF_11_Y },
    { TANK_SELF_12_X, TANK_SELF_12_Y },
    { TANK_SELF_13_X, TANK_SELF_13_Y },
    { TANK_SELF_14_X, TANK_SELF_14_Y },
    { TANK_SELF_15_X, TANK_SELF_15_Y },
    { BASE_GOOD_X,    BASE_GOOD_Y    },
    { PILL_GOOD15_X,  PILL_GOOD15_Y  },
    { GRASS_X,        GRASS_Y        },
    { SWAMP_X,        SWAMP_Y        }
};

bool skinWritePreviewPng(struct SkinSource *skin, const char *outPath) {
    SDL_Surface   *sheet;
    unsigned char *pixels;
    char           tmpPath[1024];
    const int      dstPitch = PREVIEW_W * 4;
    int            i;
    bool           ok;

    if (outPath == NULL || outPath[0] == '\0') return false;

    /* High Detail, so the preview shows the best art the skin carries rather
       than whatever the player's own Tile Detail setting asks for. */
    sheet = tileLoaderBuildSheetFor(skin, PREVIEW_TILE, TILE_DETAIL_HIGH);
    if (sheet == NULL) {
        WB_LOG_ERROR(WB_LOG_CAT_ASSET,
                     "skinWritePreviewPng: could not build the sheet");
        return false;
    }

    /* Cells the sheet does not reach are left transparent, so start cleared. */
    pixels = (unsigned char *)SDL_calloc(1, (size_t)dstPitch * PREVIEW_H);
    if (pixels == NULL) {
        SDL_DestroySurface(sheet);
        return false;
    }

    for (i = 0; i < PREVIEW_COLS * PREVIEW_ROWS; i++) {
        int srcX = kPreviewCells[i].x * PREVIEW_SCALE;
        int srcY = kPreviewCells[i].y * PREVIEW_SCALE;
        int dstX = (i % PREVIEW_COLS) * PREVIEW_TILE;
        int dstY = (i / PREVIEW_COLS) * PREVIEW_TILE;
        int row;

        if (srcX < 0 || srcY < 0 ||
            srcX + PREVIEW_TILE > sheet->w ||
            srcY + PREVIEW_TILE > sheet->h) {
            continue;
        }

        /* Both surfaces are SDL_PIXELFORMAT_RGBA32, which is what
           stbi_write_png wants, so each row is a straight copy.  The sheet's
           pitch is its own: SDL is free to pad a surface's rows. */
        for (row = 0; row < PREVIEW_TILE; row++) {
            const unsigned char *s =
                (const unsigned char *)sheet->pixels +
                (size_t)(srcY + row) * (size_t)sheet->pitch +
                (size_t)srcX * 4;
            unsigned char *d = pixels +
                               (size_t)(dstY + row) * (size_t)dstPitch +
                               (size_t)dstX * 4;
            SDL_memcpy(d, s, (size_t)PREVIEW_TILE * 4);
        }
    }

    /* Written beside the destination and moved into place, so a failed or
       interrupted encode never leaves a half-written PNG at outPath. */
    SDL_snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", outPath);
    ok = stbi_write_png(tmpPath, PREVIEW_W, PREVIEW_H, 4, pixels, dstPitch) != 0;
    if (ok) {
        ok = SDL_RenamePath(tmpPath, outPath);
    }
    if (!ok) {
        SDL_RemovePath(tmpPath);
        WB_LOG_ERROR(WB_LOG_CAT_ASSET,
                     "skinWritePreviewPng: could not write %s", outPath);
    }

    SDL_free(pixels);
    SDL_DestroySurface(sheet);
    return ok;
}
