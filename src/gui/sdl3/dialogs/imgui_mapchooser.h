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
#include "global.h"
#include "../../../mapeditor/mapeditor_generate.h"
#include "../map_preview_view.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAP_CHOOSER_MAX_MAPS 64

/* Special selectedIdx value for "Random Map" */
#define MAP_CHOOSER_IDX_RANDOM -2

typedef struct {
    char name[128];           /* Display name (without .map extension for files, raw name for folders) */
    char path[FILENAME_MAX];  /* Full path (e.g. "data/maps/Foo.map" or "data/maps/Uploaded") */
    bool isFolder;            /* true: directory the user can navigate into */
    bool isParentUp;          /* true: synthetic ".." entry that pops one level */
} MapChooserEntry;

typedef struct MapChooserState_s MapChooserState;
struct MapChooserState_s {
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

    /* Currently-browsed directory (relative path, e.g. "data/maps"
     * or "data/maps/Uploaded"). Empty = root list which always pins
     * "Everard Island (inbuilt)" at the top. Folder navigation updates
     * this and re-runs discoverMaps. */
    char            currentDir[FILENAME_MAX];

    /* When true, the widget hides its "Load from device" and "Generate
     * Random Map" buttons. The lobby's map chooser surfaces those
     * features as separate tabs, so they'd be duplicated here. */
    bool            hideExtras;

    /* Optional cap on the left (map list) panel width in pixels. <=0
     * means "use the default split". Used by the lobby chooser to keep
     * the list narrow and let the preview claim the rest of the row. */
    float           leftPanelMaxW;

    /* Local case-insensitive substring filter applied to the map list.
     * Empty = show everything. Filter is purely client-side — the full
     * map list is already in memory, so we just hide non-matching rows.
     * Editable via an InputText above the list. */
    char            searchFilter[64];

    /* Interactive preview widget — same renderer as the lobby's inline
     * preview and the modal popup. Loaded with the currently-selected
     * map's data; provides wheel-zoom / drag-pan / minimap-mode
     * fall-back at deep zoom-out. */
    MapPreviewView *previewView;
    /* Last view size we asked the widget to render at — cached so the
     * destination Image stays consistent across frames. */
    int             previewLastW;
    int             previewLastH;
    /* Optional pointer to a "maximize" flag the surrounding window
     * uses. When non-NULL the chooser renders a small maximize /
     * restore icon at the top-right corner of the preview image and
     * toggles *maximizePtr on click. NULL = no maximize affordance. */
    bool           *maximizePtr;

    /* Optional directory-listing callback. When set, the chooser
     * uses this to populate state->maps for the current `relPath`
     * (relative to whatever the caller considers the root); when
     * NULL it falls back to the local SDL_GlobDirectory scan in
     * `data/maps`. Phase 3 of the in-lobby browser passes a callback
     * that routes through serverSimEnumerateMapDir so the user
     * browses the *server's* map library, not the local client's. */
    void           *listProviderCtx;
    void          (*listProvider)(MapChooserState *state,
                                  const char *relPath, void *ctx);
};

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
