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
 * Name:          imgui_mapchooser.h
 * Purpose:       Reusable map chooser widget for ImGui.
 *                Shows built-in maps in a list on the left
 *                with a minimap preview on the right.
 *                Usable in game setup, lobby voting, etc.
 *********************************************************/

#ifndef IMGUI_MAPCHOOSER_H
#define IMGUI_MAPCHOOSER_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include "../../../bolo/global.h"
#include "../../../mapeditor/mapeditor_generate.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAP_CHOOSER_MAX_MAPS 64

/* Special selectedIdx value for "Random Map" */
#define MAP_CHOOSER_IDX_RANDOM -2

typedef struct {
    char name[128];           /* Display name (without .map extension) */
    char path[FILENAME_MAX];  /* Full path (e.g. "data/maps/Foo.map") */
} MapChooserEntry;

typedef struct {
    /* Discovered maps */
    MapChooserEntry maps[MAP_CHOOSER_MAX_MAPS];
    int             numMaps;

    /* Selection state */
    int             selectedIdx;       /* -1 = custom file, 0 = Everard Island, MAP_CHOOSER_IDX_RANDOM = random */
    char            selectedPath[FILENAME_MAX]; /* Path of selected map ("" = inbuilt, "randommap:..." = random) */
    char            selectedName[128]; /* Display name of selected map */

    /* Preview texture */
    SDL_Texture    *previewTex;
    int             previewBoundsMinX, previewBoundsMinY;
    int             previewBoundsMaxX, previewBoundsMaxY;

    /* Map statistics (populated by preview build) */
    int             previewPills;
    int             previewBases;
    int             previewStarts;

    /* File dialog state */
    bool            fileDialogPending;
    bool            fileDialogGotResult;
    char            fileDialogResult[FILENAME_MAX];

    /* Random map generation state */
    bool            randomMapSelected;
    MapGenConfig    genConfig;
    bool            genConfigInit;
    char            genSeedBuf[64];

    /* Compressed map data for popup preview (random/inbuilt maps without file paths) */
    BYTE           *compressedData;
    int             compressedLen;

    bool            initialized;
} MapChooserState;

/* Initialize the map chooser state. Discovers available maps.
 * Call once before rendering. */
void mapChooserInit(MapChooserState *state, SDL_Renderer *renderer);

/* Render the map chooser widget within the current ImGui window.
 * width/height specify the area available for the widget.
 * scale is the UI scale factor from dialogComputeScale.
 * Returns true if the selection changed this frame. */
bool mapChooserRender(MapChooserState *state, SDL_Renderer *renderer,
                      float width, float height, float scale);

/* Clean up textures and state. */
void mapChooserDestroy(MapChooserState *state);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_MAPCHOOSER_H */
