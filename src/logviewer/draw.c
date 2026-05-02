/*
 * $Id$
 *
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
*Name:          Draw
*Filename:      draw.c
*Author:        John Morrison
*Creation Date: 13/12/98
*Last Modified:  2024
*Purpose:
*  System Specific Drawing routines (Uses SDL3)
*********************************************************/

#include <math.h>
#include <stdint.h>
#include <stdio.h>  /* Already included, but needed for debug printf */
#include <string.h>

#include "global.h"
#include "screenlgm.h"
#include "tiles.h"
#include "tilenum.h"
#include "positions.h"
#include "draw.h"
#include "logviewer.h"
#include "imgui/imgui_main_menu.h"
#include "../gui/sdl3/sdl_bmp.h"
#include "../gui/sdl3/sprite_positions.h"
#include "../gui/sdl3/tileloader.h"
#include "../third_party/stb/stb_image.h"

/* Must be included after global.h to avoid bool type conflict */
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#define TEAM_IMAGE_SIZE_X (16 * 16)
#define TEAM_IMAGE_SIZE_Y (17 * 16)
#define ITEM_IMAGE_SIZE_X (17 * 16)
#define ITEM_IMAGE_SIZE_Y (17 * 16)

/* SDL3 Renderer and Window */
static SDL_Window *sdlWindow = NULL;
static SDL_Renderer *sdlRenderer = NULL;
static int sdlInitialized = 0;  /* Track if SDL is already initialized */
static bool ownsWindow = TRUE;  /* TRUE in standalone, FALSE when embedded */

/* Sprite Textures (loaded once at startup) */
static SDL_Texture *textureTiles = NULL;
static SDL_Texture *textureTanks = NULL;
static SDL_Texture *textureBoats = NULL;
static SDL_Texture *textureItems = NULL;

/* Render Target (back buffer equivalent) */
static SDL_Texture *textureTarget = NULL;
static int targetWidth = 0;
static int targetHeight = 0;

/* Font for labels */
static TTF_Font *labelFont = NULL;

/* Used for storing time */
static uint32_t g_dwFrameTotal = 0;

/* Stepped zoom table — same values as the map editor (mapeditor.c).
 * Below 1.0 the texture target grows so we can show more tiles, then
 * the blit scales it down; at and above 1.0 the texture target stays
 * sized to the visible tile count and the blit scales it up. */
static const float g_zoomSteps[] = {
    0.5f, 0.6f, 0.7f, 0.8f, 0.9f,
    1.0f, 2.0f, 3.0f, 4.0f
};
#define ZOOM_STEP_COUNT ((int)(sizeof(g_zoomSteps) / sizeof(g_zoomSteps[0])))
#define ZOOM_STEP_1X    5

static int   g_zoomStepIndex = ZOOM_STEP_1X;
static float g_zoomLevel     = 1.0f;


/* Last drawn map positions for dirty rect optimization.
 * Sized for the maximum map size (255 tiles in each direction). */
int lv_drawLast[256][256];

/* Access team colours and useTeamColours through LogViewerState */

/* Function prototypes */
BYTE lv_screenGetPillTeam(BYTE x, BYTE y, BYTE *pillHealth);
BYTE lv_screenGetBaseTeam(BYTE x, BYTE y);
extern void lv_windowNeedRedraw(void);

BYTE lv_windowGetZoomFactor(void) { return 1; }

void lv_drawDirtyScreen(void) {
    int count, count2;
    for (count = 0; count < 256; count++) {
        for (count2 = 0; count2 < 256; count2++) {
            lv_drawLast[count][count2] = 10000;
        }
    }
}

float lv_drawGetZoomLevel(void) {
    return g_zoomLevel;
}

/* Apply a stepped zoom change with the map tile under (mouseScreenX,
 * mouseScreenY) anchored — that tile stays under the cursor. The screen
 * coordinates are in window pixels (the same space SDL events report),
 * so we subtract the ImGui menu bar height to reach the game area. */
static void lv_drawApplyZoomStep(int newStepIndex, int mouseScreenX, int mouseScreenY) {
    float oldZoom, newZoom;
    int   windowW = 0, windowH = 0, gameH;
    int   menuH;
    BYTE  oldOffX = 0, oldOffY = 0;
    int   curTilesX, curTilesY;
    float gx, gy;
    float oldEffective, newEffective;
    float anchorTileX, anchorTileY;
    int   newTilesX, newTilesY;
    int   maxOffX, maxOffY, newOffX, newOffY;
    float newOffXf, newOffYf;
    BYTE  zf;
    float maxGx, maxGy;
    int   curTexW, curTexH;
    int   gameW;

    if (newStepIndex < 0) newStepIndex = 0;
    if (newStepIndex >= ZOOM_STEP_COUNT) newStepIndex = ZOOM_STEP_COUNT - 1;
    if (newStepIndex == g_zoomStepIndex) return;

    oldZoom = g_zoomLevel;
    newZoom = g_zoomSteps[newStepIndex];

    if (sdlWindow != NULL) SDL_GetWindowSize(sdlWindow, &windowW, &windowH);
    if (windowW < 1) windowW = TILE_SIZE_X;
    if (windowH < 1) windowH = TILE_SIZE_Y;
    menuH = (int)lv_imgui_get_menu_bar_height();
    gameH = windowH - menuH;
    if (gameH < 1) gameH = 1;

    /* Compute map tile under cursor at the OLD zoom. The blit width is
     * curTilesX * TILE_SIZE_X * oldZoom, and the cursor screen X covers
     * that same range, so each on-screen pixel == 1/oldZoom texture
     * pixels. We clamp to the rendered game extents so the anchor stays
     * within the loaded map even when the cursor sits outside it. */
    zf = lv_windowGetZoomFactor();
    curTilesX = lv_screenGetSizeX();
    curTilesY = lv_screenGetSizeY();
    lv_screenGetOffsets(&oldOffX, &oldOffY);
    int oldSubX = 0, oldSubY = 0;
    lv_screenGetSubOffset(&oldSubX, &oldSubY);

    oldEffective = (float)(zf * TILE_SIZE_X) * oldZoom;
    if (oldEffective <= 0.0f) oldEffective = (float)TILE_SIZE_X;

    gx = (float)mouseScreenX;
    gy = (float)mouseScreenY - (float)menuH;
    if (gx < 0.0f) gx = 0.0f;
    if (gy < 0.0f) gy = 0.0f;

    /* Use the actual rendered area (texture target * oldZoom) to clamp. */
    curTexW = targetWidth;
    curTexH = targetHeight;
    if (curTexW < 1) curTexW = curTilesX * TILE_SIZE_X;
    if (curTexH < 1) curTexH = curTilesY * TILE_SIZE_Y;
    maxGx = (float)curTexW * oldZoom;
    maxGy = (float)curTexH * oldZoom;
    if (gx > maxGx) gx = maxGx;
    if (gy > maxGy) gy = maxGy;

    /* Include the current sub-tile pan so the cursor anchors on the same
     * map pixel even when a partial drag is in flight. */
    anchorTileX = (float)oldOffX + (float)oldSubX / (float)TILE_SIZE_X + gx / oldEffective;
    anchorTileY = (float)oldOffY + (float)oldSubY / (float)TILE_SIZE_Y + gy / oldEffective;

    /* Compute new viewport tile count so the blit fills (approximately)
     * the available window area at the new zoom. Use the same nearest-
     * tile rounding the resize handler uses, just in zoomed pixel space. */
    gameW = windowW;
    newEffective = (float)(zf * TILE_SIZE_X) * newZoom;
    if (newEffective <= 0.0f) newEffective = (float)TILE_SIZE_X;
    newTilesX = (int)(((float)gameW + newEffective * 0.5f) / newEffective);
    newTilesY = (int)(((float)gameH + newEffective * 0.5f) / newEffective);
    if (newTilesX < 1) newTilesX = 1;
    if (newTilesY < 1) newTilesY = 1;
    if (newTilesX > 255) newTilesX = 255;
    if (newTilesY > 255) newTilesY = 255;

    /* Place the anchor tile back under the cursor at the new zoom. */
    newOffXf = anchorTileX - gx / newEffective;
    newOffYf = anchorTileY - gy / newEffective;

    maxOffX = 255 - newTilesX;
    maxOffY = 255 - newTilesY;
    if (maxOffX < 0) maxOffX = 0;
    if (maxOffY < 0) maxOffY = 0;
    newOffX = (int)(newOffXf + 0.5f);
    newOffY = (int)(newOffYf + 0.5f);
    if (newOffX < 0) newOffX = 0;
    if (newOffY < 0) newOffY = 0;
    if (newOffX > maxOffX) newOffX = maxOffX;
    if (newOffY > maxOffY) newOffY = maxOffY;

    /* Commit. lv_screenSetSizeX/Y reallocate the screen buffer and call
     * lv_drawResizeRenderTarget(); after that we set the new offset and
     * request a full redraw so the texture target gets repopulated. */
    g_zoomStepIndex = newStepIndex;
    g_zoomLevel = newZoom;

    if (newTilesX != curTilesX) lv_screenSetSizeX((BYTE)newTilesX);
    if (newTilesY != curTilesY) lv_screenSetSizeY((BYTE)newTilesY);

    lv_screenSetOffset((BYTE)newOffX, (BYTE)newOffY);
    /* Zoom anchors on the cursor's tile, not its sub-tile pixel; reset
     * sub-pan so the new viewport doesn't carry an offset from the
     * pre-zoom sub-pixel position. */
    lv_screenSetSubOffset(0, 0);
    lv_drawDirtyScreen();
    lv_windowNeedRedraw();
}

void lv_drawZoomIn(int mouseScreenX, int mouseScreenY) {
    lv_drawApplyZoomStep(g_zoomStepIndex + 1, mouseScreenX, mouseScreenY);
}

void lv_drawZoomOut(int mouseScreenX, int mouseScreenY) {
    lv_drawApplyZoomStep(g_zoomStepIndex - 1, mouseScreenX, mouseScreenY);
}

int lv_drawGetZoomStepIndex(void) {
    return g_zoomStepIndex;
}

int lv_drawGetZoomStepCount(void) {
    return ZOOM_STEP_COUNT;
}

float lv_drawGetZoomStepValue(int index) {
    if (index < 0 || index >= ZOOM_STEP_COUNT) return 1.0f;
    return g_zoomSteps[index];
}

void lv_drawSetZoomStep(int stepIndex, int mouseScreenX, int mouseScreenY) {
    lv_drawApplyZoomStep(stepIndex, mouseScreenX, mouseScreenY);
}

/* Build the unified tile atlas (SVG/PNG/BMP combined sheet) at scale 1.
 * Returns NULL on failure. The log viewer always uses scale 1 — the
 * blit-time scaling is in the texture target, not the source atlas. */
static SDL_Texture *buildTileAtlas(SDL_Renderer *renderer) {
    SDL_Surface *sheet = tileLoaderBuildSheet(TILE_SIZE_X);
    if (!sheet) return NULL;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, sheet);
    SDL_DestroySurface(sheet);
    if (!tex) return NULL;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    return tex;
}

/*********************************************************
*NAME:          lv_drawSetupWithHandles
*PURPOSE:
*  Sets up drawing using externally-provided window/renderer
*  (embedded mode). Does NOT create SDL window/renderer.
*  Loads textures, render target, and font.
*RETURNS:
*  TRUE on success, FALSE on failure
*********************************************************/
BYTE lv_drawSetupWithHandles(SDL_Window *window, SDL_Renderer *renderer) {
    lv_drawDirtyScreen();

    sdlWindow = window;
    sdlRenderer = renderer;
    ownsWindow = FALSE;

    /* +1 tile of margin in each dimension. The blit clips the margin
     * with a srcRect so the visible window is sizeX*TILE_SIZE_X by
     * sizeY*TILE_SIZE_Y; the margin only fills in the leading-edge
     * pixels revealed by sub-tile scrolling. */
    targetWidth  = (lv_screenGetSizeX() + 1) * TILE_SIZE_X;
    targetHeight = (lv_screenGetSizeY() + 1) * TILE_SIZE_Y;
    textureTarget = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_TARGET, targetWidth, targetHeight);
    if (!textureTarget) {
        return FALSE;
    }
    /* Tile background uses the unified atlas (SVG/PNG with skin.bmp
     * fallback) shared with the game and map editor. Tank, boat, and
     * pill/base sprites stay on their own BMPs because they use the
     * 16-row team-colour palette indexed via lv->tc[], which the
     * unified atlas does not provide. */
    textureTiles = buildTileAtlas(sdlRenderer);
    {
        const char *basePath = SDL_GetBasePath();
        if (!basePath) basePath = "./";
        char bmpPath[1024];

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/tanks.bmp", basePath);
        textureTanks = sdlLoadBmpAsTexture(sdlRenderer, bmpPath, true);

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/boats.bmp", basePath);
        textureBoats = sdlLoadBmpAsTexture(sdlRenderer, bmpPath, true);

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/items.bmp", basePath);
        textureItems = sdlLoadBmpAsTexture(sdlRenderer, bmpPath, true);
    }

    if (!textureTiles || !textureTanks || !textureBoats || !textureItems) {
        return FALSE;
    }
    {
        const char *fontBase = SDL_GetBasePath();
        if (!fontBase) fontBase = "./";
        char fontPath[1024];
        SDL_snprintf(fontPath, sizeof(fontPath), "%sdata/fonts/SarasaMonoSlabJ-Regular.ttf", fontBase);
        labelFont = TTF_OpenFont(fontPath, 10);
    }

    mapViewInit();
    return TRUE;
}

BYTE lv_drawSetup(void) {
    int width, height;

    ownsWindow = TRUE;
    lv_drawDirtyScreen();

    /* If already initialized, skip recreation entirely.
     * ImGui depends on the SDL window/renderer staying valid. */
    if (sdlRenderer != NULL && sdlWindow != NULL) {

        return TRUE;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE, "Error initializing SDL video", NULL);
        return FALSE;
    }
    if (!TTF_Init()) {
        SDL_Quit();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE, "Error initializing SDL_ttf", NULL);
        return FALSE;
    }
    /* Calculate window dimensions from screen tile size + menu bar space */
    #define IMGUI_MENU_BAR_HEIGHT 25
    width  = lv_screenGetSizeX() * TILE_SIZE_X;
    height = lv_screenGetSizeY() * TILE_SIZE_Y + IMGUI_MENU_BAR_HEIGHT;

    sdlWindow = SDL_CreateWindow("Log Viewer", width, height, SDL_WINDOW_RESIZABLE);
    if (sdlWindow == NULL) {
        TTF_Quit(); SDL_Quit();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE, "Error creating SDL window", NULL);
        return FALSE;
    }
    /* Set window icon from data/icons/logviewerr-icon.png (used on Linux/Windows
       taskbar; macOS uses the .icns bundle). */
    {
        SDL_IOStream *io = SDL_IOFromFile("data/icons/logviewerr-icon.png", "rb");
        if (!io) {
            const char *basePath = SDL_GetBasePath();
            if (!basePath) basePath = "./";
            char iconPath[1024];
            SDL_snprintf(iconPath, sizeof(iconPath), "%sdata/icons/logviewerr-icon.png", basePath);
            io = SDL_IOFromFile(iconPath, "rb");
        }
        if (io) {
            Sint64 fileSize = SDL_GetIOSize(io);
            if (fileSize > 0) {
                unsigned char *buf = (unsigned char *)SDL_malloc((size_t)fileSize);
                if (buf) {
                    SDL_ReadIO(io, buf, (size_t)fileSize);
                    int iw, ih, ch;
                    unsigned char *pixels = stbi_load_from_memory(buf, (int)fileSize, &iw, &ih, &ch, 4);
                    SDL_free(buf);
                    if (pixels) {
                        SDL_Surface *icon = SDL_CreateSurfaceFrom(iw, ih, SDL_PIXELFORMAT_RGBA32, pixels, iw * 4);
                        if (icon) {
                            SDL_SetWindowIcon(sdlWindow, icon);
                            SDL_DestroySurface(icon);
                        }
                        stbi_image_free(pixels);
                    }
                }
            }
            SDL_CloseIO(io);
        }
    }

    sdlRenderer = SDL_CreateRenderer(sdlWindow, NULL);
    if (!sdlRenderer) {
        SDL_DestroyWindow(sdlWindow); TTF_Quit(); SDL_Quit();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE, "Error creating SDL renderer", NULL);
        return FALSE;
    }
    SDL_SetRenderVSync(sdlRenderer, 1);

    /* See lv_drawSetupWithHandles for the +1 margin rationale. */
    targetWidth  = (lv_screenGetSizeX() + 1) * TILE_SIZE_X;
    targetHeight = (lv_screenGetSizeY() + 1) * TILE_SIZE_Y;
    textureTarget = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_TARGET, targetWidth, targetHeight);
    if (!textureTarget) {
        lv_drawCleanup();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE, "Error creating render target", NULL);
        return FALSE;
    }
    /* See lv_drawSetupWithHandles for the tile-vs-tank/items split. */
    textureTiles = buildTileAtlas(sdlRenderer);
    {
        const char *basePath = SDL_GetBasePath();
        if (!basePath) basePath = "./";
        char bmpPath[1024];

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/tanks.bmp", basePath);
        textureTanks = sdlLoadBmpAsTexture(sdlRenderer, bmpPath, true);

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/boats.bmp", basePath);
        textureBoats = sdlLoadBmpAsTexture(sdlRenderer, bmpPath, true);

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/items.bmp", basePath);
        textureItems = sdlLoadBmpAsTexture(sdlRenderer, bmpPath, true);
    }

    if (!textureTiles || !textureTanks || !textureBoats || !textureItems) {

        lv_drawCleanup();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE, "Error loading sprite bitmaps from data/ directory", NULL);
        return FALSE;
    }
    {
        const char *fontBase = SDL_GetBasePath();
        if (!fontBase) fontBase = "./";
        char fontPath[1024];
        SDL_snprintf(fontPath, sizeof(fontPath), "%sdata/fonts/SarasaMonoSlabJ-Regular.ttf", fontBase);
        labelFont = TTF_OpenFont(fontPath, 10);
    }

    mapViewInit();
    return TRUE;
}

void lv_drawCleanup(void) {
    if (textureTiles) { SDL_DestroyTexture(textureTiles); textureTiles = NULL; }
    if (textureTanks) { SDL_DestroyTexture(textureTanks); textureTanks = NULL; }
    if (textureBoats) { SDL_DestroyTexture(textureBoats); textureBoats = NULL; }
    if (textureItems) { SDL_DestroyTexture(textureItems); textureItems = NULL; }
    if (textureTarget) { SDL_DestroyTexture(textureTarget); textureTarget = NULL; }
    if (labelFont) { TTF_CloseFont(labelFont); labelFont = NULL; }
    if (ownsWindow == TRUE) {
        /* Only destroy renderer/window and quit SDL if we created them */
        if (sdlRenderer) { SDL_DestroyRenderer(sdlRenderer); sdlRenderer = NULL; }
        if (sdlWindow) { SDL_DestroyWindow(sdlWindow); sdlWindow = NULL; }
        TTF_Quit();
        /* Quit SDL video subsystem only (audio is used by Sound.c) */
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    } else {
        /* Embedded mode: don't destroy the main app's window/renderer */
        sdlRenderer = NULL;
        sdlWindow = NULL;
    }
    sdlInitialized = 0;  /* Reset initialization flag */
}

/*********************************************************
*NAME:          lv_drawResizeRenderTarget
*PURPOSE:
*  Recreates the render target texture when screen size changes.
*  This is critical for correct mouse coordinate mapping -
*  if the texture size doesn't match the screen size, SDL
*  will scale the texture, causing coordinate drift.
*********************************************************/
void lv_drawResizeRenderTarget(void) {
    int newWidth = (lv_screenGetSizeX() + 1) * TILE_SIZE_X;
    int newHeight = (lv_screenGetSizeY() + 1) * TILE_SIZE_Y;
    
    if (sdlRenderer == NULL) return;
    
    /* Check if size actually changed */
    if (newWidth == targetWidth && newHeight == targetHeight) {
        return;
    }
    
    /* Destroy old texture */
    if (textureTarget) {
        SDL_DestroyTexture(textureTarget);
        textureTarget = NULL;
    }
    
    /* Create new texture with correct size */
    targetWidth = newWidth;
    targetHeight = newHeight;
    textureTarget = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_TARGET, targetWidth, targetHeight);
    if (!textureTarget) {
        return;
    }
    
    /* Mark screen as dirty to force redraw */
    lv_drawDirtyScreen();
}

int lv_drawGetTargetWidth(void) { return targetWidth; }
int lv_drawGetTargetHeight(void) { return targetHeight; }

/* Static splash texture - loaded once and cached */
static SDL_Texture *textureSplash = NULL;
static int splashWidth = 800;
static int splashHeight = 600;

/*********************************************************
*NAME:          lv_drawSplashForImGui
*PURPOSE:
*  Draws the splash screen for the ImGui rendering loop.
*  This function clears the screen and draws the splash,
*  but does NOT present. ImGui will present after drawing UI.
*
*  Called from the main loop BEFORE ImGui renders.
*********************************************************/
void lv_drawSplashForImGui(void) {
    int x = 0, y = 0;
    SDL_FRect dstRect;
    int windowWidth, windowHeight;
    
    /* Ensure we're rendering to the screen, not to a texture target */
    SDL_SetRenderTarget(sdlRenderer, NULL);
    
    /* Get window size for centering */
    SDL_GetWindowSize(sdlWindow, &windowWidth, &windowHeight);
    
    /* Clear the screen first */
    SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
    SDL_RenderClear(sdlRenderer);
    
    /* Load splash texture if not already loaded */
    if (textureSplash == NULL) {
        const char *basePath = SDL_GetBasePath();
        if (!basePath) basePath = "./";
        char splashPath[1024];
        SDL_snprintf(splashPath, sizeof(splashPath), "%sdata/splash.bmp", basePath);
        textureSplash = sdlLoadBmpAsTexture(sdlRenderer, splashPath, false);
    }
    
    if (textureSplash) {
        /* Center the splash image */
        if (windowWidth > splashWidth) x = (windowWidth - splashWidth) / 2;
        if (windowHeight > splashHeight) y = (windowHeight - splashHeight) / 2;
        
        dstRect.x = (float)x;
        dstRect.y = (float)y;
        dstRect.w = (float)splashWidth;
        dstRect.h = (float)splashHeight;
        SDL_RenderTexture(sdlRenderer, textureSplash, NULL, &dstRect);
    }
    
    /* NOTE: Do NOT call SDL_RenderPresent here - ImGui will present after drawing UI */
}

/*********************************************************
*NAME:          lv_drawCleanupSplash
*PURPOSE:
*  Cleans up the cached splash texture
*********************************************************/
void lv_drawCleanupSplash(void) {
    if (textureSplash) {
        SDL_DestroyTexture(textureSplash);
        textureSplash = NULL;
    }
}

static void drawRenderTexture(SDL_Texture *texture, int srcX, int srcY, int srcW, int srcH, int dstX, int dstY) {
    SDL_FRect srcRect = { (float)srcX, (float)srcY, (float)srcW, (float)srcH };
    SDL_FRect dstRect = { (float)dstX, (float)dstY, (float)srcW, (float)srcH };
    SDL_RenderTexture(sdlRenderer, texture, &srcRect, &dstRect);
}

void lv_drawMainScreen(screen *value, screenMines *mineView, screenTanks *tks, screenGunsight *gs, screenBullets *sBullets, screenLgm *lgms, BYTE showPillLabels, BYTE showBaseLabels, int32_t srtDelay, BYTE isPillView, int edgeX, int edgeY, BYTE useCursor, BYTE cursorLeft, BYTE cursorTop) {
    bool done, isPill, isBase, shouldDraw;
    BYTE x, y, pos, zoomFactor, itc, pillHealth;
    int outputX, outputY;
    SDL_FRect dstRect;
    LogViewerState *lv = lv_screenGetState();

    (void)gs; (void)showPillLabels; (void)showBaseLabels; (void)srtDelay;
    (void)isPillView; (void)edgeX; (void)edgeY; (void)useCursor;
    (void)cursorLeft; (void)cursorTop;

    zoomFactor = lv_windowGetZoomFactor();
    SDL_SetRenderTarget(sdlRenderer, textureTarget);

    for (x = 0, y = 0, done = FALSE; !done; ) {
        pos = lv_screenGetPos(value, x, y);
        shouldDraw = (lv_drawLast[x][y] == 10000) ||
                     (lv_screenIsMine(mineView, x, y) && lv_drawLast[x][y] != (0 - pos)) ||
                     (!lv_screenIsMine(mineView, x, y) && lv_drawLast[x][y] != pos);
        
        if (shouldDraw) {
            lv_drawLast[x][y] = pos;
            isPill = isBase = FALSE;
            
            if (pos == PILL_EVIL_15 || (pos >= PILL_EVIL_14 && pos <= PILL_EVIL_0) || (pos >= PILL_GOOD_15 && pos <= PILL_GOOD_0)) {
                isPill = TRUE;
                lv_drawLast[x][y] = 10000;
            }
            if (pos == BASE_GOOD || pos == BASE_NEUTRAL || pos == BASE_EVIL) {
                isBase = TRUE;
                lv_drawLast[x][y] = 10000;
            }
            
            if (isPill && lv->useTeamColours) {
                itc = lv_screenGetPillTeam(x, y, &pillHealth);
                drawRenderTexture(textureItems, pillHealth * zoomFactor * TILE_SIZE_X, lv->tc[itc] * zoomFactor * TILE_SIZE_Y,
                    zoomFactor * TILE_SIZE_X, zoomFactor * TILE_SIZE_Y, zoomFactor * (x * TILE_SIZE_X), zoomFactor * (y * TILE_SIZE_Y));
            } else if (isBase && lv->useTeamColours) {
                itc = lv_screenGetBaseTeam(x, y);
                drawRenderTexture(textureItems, 16 * zoomFactor * TILE_SIZE_X, lv->tc[itc] * zoomFactor * TILE_SIZE_Y,
                    zoomFactor * TILE_SIZE_X, zoomFactor * TILE_SIZE_Y, zoomFactor * (x * TILE_SIZE_X), zoomFactor * (y * TILE_SIZE_Y));
            } else {
                outputX = mapViewPosX[pos];
                outputY = mapViewPosY[pos];
                drawRenderTexture(textureTiles, outputX, outputY, zoomFactor * TILE_SIZE_X, zoomFactor * TILE_SIZE_Y,
                    zoomFactor * (x * TILE_SIZE_X), zoomFactor * (y * TILE_SIZE_Y));
            }
            
            if (lv_screenIsMine(mineView, x, y)) {
                drawRenderTexture(textureTiles, zoomFactor * MINE_X, zoomFactor * MINE_Y,
                    zoomFactor * TILE_SIZE_X, zoomFactor * TILE_SIZE_Y,
                    zoomFactor * (x * TILE_SIZE_X), zoomFactor * (y * TILE_SIZE_Y));
                lv_drawLast[x][y] = -pos;
            }
        }
        
        /* Iterate sizeX+1 by sizeY+1 to paint the +1 margin column/row;
         * clipped from view by the final-blit srcRect. */
        if (++x > lv_screenGetSizeX()) { x = 0; y++; if (y > lv_screenGetSizeY()) done = TRUE; }
    }
    
    lv_drawShells(sBullets);
    lv_drawTanks(tks);
    lv_drawLGMs(lgms);
    
    SDL_SetRenderTarget(sdlRenderer, NULL);

    /* Get ImGui menu bar height to offset game rendering below it.
     * This prevents the game from drawing over the menu bar. */
    float menuBarHeight = 0.0f;
    menuBarHeight = lv_imgui_get_menu_bar_height();

    /* SDL expects client-area coordinates (0,0), not screen coordinates.
     * The rcWindow passed in contains screen coordinates which would offset
     * the drawing by the window position + title bar + menu bar.
     * We offset by menuBarHeight to leave space for ImGui's menu bar.
     * Width/height are scaled by g_zoomLevel; texture target stays at
     * native (1x) tile resolution.
     *
     * Sub-tile pan: the texture target is sized (sizeX+1, sizeY+1) tiles
     * and gets painted with a 1-tile margin on every edge. The srcRect
     * picks the visible (sizeX, sizeY)-tile slice starting at the
     * sub-pixel offset, so the leading edge reveals the margin instead
     * of empty pixels. */
    float gameW = (float)(zoomFactor * lv_screenGetSizeX() * TILE_SIZE_X) * g_zoomLevel;
    float gameH = (float)(zoomFactor * lv_screenGetSizeY() * TILE_SIZE_Y) * g_zoomLevel;
    SDL_FRect srcRect = {
        (float)lv->subPxX,
        (float)lv->subPxY,
        (float)(lv_screenGetSizeX() * TILE_SIZE_X),
        (float)(lv_screenGetSizeY() * TILE_SIZE_Y),
    };
    dstRect.x = 0.0f;
    dstRect.y = menuBarHeight;
    dstRect.w = gameW;
    dstRect.h = gameH;
    SDL_RenderTexture(sdlRenderer, textureTarget, &srcRect, &dstRect);
    
    /* NOTE: Don't call SDL_RenderPresent here - ImGui needs to render after the game
     * and present once at the end. Calling present here causes the game to overwrite
     * the ImGui menu bar each frame. ImGui's lv_imgui_context_render() handles the final present. */
}

void lv_drawMarkRedraw(int mx, int my, int px, int py, int itemSize) {
    if (mx < 0 || mx > 255 || my < 0 || my > 255) return;
    lv_drawLast[mx][my] = 10000;
    if (px > itemSize && mx < 255) { lv_drawLast[mx+1][my] = 10000; if (my < 255) lv_drawLast[mx+1][my+1] = 10000; }
    if (py > itemSize && my < 255) { lv_drawLast[mx][my+1] = 10000; if (mx < 255) lv_drawLast[mx+1][my+1] = 10000; }
}

int lv_drawGetFrameRate(void) { return (int)g_dwFrameTotal; }

void lv_drawShells(screenBullets *sBullets) {
    int total, count, x, y, srcX, srcY, srcW, srcH;
    BYTE px, py, frame, mx, my, zf;
    
    total = lv_screenBulletsGetNumEntries(sBullets);
    zf = lv_windowGetZoomFactor();
    
    for (count = 1; count <= total; count++) {
        lv_screenBulletsGetItem(sBullets, count, &mx, &my, &px, &py, &frame);
        x = (mx * zf * TILE_SIZE_X) + zf * px;
        y = (my * zf * TILE_SIZE_Y) + zf * py;
        
        switch (frame) {
        case SHELL_EXPLOSION8: srcX = zf*EXPLOSION8_X; srcY = zf*EXPLOSION8_Y; srcW = zf*TILE_SIZE_X; srcH = zf*TILE_SIZE_Y; break;
        case SHELL_EXPLOSION7: srcX = zf*EXPLOSION7_X; srcY = zf*EXPLOSION7_Y; srcW = zf*TILE_SIZE_X; srcH = zf*TILE_SIZE_Y; break;
        case SHELL_EXPLOSION6: srcX = zf*EXPLOSION6_X; srcY = zf*EXPLOSION6_Y; srcW = zf*TILE_SIZE_X; srcH = zf*TILE_SIZE_Y; break;
        case SHELL_EXPLOSION5: srcX = zf*EXPLOSION5_X; srcY = zf*EXPLOSION5_Y; srcW = zf*TILE_SIZE_X; srcH = zf*TILE_SIZE_Y; break;
        case SHELL_EXPLOSION4: srcX = zf*EXPLOSION4_X; srcY = zf*EXPLOSION4_Y; srcW = zf*TILE_SIZE_X; srcH = zf*TILE_SIZE_Y; break;
        case SHELL_EXPLOSION3: srcX = zf*EXPLOSION3_X; srcY = zf*EXPLOSION3_Y; srcW = zf*TILE_SIZE_X; srcH = zf*TILE_SIZE_Y; break;
        case SHELL_EXPLOSION2: srcX = zf*EXPLOSION2_X; srcY = zf*EXPLOSION2_Y; srcW = zf*TILE_SIZE_X; srcH = zf*TILE_SIZE_Y; break;
        case SHELL_EXPLOSION1: srcX = zf*EXPLOSION1_X; srcY = zf*EXPLOSION1_Y; srcW = zf*TILE_SIZE_X; srcH = zf*TILE_SIZE_Y; break;
        case SHELL_DIR0: srcX = zf*SHELL_0_X; srcY = zf*SHELL_0_Y; srcW = zf*SHELL_0_WIDTH; srcH = zf*SHELL_0_HEIGHT; break;
        case SHELL_DIR1: srcX = zf*SHELL_1_X; srcY = zf*SHELL_1_Y; srcW = zf*SHELL_1_WIDTH; srcH = zf*SHELL_1_HEIGHT; break;
        case SHELL_DIR2: srcX = zf*SHELL_2_X; srcY = zf*SHELL_2_Y; srcW = zf*SHELL_2_WIDTH; srcH = zf*SHELL_2_HEIGHT; break;
        case SHELL_DIR3: srcX = zf*SHELL_3_X; srcY = zf*SHELL_3_Y; srcW = zf*SHELL_3_WIDTH; srcH = zf*SHELL_3_HEIGHT; break;
        case SHELL_DIR4: srcX = zf*SHELL_4_X; srcY = zf*SHELL_4_Y; srcW = zf*SHELL_4_WIDTH; srcH = zf*SHELL_4_HEIGHT; break;
        case SHELL_DIR5: srcX = zf*SHELL_5_X; srcY = zf*SHELL_5_Y; srcW = zf*SHELL_5_WIDTH; srcH = zf*SHELL_5_HEIGHT; break;
        case SHELL_DIR6: srcX = zf*SHELL_6_X; srcY = zf*SHELL_6_Y; srcW = zf*SHELL_6_WIDTH; srcH = zf*SHELL_6_HEIGHT; break;
        case SHELL_DIR7: srcX = zf*SHELL_7_X; srcY = zf*SHELL_7_Y; srcW = zf*SHELL_7_WIDTH; srcH = zf*SHELL_7_HEIGHT; break;
        case SHELL_DIR8: srcX = zf*SHELL_8_X; srcY = zf*SHELL_8_Y; srcW = zf*SHELL_8_WIDTH; srcH = zf*SHELL_8_HEIGHT; break;
        case SHELL_DIR9: srcX = zf*SHELL_9_X; srcY = zf*SHELL_9_Y; srcW = zf*SHELL_9_WIDTH; srcH = zf*SHELL_9_HEIGHT; break;
        case SHELL_DIR10: srcX = zf*SHELL_10_X; srcY = zf*SHELL_10_Y; srcW = zf*SHELL_10_WIDTH; srcH = zf*SHELL_10_HEIGHT; break;
        case SHELL_DIR11: srcX = zf*SHELL_11_X; srcY = zf*SHELL_11_Y; srcW = zf*SHELL_11_WIDTH; srcH = zf*SHELL_11_HEIGHT; break;
        case SHELL_DIR12: srcX = zf*SHELL_12_X; srcY = zf*SHELL_12_Y; srcW = zf*SHELL_12_WIDTH; srcH = zf*SHELL_12_HEIGHT; break;
        case SHELL_DIR13: srcX = zf*SHELL_13_X; srcY = zf*SHELL_13_Y; srcW = zf*SHELL_13_WIDTH; srcH = zf*SHELL_13_HEIGHT; break;
        case SHELL_DIR14: srcX = zf*SHELL_14_X; srcY = zf*SHELL_14_Y; srcW = zf*SHELL_14_WIDTH; srcH = zf*SHELL_14_HEIGHT; break;
        default: srcX = zf*SHELL_15_X; srcY = zf*SHELL_15_Y; srcW = zf*SHELL_15_WIDTH; srcH = zf*SHELL_15_HEIGHT; break;
        }
        
        if (frame >= SHELL_EXPLOSION8 && frame <= SHELL_EXPLOSION1) lv_drawMarkRedraw(mx, my, px, py, 0);
        else lv_drawMarkRedraw(mx, my, px, py, 10);
        drawRenderTexture(textureTiles, srcX, srcY, srcW, srcH, x, y);
    }
}

void lv_drawTanks(screenTanks *tks) {
    int x, y, srcX, srcY;
    BYTE count, total, px, py, mx, my, team, zoomFactor, dir;
    bool onBoat;
    char playerName[PLAYER_NAME_LEN];
    LogViewerState *lv = lv_screenGetState();

    total = lv_screenTanksGetNumEntries(tks);
    zoomFactor = lv_windowGetZoomFactor();

    for (count = 1; count <= total; count++) {
        lv_screenTanksGetItem(tks, count, &mx, &my, &px, &py, NULL, &team, &dir, &onBoat, playerName);
        px += 2; py += 2;
        x = mx * (zoomFactor * TILE_SIZE_X) + (zoomFactor * px);
        y = my * (zoomFactor * TILE_SIZE_Y) + (zoomFactor * py);

        if (lv->useTeamColours) {
            srcX = zoomFactor * TILE_SIZE_X * dir;
            srcY = zoomFactor * TILE_SIZE_Y * lv->tc[team];
            drawRenderTexture(onBoat ? textureBoats : textureTanks, srcX, srcY, zoomFactor * TILE_SIZE_X, zoomFactor * TILE_SIZE_Y, x, y);
        } else {
            /* Simplified: use direction-based sprite selection for non-team mode */
            srcX = zoomFactor * TILE_SIZE_X * dir;
            srcY = zoomFactor * TILE_SIZE_Y * lv->tc[team];
            drawRenderTexture(onBoat ? textureBoats : textureTanks, srcX, srcY, zoomFactor * TILE_SIZE_X, zoomFactor * TILE_SIZE_Y, x, y);
        }
        lv_drawTankLabel(playerName, mx, my, px, py);
        lv_drawMarkRedraw(mx, my, px, py, 0);
    }
}

void lv_drawLGMs(screenLgm *lgms) {
    BYTE total, count, frame, mx, my, px, py, zf;
    int x, y, srcX, srcY, srcW, srcH;
    
    total = lv_screenLgmGetNumEntries(lgms);
    zf = lv_windowGetZoomFactor();
    
    for (count = 1; count <= total; count++) {
        lv_screenLgmGetItem(lgms, count, &mx, &my, &px, &py, &frame);
        switch (frame) {
        case LGM0: srcX = zf*LGM0_X; srcY = zf*LGM0_Y; srcW = zf*LGM_WIDTH; srcH = zf*LGM_HEIGHT; lv_drawMarkRedraw(mx,my,px,py,12); break;
        case LGM1: srcX = zf*LGM1_X; srcY = zf*LGM1_Y; srcW = zf*LGM_WIDTH; srcH = zf*LGM_HEIGHT; lv_drawMarkRedraw(mx,my,px,py,12); break;
        case LGM2: srcX = zf*LGM2_X; srcY = zf*LGM2_Y; srcW = zf*LGM_WIDTH; srcH = zf*LGM_HEIGHT; lv_drawMarkRedraw(mx,my,px,py,12); break;
        default: srcX = zf*LGM_HELICOPTER_X; srcY = zf*LGM_HELICOPTER_Y; srcW = zf*TILE_SIZE_X; srcH = zf*TILE_SIZE_Y; lv_drawMarkRedraw(mx,my,px,py,1); break;
        }
        x = (zf * mx * TILE_SIZE_X) + (zf * px);
        y = (zf * my * TILE_SIZE_Y) + (zf * py);
        drawRenderTexture(textureTiles, srcX, srcY, srcW, srcH, x, y);
    }
}

void lv_drawTankLabel(char *str, int mx, int my, BYTE px, BYTE py) {
    SDL_Surface *surface;
    SDL_Texture *texture;
    int len, x, y, width = 5, count1, count2;
    SDL_FRect dstRect;
    SDL_Color color = {255, 255, 255, 255};
    BYTE zf;
    
    if (!labelFont || !str || (len = (int)strlen(str)) == 0) return;
    
    zf = lv_windowGetZoomFactor();
    /* SDL3_ttf: TTF_RenderText_Blended renders with transparent background and proper alpha anti-aliasing.
     * This avoids the green fringe issue that occurred with TTF_RenderText_Shaded + color key,
     * since anti-aliased edge pixels were blended with green and didn't match the exact color key. */
    surface = TTF_RenderText_Blended(labelFont, str, len, color);
    if (!surface) return;
    
    texture = SDL_CreateTextureFromSurface(sdlRenderer, surface);
    if (!texture) { SDL_DestroySurface(surface); return; }
    
    x = (mx+1) * zf * TILE_SIZE_X + zf * (px+1);
    y = my * zf * TILE_SIZE_Y + zf * py;
    
    dstRect.x = (float)x; dstRect.y = (float)y;
    dstRect.w = (float)surface->w; dstRect.h = (float)surface->h;
    
    if ((x + surface->w) > zf * lv_screenGetSizeX() * TILE_SIZE_X) dstRect.w = zf * lv_screenGetSizeX() * TILE_SIZE_X - x;
    if ((y + surface->h) > lv_screenGetSizeY() * (zf * TILE_SIZE_Y)) dstRect.h = zf * (lv_screenGetSizeY() * TILE_SIZE_Y - y);
    
    SDL_RenderTexture(sdlRenderer, texture, NULL, &dstRect);
    SDL_DestroyTexture(texture);
    SDL_DestroySurface(surface);
    
    if (len > 6) width = 25;
    for (count1 = 0; count1 < 2; count1++) {
        for (count2 = 0; count2 < width; count2++) {
            lv_drawMarkRedraw(mx+count2, my+count1, px, py, 10);
        }
    }
}

/*********************************************************
*NAME:          lv_drawBlitGameTexture
*PURPOSE:
*  Blits the game render texture to the screen without
*  re-rendering the game. Used when the game state hasn't
*  changed but we need to display the last frame.
*********************************************************/
void lv_drawBlitGameTexture(void) {
    SDL_FRect dstRect;
    float menuBarHeight = 0.0f;
    BYTE zoomFactor = lv_windowGetZoomFactor();
    LogViewerState *lv = lv_screenGetState();

    if (!textureTarget || !sdlRenderer) return;

    /* Get ImGui menu bar height to offset game rendering below it */
    menuBarHeight = lv_imgui_get_menu_bar_height();

    /* Blit the game texture to the screen, scaled by the user zoom level.
     * Sub-tile pan: srcRect picks the visible (sizeX, sizeY)-tile slice
     * starting at the sub-pixel offset within the (sizeX+1, sizeY+1)
     * texture target — the +1 margin is what fills the leading edge.
     * See lv_drawMainScreen for the matching paint. */
    float gameW = (float)(zoomFactor * lv_screenGetSizeX() * TILE_SIZE_X) * g_zoomLevel;
    float gameH = (float)(zoomFactor * lv_screenGetSizeY() * TILE_SIZE_Y) * g_zoomLevel;
    SDL_FRect srcRect = {
        (float)lv->subPxX,
        (float)lv->subPxY,
        (float)(lv_screenGetSizeX() * TILE_SIZE_X),
        (float)(lv_screenGetSizeY() * TILE_SIZE_Y),
    };
    dstRect.x = 0.0f;
    dstRect.y = menuBarHeight;
    dstRect.w = gameW;
    dstRect.h = gameH;
    SDL_RenderTexture(sdlRenderer, textureTarget, &srcRect, &dstRect);
}

/*********************************************************
*NAME:          lv_drawGetSDLWindow
*PURPOSE:
*  Returns the SDL window handle for ImGui integration
*********************************************************/
SDL_Window* lv_drawGetSDLWindow(void) {
    return sdlWindow;
}

/*********************************************************
*NAME:          lv_drawGetSDLRenderer
*PURPOSE:
*  Returns the SDL renderer handle for ImGui integration
*********************************************************/
SDL_Renderer* lv_drawGetSDLRenderer(void) {
    return sdlRenderer;
}

/*********************************************************
*NAME:          lv_drawGetGameTexture
*PURPOSE:
*  Returns the game render texture for ImGui viewport display
*********************************************************/
SDL_Texture* lv_drawGetGameTexture(void) {
    return textureTarget;
}
