/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_text.h
 * Purpose:
 *   Text rasterization for the map editor. Converts text
 *   into terrain tiles using a built-in bitmap font.
 *********************************************************/

#ifndef MAPEDITOR_TEXT_H
#define MAPEDITOR_TEXT_H

#include <stdbool.h>
#include "global.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TEXT_SIZE_SMALL   0
#define TEXT_SIZE_MEDIUM  1
#define TEXT_SIZE_LARGE   2
#define TEXT_SIZE_XL      3
#define TEXT_SIZE_COUNT   4

#define TEXT_STYLE_REGULAR     0
#define TEXT_STYLE_BOLD        1
#define TEXT_STYLE_ITALIC      2
#define TEXT_STYLE_BOLD_ITALIC 3
#define TEXT_STYLE_COUNT       4

typedef struct {
    char text[1024];
    int fontSize;         /* TEXT_SIZE_* */
    int style;            /* TEXT_STYLE_* */
    BYTE textTerrain;     /* terrain for letter fill */
    BYTE edgeTerrain;     /* terrain for 1-tile outline, 0xFF = none */
    BYTE bgTerrain;       /* terrain for background, 0xFF = none/transparent */
} TextConfig;

TextConfig textDefaultConfig(void);

/* Rasterize text into a terrain buffer.
 * outTerrain: [256][256] buffer — only (0,0) to (outW-1, outH-1) is written.
 * Tiles with terrain 0xFF (DEEP_SEA) are "transparent" (not placed on paste).
 * outW, outH: set to bounding box dimensions of the result.
 * Returns false if text is empty or result exceeds 256x256. */
bool textRasterize(const TextConfig *cfg,
                   BYTE outTerrain[256][256],
                   int *outW, int *outH);

/* Rasterize text using a TTF font file (desktop only).
 * fontPath: path to .ttf/.otf file (should already be the correct style variant)
 * pixelHeight: font size in pixels (= tiles)
 * Other params same as textRasterize(). */
bool textRasterizeTTF(const char *fontPath, int pixelHeight,
                      const char *text,
                      BYTE textTerrain, BYTE edgeTerrain, BYTE bgTerrain,
                      BYTE outTerrain[256][256], int *outW, int *outH);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_TEXT_H */
