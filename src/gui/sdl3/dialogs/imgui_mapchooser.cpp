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

static void discoverMaps(MapChooserState *state) {
    state->numMaps = 0;

    /* First entry is always "Everard Island (Inbuilt)" */
    SDL_strlcpy(state->maps[0].name, langGetText(STR_MAPCHOOSER_EVERARD), sizeof(state->maps[0].name));
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

    /* Sort all maps alphabetically — Everard Island (the inbuilt entry
     * added at index 0) gets sorted in alongside the rest rather than
     * pinned at the top. */
    if (state->numMaps > 1) {
        std::sort(&state->maps[0], &state->maps[state->numMaps],
            [](const MapChooserEntry &a, const MapChooserEntry &b) {
                return SDL_strcasecmp(a.name, b.name) < 0;
            });
    }
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

/* Generate a random map preview from the current config */
static void generateRandomPreview(MapChooserState *state, SDL_Renderer *renderer) {
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

    /* Mirror into the interactive preview widget. */
    if (state->previewView && state->compressedData && state->compressedLen > 0) {
        mapPreviewViewLoadCompressed(state->previewView,
                                      state->compressedData,
                                      state->compressedLen);
        mapPreviewViewSetInitialBounds(state->previewView,
            state->previewBoundsMinX, state->previewBoundsMinY,
            state->previewBoundsMaxX, state->previewBoundsMaxY);
    }

    /* Update seed display */
    mapGenConfigToSeed(&state->genConfig, state->genSeedBuf, sizeof(state->genSeedBuf));

    /* Store the seed as the selected path for the caller */
    SDL_snprintf(state->selectedPath, FILENAME_MAX, "randommap:%s", state->genSeedBuf);
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

    /* Layout: left panel (map list) + right preview.
     * If the caller capped the list width (leftPanelMaxW > 0) it wins —
     * preview claims the rest of the row. Otherwise fall back to the
     * legacy "preview is the natural minimap size, list takes the
     * remainder" split. */
    float leftPanelW;
    float previewPanelW;
    if (state->leftPanelMaxW > 0.0f) {
        leftPanelW = state->leftPanelMaxW;
        if (leftPanelW > width * 0.5f) leftPanelW = width * 0.5f;
        previewPanelW = width - leftPanelW - 8.0f;
        if (previewPanelW < 100.0f) {
            /* Window too narrow — fall back to the legacy split so the
             * preview doesn't disappear entirely. */
            previewPanelW = (PREVIEW_SIZE + 40) * (scale > 1.5f ? 0.75f : 1.0f);
            if (previewPanelW > width * 0.55f) previewPanelW = width * 0.55f;
            leftPanelW = width - previewPanelW - 8.0f;
        }
    } else {
        previewPanelW = (PREVIEW_SIZE + 40) * (scale > 1.5f ? 0.75f : 1.0f);
        if (previewPanelW > width * 0.55f) previewPanelW = width * 0.55f;
        leftPanelW = width - previewPanelW - 8.0f;
    }

    /* Left panel */
    ImGui::BeginChild("##MapLeft", ImVec2(leftPanelW, height), ImGuiChildFlags_Borders);

    if (state->randomMapSelected) {
        /* --- Random map generator mode --- */

        /* Back button */
        if (ImGui::Button(langGetText(STR_MAPCHOOSER_LOADMAP), ImVec2(-1, 0))) {
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

            /* Generate Random Map button */
            if (ImGui::Button(langGetText(STR_MAPCHOOSER_GENRANDOM), ImVec2(-1, 0))) {
                state->randomMapSelected = true;
                state->selectedIdx = MAP_CHOOSER_IDX_RANDOM;
                SDL_strlcpy(state->selectedName, langGetText(STR_MAPCHOOSER_RANDOMMAP), sizeof(state->selectedName));
                initGenConfig(state);
                generateRandomPreview(state, renderer);
                changed = true;
            }

            ImGui::Separator();
        }

        /* Local search filter — case-insensitive substring match on the
         * display name. Sits above the list so it stays visible while
         * the list scrolls. */
        {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##MapSearch", "Search...",
                                     state->searchFilter,
                                     sizeof(state->searchFilter));
        }

        /* Scrollable map list */
        ImGui::BeginChild("##MapListScroll", ImVec2(0, 0));

        bool hasFilter = state->searchFilter[0] != '\0';
        for (int i = 0; i < state->numMaps; i++) {
            if (hasFilter) {
                /* Case-insensitive substring match against the display
                 * name. SDL has no strcasestr, so do it manually. */
                const char *hay = state->maps[i].name;
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
            ImGui::InvisibleButton("##MapPreviewDrag",
                                    ImVec2(availW, availH));
            bool hovered = ImGui::IsItemHovered();
            MapPreviewInputOpts opts = { true, true, true, false };
            mapPreviewViewHandleInput(state->previewView, hovered, &opts);
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
