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
#include "mapgen.h"
#include "../map_preview_view.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cap on entries in a single chooser view. Sized to fit WBN's
 * 250+ subfolder collections; local-filesystem tabs use far fewer. */
#define MAP_CHOOSER_MAX_MAPS 512

/* Special selectedIdx value for "Random Map" */
#define MAP_CHOOSER_IDX_RANDOM -2

typedef struct {
    char    name[128];           /* Display name (without .map extension for files, raw name for folders) */
    char    path[FILENAME_MAX];  /* Full path (e.g. "data/maps/Foo.map" or "data/maps/Uploaded") */
    bool    isFolder;            /* true: directory the user can navigate into */
    bool    isParentUp;          /* true: synthetic ".." entry that pops one level */
    int64_t modTime;             /* file mtime, ns since UNIX epoch (SDL_Time); 0 = unknown */
    char    tooltipFolder[128];  /* Optional: shown on hover as "in <tooltipFolder>".
                                  * Used by the WBN tab to surface the catalogue
                                  * folder a search result lives in, with no impact
                                  * on the row's name or path. Empty = no hint. */
    bool    highlighted;         /* Optional: provider marks the row
                                  * as "featured" / pinned. The chooser
                                  * paints a subtle background tint
                                  * so the user notices it among the
                                  * surrounding entries. */
    char    crumbsPath[FILENAME_MAX]; /* Optional: slash-separated friendly
                                       * folder path (e.g. "Collections/
                                       * Tournament Maps"). The chooser
                                       * renders each segment as its own
                                       * clickable button below the preview;
                                       * clicking a segment sets the
                                       * pendingJumpPath to the prefix up
                                       * to and including that segment. */
} MapChooserEntry;

typedef struct MapChooserState_s MapChooserState;

/* A "maps filesystem" plugged into the chooser. The three tabs in
 * the lobby (Server Maps, Upload, Winbolo.net Maps) each supply one;
 * the chooser UI is provider-agnostic and just speaks this vtable.
 *
 *   enumerate     — populate state->maps[] for `relPath`. Honours
 *                   state->searchRecursive + state->searchFilter for
 *                   the recursive-search mode. The chooser resets
 *                   state->numMaps to 0 before invoking, and runs
 *                   normaliseAndDedupe afterwards.
 *   onSelect      — called when a non-folder row is picked and the
 *                   chooser has already updated state->selectedPath /
 *                   selectedName. The provider does whatever the tab
 *                   needs (push the map to the server, queue an
 *                   upload, kick a WBN download, etc.).
 *   onFolderJump  — called when the user clicks a breadcrumb segment
 *                   under the preview. The provider resolves
 *                   `jumpPath` (in its own friendly-path scheme,
 *                   possibly prefixed with crumbsRootLabel) and sets
 *                   state->currentDir to the matching folder.
 *   refreshEveryFrame — when true, lobbyRenderMapTab re-runs
 *                   enumerate every frame instead of only on user
 *                   actions, so async cache updates surface without
 *                   the user moving the mouse. Set for the WBN
 *                   provider (network fetch lands on a worker thread).
 */
/* Per-row preview buffer. The cache hands ownership of the
 * malloc'd pixel buffer to the caller; the provider's
 * generatePreview implementation fills this in. Pixels are RGBA32,
 * row-major, width*height*4 bytes. The cache disposes of `pixels`
 * with SDL_free when it's done. */
typedef struct {
    int      w, h;
    uint8_t *pixels;
} MapPreviewPixels;

typedef struct MapFsProvider_s MapFsProvider;
struct MapFsProvider_s {
    void *ctx;
    void (*enumerate)(MapChooserState *state, const char *relPath,
                      void *ctx);
    void (*onSelect)(MapChooserState *state, void *ctx);
    void (*onFolderJump)(MapChooserState *state, const char *jumpPath,
                          void *ctx);
    /* Optional. Called on the preview-cache worker thread (NOT the
     * UI thread) when an on-screen row needs its thumbnail built and
     * the disk cache doesn't already have one for this key. Must be
     * thread-safe — no ImGui, no SDL_Renderer/SDL_Texture work.
     * `entryPath` is the entry's `path` field (e.g. "data/maps/foo.map"
     * for Upload/Server, "wbn:298" for WBN). On success, fill outBuf
     * with malloc'd (SDL_malloc) RGBA pixels and return true; the
     * cache then writes a PNG to disk and creates the texture on
     * the UI thread next frame. Return false to mark this key as
     * unavailable (the cache won't retry). */
    bool (*generatePreview)(const char *entryPath,
                            MapPreviewPixels *outBuf, void *ctx);
    /* Cache namespace identifier. The thumbnail cache mixes this in
     * with path + mtime when forming both the in-memory key and the
     * on-disk filename, so identically-named maps on different
     * sources (e.g. a "Fun Map.map" that exists in Upload, on the
     * server, AND on winbolo.net) don't cross-contaminate each
     * other's previews. Set per provider: e.g. "upload", "server",
     * "wbn". Empty/NULL collapses to the legacy global namespace. */
    const char *cacheScope;
    /* Optional. Called once per frame before the chooser renders;
     * the implementation writes state->pathTooltipPrefix (the hover
     * tooltip on the path label above the search box) to whatever
     * makes sense for the tab — Upload writes the cwd path, WBN
     * writes the host URL, Server Maps writes "" (no tooltip). */
    void (*refreshTooltipPrefix)(MapChooserState *state, void *ctx);
    /* Optional. Renders a small status line under the chooser
     * (e.g. "Upload rejected" / "Download failed"). Called after
     * mapChooserRender on every frame, inside the tab item. */
    void (*renderStatusFooter)(MapChooserState *state, void *ctx);
    /* Optional. Drains async work at the top of every frame — e.g.
     * the WBN provider uses this to poll its background download
     * worker and apply any completed bytes to the chooser preview.
     * The renderer is forwarded so anything that needs to rebuild
     * textures can do so without grabbing globals. */
    void (*tick)(MapChooserState *state, SDL_Renderer *renderer, void *ctx);
    bool  refreshEveryFrame;
};
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

    /* When true, the widget runs in "random-only" mode: the back
     * button is hidden, randomMapSelected is treated as always-on,
     * and config changes increment genSeq so external code can
     * detect them and forward the seed to the server. Used by the
     * lobby's Random tab. */
    bool            randomTabOnly;

    /* Bumps every time the user changes a generator control (i.e.
     * mapGenImguiControls reports a change and the chooser
     * regenerates the preview). External code caches the last-seen
     * value to drive a side-effect like a server-side preview. */
    uint32_t        genSeq;

    /* Optional cap on the left (map list) panel width in pixels. <=0
     * means "use the default split". Used by the lobby chooser to keep
     * the list narrow and let the preview claim the rest of the row. */
    float           leftPanelMaxW;

    /* User-applied delta to the left/right split, in pixels. The
     * chooser renders a draggable vertical bar between the list and
     * the preview; dragging accumulates into this field so the
     * user's chosen ratio survives across re-opens of the dialog.
     * 0 = default split from leftPanelMaxW (or the legacy
     * preview-natural-size fallback). */
    float           splitDragOffset;

    /* Local case-insensitive substring filter applied to the map list.
     * Empty = show everything. When searchRecursive is FALSE the
     * filter is purely client-side — applied to whatever the
     * listProvider (or the local glob) returned for the current
     * folder. When searchRecursive is TRUE the filter becomes a
     * query parameter: the chooser asks the listProvider's owner
     * to do a recursive walk and return matching entries from
     * every subfolder. */
    char            searchFilter[64];

    /* When true and searchFilter is non-empty, the chooser displays
     * matches from `currentDir` and all subfolders rather than only
     * the current folder. The "[..]" entry and inbuilt Everard are
     * hidden in this mode since results may span multiple folders.
     * Toggled by a checkbox under the search input. */
    bool            searchRecursive;

    /* When true, the chooser table shows a "Modified" column with each
     * file's mtime. Off by default to keep the list narrow; toggled by
     * a checkbox next to "Search subfolders". */
    bool            showModifiedColumn;

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

    /* Filesystem provider plugged in by the owning tab. The chooser
     * is provider-agnostic: enumerate populates maps[], onSelect
     * handles row picks, onFolderJump resolves breadcrumb clicks.
     * Must be wired before the first mapChooserRender — there is no
     * built-in fallback. */
    MapFsProvider   provider;

    /* Latched on a click of the right-anchored "go to folder" link
     * below the preview. Drained by mapChooserConsumeFolderJump.
     * Empty string = no pending click. */
    char            pendingJumpPath[FILENAME_MAX];

    /* Updated by the chooser each frame to whichever row index is
     * currently mouse-hovered (-1 = none). The WBN tab uses this
     * to kick an async info fetch for the hovered map so its
     * enclosing folder path can be displayed below the preview. */
    int             lastHoveredIdx;

    /* Persistent enclosing-path string for the right-anchored
     * breadcrumb under the preview. The chooser snapshots the
     * selected entry's crumbsPath into this field on every
     * selection change, and the renderer always reads from here
     * — so the breadcrumb survives selection state changes
     * (e.g. mapChooserSetSelectedFile flipping selectedIdx to
     * -1 once the WBN download lands and a local preview file
     * becomes the active path). */
    char            activeCrumbsPath[FILENAME_MAX];

    /* Optional full-path tooltip prefix shown when the user hovers
     * the wrapped "maps/<currentDir>" label above the search box.
     * Empty = no tooltip. The widget appends state->currentDir at
     * render time so the tooltip stays in sync with folder nav.
     * Used by the lobby's Upload tab to surface the absolute local
     * directory (something like "C:/.../winbolo/data/maps"); the
     * Server Maps tab leaves it empty so no tooltip pops. */
    char            pathTooltipPrefix[FILENAME_MAX];

    /* Optional label prepended to every breadcrumb the chooser
     * renders under the preview. e.g. "Maps" for the Server Maps
     * tab, so a row in the Uploads folder reads "Maps / Uploads"
     * instead of just "Uploads". Empty = no prefix. */
    char            crumbsRootLabel[64];

    /* List vs grid view selector.
     *   0 = list — vertical table of names; hover pops a preview
     *       tooltip on file rows (lazily generated + cached).
     *   1 = grid — flow layout of preview thumbnails with names
     *       below, packed as many across as the panel width fits.
     * The user toggles via two SVG icon buttons at the top of the
     * chooser panel. */
    int             viewMode;
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

/* Stop the shared preview-generation worker thread (no-op if not
 * running). The worker is restarted lazily on the next preview
 * request, so callers can use this freely whenever previews aren't
 * needed — when the chooser dialog closes, and on app shutdown. */
void mapChooserStopPreviewWorker(void);

/* Force-set the chooser's selectedPath/Name and rebuild the preview
 * texture. Used by the WBN tab after an async download completes —
 * map rows there are emitted with synthetic "wbn:<id>" paths so
 * the chooser's normal preview-on-click attempt fails. Once the
 * bytes are on disk this gives us a way to point the chooser at
 * the real file. No-op if state or path is NULL. */
void mapChooserSetSelectedFile(MapChooserState *state,
                                SDL_Renderer *renderer,
                                const char *path,
                                const char *displayName);

/* Pop the most recent "go to folder" click out of the chooser, if
 * any. Returns true and fills outPath when the user clicked the
 * right-anchored folder link below the preview (set up via the
 * selected entry's jumpPath field). The caller then redirects
 * navigation. False with outPath untouched when no click is
 * pending. */
bool mapChooserConsumeFolderJump(MapChooserState *state,
                                  char *outPath, size_t outPathSz);

/* Run the wired provider's enumerate for the current directory and
 * rebuild state->maps[]. The chooser already calls this on folder
 * clicks and search-state changes; the lobby's per-tab render helper
 * calls it every frame when provider.refreshEveryFrame is true so
 * async cache updates surface without user interaction. */
void mapChooserRefresh(MapChooserState *state);

/* Ready-made enumerate function for a local-filesystem provider
 * rooted at data/maps. Used by the Upload tab's provider; mirrors
 * the legacy in-chooser SDL_GlobDirectory scan. The ctx argument is
 * unused (passed through from the provider for signature
 * compatibility). */
void mapChooserLocalFsEnumerate(MapChooserState *state,
                                 const char *relPath, void *ctx);

/* Post-pass for listProviders running a recursive search. The provider
 * appends matching file rows with their enclosing folder path in
 * crumbsPath; this helper aggregates the unique folders, prepends them
 * as folder rows at the top of the list, and sets each folder row's
 * `path` to <pathPrefix>/<folder> so a click sets state->currentDir to
 * something the provider's folder-listing branch can resolve. */
void mapChooserEmitFolderRowsForSearchHits(MapChooserState *state,
                                            const char *pathPrefix);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_MAPCHOOSER_H */
