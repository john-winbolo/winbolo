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
 * Name:          map_preview_popup.cpp
 * Purpose:       Reusable zoomable/pannable map preview
 *                popup for ImGui dialogs.
 *********************************************************/

#include <cstdio>
#include <cstring>

#include <SDL3/SDL.h>

#include "imgui.h"

extern "C" {
#include "global.h"
#include "client_mappreview.h"
#include "screencalc.h"
#include "tilenum.h"
#include "../tiles.h"
#include "map_preview_popup.h"
#include "macos_pinch.h"
#include "sprite_positions.h"

/* From tileloader.h */
extern SDL_Surface *tileLoaderBuildSheet(int tileSize);
}

/* Popup state */
static bool  mapPopupOpen    = false;
static int   popupZoomIndex  = 5;        /* index into zoomSteps[] (1.0x default) */
static float popupZoomLevel  = 1.0f;     /* actual zoom multiplier */
static WORLD popupCenterX    = 128 << 8; /* camera center in WORLD coords (map centre) */
static WORLD popupCenterY    = 128 << 8;

/* Source data — either compressed network data or a file path */
static BYTE        *popupCompressedData = NULL;
static int          popupCompressedLen  = 0;
static char        *popupFilePath       = NULL;

/* Tile rendering resources (created once when popup opens) */
static SDL_Texture *popupTilesTex    = NULL;  /* tile atlas */
static SDL_Texture *popupOffscreen   = NULL;  /* offscreen render target */
static int          popupOffscreenW  = 0;
static int          popupOffscreenH  = 0;
static MapPreview  *popupPreview     = NULL;
static bool         popupDataLoaded  = false;

/* Track last window size for resize detection */
static int          popupLastWinW = 0;
static int          popupLastWinH = 0;

/* Whether auto-fit zoom has been applied for this open */
static bool         popupAutoFitDone = false;

/* Zoom steps — same as map editor */
static const float zoomSteps[] = {
    0.5f, 0.6f, 0.7f, 0.8f, 0.9f,
    1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
    9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f
};
#define POPUP_ZOOM_STEP_COUNT 21
#define POPUP_ZOOM_STEP_1X 5

/* Boat sprite atlas coordinates for start position overlays */
static const int popupBoatAtlasX[16] = {
    TANK_SELFBOAT_0_X,  TANK_SELFBOAT_1_X,  TANK_SELFBOAT_2_X,  TANK_SELFBOAT_3_X,
    TANK_SELFBOAT_4_X,  TANK_SELFBOAT_5_X,  TANK_SELFBOAT_6_X,  TANK_SELFBOAT_7_X,
    TANK_SELFBOAT_8_X,  TANK_SELFBOAT_9_X,  TANK_SELFBOAT_10_X, TANK_SELFBOAT_11_X,
    TANK_SELFBOAT_12_X, TANK_SELFBOAT_13_X, TANK_SELFBOAT_14_X, TANK_SELFBOAT_15_X
};
static const int popupBoatAtlasY[16] = {
    TANK_SELFBOAT_0_Y,  TANK_SELFBOAT_1_Y,  TANK_SELFBOAT_2_Y,  TANK_SELFBOAT_3_Y,
    TANK_SELFBOAT_4_Y,  TANK_SELFBOAT_5_Y,  TANK_SELFBOAT_6_Y,  TANK_SELFBOAT_7_Y,
    TANK_SELFBOAT_8_Y,  TANK_SELFBOAT_9_Y,  TANK_SELFBOAT_10_Y, TANK_SELFBOAT_11_Y,
    TANK_SELFBOAT_12_Y, TANK_SELFBOAT_13_Y, TANK_SELFBOAT_14_Y, TANK_SELFBOAT_15_Y
};

/* Read a neighbour tile for adjacency calculation, treating bases as ROAD
 * and stripping mine variants. Adapted from meNeighbour() in mapeditor.c. */
static BYTE popupNeighbour(BYTE nx, BYTE ny) {
    if (clientMapPreviewIsBase(popupPreview, nx, ny)) return ROAD;
    BYTE t = clientMapPreviewGetTerrain(popupPreview, nx, ny);
    if (t >= MINE_START && t <= MINE_END) return (BYTE)(t - MINE_SUBTRACT);
    return t;
}

/* Adjacency-aware tile calculation. Adapted from meCalcTile() in mapeditor.c. */
static BYTE popupCalcTile(BYTE xValue, BYTE yValue) {
    if (clientMapPreviewIsPill(popupPreview, xValue, yValue)) {
        static const BYTE pillTileForArmour[16] = {
            PILL_EVIL_0,  PILL_EVIL_1,  PILL_EVIL_2,  PILL_EVIL_3,
            PILL_EVIL_4,  PILL_EVIL_5,  PILL_EVIL_6,  PILL_EVIL_7,
            PILL_EVIL_8,  PILL_EVIL_9,  PILL_EVIL_10, PILL_EVIL_11,
            PILL_EVIL_12, PILL_EVIL_13, PILL_EVIL_14, PILL_EVIL_15
        };
        BYTE armour = clientMapPreviewGetPillArmourAt(popupPreview, xValue, yValue);
        if (armour <= 15) return pillTileForArmour[armour];
        return PILL_EVIL_15;
    }
    if (clientMapPreviewIsBase(popupPreview, xValue, yValue)) return BASE_NEUTRAL;
    if (clientMapPreviewIsStart(popupPreview, xValue, yValue)) return DEEP_SEA_SOLID;

    BYTE currentPos = clientMapPreviewGetTerrain(popupPreview, xValue, yValue);
    if (currentPos >= MINE_START && currentPos <= MINE_END)
        currentPos = (BYTE)(currentPos - MINE_SUBTRACT);

    BYTE aboveLeft  = popupNeighbour((BYTE)(xValue-1), (BYTE)(yValue-1));
    BYTE above      = popupNeighbour(xValue,            (BYTE)(yValue-1));
    BYTE aboveRight = popupNeighbour((BYTE)(xValue+1), (BYTE)(yValue-1));
    BYTE leftPos    = popupNeighbour((BYTE)(xValue-1), yValue);
    BYTE rightPos   = popupNeighbour((BYTE)(xValue+1), yValue);
    BYTE belowLeft  = popupNeighbour((BYTE)(xValue-1), (BYTE)(yValue+1));
    BYTE below      = popupNeighbour(xValue,            (BYTE)(yValue+1));
    BYTE belowRight = popupNeighbour((BYTE)(xValue+1), (BYTE)(yValue+1));

    switch (currentPos) {
    case ROAD:     return screenCalcRoad(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case BUILDING: return screenCalcBuilding(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case FOREST:   return screenCalcForest(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case RIVER:    return screenCalcRiver(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case DEEP_SEA: return screenCalcDeepSea(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case BOAT:     return screenCalcBoat(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case CRATER:   return screenCalcCrater(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    default:       return currentPos;
    }
}

/* Render start positions as boat sprites. */
static void popupRenderStarts(SDL_Renderer *renderer, int screenW, int screenH) {
    int zf = (int)popupZoomLevel;
    if (zf < 1) zf = 1;
    int tileSize = TILE_SIZE_X;
    int scaledTile = tileSize * zf;

    int centerPX = ((int)popupCenterX * tileSize) >> 8;
    int centerPY = ((int)popupCenterY * tileSize) >> 8;
    int camPX = centerPX - screenW / (2 * zf);
    int camPY = centerPY - screenH / (2 * zf);

    SDL_SetTextureAlphaMod(popupTilesTex, 200);

    BYTE numStarts = clientMapPreviewGetStartCount(popupPreview);
    for (BYTE i = 1; i <= numStarts; i++) {
        BYTE sx, sy, sdir;
        if (!clientMapPreviewGetStart(popupPreview, i, &sx, &sy, &sdir)) continue;
        float dx = (float)((int)sx * tileSize - camPX) * zf;
        float dy = (float)((int)sy * tileSize - camPY) * zf;

        if (dx + scaledTile < 0 || dx > screenW ||
            dy + scaledTile < 0 || dy > screenH) continue;

        int dir = sdir;
        SDL_FRect src = {
            (float)popupBoatAtlasX[dir],
            (float)popupBoatAtlasY[dir],
            (float)tileSize,
            (float)tileSize
        };
        SDL_FRect dest = { dx, dy, (float)scaledTile, (float)scaledTile };
        SDL_RenderTexture(renderer, popupTilesTex, &src, &dest);
    }

    SDL_SetTextureAlphaMod(popupTilesTex, 255);
}

/* Render tiles to the offscreen texture. */
static void popupRenderTilesToOffscreen(SDL_Renderer *renderer, int screenW, int screenH) {
    int zf = (int)popupZoomLevel;
    if (zf < 1) zf = 1;
    int tileSize = TILE_SIZE_X;
    int scaledTile = tileSize * zf;

    int centerPX = ((int)popupCenterX * tileSize) >> 8;
    int centerPY = ((int)popupCenterY * tileSize) >> 8;

    int camPX = centerPX - screenW / (2 * zf);
    int camPY = centerPY - screenH / (2 * zf);

    int camMX = camPX / tileSize;
    int camMY = camPY / tileSize;
    if (camPX < 0) camMX--;
    if (camPY < 0) camMY--;
    int edgeX = (camPX - camMX * tileSize) * zf;
    int edgeY = (camPY - camMY * tileSize) * zf;

    int tilesW = screenW / scaledTile + 3;
    int tilesH = screenH / scaledTile + 3;
    if (tilesW > 256) tilesW = 256;
    if (tilesH > 256) tilesH = 256;

    for (int x = 0; x < tilesW; x++) {
        for (int y = 0; y < tilesH; y++) {
            int mapX = camMX + x;
            int mapY = camMY + y;

            BYTE tileNum;
            if (mapX < 0 || mapX > 255 || mapY < 0 || mapY > 255) {
                tileNum = DEEP_SEA_SOLID;
            } else {
                tileNum = popupCalcTile((BYTE)mapX, (BYTE)mapY);
            }

            SDL_FRect src = {
                (float)(mapViewPosX[tileNum]),
                (float)(mapViewPosY[tileNum]),
                (float)tileSize,
                (float)tileSize
            };
            SDL_FRect dest = {
                (float)(x * scaledTile - edgeX),
                (float)(y * scaledTile - edgeY),
                (float)scaledTile,
                (float)scaledTile
            };
            SDL_RenderTexture(renderer, popupTilesTex, &src, &dest);
        }
    }

    /* Mine overlay — render mine sprite on top of mined tiles and border zone */
    SDL_SetTextureAlphaMod(popupTilesTex, 180);
    for (int x = 0; x < tilesW; x++) {
        for (int y = 0; y < tilesH; y++) {
            int mapX = camMX + x;
            int mapY = camMY + y;
            if (mapX < 0 || mapX > 255 || mapY < 0 || mapY > 255) continue;
            bool mined = false;
            BYTE raw = clientMapPreviewGetTerrain(popupPreview, (BYTE)mapX, (BYTE)mapY);
            if (raw >= MINE_START && raw <= MINE_END) {
                mined = true;
            } else if (mapX <= MAP_MINE_EDGE_LEFT || mapX >= MAP_MINE_EDGE_RIGHT ||
                       mapY <= MAP_MINE_EDGE_TOP  || mapY >= MAP_MINE_EDGE_BOTTOM) {
                mined = true;
            }
            if (mined) {
                SDL_FRect mineSrc = {
                    (float)MINE_X, (float)MINE_Y,
                    (float)tileSize, (float)tileSize
                };
                SDL_FRect dest = {
                    (float)(x * scaledTile - edgeX),
                    (float)(y * scaledTile - edgeY),
                    (float)scaledTile,
                    (float)scaledTile
                };
                SDL_RenderTexture(renderer, popupTilesTex, &mineSrc, &dest);
            }
        }
    }
    SDL_SetTextureAlphaMod(popupTilesTex, 255);

    popupRenderStarts(renderer, screenW, screenH);
}

/* Free popup map data (but not the tile atlas or source data). */
static void popupFreeMapData(void) {
    if (popupOffscreen) { SDL_DestroyTexture(popupOffscreen); popupOffscreen = NULL; }
    popupOffscreenW = 0;
    popupOffscreenH = 0;
    if (popupDataLoaded) {
        clientMapPreviewDestroy(popupPreview);
        popupPreview = NULL;
        popupDataLoaded = false;
    }
}

/* Scan the loaded map data for the actual non-deep-sea bounds, then compute
 * the best zoom level to fit the map content in the given viewport size. */
static void popupAutoFitZoom(int viewW, int viewH) {
    if (!popupDataLoaded || !popupPreview) return;

    /* Find the bounding box of non-deep-sea tiles, pillboxes, bases, starts */
    int minX = 255, minY = 255, maxX = 0, maxY = 0;
    for (int y = 0; y < 256; y++) {
        for (int x = 0; x < 256; x++) {
            BYTE t = clientMapPreviewGetTerrain(popupPreview, (BYTE)x, (BYTE)y);
            if (t != DEEP_SEA) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    /* Include pillboxes, bases, starts in bounds */
    {
        BYTE n = clientMapPreviewGetPillCount(popupPreview);
        for (BYTE i = 1; i <= n; i++) {
            BYTE px, py;
            if (!clientMapPreviewGetPill(popupPreview, i, &px, &py, NULL, NULL)) continue;
            if (px < minX) minX = px; if (px > maxX) maxX = px;
            if (py < minY) minY = py; if (py > maxY) maxY = py;
        }
    }
    {
        BYTE n = clientMapPreviewGetBaseCount(popupPreview);
        for (BYTE i = 1; i <= n; i++) {
            BYTE bx, by;
            if (!clientMapPreviewGetBase(popupPreview, i, &bx, &by, NULL)) continue;
            if (bx < minX) minX = bx; if (bx > maxX) maxX = bx;
            if (by < minY) minY = by; if (by > maxY) maxY = by;
        }
    }
    {
        BYTE n = clientMapPreviewGetStartCount(popupPreview);
        for (BYTE i = 1; i <= n; i++) {
            BYTE sx, sy;
            if (!clientMapPreviewGetStart(popupPreview, i, &sx, &sy, NULL)) continue;
            if (sx < minX) minX = sx; if (sx > maxX) maxX = sx;
            if (sy < minY) minY = sy; if (sy > maxY) maxY = sy;
        }
    }

    if (maxX < minX || maxY < minY) {
        /* Empty map — use defaults */
        popupZoomIndex = POPUP_ZOOM_STEP_1X;
        popupZoomLevel = 1.0f;
        return;
    }

    /* Add a small padding (2 tiles) around the content */
    minX -= 2; minY -= 2; maxX += 2; maxY += 2;
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > 255) maxX = 255;
    if (maxY > 255) maxY = 255;

    /* Center on content */
    popupCenterX = ((minX + maxX) / 2) << 8;
    popupCenterY = ((minY + maxY) / 2) << 8;

    /* Map content size in pixels at 1x zoom */
    int contentW = (maxX - minX + 1) * TILE_SIZE_X;
    int contentH = (maxY - minY + 1) * TILE_SIZE_Y;

    /* Compute zoom to fit */
    float zoomW = (float)viewW / (float)contentW;
    float zoomH = (float)viewH / (float)contentH;
    float idealZoom = (zoomW < zoomH) ? zoomW : zoomH;

    /* Clamp to minimum 0.5x */
    if (idealZoom < 0.5f) idealZoom = 0.5f;

    /* Find the best matching zoom step (largest step <= idealZoom) */
    popupZoomIndex = 0;
    for (int i = 0; i < POPUP_ZOOM_STEP_COUNT; i++) {
        if (zoomSteps[i] <= idealZoom) {
            popupZoomIndex = i;
        } else {
            break;
        }
    }
    popupZoomLevel = zoomSteps[popupZoomIndex];
}

/* Open the popup with initial zoom/center from bounds. */
static void popupOpen(int boundsMinX, int boundsMinY, int boundsMaxX, int boundsMaxY) {
    mapPopupOpen = true;
    /* Temporary center from minimap bounds; auto-fit will refine after data loads */
    popupZoomIndex = POPUP_ZOOM_STEP_1X + 1;
    popupZoomLevel = 2.0f;
    popupCenterX = ((boundsMinX + boundsMaxX) / 2) << 8;
    popupCenterY = ((boundsMinY + boundsMaxY) / 2) << 8;
    popupAutoFitDone = false;
}

void mapPreviewPopupOpenCompressed(const BYTE *compressedData, int compressedLen,
                                   int boundsMinX, int boundsMinY,
                                   int boundsMaxX, int boundsMaxY) {
    if (!compressedData || compressedLen <= 0) return;

    if (popupCompressedData) { SDL_free(popupCompressedData); popupCompressedData = NULL; }
    if (popupFilePath) { SDL_free(popupFilePath); popupFilePath = NULL; }
    popupCompressedData = (BYTE *)SDL_malloc(compressedLen);
    if (popupCompressedData) {
        SDL_memcpy(popupCompressedData, compressedData, compressedLen);
        popupCompressedLen = compressedLen;
    }

    popupFreeMapData();
    popupOpen(boundsMinX, boundsMinY, boundsMaxX, boundsMaxY);
}

void mapPreviewPopupOpenFile(const char *mapPath,
                             int boundsMinX, int boundsMinY,
                             int boundsMaxX, int boundsMaxY) {
    if (!mapPath || mapPath[0] == '\0') return;

    if (popupCompressedData) { SDL_free(popupCompressedData); popupCompressedData = NULL; popupCompressedLen = 0; }
    if (popupFilePath) { SDL_free(popupFilePath); popupFilePath = NULL; }
    popupFilePath = SDL_strdup(mapPath);

    popupFreeMapData();
    popupOpen(boundsMinX, boundsMinY, boundsMaxX, boundsMaxY);
}

void mapPreviewPopupOnClick(const BYTE *compressedData, int compressedLen,
                            int boundsMinX, int boundsMinY,
                            int boundsMaxX, int boundsMaxY) {
    if (!ImGui::IsItemClicked()) return;
    mapPreviewPopupOpenCompressed(compressedData, compressedLen,
                                  boundsMinX, boundsMinY, boundsMaxX, boundsMaxY);
}

void mapPreviewPopupOnClickFile(const char *mapPath,
                                int boundsMinX, int boundsMinY,
                                int boundsMaxX, int boundsMaxY) {
    if (!ImGui::IsItemClicked()) return;
    mapPreviewPopupOpenFile(mapPath, boundsMinX, boundsMinY, boundsMaxX, boundsMaxY);
}

void mapPreviewPopupRenderOffscreen(SDL_Renderer *renderer, int winW, int winH) {
    if (!mapPopupOpen || !popupDataLoaded || !popupTilesTex) return;

    /* Detect window resize — force offscreen texture recreation */
    if (winW != popupLastWinW || winH != popupLastWinH) {
        popupLastWinW = winW;
        popupLastWinH = winH;
        if (popupOffscreen) {
            SDL_DestroyTexture(popupOffscreen);
            popupOffscreen = NULL;
            popupOffscreenW = 0;
            popupOffscreenH = 0;
        }
    }

    int viewW = (int)(winW * 0.8f);
    int viewH = (int)(winH * 0.8f);
    if (viewW < 1) viewW = 1;
    if (viewH < 1) viewH = 1;
    /* Sub-1x zoom: render at 1x into larger texture, ImGui scales down */
    int ofsW = viewW, ofsH = viewH;
    if (popupZoomLevel < 1.0f) {
        ofsW = (int)(viewW / popupZoomLevel);
        ofsH = (int)(viewH / popupZoomLevel);
        if (ofsW > 4096) ofsW = 4096;
        if (ofsH > 4096) ofsH = 4096;
    }
    if (!popupOffscreen || popupOffscreenW != ofsW || popupOffscreenH != ofsH) {
        if (popupOffscreen) SDL_DestroyTexture(popupOffscreen);
        popupOffscreen = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                           SDL_TEXTUREACCESS_TARGET, ofsW, ofsH);
        popupOffscreenW = ofsW;
        popupOffscreenH = ofsH;
    }
    if (popupOffscreen) {
        SDL_SetRenderTarget(renderer, popupOffscreen);
        SDL_SetRenderDrawColor(renderer, 0, 0, 64, 255);
        SDL_RenderClear(renderer);
        popupRenderTilesToOffscreen(renderer, ofsW, ofsH);
        SDL_SetRenderTarget(renderer, NULL);
    }
}

void mapPreviewPopupRenderModal(SDL_Renderer *renderer) {
    if (mapPopupOpen && !ImGui::IsPopupOpen("Map Preview##full")) {
        ImGui::OpenPopup("Map Preview##full");
    }
    {
        ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        ImVec2 popupSize(displaySize.x * 0.8f, displaySize.y * 0.8f);
        ImGui::SetNextWindowSize(popupSize, ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    bool modalOpen = ImGui::BeginPopupModal("Map Preview##full", &mapPopupOpen,
                               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    if (modalOpen) {
        /* Escape key closes the popup */
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            mapPopupOpen = false;
            ImGui::CloseCurrentPopup();
        }

        /* Load tile resources on first open */
        if (!popupDataLoaded && (popupCompressedData || popupFilePath)) {
            if (!popupTilesTex) {
                SDL_Surface *sheet = tileLoaderBuildSheet(16);
                if (sheet) {
                    popupTilesTex = SDL_CreateTextureFromSurface(renderer, sheet);
                    SDL_SetTextureScaleMode(popupTilesTex, SDL_SCALEMODE_NEAREST);
                    SDL_DestroySurface(sheet);
                }
            }
            if (popupCompressedData) {
                popupPreview = clientMapPreviewLoadFromBuffer(popupCompressedData, popupCompressedLen);
            } else if (popupFilePath) {
                popupPreview = clientMapPreviewLoadFromFile(popupFilePath);
                if (!popupPreview) {
                    /* Fallback: SDL_LoadFile handles iOS/macOS bundle paths */
                    size_t fileSize = 0;
                    void *fileData = SDL_LoadFile(popupFilePath, &fileSize);
                    if (fileData && fileSize > 0) {
                        char *tmpDir = SDL_GetPrefPath("WinBolo", "WinBolo");
                        char tmpPath[512];
                        SDL_snprintf(tmpPath, sizeof(tmpPath), "%s_popup_temp.map", tmpDir ? tmpDir : "");
                        SDL_free(tmpDir);
                        FILE *fp = fopen(tmpPath, "wb");
                        if (fp) {
                            fwrite(fileData, 1, fileSize, fp);
                            fclose(fp);
                            popupPreview = clientMapPreviewLoadFromFile(tmpPath);
                            remove(tmpPath);
                        }
                    }
                    SDL_free(fileData);
                }
            }
            popupDataLoaded = (popupPreview != NULL);
        }

        /* Auto-fit zoom on first frame after data is loaded */
        if (popupDataLoaded && !popupAutoFitDone) {
            ImVec2 contentSize = ImGui::GetContentRegionAvail();
            popupAutoFitZoom((int)contentSize.x, (int)contentSize.y);
            popupAutoFitDone = true;
        }

        if (popupDataLoaded && popupOffscreen) {
            ImVec2 contentSize = ImGui::GetContentRegionAvail();
            ImGui::Image((ImTextureID)popupOffscreen, contentSize);

            /* Cursor and drag-to-pan */
            ImGuiIO &io = ImGui::GetIO();
            if (ImGui::IsItemHovered()) {
                if (ImGui::IsMouseDragging(0)) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                    float zf = popupZoomLevel;
                    float tileSize = 16.0f;
                    float dx = io.MouseDelta.x / (zf * tileSize) * 256.0f;
                    float dy = io.MouseDelta.y / (zf * tileSize) * 256.0f;
                    int newCX = (int)popupCenterX - (int)dx;
                    int newCY = (int)popupCenterY - (int)dy;
                    if (newCX < 0) newCX = 0;
                    if (newCX > 65280) newCX = 65280;
                    if (newCY < 0) newCY = 0;
                    if (newCY > 65280) newCY = 65280;
                    popupCenterX = (WORLD)newCX;
                    popupCenterY = (WORLD)newCY;
                } else {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                }
            }

            /* Zoom via scroll wheel */
            if (ImGui::IsItemHovered() && io.MouseWheel != 0) {
                if (io.MouseWheel > 0 && popupZoomIndex < POPUP_ZOOM_STEP_COUNT - 1)
                    popupZoomIndex++;
                else if (io.MouseWheel < 0 && popupZoomIndex > 0)
                    popupZoomIndex--;
                popupZoomLevel = zoomSteps[popupZoomIndex];
            }

            /* Trackpad pinch-to-zoom (macOS) — consume unconditionally
             * since the modal covers the entire screen. */
            {
                float pinch = macOSPinchZoomConsume();
                if (pinch != 0.0f) {
                    static float pinchAccum = 0.0f;
                    pinchAccum += pinch;
                    while (pinchAccum > 0.15f) {
                        if (popupZoomIndex < POPUP_ZOOM_STEP_COUNT - 1) popupZoomIndex++;
                        pinchAccum -= 0.15f;
                    }
                    while (pinchAccum < -0.15f) {
                        if (popupZoomIndex > 0) popupZoomIndex--;
                        pinchAccum += 0.15f;
                    }
                    popupZoomLevel = zoomSteps[popupZoomIndex];
                }
            }

            /* Arrow key panning */
            {
                float panSpeed = 512.0f / popupZoomLevel; /* WORLD units per frame */
                if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) {
                    int newCX = (int)popupCenterX - (int)panSpeed;
                    popupCenterX = (WORLD)(newCX < 0 ? 0 : newCX);
                }
                if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) {
                    int newCX = (int)popupCenterX + (int)panSpeed;
                    popupCenterX = (WORLD)(newCX > 65280 ? 65280 : newCX);
                }
                if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) {
                    int newCY = (int)popupCenterY - (int)panSpeed;
                    popupCenterY = (WORLD)(newCY < 0 ? 0 : newCY);
                }
                if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) {
                    int newCY = (int)popupCenterY + (int)panSpeed;
                    popupCenterY = (WORLD)(newCY > 65280 ? 65280 : newCY);
                }
            }

            /* Zoom indicator overlay */
            {
                char zoomText[16];
                SDL_snprintf(zoomText, sizeof(zoomText), "%.1fx", popupZoomLevel);
                ImVec2 textSize = ImGui::CalcTextSize(zoomText);
                ImVec2 windowPos = ImGui::GetWindowPos();
                ImVec2 windowSize = ImGui::GetWindowSize();
                float pad = 8.0f;
                ImVec2 textPos(windowPos.x + windowSize.x - textSize.x - pad - ImGui::GetStyle().WindowPadding.x,
                              windowPos.y + windowSize.y - textSize.y - pad - ImGui::GetStyle().WindowPadding.y);
                ImDrawList *dl = ImGui::GetWindowDrawList();
                ImVec2 bgMin(textPos.x - 4, textPos.y - 2);
                ImVec2 bgMax(textPos.x + textSize.x + 4, textPos.y + textSize.y + 2);
                dl->AddRectFilled(bgMin, bgMax, IM_COL32(0, 0, 0, 160), 4.0f);
                dl->AddText(textPos, IM_COL32(255, 255, 255, 220), zoomText);
            }
        } else {
            macOSPinchZoomConsume(); /* drain so it doesn't jump when data arrives */
            ImGui::Text("Map preview loading...");
        }
        ImGui::EndPopup();
    }
    if (!mapPopupOpen && popupDataLoaded) {
        popupFreeMapData();
    }
}

void mapPreviewPopupClose(void) {
    if (mapPopupOpen) {
        mapPopupOpen = false;
        popupFreeMapData();
        if (popupCompressedData) { SDL_free(popupCompressedData); popupCompressedData = NULL; popupCompressedLen = 0; }
        if (popupFilePath) { SDL_free(popupFilePath); popupFilePath = NULL; }
    }
}

void mapPreviewPopupDestroy(void) {
    popupFreeMapData();
    if (popupCompressedData) { SDL_free(popupCompressedData); popupCompressedData = NULL; popupCompressedLen = 0; }
    if (popupFilePath) { SDL_free(popupFilePath); popupFilePath = NULL; }
    if (popupTilesTex) { SDL_DestroyTexture(popupTilesTex); popupTilesTex = NULL; }
    mapPopupOpen = false;
}
