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
 *********************************************************/

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>

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
#include "imgui_mapchooser.h"
}

#define PREVIEW_SIZE 256

/* Terrain color lookup — same as imgui_lobby.cpp */
static void terrainColor(BYTE terrain, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (terrain >= MINE_START && terrain <= MINE_END) {
        terrain -= MINE_SUBTRACT;
    }
    switch (terrain) {
        case BUILDING:      *r = 128; *g = 128; *b = 128; break;
        case RIVER:         *r = 0;   *g = 80;  *b = 200; break;
        case SWAMP:         *r = 0;   *g = 100; *b = 0;   break;
        case CRATER:        *r = 139; *g = 90;  *b = 43;  break;
        case ROAD:          *r = 80;  *g = 80;  *b = 80;  break;
        case FOREST:        *r = 0;   *g = 140; *b = 0;   break;
        case RUBBLE:        *r = 180; *g = 160; *b = 130; break;
        case GRASS:         *r = 100; *g = 200; *b = 50;  break;
        case HALFBUILDING:  *r = 160; *g = 160; *b = 160; break;
        case BOAT:          *r = 0;   *g = 80;  *b = 200; break;
        case DEEP_SEA:
        default:            *r = 0;   *g = 0;   *b = 80;  break;
    }
}

/* Build a minimap preview texture from a .map file path.
 * Uses SDL_LoadFile → temp file → mapRead to work on Android APK assets.
 * Also populates bounds with the bounding box of non-sea terrain. */
static SDL_Texture *buildPreviewFromMapPath(SDL_Renderer *renderer,
                                             const char *mapPath,
                                             int *bMinX, int *bMinY, int *bMaxX, int *bMaxY,
                                             int *outPills, int *outBases, int *outStarts) {
    map mp;
    pillboxes pb;
    bases bs;
    starts ss;

    mapCreate(&mp);
    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);

    /* Try direct fopen-based mapRead first (desktop) */
    bool loaded = (mapRead((char *)mapPath, &mp, &pb, &bs, &ss) == TRUE);
    if (!loaded) {
        /* Try SDL_LoadFile (Android APK) → temp file → mapRead */
        size_t fileSize = 0;
        void *fileData = SDL_LoadFile(mapPath, &fileSize);
        if (fileData && fileSize > 0) {
            const char *tmpDir = SDL_GetPrefPath("WinBolo", "WinBolo");
            char tmpPath[512];
            SDL_snprintf(tmpPath, sizeof(tmpPath), "%s_preview_temp.map", tmpDir ? tmpDir : "");
            FILE *fp = fopen(tmpPath, "wb");
            if (fp) {
                fwrite(fileData, 1, fileSize, fp);
                fclose(fp);
                loaded = (mapRead(tmpPath, &mp, &pb, &bs, &ss) == TRUE);
                remove(tmpPath);
            }
            SDL_free(fileData);
        }
    }

    if (!loaded) {
        mapDestroy(&mp);
        pillsDestroy(&pb);
        basesDestroy(&bs);
        startsDestroy(&ss);
        return NULL;
    }

    if (outPills) *outPills = pillsGetNumPills(&pb);
    if (outBases) *outBases = basesGetNumBases(&bs);
    if (outStarts) *outStarts = startsGetNumStarts(&ss);

    uint8_t *pixels = (uint8_t *)malloc(PREVIEW_SIZE * PREVIEW_SIZE * 4);
    if (!pixels) {
        mapDestroy(&mp);
        pillsDestroy(&pb);
        basesDestroy(&bs);
        startsDestroy(&ss);
        return NULL;
    }

    int minX = PREVIEW_SIZE, minY = PREVIEW_SIZE, maxX = 0, maxY = 0;
    for (int y = 0; y < PREVIEW_SIZE; y++) {
        for (int x = 0; x < PREVIEW_SIZE; x++) {
            int idx = (y * PREVIEW_SIZE + x) * 4;
            BYTE t = mapGetPos(&mp, (BYTE)x, (BYTE)y);
            terrainColor(t, &pixels[idx], &pixels[idx+1], &pixels[idx+2]);
            pixels[idx+3] = 255;
            BYTE base = t;
            if (base >= MINE_START && base <= MINE_END) base -= MINE_SUBTRACT;
            if (base != DEEP_SEA) {
                if (x < minX) minX = x;
                if (y < minY) minY = y;
                if (x > maxX) maxX = x;
                if (y > maxY) maxY = y;
            }
        }
    }
    if (minX > maxX) {
        minX = 0; minY = 0; maxX = PREVIEW_SIZE - 1; maxY = PREVIEW_SIZE - 1;
    }

    /* Overlay pillboxes (red) */
    BYTE numPills = pillsGetNumPills(&pb);
    for (int i = 0; i < numPills; i++) {
        int px = pb->item[i].x;
        int py = pb->item[i].y;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int nx = px + dx, ny = py + dy;
                if (nx >= 0 && nx < PREVIEW_SIZE && ny >= 0 && ny < PREVIEW_SIZE) {
                    int idx = (ny * PREVIEW_SIZE + nx) * 4;
                    pixels[idx] = 255; pixels[idx+1] = 0; pixels[idx+2] = 0; pixels[idx+3] = 255;
                }
            }
        }
    }

    /* Overlay bases (white) */
    BYTE numBases = basesGetNumBases(&bs);
    for (int i = 0; i < numBases; i++) {
        int bx = bs->item[i].x;
        int by = bs->item[i].y;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int nx = bx + dx, ny = by + dy;
                if (nx >= 0 && nx < PREVIEW_SIZE && ny >= 0 && ny < PREVIEW_SIZE) {
                    int idx = (ny * PREVIEW_SIZE + nx) * 4;
                    pixels[idx] = 255; pixels[idx+1] = 255; pixels[idx+2] = 255; pixels[idx+3] = 255;
                }
            }
        }
    }

    /* Overlay starts (yellow) */
    BYTE numStarts = startsGetNumStarts(&ss);
    for (int i = 0; i < numStarts; i++) {
        int sx = ss->item[i].x;
        int sy = ss->item[i].y;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int nx = sx + dx, ny = sy + dy;
                if (nx >= 0 && nx < PREVIEW_SIZE && ny >= 0 && ny < PREVIEW_SIZE) {
                    int idx = (ny * PREVIEW_SIZE + nx) * 4;
                    pixels[idx] = 255; pixels[idx+1] = 255; pixels[idx+2] = 0; pixels[idx+3] = 255;
                }
            }
        }
    }

    *bMinX = minX; *bMinY = minY; *bMaxX = maxX; *bMaxY = maxY;

    SDL_Surface *surface = SDL_CreateSurfaceFrom(
        PREVIEW_SIZE, PREVIEW_SIZE, SDL_PIXELFORMAT_RGBA32,
        pixels, PREVIEW_SIZE * 4);
    SDL_Texture *tex = NULL;
    if (surface) {
        tex = SDL_CreateTextureFromSurface(renderer, surface);
        if (tex) SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
        SDL_DestroySurface(surface);
    }

    free(pixels);
    mapDestroy(&mp);
    pillsDestroy(&pb);
    basesDestroy(&bs);
    startsDestroy(&ss);
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

    const char *dirs[] = { "data/maps" };
    for (int d = 0; d < 1 && state->numMaps < MAP_CHOOSER_MAX_MAPS; d++) {
        int count = 0;
        char **list = SDL_GlobDirectory(dirs[d], "*.map", 0, &count);
        if (!list) continue;
        for (int i = 0; i < count && state->numMaps < MAP_CHOOSER_MAX_MAPS; i++) {
            /* Skip Everard Island duplicate */
            if (SDL_strcasecmp(list[i], "Everard Island.map") == 0) continue;

            MapChooserEntry *e = &state->maps[state->numMaps];
            SDL_snprintf(e->path, sizeof(e->path), "%s/%s", dirs[d], list[i]);

            /* Strip .map extension for display name */
            SDL_strlcpy(e->name, list[i], sizeof(e->name));
            size_t len = SDL_strlen(e->name);
            if (len > 4 && SDL_strcasecmp(e->name + len - 4, ".map") == 0) {
                e->name[len - 4] = '\0';
            }
            state->numMaps++;
        }
        SDL_free(list);
    }

    /* Sort maps alphabetically (skip first entry which is always Everard Island) */
    if (state->numMaps > 2) {
        std::sort(&state->maps[1], &state->maps[state->numMaps],
            [](const MapChooserEntry &a, const MapChooserEntry &b) {
                return SDL_strcasecmp(a.name, b.name) < 0;
            });
    }
}

static void updatePreview(MapChooserState *state, SDL_Renderer *renderer) {
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

void mapChooserInit(MapChooserState *state, SDL_Renderer *renderer) {
    SDL_memset(state, 0, sizeof(*state));
    state->selectedIdx = 0;
    state->selectedPath[0] = '\0';
    SDL_strlcpy(state->selectedName, "Everard Island (Inbuilt)", sizeof(state->selectedName));
    state->fileDialogPending = false;
    state->fileDialogGotResult = false;

    discoverMaps(state);
    updatePreview(state, renderer);
    state->initialized = true;
}

bool mapChooserRender(MapChooserState *state, SDL_Renderer *renderer,
                      float width, float height, float scale) {
    bool changed = false;

    /* Handle file dialog result */
    if (state->fileDialogGotResult) {
        state->fileDialogGotResult = false;
        if (state->fileDialogResult[0] != '\0') {
            state->selectedIdx = -1; /* custom file, not in list */
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

    /* Layout: list on left, preview on right */
    float previewPanelW = (PREVIEW_SIZE + 40) * (scale > 1.5f ? 0.75f : 1.0f);
    if (previewPanelW > width * 0.5f) previewPanelW = width * 0.5f;
    float listPanelW = width - previewPanelW - 8.0f;

    /* Left panel: map list */
    ImGui::BeginChild("##MapList", ImVec2(listPanelW, height), ImGuiChildFlags_Borders);

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
    ImGui::EndChild(); /* ##MapList */

    ImGui::SameLine(0, 8.0f);

    /* Right panel: preview + info */
    ImGui::BeginChild("##MapPreview", ImVec2(previewPanelW, height), ImGuiChildFlags_Borders);

    ImGui::Text("%s", state->selectedName);
    ImGui::Separator();

    if (state->previewTex) {
        /* Compute UV coordinates to zoom into interesting area */
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
        float availH = ImGui::GetContentRegionAvail().y;
        float previewSize = panelWidth;
        if (availH < previewSize) previewSize = availH;

        float offsetX = (panelWidth - previewSize) * 0.5f;
        if (offsetX > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
        ImGui::Image((ImTextureID)state->previewTex, ImVec2(previewSize, previewSize), uv0, uv1);
    } else {
        ImGui::TextDisabled("No preview available");
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
