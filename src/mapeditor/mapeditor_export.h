/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_export.h
 * Purpose:
 *   Export the current map as a PNG image — either
 *   full-resolution tile render or compact minimap preview.
 *********************************************************/

#ifndef MAPEDITOR_EXPORT_H
#define MAPEDITOR_EXPORT_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include "global.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ME_EXPORT_FULL     0   /* tile-atlas render, 16px per tile = 4096x4096 */
#define ME_EXPORT_PREVIEW  1   /* minimap-style, 1 or 2 px per tile */

#define ME_PREVIEW_256   0
#define ME_PREVIEW_512   1

typedef struct {
    int mode;              /* ME_EXPORT_FULL or ME_EXPORT_PREVIEW */
    int previewSize;       /* ME_PREVIEW_256 or ME_PREVIEW_512 */
    bool showObjects;      /* render bases/pills/starts */
    bool showMines;        /* render mine overlays */
    bool showGrid;         /* render tile grid lines (full mode only) */
} ExportConfig;

/* Export the map as a PNG file.
 * Returns true on success. */
bool mapExportPNG(const char *filePath, ExportConfig *cfg,
                  SDL_Renderer *renderer, SDL_Texture *tilesTex, int tileSize,
                  map mp, bases bs, pillboxes pb, starts ss);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_EXPORT_H */
