/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_image.h
 * Purpose:
 *   Image import for the map editor. Loads a raster image
 *   and converts its pixels to terrain tiles via color
 *   quantization and nearest-terrain mapping.
 *********************************************************/

#ifndef MAPEDITOR_IMAGE_H
#define MAPEDITOR_IMAGE_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>
#include "../bolo/global.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Scale modes */
#define ME_SCALE_FIT_SELECTION  0
#define ME_SCALE_FIT_PLAYABLE   1
#define ME_SCALE_1TO1           2

/* Maximum color clusters */
#define ME_IMAGE_MAX_COLORS     16

/* Color-to-terrain mapping entry */
typedef struct {
    uint8_t r, g, b;       /* Representative cluster color */
    BYTE    terrain;       /* Mapped terrain type */
    bool    enabled;       /* false = skip (pixels become ME_TRANSPARENT) */
} ColorMapping;

/* Image import configuration and state */
typedef struct {
    char filePath[512];

    /* Source image (RGBA) */
    uint8_t *pixels;       /* RGBA pixel data from stbi_load(), NULL if not loaded */
    int imgW, imgH;        /* Source image dimensions */

    /* Scale mode and output dimensions */
    int scaleMode;         /* ME_SCALE_* */
    int outW, outH;        /* Computed output dimensions */

    /* Selection bounds for FIT_SELECTION mode */
    int selW, selH;        /* Selection dimensions (default 64x64 if no selection) */

    /* Color quantization */
    int numColors;         /* Target quantization colors (2-16) */
    ColorMapping mappings[ME_IMAGE_MAX_COLORS];
    int numMappings;       /* Number of active color mappings */

    /* Result terrain */
    BYTE resultTerrain[256][256];
    int  resultW, resultH;
    bool resultReady;

    /* Preview textures */
    SDL_Texture *previewTex;       /* Source image preview */
    SDL_Texture *resultPreviewTex; /* Terrain result preview */

    /* Error message (empty = no error) */
    char errorMsg[128];
} ImageImportConfig;

/* Load an image file into cfg. Creates previewTex scaled to ~200x200.
 * Frees any previously loaded image first. */
void imageImportLoad(ImageImportConfig *cfg, const char *path,
                     SDL_Renderer *renderer);

/* Run color quantization and auto-map colors to terrain types.
 * Populates cfg->mappings[] and sets cfg->numMappings.
 * Requires cfg->pixels to be loaded. */
void imageImportAutoMap(ImageImportConfig *cfg);

/* Convert loaded image to terrain using current mappings.
 * Writes to cfg->resultTerrain, sets resultReady.
 * Rebuilds cfg->resultPreviewTex. */
void imageImportConvert(ImageImportConfig *cfg, SDL_Renderer *renderer);

/* Free all image import resources (pixels, textures). Zeros the struct. */
void imageImportFree(ImageImportConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_IMAGE_H */
