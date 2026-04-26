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
#include "draw_setup_arrays.h"
#include "logviewer.h"
#include "imgui/imgui_main_menu.h"

/* Must be included after global.h to avoid bool type conflict */
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#define TEAM_IMAGE_SIZE_X (16 * 16)
#define TEAM_IMAGE_SIZE_Y (17 * 16)
#define ITEM_IMAGE_SIZE_X (17 * 16)
#define ITEM_IMAGE_SIZE_Y (17 * 16)

/* Color key for transparency (green) */
#define COLOR_KEY_R 0
#define COLOR_KEY_G 255
#define COLOR_KEY_B 0

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


/* Last drawn map positions for dirty rect optimization.
 * Sized for the maximum map size (255 tiles in each direction). */
int lv_drawLast[256][256];

/* Access team colours and useTeamColours through LogViewerState */

/* Function prototypes */
BYTE lv_screenGetPillTeam(BYTE x, BYTE y, BYTE *pillHealth);
BYTE lv_screenGetBaseTeam(BYTE x, BYTE y);

BYTE lv_windowGetZoomFactor(void) { return 1; }

void lv_drawDirtyScreen(void) {
    int count, count2;
    for (count = 0; count < 256; count++) {
        for (count2 = 0; count2 < 256; count2++) {
            lv_drawLast[count][count2] = 10000;
        }
    }
}

/* Load a BMP file from the filesystem and create an SDL texture.
 * Applies green (0,255,0) color key for sprite transparency. */
static SDL_Texture *loadTextureFromFile(const char *filename, SDL_Renderer *renderer) {
    SDL_Surface *surface;
    SDL_Surface *converted;
    SDL_Texture *texture;
    const SDL_PixelFormatDetails *fmt;
    Uint32 colorKey;

    surface = SDL_LoadBMP(filename);
    if (!surface) {

        return NULL;
    }

    /* Convert to ARGB8888 so color key transparency works with alpha blending */
    converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(surface);
    if (!converted) {
        return NULL;
    }

    fmt = SDL_GetPixelFormatDetails(converted->format);
    colorKey = SDL_MapRGB(fmt, NULL, COLOR_KEY_R, COLOR_KEY_G, COLOR_KEY_B);
    SDL_SetSurfaceColorKey(converted, true, colorKey);

    texture = SDL_CreateTextureFromSurface(renderer, converted);
    SDL_DestroySurface(converted);
    if (!texture) {
        return NULL;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return texture;
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

    targetWidth  = lv_screenGetSizeX() * TILE_SIZE_X;
    targetHeight = lv_screenGetSizeY() * TILE_SIZE_Y;
    textureTarget = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_TARGET, targetWidth, targetHeight);
    if (!textureTarget) {
        return FALSE;
    }
    {
        const char *basePath = SDL_GetBasePath();
        if (!basePath) basePath = "./";
        char bmpPath[1024];

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/tile.bmp", basePath);
        textureTiles = loadTextureFromFile(bmpPath, sdlRenderer);

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/tanks.bmp", basePath);
        textureTanks = loadTextureFromFile(bmpPath, sdlRenderer);

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/boats.bmp", basePath);
        textureBoats = loadTextureFromFile(bmpPath, sdlRenderer);

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/items.bmp", basePath);
        textureItems = loadTextureFromFile(bmpPath, sdlRenderer);
    }

    if (!textureTiles || !textureTanks || !textureBoats || !textureItems) {
        return FALSE;
    }
    {
        const char *fontBase = SDL_GetBasePath();
        if (!fontBase) fontBase = "./";
        char fontPath[1024];
        SDL_snprintf(fontPath, sizeof(fontPath), "%sdata/fonts/CourierPrime-Regular.ttf", fontBase);
        labelFont = TTF_OpenFont(fontPath, 10);
    }

    lv_drawSetupArrays(1);
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
    /* Set window icon from logo.bmp (used on Linux taskbar; macOS uses the .icns bundle) */
    {
        const char *basePath = SDL_GetBasePath();
        if (!basePath) basePath = "./";
        char iconPath[1024];
        SDL_snprintf(iconPath, sizeof(iconPath), "%sdata/logo.bmp", basePath);
        SDL_Surface *icon = SDL_LoadBMP(iconPath);
        if (icon) {
            SDL_SetWindowIcon(sdlWindow, icon);
            SDL_DestroySurface(icon);
        }
    }

    sdlRenderer = SDL_CreateRenderer(sdlWindow, NULL);
    if (!sdlRenderer) {
        SDL_DestroyWindow(sdlWindow); TTF_Quit(); SDL_Quit();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE, "Error creating SDL renderer", NULL);
        return FALSE;
    }
    SDL_SetRenderVSync(sdlRenderer, 1);

    targetWidth  = lv_screenGetSizeX() * TILE_SIZE_X;
    targetHeight = lv_screenGetSizeY() * TILE_SIZE_Y;
    textureTarget = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_TARGET, targetWidth, targetHeight);
    if (!textureTarget) {
        lv_drawCleanup();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE, "Error creating render target", NULL);
        return FALSE;
    }
    {
        const char *basePath = SDL_GetBasePath();
        if (!basePath) basePath = "./";
        char bmpPath[1024];

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/tile.bmp", basePath);
        textureTiles = loadTextureFromFile(bmpPath, sdlRenderer);

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/tanks.bmp", basePath);
        textureTanks = loadTextureFromFile(bmpPath, sdlRenderer);

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/boats.bmp", basePath);
        textureBoats = loadTextureFromFile(bmpPath, sdlRenderer);

        SDL_snprintf(bmpPath, sizeof(bmpPath), "%sdata/items.bmp", basePath);
        textureItems = loadTextureFromFile(bmpPath, sdlRenderer);
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
        SDL_snprintf(fontPath, sizeof(fontPath), "%sdata/fonts/CourierPrime-Regular.ttf", fontBase);
        labelFont = TTF_OpenFont(fontPath, 10);
    }

    lv_drawSetupArrays(1);
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
    int newWidth = lv_screenGetSizeX() * TILE_SIZE_X;
    int newHeight = lv_screenGetSizeY() * TILE_SIZE_Y;
    
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
        SDL_Surface *surface = SDL_LoadBMP(splashPath);
        if (surface) {
            textureSplash = SDL_CreateTextureFromSurface(sdlRenderer, surface);
            SDL_DestroySurface(surface);
        }
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
            
            if ((pos >= PILL_EVIL_0 && pos <= PILL_EVIL_15) || (pos >= PILL_GOOD_0 && pos <= PILL_GOOD_15)) {
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
                outputX = lv_drawPosX[pos];
                outputY = lv_drawPosY[pos];
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
        
        if (++x == lv_screenGetSizeX()) { x = 0; y++; if (y == lv_screenGetSizeY()) done = TRUE; }
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
     * We offset by menuBarHeight to leave space for ImGui's menu bar. */
    dstRect.x = 0.0f;
    dstRect.y = menuBarHeight;  /* Offset below ImGui menu bar */
    dstRect.w = (float)(zoomFactor * lv_screenGetSizeX() * TILE_SIZE_X);
    dstRect.h = (float)(zoomFactor * lv_screenGetSizeY() * TILE_SIZE_Y);
    SDL_RenderTexture(sdlRenderer, textureTarget, NULL, &dstRect);
    
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
    
    if (!textureTarget || !sdlRenderer) return;
    
    /* Get ImGui menu bar height to offset game rendering below it */
    menuBarHeight = lv_imgui_get_menu_bar_height();
    
    /* Blit the game texture to the screen */
    dstRect.x = 0.0f;
    dstRect.y = menuBarHeight;
    dstRect.w = (float)(zoomFactor * lv_screenGetSizeX() * TILE_SIZE_X);
    dstRect.h = (float)(zoomFactor * lv_screenGetSizeY() * TILE_SIZE_Y);
    SDL_RenderTexture(sdlRenderer, textureTarget, NULL, &dstRect);
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
