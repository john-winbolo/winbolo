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
#include "../../../bolo/global.h"
#include "../../../bolo/bolo_map.h"
#include "../../../bolo/pillbox.h"
#include "../../../bolo/bases.h"
#include "../../../bolo/starts.h"
#include "../minimap_render.h"
#include "../../../mapeditor/mapeditor_generate.h"
#include "../../../mapeditor/mapeditor_maze.h"
#include "imgui_mapchooser.h"
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

static void discoverMaps(MapChooserState *state) {
    state->numMaps = 0;

    /* First entry is always "Everard Island (Inbuilt)" */
    SDL_strlcpy(state->maps[0].name, "Everard Island (Inbuilt)", sizeof(state->maps[0].name));
    state->maps[0].path[0] = '\0'; /* empty = inbuilt */
    state->numMaps = 1;

    const char *dir = "data/maps";

#if defined(__IPHONEOS__)
    /* SDL_GlobDirectory doesn't work with the iOS app bundle filesystem.
     * Use opendir/readdir directly instead. */
    {
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL && state->numMaps < MAP_CHOOSER_MAX_MAPS) {
                size_t len = strlen(ent->d_name);
                if (len <= 4 || strcasecmp(ent->d_name + len - 4, ".map") != 0) continue;
                if (SDL_strcasecmp(ent->d_name, "Everard Island.map") == 0) continue;

                MapChooserEntry *e = &state->maps[state->numMaps];
                SDL_snprintf(e->path, sizeof(e->path), "%s/%s", dir, ent->d_name);
                SDL_strlcpy(e->name, ent->d_name, sizeof(e->name));
                size_t nlen = SDL_strlen(e->name);
                if (nlen > 4 && SDL_strcasecmp(e->name + nlen - 4, ".map") == 0) {
                    e->name[nlen - 4] = '\0';
                }
                state->numMaps++;
            }
            closedir(d);
        }
    }
#else
    {
        int count = 0;
        char **list = SDL_GlobDirectory(dir, "*.map", 0, &count);
        if (list) {
            for (int i = 0; i < count && state->numMaps < MAP_CHOOSER_MAX_MAPS; i++) {
                if (SDL_strcasecmp(list[i], "Everard Island.map") == 0) continue;

                MapChooserEntry *e = &state->maps[state->numMaps];
                SDL_snprintf(e->path, sizeof(e->path), "%s/%s", dir, list[i]);
                SDL_strlcpy(e->name, list[i], sizeof(e->name));
                size_t len = SDL_strlen(e->name);
                if (len > 4 && SDL_strcasecmp(e->name + len - 4, ".map") == 0) {
                    e->name[len - 4] = '\0';
                }
                state->numMaps++;
            }
            SDL_free(list);
        }
    }
#endif

    /* Sort maps alphabetically (skip first entry which is always Everard Island) */
    if (state->numMaps > 2) {
        std::sort(&state->maps[1], &state->maps[state->numMaps],
            [](const MapChooserEntry &a, const MapChooserEntry &b) {
                return SDL_strcasecmp(a.name, b.name) < 0;
            });
    }
}

static void updatePreview(MapChooserState *state, SDL_Renderer *renderer) {
    if (state->randomMapSelected) {
        /* Random map preview is managed by generateRandomPreview */
        return;
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
}

/* Generate a random map preview from the current config */
static void generateRandomPreview(MapChooserState *state, SDL_Renderer *renderer) {
    map mp;
    pillboxes pb;
    bases bs;
    starts ss;

    mapCreate(&mp);
    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);

    /* Clear map to DEEP_SEA */
    memset((*mp).mapItem, DEEP_SEA, sizeof((*mp).mapItem));
    pb->numPills = 0;
    bs->numBases = 0;
    ss->numStarts = 0;

    /* Set region to full playable area */
    state->genConfig.x1 = MAP_MINE_EDGE_LEFT + 1; state->genConfig.y1 = MAP_MINE_EDGE_TOP + 1;
    state->genConfig.x2 = MAP_MINE_EDGE_RIGHT - 1; state->genConfig.y2 = MAP_MINE_EDGE_BOTTOM - 1;

    /* Generate */
    mapEditorGenerate(mp, bs, pb, ss, &state->genConfig);

    /* Build preview texture */
    if (state->previewTex) {
        SDL_DestroyTexture(state->previewTex);
        state->previewTex = NULL;
    }

    MinimapBounds mb;
    state->previewTex = minimapCreateTexture(renderer, mp, bs, pb, ss, &mb, 0);
    state->previewBoundsMinX = mb.minX;
    state->previewBoundsMinY = mb.minY;
    state->previewBoundsMaxX = mb.maxX;
    state->previewBoundsMaxY = mb.maxY;
    state->previewPills = pb->numPills;
    state->previewBases = bs->numBases;
    state->previewStarts = ss->numStarts;

    /* Update seed display */
    mapGenConfigToSeed(&state->genConfig, state->genSeedBuf, sizeof(state->genSeedBuf));

    /* Store the seed as the selected path for the caller */
    SDL_snprintf(state->selectedPath, FILENAME_MAX, "randommap:%s", state->genSeedBuf);

    mapDestroy(&mp);
    pillsDestroy(&pb);
    basesDestroy(&bs);
    startsDestroy(&ss);
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
    SDL_strlcpy(state->selectedName, "Everard Island (Inbuilt)", sizeof(state->selectedName));
    state->fileDialogPending = false;
    state->fileDialogGotResult = false;
    state->randomMapSelected = false;
    state->genConfigInit = false;

    /* Ensure the shared generate controls can load lock icons */
    mapEditorImguiSetRenderer(renderer);

    discoverMaps(state);
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

    /* Layout: left panel + right preview */
    float previewPanelW = (PREVIEW_SIZE + 40) * (scale > 1.5f ? 0.75f : 1.0f);
    if (previewPanelW > width * 0.55f) previewPanelW = width * 0.55f;
    float leftPanelW = width - previewPanelW - 8.0f;

    /* Left panel */
    ImGui::BeginChild("##MapLeft", ImVec2(leftPanelW, height), ImGuiChildFlags_Borders);

    if (state->randomMapSelected) {
        /* --- Random map generator mode --- */

        /* Back button */
        if (ImGui::Button("Load a Map", ImVec2(-1, 0))) {
            state->randomMapSelected = false;
            /* Restore the previously selected map preview */
            updatePreview(state, renderer);
            changed = true;
        }

        ImGui::Separator();

        /* Scrollable generator options */
        ImGui::BeginChild("##GenOptions", ImVec2(0, 0));

        MapGenConfig *cfg = &state->genConfig;

        if (mapGenImguiControls(cfg)) {
            generateRandomPreview(state, renderer);
            changed = true;
        }

        ImGui::EndChild(); /* ##GenOptions */
    } else {
        /* --- Normal map list mode --- */

        /* Load from device button */
        {
            bool disabled = state->fileDialogPending;
            if (disabled) ImGui::BeginDisabled();
            if (ImGui::Button("Load from Device...", ImVec2(-1, 0))) {
                SDL_Window *window = sdl3DrawGetWindow();
                SDL_DialogFileFilter filters[] = {
                    { "Map Files", "map" },
                    { "All Files", "*" },
                };
                state->fileDialogPending = true;
                state->fileDialogGotResult = false;
                SDL_ShowOpenFileDialog(mapChooserFileDialogCallback, state, window, filters, 2, NULL, false);
            }
            if (disabled) ImGui::EndDisabled();
        }

        /* Generate Random Map button */
        if (ImGui::Button("Generate Random Map", ImVec2(-1, 0))) {
            state->randomMapSelected = true;
            state->selectedIdx = MAP_CHOOSER_IDX_RANDOM;
            SDL_strlcpy(state->selectedName, "Random Map", sizeof(state->selectedName));
            initGenConfig(state);
            generateRandomPreview(state, renderer);
            changed = true;
        }

        ImGui::Separator();

        /* Scrollable map list */
        ImGui::BeginChild("##MapListScroll", ImVec2(0, 0));

        for (int i = 0; i < state->numMaps; i++) {
            bool selected = (i == state->selectedIdx);
            if (ImGui::Selectable(state->maps[i].name, selected)) {
                if (state->selectedIdx != i) {
                    state->selectedIdx = i;
                    SDL_strlcpy(state->selectedPath, state->maps[i].path, FILENAME_MAX);
                    SDL_strlcpy(state->selectedName, state->maps[i].name, sizeof(state->selectedName));
                    updatePreview(state, renderer);
                    changed = true;
                }
            }
        }
        ImGui::EndChild(); /* ##MapListScroll */
    }

    ImGui::EndChild(); /* ##MapLeft */

    ImGui::SameLine(0, 8.0f);

    /* Right panel: preview + stats */
    ImGui::BeginChild("##MapPreview", ImVec2(previewPanelW, height),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);

    ImGui::Text("%s", state->selectedName);
    ImGui::Separator();

    if (!renderPreviewImage(state)) {
        if (state->randomMapSelected) {
            ImGui::TextDisabled("Click Generate to preview");
        } else {
            ImGui::TextDisabled("No preview available");
        }
    }

    if (state->previewTex) {
        ImGui::Text("Pillboxes: %d  Bases: %d  Starts: %d",
                     state->previewPills, state->previewBases, state->previewStarts);
    }

    ImGui::EndChild(); /* ##MapPreview */

    return changed;
}

void mapChooserDestroy(MapChooserState *state) {
    if (state->previewTex) {
        SDL_DestroyTexture(state->previewTex);
        state->previewTex = NULL;
    }
    state->initialized = false;
}
