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
 * Name:          imgui_mapchooser.cpp
 * Purpose:       Reusable map chooser widget for ImGui.
 *                Discovers maps in data/maps/, shows a
 *                scrollable list with minimap preview.
 *                Includes "Random Map" option with inline
 *                generator controls.
 *********************************************************/

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <algorithm>
#if defined(__IPHONEOS__)
#include <dirent.h>
#endif

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_dialog_utils.h"

extern "C" {
#include "../sdl3draw.h"
#include "global.h"
#include "../minimap_render.h"
#include "../map_preview_popup.h"
#include "../../../mapeditor/mapeditor_generate.h"
#include "../../../mapeditor/mapeditor_maze.h"
#include "imgui_mapchooser.h"
#include "../../lang.h"
}
#include "../../../mapeditor/mapeditor_imgui.h"

#define PREVIEW_SIZE 256

/* Build a minimap preview texture from a .map file path.
 * Delegates to the shared minimap renderer. */
static SDL_Texture *buildPreviewFromMapPath(SDL_Renderer *renderer,
                                             const char *mapPath,
                                             int *bMinX, int *bMinY, int *bMaxX, int *bMaxY,
                                             int *outPills, int *outBases, int *outStarts) {
    MinimapBounds mb;
    SDL_Texture *tex = minimapFromFile(renderer, mapPath, &mb,
                                       outPills, outBases, outStarts);
    if (tex) {
        *bMinX = mb.minX; *bMinY = mb.minY;
        *bMaxX = mb.maxX; *bMaxY = mb.maxY;
    }
    return tex;
}

/* Build preview for the embedded Everard Island map */
static SDL_Texture *buildEverardPreview(SDL_Renderer *renderer,
                                         int *bMinX, int *bMinY, int *bMaxX, int *bMaxY,
                                         int *outPills, int *outBases, int *outStarts) {
    const char *paths[] = { "Everard Island.map", "data/maps/Everard Island.map" };
    for (int i = 0; i < 2; i++) {
        SDL_Texture *tex = buildPreviewFromMapPath(renderer, paths[i],
            bMinX, bMinY, bMaxX, bMaxY, outPills, outBases, outStarts);
        if (tex) return tex;
    }
    return NULL;
}

static void SDLCALL mapChooserFileDialogCallback(void *userdata, const char *const *filelist, int filter) {
    MapChooserState *state = (MapChooserState *)userdata;
    (void)filter;
    state->fileDialogResult[0] = '\0';
    if (filelist && filelist[0]) {
        SDL_strlcpy(state->fileDialogResult, filelist[0], FILENAME_MAX);
    }
    state->fileDialogGotResult = true;
    state->fileDialogPending = false;
}

/* Helpers — check if a path is a directory using SDL3's path-info API. */
static bool pathIsDirectory(const char *path) {
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return false;
    return info.type == SDL_PATHTYPE_DIRECTORY;
}

/* Recursive walker for the local Upload-tab case (no listProvider).
 * Appends every .map file in `dir` and its subdirectories whose
 * basename contains `queryLower` (case-insensitive substring) to
 * state->maps, with `name` set to the basename and `path` set to
 * the on-disk path. Stops at MAP_CHOOSER_MAX_MAPS. Depth limited
 * so a pathological symlink loop can't pin the UI. */
static void discoverMapsRecursive(MapChooserState *state,
                                    const char *dir,
                                    const char *subRel,
                                    const char *queryLower,
                                    size_t qlen,
                                    int depth) {
    if (state->numMaps >= MAP_CHOOSER_MAX_MAPS) return;
    if (depth > 8) return;

    int count = 0;
    char **list = SDL_GlobDirectory(dir, NULL, 0, &count);
    if (!list) return;

    for (int i = 0; i < count && state->numMaps < MAP_CHOOSER_MAX_MAPS; i++) {
        const char *name = list[i];
        if (!name || name[0] == '.') continue;

        char full[FILENAME_MAX];
        SDL_snprintf(full, sizeof(full), "%s/%s", dir, name);

        SDL_PathInfo pi;
        if (!SDL_GetPathInfo(full, &pi)) continue;
        bool isDir = (pi.type == SDL_PATHTYPE_DIRECTORY);

        /* Build the entry's relative-path-from-search-root: e.g.
         * at depth 0 this is just `name`, deeper it becomes
         * "Map Test/Baringi.map" etc. Used as the display name
         * for recursive hits so duplicates in different folders
         * are visually distinct. */
        char rel[256];
        if (subRel[0] == '\0') {
            SDL_strlcpy(rel, name, sizeof(rel));
        } else {
            SDL_snprintf(rel, sizeof(rel), "%s/%s", subRel, name);
        }

        if (isDir) {
            discoverMapsRecursive(state, full, rel, queryLower,
                                  qlen, depth + 1);
            continue;
        }
        size_t nlen = SDL_strlen(name);
        if (nlen <= 4 ||
            SDL_strcasecmp(name + nlen - 4, ".map") != 0) continue;

        /* Case-insensitive substring match against basename only. */
        bool match = false;
        for (size_t k = 0; k + qlen <= nlen; k++) {
            size_t m;
            for (m = 0; m < qlen; m++) {
                char hc = name[k + m];
                if (hc >= 'A' && hc <= 'Z') hc = (char)(hc + 32);
                if (hc != queryLower[m]) break;
            }
            if (m == qlen) { match = true; break; }
        }
        if (!match) continue;

        MapChooserEntry *e = &state->maps[state->numMaps++];
        memset(e, 0, sizeof(*e));
        SDL_strlcpy(e->path, full, sizeof(e->path));
        /* Display name = relative-path-from-search-root, with the
         * trailing .map stripped — keeps the folder context visible
         * in the table so the user can tell duplicates apart. */
        SDL_strlcpy(e->name, rel, sizeof(e->name));
        e->modTime = (int64_t)pi.modify_time;
        size_t dlen = SDL_strlen(e->name);
        if (dlen > 4 &&
            SDL_strcasecmp(e->name + dlen - 4, ".map") == 0) {
            e->name[dlen - 4] = '\0';
        }
    }
    SDL_free(list);
}

/* Normalise backslashes to forward slashes in every entry's display
 * name and on-disk path, then drop duplicates whose path collapses
 * to the same string after normalisation. Some platforms surface
 * mixed-separator paths from SDL_GlobDirectory or stash native
 * separators in user-typed config — normalising at display time
 * stops "Map Test/Foo" and "Map Test\Foo" rows from appearing twice
 * for the same file. */
static void normaliseAndDedupe(MapChooserState *state) {
    for (int i = 0; i < state->numMaps; i++) {
        for (char *p = state->maps[i].name; *p; p++) {
            if (*p == '\\') *p = '/';
        }
        for (char *p = state->maps[i].path; *p; p++) {
            if (*p == '\\') *p = '/';
        }
    }
    int writeIdx = 0;
    for (int i = 0; i < state->numMaps; i++) {
        bool dup = false;
        for (int j = 0; j < writeIdx; j++) {
            /* Match on the on-disk path when it's set; otherwise
             * the name (inbuilt / synthetic entries have empty
             * path so we fall back to comparing display names). */
            const char *ap = state->maps[i].path;
            const char *bp = state->maps[j].path;
            if (ap[0] != '\0' && bp[0] != '\0') {
                if (SDL_strcasecmp(ap, bp) == 0) { dup = true; break; }
            } else if (SDL_strcasecmp(state->maps[i].name,
                                       state->maps[j].name) == 0) {
                dup = true; break;
            }
        }
        if (dup) continue;
        if (writeIdx != i) state->maps[writeIdx] = state->maps[i];
        writeIdx++;
    }
    state->numMaps = writeIdx;
}

static void discoverMaps(MapChooserState *state) {
    /* If the caller wired a list provider, defer to it entirely.
     * The provider is responsible for populating state->maps + setting
     * isParentUp / isFolder flags. The provider also handles the
     * recursive-search case: it reads state->searchRecursive +
     * state->searchFilter and routes to a recursive network call
     * when appropriate. */
    if (state->listProvider) {
        state->numMaps = 0;
        state->listProvider(state, state->currentDir,
                             state->listProviderCtx);
        normaliseAndDedupe(state);
        return;
    }

    /* Recursive-search shortcut for the local (Upload) case. Skips
     * the synthetic ".." / inbuilt Everard since results span
     * multiple folders. */
    if (state->searchRecursive && state->searchFilter[0] != '\0') {
        state->numMaps = 0;
        const char *root = (state->currentDir[0] != '\0')
                            ? state->currentDir : "data/maps";
        char qlow[64];
        size_t qlen = SDL_strlen(state->searchFilter);
        if (qlen >= sizeof(qlow)) qlen = sizeof(qlow) - 1;
        for (size_t i = 0; i < qlen; i++) {
            char c = state->searchFilter[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            qlow[i] = c;
        }
        qlow[qlen] = '\0';
        discoverMapsRecursive(state, root, "", qlow, qlen, 0);
        normaliseAndDedupe(state);
        return;
    }

    state->numMaps = 0;

    /* Source directory — explicit currentDir if set, else default. */
    const char *dir = (state->currentDir[0] != '\0')
                        ? state->currentDir
                        : "data/maps";
    const char *root = "data/maps";

    /* At the root, the first entry is always "Everard Island (Inbuilt)".
     * Inside a subfolder, the first entry is a synthetic ".." that
     * navigates back up. */
    bool inSubfolder = (SDL_strcasecmp(dir, root) != 0);
    if (inSubfolder) {
        MapChooserEntry *e = &state->maps[state->numMaps++];
        memset(e, 0, sizeof(*e));
        SDL_strlcpy(e->name, "[..]", sizeof(e->name));
        /* Parent path = strip last segment from `dir`. */
        SDL_strlcpy(e->path, dir, sizeof(e->path));
        size_t plen = SDL_strlen(e->path);
        while (plen > 0 && e->path[plen - 1] != '/' && e->path[plen - 1] != '\\') {
            e->path[--plen] = '\0';
        }
        if (plen > 0) e->path[plen - 1] = '\0'; /* drop separator */
        if (e->path[0] == '\0') {
            SDL_strlcpy(e->path, root, sizeof(e->path));
        }
        e->isFolder = true;
        e->isParentUp = true;
    } else {
        SDL_strlcpy(state->maps[0].name, langGetText(STR_MAPCHOOSER_EVERARD),
                    sizeof(state->maps[0].name));
        state->maps[0].path[0] = '\0';   /* empty = inbuilt */
        state->maps[0].isFolder   = false;
        state->maps[0].isParentUp = false;
        state->numMaps = 1;
    }

    /* Enumerate `dir`. SDL_GlobDirectory returns just basenames; for
     * each we stat to distinguish folders from .map files.  We don't
     * filter by "*.map" since folders need to be picked up too. */
    int count = 0;
    char **list = SDL_GlobDirectory(dir, NULL, 0, &count);
    if (list) {
        for (int i = 0; i < count && state->numMaps < MAP_CHOOSER_MAX_MAPS; i++) {
            const char *name = list[i];
            if (name[0] == '.') continue; /* skip dotfiles + . / .. */

            char full[FILENAME_MAX];
            SDL_snprintf(full, sizeof(full), "%s/%s", dir, name);

            /* One stat per entry: SDL_PathInfo gives us isDir AND
             * modify_time in a single syscall, so we use it instead
             * of pathIsDirectory. modify_time is SDL_Time = Sint64
             * ns since the UNIX epoch, which the table view formats
             * into a "YYYY-MM-DD HH:MM" cell. */
            SDL_PathInfo pi;
            if (!SDL_GetPathInfo(full, &pi)) continue;
            bool isDir = (pi.type == SDL_PATHTYPE_DIRECTORY);

            if (isDir) {
                MapChooserEntry *e = &state->maps[state->numMaps++];
                memset(e, 0, sizeof(*e));
                SDL_strlcpy(e->name, name, sizeof(e->name));
                SDL_strlcpy(e->path, full, sizeof(e->path));
                e->isFolder = true;
                e->modTime  = (int64_t)pi.modify_time;
                continue;
            }

            /* Files: only *.map. Skip the inbuilt Everard duplicate at root. */
            size_t nlen = SDL_strlen(name);
            if (nlen <= 4 ||
                SDL_strcasecmp(name + nlen - 4, ".map") != 0) continue;
            if (!inSubfolder &&
                SDL_strcasecmp(name, "Everard Island.map") == 0) continue;

            MapChooserEntry *e = &state->maps[state->numMaps++];
            memset(e, 0, sizeof(*e));
            SDL_strlcpy(e->path, full, sizeof(e->path));
            SDL_strlcpy(e->name, name, sizeof(e->name));
            e->modTime  = (int64_t)pi.modify_time;
            size_t dlen = SDL_strlen(e->name);
            if (dlen > 4 &&
                SDL_strcasecmp(e->name + dlen - 4, ".map") == 0) {
                e->name[dlen - 4] = '\0';
            }
        }
        SDL_free(list);
    }

    /* Sort everything past the synthetic ".." (when present)
     * alphabetically with folders FIRST so they cluster at the top. */
    int sortFrom = inSubfolder ? 1 : 0;
    if (state->numMaps - sortFrom > 1) {
        std::sort(&state->maps[sortFrom], &state->maps[state->numMaps],
            [](const MapChooserEntry &a, const MapChooserEntry &b) {
                if (a.isFolder != b.isFolder) return a.isFolder;
                return SDL_strcasecmp(a.name, b.name) < 0;
            });
    }

    /* Final pass: normalise backslashes to forward slashes in every
     * entry's name and path, then squash any duplicates the
     * normalisation reveals. Same-file rows with mixed-separator
     * paths (some platforms return them that way) now collapse to a
     * single entry. */
    normaliseAndDedupe(state);
}

static void updatePreview(MapChooserState *state, SDL_Renderer *renderer) {
    /* Close the popup if it's showing the old map */
    mapPreviewPopupClose();

    if (state->randomMapSelected) {
        /* Random map preview is managed by generateRandomPreview */
        return;
    }

    /* Clear stashed compressed data from random maps */
    if (state->compressedData) {
        SDL_free(state->compressedData);
        state->compressedData = NULL;
        state->compressedLen = 0;
    }

    if (state->previewTex) {
        SDL_DestroyTexture(state->previewTex);
        state->previewTex = NULL;
    }

    int pills = 0, bases = 0, starts = 0;

    if (state->selectedIdx == 0 || state->selectedPath[0] == '\0') {
        /* Inbuilt Everard Island */
        state->previewTex = buildEverardPreview(renderer,
            &state->previewBoundsMinX, &state->previewBoundsMinY,
            &state->previewBoundsMaxX, &state->previewBoundsMaxY,
            &pills, &bases, &starts);
    } else {
        state->previewTex = buildPreviewFromMapPath(renderer,
            state->selectedPath,
            &state->previewBoundsMinX, &state->previewBoundsMinY,
            &state->previewBoundsMaxX, &state->previewBoundsMaxY,
            &pills, &bases, &starts);
    }

    state->previewPills = pills;
    state->previewBases = bases;
    state->previewStarts = starts;

    /* Feed the same selection into the interactive widget so its
     * tile-based preview matches. Inbuilt Everard falls back to the
     * known path; everything else uses the discovered map path. */
    if (state->previewView) {
        if (state->selectedIdx == 0 || state->selectedPath[0] == '\0') {
            mapPreviewViewLoadFile(state->previewView,
                                    "data/maps/Everard Island.map");
        } else {
            mapPreviewViewLoadFile(state->previewView, state->selectedPath);
        }
        mapPreviewViewSetInitialBounds(state->previewView,
            state->previewBoundsMinX, state->previewBoundsMinY,
            state->previewBoundsMaxX, state->previewBoundsMaxY);
    }
}

/* Reentry guard for generateRandomPreview. Generation is currently
 * synchronous on the GUI thread, so this can't trip in practice —
 * but if mapGenImguiControls ever grew a callback that ran during
 * the generate call (or if generation moves off-thread later), this
 * keeps the old map texture visible and prevents a double-enter
 * corrupting the chooser state. */
static bool s_generateInFlight = false;

/* Generate a random map preview from the current config.
 * When keepCamera is true the interactive preview widget snapshots
 * its zoom/pan across the reload so tweaking a slider doesn't
 * snap the view back to auto-fit. Caller passes false on first
 * generation (no prior camera worth keeping). */
static void generateRandomPreview(MapChooserState *state, SDL_Renderer *renderer,
                                  bool keepCamera) {
    if (s_generateInFlight) return;
    s_generateInFlight = true;

    /* Close the popup if it's showing the old random map */
    mapPreviewPopupClose();

    /* Set region to full playable area */
    state->genConfig.x1 = MAP_MINE_EDGE_LEFT + 1; state->genConfig.y1 = MAP_MINE_EDGE_TOP + 1;
    state->genConfig.x2 = MAP_MINE_EDGE_RIGHT - 1; state->genConfig.y2 = MAP_MINE_EDGE_BOTTOM - 1;

    /* Generate map + compressed serialization in one call. The mapeditor
     * owns the substruct lifetimes; the returned MapPreview is heap-owned
     * and read via clientMapPreview* accessors. */
    BYTE *buf = (BYTE *)SDL_malloc(256 * 1024);
    int   compressedLen = 0;
    MapPreview *view = NULL;
    if (buf) {
        view = mapEditorGenerateAsPreview(&state->genConfig, buf, 256 * 1024, &compressedLen);
    }

    /* Build preview texture */
    if (state->previewTex) {
        SDL_DestroyTexture(state->previewTex);
        state->previewTex = NULL;
    }

    MinimapBounds mb = {0, 0, 0, 0};
    if (view) {
        state->previewTex = minimapCreateTexture(renderer, view, &mb, 0);
        state->previewPills  = clientMapPreviewGetPillCount(view);
        state->previewBases  = clientMapPreviewGetBaseCount(view);
        state->previewStarts = clientMapPreviewGetStartCount(view);
        clientMapPreviewDestroy(view);
    }
    state->previewBoundsMinX = mb.minX;
    state->previewBoundsMinY = mb.minY;
    state->previewBoundsMaxX = mb.maxX;
    state->previewBoundsMaxY = mb.maxY;

    /* Stash compressed map data for popup preview */
    if (state->compressedData) { SDL_free(state->compressedData); state->compressedData = NULL; }
    if (buf && compressedLen > 0) {
        state->compressedData = (BYTE *)SDL_realloc(buf, compressedLen);
        if (!state->compressedData) state->compressedData = buf;
        state->compressedLen = compressedLen;
    } else if (buf) {
        SDL_free(buf);
    }

    /* Mirror into the interactive preview widget. Live preview
     * rebuilds (slider tweak, lock toggle, etc.) take the
     * keep-camera path so the user's zoom/pan survives; the first
     * generation falls through to SetInitialBounds so it gets a
     * sensible auto-fit. */
    if (state->previewView && state->compressedData && state->compressedLen > 0) {
        if (keepCamera) {
            mapPreviewViewLoadCompressedKeepCamera(state->previewView,
                                                    state->compressedData,
                                                    state->compressedLen);
        } else {
            mapPreviewViewLoadCompressed(state->previewView,
                                          state->compressedData,
                                          state->compressedLen);
            mapPreviewViewSetInitialBounds(state->previewView,
                state->previewBoundsMinX, state->previewBoundsMinY,
                state->previewBoundsMaxX, state->previewBoundsMaxY);
        }
    }

    /* Update seed display */
    mapGenConfigToSeed(&state->genConfig, state->genSeedBuf, sizeof(state->genSeedBuf));

    /* Store the seed as the selected path for the caller */
    SDL_snprintf(state->selectedPath, FILENAME_MAX, "randommap:%s", state->genSeedBuf);

    s_generateInFlight = false;
}

static void initGenConfig(MapChooserState *state) {
    if (state->genConfigInit) return;
    state->genConfig = mapGenDefaultConfig(MAPGEN_TOURNAMENT);
    state->genConfig.bases = 16;
    state->genConfig.pills = 16;
    state->genConfig.starts = 16;
    state->genConfig.locks |= MAPGEN_LOCK_BASES | MAPGEN_LOCK_PILLS | MAPGEN_LOCK_STARTS;
    state->genConfig.seed = (uint32_t)time(NULL) ^ ((uint32_t)SDL_GetTicks() << 16);
    if (state->genConfig.seed == 0) state->genConfig.seed = 1;
    state->genSeedBuf[0] = '\0';
    state->genConfigInit = true;
}

void mapChooserInit(MapChooserState *state, SDL_Renderer *renderer) {
    SDL_memset(state, 0, sizeof(*state));
    state->selectedIdx = 0;
    state->selectedPath[0] = '\0';
    SDL_strlcpy(state->selectedName, langGetText(STR_MAPCHOOSER_EVERARD), sizeof(state->selectedName));
    state->fileDialogPending = false;
    state->fileDialogGotResult = false;
    state->randomMapSelected = false;
    state->genConfigInit = false;

    /* Ensure the shared generate controls can load lock icons */
    mapEditorImguiSetRenderer(renderer);

    /* Interactive preview widget — same renderer as the lobby's
     * inline preview. mapChooserDestroy frees it. */
    state->previewView = mapPreviewViewCreate();
    state->previewLastW = 0;
    state->previewLastH = 0;

    discoverMaps(state);

    /* Default selection is Everard (the inbuilt map — empty path). The
     * alphabetical sort in discoverMaps may have moved it from index 0;
     * find its new position so the highlighted row matches the
     * "currently selected" name we wrote above. */
    for (int i = 0; i < state->numMaps; i++) {
        if (state->maps[i].path[0] == '\0') {
            state->selectedIdx = i;
            break;
        }
    }

    updatePreview(state, renderer);
    state->initialized = true;
}

/* Render the zoomed minimap preview and return true if displayed */
static bool renderPreviewImage(MapChooserState *state) {
    if (!state->previewTex) return false;

    int pad = 4;
    int bx0 = state->previewBoundsMinX - pad; if (bx0 < 0) bx0 = 0;
    int by0 = state->previewBoundsMinY - pad; if (by0 < 0) by0 = 0;
    int bx1 = state->previewBoundsMaxX + pad; if (bx1 >= PREVIEW_SIZE) bx1 = PREVIEW_SIZE - 1;
    int by1 = state->previewBoundsMaxY + pad; if (by1 >= PREVIEW_SIZE) by1 = PREVIEW_SIZE - 1;
    /* Make region square */
    int bw = bx1 - bx0;
    int bh = by1 - by0;
    if (bw > bh) {
        int diff = bw - bh;
        by0 -= diff / 2;
        by1 += (diff + 1) / 2;
        if (by0 < 0) { by1 -= by0; by0 = 0; }
        if (by1 >= PREVIEW_SIZE) { by0 -= (by1 - PREVIEW_SIZE + 1); by1 = PREVIEW_SIZE - 1; }
        if (by0 < 0) by0 = 0;
    } else if (bh > bw) {
        int diff = bh - bw;
        bx0 -= diff / 2;
        bx1 += (diff + 1) / 2;
        if (bx0 < 0) { bx1 -= bx0; bx0 = 0; }
        if (bx1 >= PREVIEW_SIZE) { bx0 -= (bx1 - PREVIEW_SIZE + 1); bx1 = PREVIEW_SIZE - 1; }
        if (bx0 < 0) bx0 = 0;
    }
    ImVec2 uv0((float)bx0 / PREVIEW_SIZE, (float)by0 / PREVIEW_SIZE);
    ImVec2 uv1((float)(bx1 + 1) / PREVIEW_SIZE, (float)(by1 + 1) / PREVIEW_SIZE);

    float panelWidth = ImGui::GetContentRegionAvail().x;
    /* Reserve space for one line of text + spacing below the image */
    float reserveH = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    float availH = ImGui::GetContentRegionAvail().y - reserveH;
    float previewSize = panelWidth;
    if (availH < previewSize) previewSize = availH;
    if (previewSize < 32) previewSize = 32;
    /* Snap to integer pixels to avoid sub-pixel blurriness */
    previewSize = floorf(previewSize);

    float offsetX = floorf((panelWidth - previewSize) * 0.5f);
    if (offsetX > 0) ImGui::SetCursorPosX(floorf(ImGui::GetCursorPosX() + offsetX));
    SDL_SetTextureScaleMode(state->previewTex, SDL_SCALEMODE_NEAREST);
    ImGui::Image((ImTextureID)state->previewTex, ImVec2(previewSize, previewSize), uv0, uv1);
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (ImGui::IsItemClicked()) {
        if (state->compressedData && state->compressedLen > 0) {
            /* Random map — use stashed compressed data */
            mapPreviewPopupOpenCompressed(state->compressedData, state->compressedLen,
                                          state->previewBoundsMinX, state->previewBoundsMinY,
                                          state->previewBoundsMaxX, state->previewBoundsMaxY);
        } else {
            const char *popupPath = state->selectedPath;
            if (popupPath[0] == '\0') popupPath = "data/maps/Everard Island.map";
            mapPreviewPopupOpenFile(popupPath,
                                    state->previewBoundsMinX, state->previewBoundsMinY,
                                    state->previewBoundsMaxX, state->previewBoundsMaxY);
        }
    }
    return true;
}

bool mapChooserRender(MapChooserState *state, SDL_Renderer *renderer,
                      float width, float height, float scale) {
    bool changed = false;

    /* Handle file dialog result */
    if (state->fileDialogGotResult) {
        state->fileDialogGotResult = false;
        if (state->fileDialogResult[0] != '\0') {
            state->selectedIdx = -1; /* custom file, not in list */
            state->randomMapSelected = false;
            SDL_strlcpy(state->selectedPath, state->fileDialogResult, FILENAME_MAX);

            /* Extract filename for display */
            const char *base = state->fileDialogResult;
            for (const char *p = state->fileDialogResult; *p; p++) {
                if (*p == '/' || *p == '\\') base = p + 1;
            }
            SDL_strlcpy(state->selectedName, base, sizeof(state->selectedName));
            size_t len = SDL_strlen(state->selectedName);
            if (len > 4 && SDL_strcasecmp(state->selectedName + len - 4, ".map") == 0) {
                state->selectedName[len - 4] = '\0';
            }

            updatePreview(state, renderer);
            changed = true;
        }
    }

    /* Layout: left panel (map list) + draggable splitter + preview.
     * Default split: if leftPanelMaxW > 0 the caller wants a narrow
     * list (lobby chooser); otherwise the preview keeps its natural
     * minimap-sized panel and the list claims the rest. The user's
     * splitDragOffset (persisted on state) shifts the divider after
     * the default is computed — so they can give either side more
     * room and that ratio survives until the next re-open. */
    const float kSplitterW = 6.0f;
    float leftPanelW;
    float previewPanelW;
    if (state->leftPanelMaxW > 0.0f) {
        leftPanelW = state->leftPanelMaxW;
        if (leftPanelW > width * 0.5f) leftPanelW = width * 0.5f;
        previewPanelW = width - leftPanelW - kSplitterW;
        if (previewPanelW < 100.0f) {
            previewPanelW = (PREVIEW_SIZE + 40) * (scale > 1.5f ? 0.75f : 1.0f);
            if (previewPanelW > width * 0.55f) previewPanelW = width * 0.55f;
            leftPanelW = width - previewPanelW - kSplitterW;
        }
    } else {
        previewPanelW = (PREVIEW_SIZE + 40) * (scale > 1.5f ? 0.75f : 1.0f);
        if (previewPanelW > width * 0.55f) previewPanelW = width * 0.55f;
        leftPanelW = width - previewPanelW - kSplitterW;
    }
    /* Apply the persisted drag delta and clamp both sides to a
     * minimum so the user can't drag either panel down to zero. */
    {
        const float kMinPanel = 120.0f;
        leftPanelW    += state->splitDragOffset;
        previewPanelW -= state->splitDragOffset;
        if (leftPanelW < kMinPanel) {
            float over = kMinPanel - leftPanelW;
            leftPanelW    += over;
            previewPanelW -= over;
            state->splitDragOffset += over;
        }
        if (previewPanelW < kMinPanel) {
            float over = kMinPanel - previewPanelW;
            previewPanelW += over;
            leftPanelW    -= over;
            state->splitDragOffset -= over;
        }
    }

    /* Left panel */
    ImGui::BeginChild("##MapLeft", ImVec2(leftPanelW, height), ImGuiChildFlags_Borders);

    if (state->randomMapSelected || state->randomTabOnly) {
        /* --- Random map generator mode ---
         *
         * randomTabOnly clients (lobby's Random tab) skip the
         * "Back to map list" button — they live inside a separate
         * tab and there's no list to go back to here. */
        if (!state->randomTabOnly) {
            if (ImGui::Button(langGetText(STR_MAPCHOOSER_LOADMAP), ImVec2(-1, 0))) {
                state->randomMapSelected = false;
                updatePreview(state, renderer);
                changed = true;
            }
            ImGui::Separator();
        } else if (!state->randomMapSelected) {
            /* First render in randomTabOnly mode — bootstrap so the
             * controls and preview show up immediately. */
            state->randomMapSelected = true;
            state->selectedIdx = MAP_CHOOSER_IDX_RANDOM;
            initGenConfig(state);
            generateRandomPreview(state, renderer, /*keepCamera=*/false);
            state->genSeq++;
            changed = true;
        }

        /* Scrollable generator options */
        ImGui::BeginChild("##GenOptions", ImVec2(0, 0));

        MapGenConfig *cfg = &state->genConfig;

        if (mapGenImguiControls(cfg)) {
            generateRandomPreview(state, renderer, /*keepCamera=*/true);
            state->genSeq++;
            changed = true;
        }

        ImGui::EndChild(); /* ##GenOptions */
    } else {
        /* --- Normal map list mode --- */

        if (!state->hideExtras) {
            /* Load from device button */
            {
                bool disabled = state->fileDialogPending;
                if (disabled) ImGui::BeginDisabled();
                if (ImGui::Button(langGetText(STR_MAPCHOOSER_LOADDEVICE), ImVec2(-1, 0))) {
                    SDL_Window *window = sdl3DrawGetWindow();
                    SDL_DialogFileFilter filters[] = {
                        { langGetText(STR_MAPCHOOSER_MAPFILES), "map" },
                        { langGetText(STR_MAPCHOOSER_ALLFILES), "*" },
                    };
                    state->fileDialogPending = true;
                    state->fileDialogGotResult = false;
                    SDL_ShowOpenFileDialog(mapChooserFileDialogCallback, state, window, filters, 2, NULL, false);
                }
                if (disabled) ImGui::EndDisabled();
            }

            /* Generate Random Map button — gated on the in-flight
             * flag so a reentrant call (e.g. ImGui repeated activation
             * across one frame) can't start a second generate on top
             * of an already-running one. */
            if (s_generateInFlight) ImGui::BeginDisabled();
            bool genClicked = ImGui::Button(langGetText(STR_MAPCHOOSER_GENRANDOM), ImVec2(-1, 0));
            if (s_generateInFlight) ImGui::EndDisabled();
            if (genClicked) {
                state->randomMapSelected = true;
                state->selectedIdx = MAP_CHOOSER_IDX_RANDOM;
                SDL_strlcpy(state->selectedName, langGetText(STR_MAPCHOOSER_RANDOMMAP), sizeof(state->selectedName));
                initGenConfig(state);
                generateRandomPreview(state, renderer, /*keepCamera=*/false);
                changed = true;
            }

            ImGui::Separator();
        }

        /* Path breadcrumb — shown directly above the search field so
         * the wrapping fits inside the list column. Always reads
         * "maps/<currentDir>" relative to the data/maps root; the
         * full local path lives in an optional hover tooltip
         * (state->pathTooltipPrefix), wired by the lobby's Upload
         * tab only. TextWrapped so a long folder name doesn't push
         * the search field off the edge. */
        {
            char pathLine[FILENAME_MAX + 16];
            if (state->currentDir[0] != '\0') {
                SDL_snprintf(pathLine, sizeof(pathLine), "maps/%s",
                             state->currentDir);
            } else {
                SDL_strlcpy(pathLine, "maps/", sizeof(pathLine));
            }
            ImGui::TextWrapped("%s", pathLine);
            if (state->pathTooltipPrefix[0] != '\0' &&
                ImGui::IsItemHovered()) {
                char tipBuf[FILENAME_MAX * 2];
                if (state->currentDir[0] != '\0') {
                    SDL_snprintf(tipBuf, sizeof(tipBuf), "%s/%s",
                                 state->pathTooltipPrefix,
                                 state->currentDir);
                } else {
                    SDL_strlcpy(tipBuf, state->pathTooltipPrefix,
                                sizeof(tipBuf));
                }
                ImGui::SetTooltip("%s", tipBuf);
            }
        }

        /* Search filter + recursive toggle. The text field is a
         * case-insensitive substring match on the displayed map
         * names; "Search subfolders" extends the search to the
         * entire subtree (re-runs discoverMaps so the list is
         * actually replaced, not just visually filtered).
         *
         * Recursive search hides the synthetic ".." and inbuilt
         * Everard since results may come from any depth. The user
         * untoggles or clears the search to get the navigable
         * folder view back. */
        /* Track text edits and recursive-toggle edits independently
         * so we can drive discoverMaps with the right policy:
         *
         *   - Text edit:
         *       - in recursive mode → re-run discoverMaps so the
         *         provider can fire a fresh search request.
         *       - in plain mode → the per-row client filter handles
         *         it; no rescan needed.
         *   - Recursive toggle:
         *       - ALWAYS re-run discoverMaps. Switching ON wants the
         *         server's recursive results; switching OFF wants the
         *         plain folder listing back — both replace
         *         state->maps wholesale. */
        bool textChanged   = false;
        bool toggleChanged = false;
        {
            char prevFilter[64];
            SDL_strlcpy(prevFilter, state->searchFilter,
                        sizeof(prevFilter));
            /* Reserve a square button slot on the right for the
             * clear-X — the InputText fills everything else. */
            float xBtnW = ImGui::GetFrameHeight();
            float gap   = ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetNextItemWidth(-(xBtnW + gap));
            ImGui::InputTextWithHint("##MapSearch", "Search...",
                                     state->searchFilter,
                                     sizeof(state->searchFilter));
            if (SDL_strcmp(prevFilter, state->searchFilter) != 0) {
                textChanged = true;
            }
            ImGui::SameLine(0.0f, gap);
            bool emptyFilter = (state->searchFilter[0] == '\0');
            if (emptyFilter) ImGui::BeginDisabled();
            if (ImGui::Button("X##MapSearchClear",
                              ImVec2(xBtnW, xBtnW))) {
                state->searchFilter[0] = '\0';
                textChanged = true;
            }
            if (emptyFilter) ImGui::EndDisabled();
            if (ImGui::IsItemHovered() && !emptyFilter) {
                ImGui::SetTooltip("Clear search");
            }
            bool prevRecursive = state->searchRecursive;
            ImGui::Checkbox("Search subfolders", &state->searchRecursive);
            if (prevRecursive != state->searchRecursive) {
                toggleChanged = true;
            }
        }
        /* Stale-state safety net: in non-recursive mode the legacy
         * enumerate populates state->maps with basenames only — no
         * path separators. If any entry's name contains "/" or "\"
         * we know state is left over from a previous recursive search
         * (the entries are relative paths like "Map Test/Foo"), and
         * a refresh is required even when neither text nor toggle
         * just changed (e.g. dialog reopened with stale state). */
        bool staleRecursive = false;
        if (!state->searchRecursive) {
            for (int i = 0; i < state->numMaps; i++) {
                if (SDL_strchr(state->maps[i].name, '/')  != NULL ||
                    SDL_strchr(state->maps[i].name, '\\') != NULL) {
                    staleRecursive = true;
                    break;
                }
            }
        }
        if (toggleChanged || staleRecursive ||
            (textChanged && state->searchRecursive) ||
            (textChanged && state->searchFilter[0] == '\0')) {
            discoverMaps(state);
        }

        /* Skip the per-row client-side filter when recursive search
         * is active — discoverMaps already filtered the entries via
         * the recursive walker (or the network search response),
         * and the display names are relative paths that don't
         * necessarily contain the query as a substring (the query
         * matched the BASENAME, not the parent dir). */
        bool hasFilter = state->searchFilter[0] != '\0'
                      && !state->searchRecursive;
        ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg
                                   | ImGuiTableFlags_ScrollY
                                   | ImGuiTableFlags_Resizable
                                   | ImGuiTableFlags_Sortable
                                   | ImGuiTableFlags_SortTristate
                                   | ImGuiTableFlags_BordersInnerH
                                   | ImGuiTableFlags_BordersInnerV;
        if (ImGui::BeginTable("##MapTable", 2, tableFlags)) {
            ImGui::TableSetupColumn("Name",
                ImGuiTableColumnFlags_WidthStretch
                | ImGuiTableColumnFlags_PreferSortAscending, 1.0f,
                0 /* user_id 0 = name column */);
            /* "YYYY-MM-DD HH:MM" plus a few pixels of padding — at the
             * default font size 130 px clears the trailing minutes
             * with a touch of breathing room so nothing clips. */
            ImGui::TableSetupColumn("Modified",
                ImGuiTableColumnFlags_WidthFixed
                | ImGuiTableColumnFlags_PreferSortDescending, 130.0f,
                1 /* user_id 1 = modified column */);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            /* Click-to-sort. ImGui's TableSortSpecs is set by the
             * header click; we re-sort state->maps when SpecsDirty
             * flips true. SortTristate lets the user click a column
             * three times to remove the sort — when no sort spec is
             * active we fall back to the natural order produced by
             * discoverMaps (folders first, alphabetical within).
             *
             * Folders are pinned to the top of any sort so navigation
             * doesn't get buried under a hundred .map files; ".." is
             * always first when present. */
            ImGuiTableSortSpecs *sortSpecs = ImGui::TableGetSortSpecs();
            if (sortSpecs && sortSpecs->SpecsDirty && state->numMaps > 1) {
                int sortCol = -1;
                bool ascending = true;
                if (sortSpecs->SpecsCount > 0) {
                    sortCol   = sortSpecs->Specs[0].ColumnUserID;
                    ascending = (sortSpecs->Specs[0].SortDirection
                                 == ImGuiSortDirection_Ascending);
                }
                /* Find the first non-parent-up index — keep ".." at
                 * row 0 if it's present. Everything from that index
                 * onward gets sorted. */
                int sortFrom = 0;
                if (state->numMaps > 0 && state->maps[0].isParentUp) {
                    sortFrom = 1;
                }
                if (sortCol >= 0 && state->numMaps - sortFrom > 1) {
                    auto cmp = [&](const MapChooserEntry &a,
                                   const MapChooserEntry &b) -> bool {
                        if (a.isFolder != b.isFolder) return a.isFolder;
                        int c;
                        if (sortCol == 1) {
                            /* Modified — int64 compare. Equal mtimes
                             * fall back to name so the order is
                             * deterministic. */
                            if (a.modTime != b.modTime) {
                                bool aFirst = a.modTime < b.modTime;
                                return ascending ? aFirst : !aFirst;
                            }
                            c = SDL_strcasecmp(a.name, b.name);
                        } else {
                            c = SDL_strcasecmp(a.name, b.name);
                        }
                        return ascending ? (c < 0) : (c > 0);
                    };
                    std::sort(&state->maps[sortFrom],
                              &state->maps[state->numMaps], cmp);
                } else if (sortCol < 0) {
                    /* Tristate "no sort" — restore the default
                     * order by re-running discoverMaps. */
                    discoverMaps(state);
                }
                sortSpecs->SpecsDirty = false;
            }

            for (int i = 0; i < state->numMaps; i++) {
                const MapChooserEntry &ent = state->maps[i];
                /* Defensive: in non-recursive mode, an entry whose
                 * name contains a path separator is a recursive
                 * remnant left over from a previous search. The
                 * stale-state detection at the top of this scope
                 * triggers a discoverMaps refresh, but in case a
                 * frame slips through with stale state, skip the
                 * row here so the user never sees a "Subdir/Foo"
                 * row in the current-folder view. */
                if (!state->searchRecursive && !ent.isParentUp) {
                    if (SDL_strchr(ent.name, '/')  != NULL ||
                        SDL_strchr(ent.name, '\\') != NULL) {
                        continue;
                    }
                }
                if (hasFilter) {
                    const char *hay = ent.name;
                    const char *needle = state->searchFilter;
                    size_t needleLen = SDL_strlen(needle);
                    bool match = false;
                    for (size_t k = 0; hay[k] != '\0'; k++) {
                        size_t j;
                        for (j = 0; j < needleLen; j++) {
                            char a = hay[k + j];
                            if (a == '\0') break;
                            if (SDL_tolower((unsigned char)a)
                                != SDL_tolower((unsigned char)needle[j])) break;
                        }
                        if (j == needleLen) { match = true; break; }
                    }
                    if (!match) continue;
                }

                ImGui::TableNextRow();

                /* Column 0 — selectable name, spans the row so a
                 * click anywhere on the line picks the entry.
                 * Folders show a "[Folder] " prefix until a proper
                 * icon lands; the synthetic parent reads as "[..]". */
                ImGui::TableSetColumnIndex(0);
                char displayBuf[160];
                if (ent.isFolder && !ent.isParentUp) {
                    SDL_snprintf(displayBuf, sizeof(displayBuf),
                                 "[Folder] %s", ent.name);
                } else {
                    SDL_strlcpy(displayBuf, ent.name, sizeof(displayBuf));
                }
                /* ImGui clips text to the cell width automatically,
                 * so a long name silently truncates and a hover
                 * tooltip with the full name kicks in below. */
                char selId[24];
                SDL_snprintf(selId, sizeof(selId), "##sel%d", i);
                bool selected = (!ent.isFolder) &&
                                (i == state->selectedIdx);
                bool clicked = ImGui::Selectable(selId, selected,
                    ImGuiSelectableFlags_SpanAllColumns);
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextUnformatted(displayBuf);
                if (ImGui::IsItemHovered()) {
                    /* Two reasons to show a tooltip:
                     *   1. The name overflowed the column and got
                     *      clipped — show the full name.
                     *   2. The entry is a search hit from a
                     *      subfolder (its `name` includes a path
                     *      separator because the recursive walker
                     *      stores the relative path under the
                     *      search root). Show the parent folder so
                     *      the user can tell duplicates apart. */
                    float textW = ImGui::CalcTextSize(displayBuf).x;
                    float cellW = ImGui::GetContentRegionAvail().x;
                    bool nameClipped   = (textW > cellW);
                    bool fromSubfolder = (SDL_strchr(ent.name, '/')  != NULL ||
                                          SDL_strchr(ent.name, '\\') != NULL);
                    if (nameClipped || fromSubfolder) {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(displayBuf);
                        if (fromSubfolder && ent.path[0] != '\0') {
                            /* Parent folder = strip the trailing
                             * filename segment from the on-disk path. */
                            char folder[FILENAME_MAX];
                            SDL_strlcpy(folder, ent.path, sizeof(folder));
                            size_t flen = SDL_strlen(folder);
                            while (flen > 0 &&
                                   folder[flen - 1] != '/' &&
                                   folder[flen - 1] != '\\') {
                                folder[--flen] = '\0';
                            }
                            if (flen > 0) folder[flen - 1] = '\0';
                            if (folder[0] != '\0') {
                                ImGui::TextDisabled("in %s", folder);
                            }
                        }
                        ImGui::EndTooltip();
                    }
                }

                /* Column 1 — formatted mtime. SDL_TimeToDateTime
                 * gives us a calendar struct in local time which we
                 * render as "YYYY-MM-DD HH:MM". A zero/unknown
                 * modTime shows a dim "—" so the column doesn't
                 * read as broken. */
                ImGui::TableSetColumnIndex(1);
                if (ent.modTime > 0) {
                    SDL_DateTime dt;
                    if (SDL_TimeToDateTime((SDL_Time)ent.modTime,
                                           &dt, true)) {
                        ImGui::Text("%04d-%02d-%02d %02d:%02d",
                                    dt.year, dt.month, dt.day,
                                    dt.hour, dt.minute);
                    } else {
                        ImGui::TextDisabled("-");
                    }
                } else {
                    ImGui::TextDisabled("-");
                }

                if (clicked) {
                    if (ent.isFolder) {
                        SDL_strlcpy(state->currentDir, ent.path,
                                    sizeof(state->currentDir));
                        discoverMaps(state);
                        state->searchFilter[0] = '\0';
                        int firstFile = -1;
                        for (int j = 0; j < state->numMaps; j++) {
                            if (!state->maps[j].isFolder) {
                                firstFile = j; break;
                            }
                        }
                        state->selectedIdx = (firstFile >= 0) ? firstFile : 0;
                        if (firstFile >= 0) {
                            SDL_strlcpy(state->selectedPath,
                                        state->maps[firstFile].path,
                                        sizeof(state->selectedPath));
                            SDL_strlcpy(state->selectedName,
                                        state->maps[firstFile].name,
                                        sizeof(state->selectedName));
                        } else {
                            state->selectedPath[0] = '\0';
                            state->selectedName[0] = '\0';
                        }
                        updatePreview(state, renderer);
                        changed = true;
                    } else if (state->selectedIdx != i) {
                        state->selectedIdx = i;
                        SDL_strlcpy(state->selectedPath, ent.path,
                                    FILENAME_MAX);
                        SDL_strlcpy(state->selectedName, ent.name,
                                    sizeof(state->selectedName));
                        updatePreview(state, renderer);
                        changed = true;
                    }
                }
            }
            ImGui::EndTable();
        }
    }

    ImGui::EndChild(); /* ##MapLeft */

    /* Draggable splitter between the list and the preview. We render
     * an InvisibleButton at the column boundary, set a horizontal-
     * resize cursor on hover, and feed the delta-x from any drag
     * back into splitDragOffset (clamped above so neither panel
     * collapses). A faint vertical line draws the visual handle —
     * ImGui's default theme doesn't render the InvisibleButton's
     * background. */
    ImGui::SameLine(0, 0.0f);
    {
        ImVec2 splPos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##MapChooserSplit",
                               ImVec2(kSplitterW, height));
        bool hovered = ImGui::IsItemHovered();
        bool active  = ImGui::IsItemActive();
        if (hovered || active) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (active) {
            state->splitDragOffset += ImGui::GetIO().MouseDelta.x;
        }
        ImU32 col = active   ? IM_COL32(180, 180, 220, 200) :
                    hovered  ? IM_COL32(140, 140, 170, 200) :
                                IM_COL32( 80,  80, 100, 160);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float cx = splPos.x + kSplitterW * 0.5f;
        dl->AddLine(ImVec2(cx, splPos.y + 2.0f),
                    ImVec2(cx, splPos.y + height - 2.0f),
                    col, 1.5f);
    }
    ImGui::SameLine(0, 0.0f);

    /* Right panel: preview + stats */
    ImGui::BeginChild("##MapPreview", ImVec2(previewPanelW, height),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);

    ImGui::Text("%s", state->selectedName);
    ImGui::Separator();

    /* Interactive preview — same widget as the lobby's inline view.
     * Wheel zoom, drag pan, deep zoom-out falls back to minimap
     * colours.  Falls through to the static thumbnail (and the
     * "no preview" placeholder) if the widget isn't ready yet.
     *
     * RenderOffscreen lazily parses the source data and flips the
     * IsReady flag — so we call it unconditionally and then check
     * for a valid texture, not the other way around. */
    bool previewShown = false;
    if (state->previewView) {
        float availW = ImGui::GetContentRegionAvail().x;
        float availH = ImGui::GetContentRegionAvail().y
                     - ImGui::GetTextLineHeightWithSpacing() * 1.5f;
        if (availW < 64.0f) availW = 64.0f;
        if (availH < 64.0f) availH = 64.0f;
        /* mapChooserRender is invoked inside the ImGui frame so we
         * swap SDL render target mid-frame — same pattern the
         * chooser already uses for buildPreviewFromMapPath. */
        mapPreviewViewRenderOffscreen(state->previewView, renderer,
                                       (int)availW, (int)availH);
        state->previewLastW = (int)availW;
        state->previewLastH = (int)availH;
        SDL_Texture *tex = mapPreviewViewGetTexture(state->previewView);
        if (tex && mapPreviewViewIsReady(state->previewView)) {
            /* Draw the texture, then overlay an InvisibleButton sized
             * the same way to *claim* the click/drag for this item.
             * Without that, ImGui::Image is non-interactive — a
             * drag-pan on it would bubble up to the parent window,
             * which is now movable, and end up dragging the chooser
             * window itself instead of panning the map. */
            ImVec2 imgPos = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)tex, ImVec2(availW, availH));
            ImGui::SetCursorScreenPos(imgPos);
            /* Mark the upcoming InvisibleButton as allow-overlap so the
             * maximize icon we draw afterwards (covering a small corner
             * of the same area) can claim its own clicks. Without this,
             * the InvisibleButton — submitted first and covering the
             * whole image — wins hit-testing across the entire region
             * and the maximize button never fires. */
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##MapPreviewDrag",
                                    ImVec2(availW, availH));
            bool hovered = ImGui::IsItemHovered();
            MapPreviewInputOpts opts = { true, true, true, false };
            mapPreviewViewHandleInput(state->previewView, hovered, &opts);

            /* Zoom-level indicator — bottom-right corner of the
             * preview image. Drawn as an overlay so it doesn't
             * disturb the layout. */
            {
                char zoomText[16];
                SDL_snprintf(zoomText, sizeof(zoomText), "%.2fx",
                             mapPreviewViewGetZoom(state->previewView));
                ImVec2 textSize = ImGui::CalcTextSize(zoomText);
                float pad = 6.0f;
                ImVec2 textPos(imgPos.x + availW - textSize.x - pad,
                               imgPos.y + availH - textSize.y - pad);
                ImDrawList *dl = ImGui::GetWindowDrawList();
                ImVec2 bgMin(textPos.x - 4.0f, textPos.y - 2.0f);
                ImVec2 bgMax(textPos.x + textSize.x + 4.0f,
                             textPos.y + textSize.y + 2.0f);
                dl->AddRectFilled(bgMin, bgMax,
                                  IM_COL32(0, 0, 0, 160), 4.0f);
                dl->AddText(textPos,
                            IM_COL32(255, 255, 255, 220), zoomText);
            }

            /* Maximize / restore toggle — top-right corner of the
             * preview image. Only rendered if the surrounding window
             * wired up a maximize flag. Translucent black backing so
             * it stays legible over any map terrain. */
            if (state->maximizePtr) {
                bool maxed = *state->maximizePtr;
                float btnSz = 24.0f;
                ImVec2 btnPos(imgPos.x + availW - btnSz - 6.0f,
                              imgPos.y + 6.0f);
                ImVec2 saved = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos(btnPos);
                ImGui::PushStyleColor(ImGuiCol_Button,
                    ImVec4(0.0f, 0.0f, 0.0f, 0.55f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                    ImVec4(0.2f, 0.2f, 0.2f, 0.80f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(1.0f, 1.0f, 1.0f, 0.9f));
                const char *label = maxed
                    ? "\xe2\xa4\xa6"   /* ⤦ : restore */
                    : "\xe2\xa4\xa2";  /* ⤢ : maximize */
                if (ImGui::Button(label, ImVec2(btnSz, btnSz))) {
                    *state->maximizePtr = !maxed;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(maxed
                        ? "Restore default size (Esc)"
                        : "Maximize");
                }
                ImGui::PopStyleColor(3);
                ImGui::SetCursorScreenPos(saved);
                /* Submit a zero-size dummy so ImGui re-anchors the
                 * "last item" rect to the restored cursor position
                 * — silences the SetCursorScreenPos warning about
                 * extending boundaries without an item. */
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
            }

            previewShown = true;
        }
    }
    if (!previewShown) {
        if (!renderPreviewImage(state)) {
            if (state->randomMapSelected) {
                ImGui::TextDisabled("%s", langGetText(STR_MAPCHOOSER_CLICKGEN));
            } else {
                ImGui::TextDisabled("%s", langGetText(STR_MAPCHOOSER_NOPREVIEW));
            }
        }
    }

    if (state->previewTex ||
        (state->previewView && mapPreviewViewIsReady(state->previewView))) {
        MessageArgs args = {};
        args.number = state->previewPills;
        args.number2 = state->previewBases;
        args.number3 = state->previewStarts;
        ImGui::TextUnformatted(langGetTextFmt(STR_MAPCHOOSER_STATS, &args));
    }

    ImGui::EndChild(); /* ##MapPreview */

    return changed;
}

void mapChooserDestroy(MapChooserState *state) {
    if (state->previewTex) {
        SDL_DestroyTexture(state->previewTex);
        state->previewTex = NULL;
    }
    if (state->compressedData) {
        SDL_free(state->compressedData);
        state->compressedData = NULL;
        state->compressedLen = 0;
    }
    if (state->previewView) {
        mapPreviewViewDestroy(state->previewView);
        state->previewView = NULL;
    }
    state->initialized = false;
}
