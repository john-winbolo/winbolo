/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_main.c
 * Purpose:
 *   Standalone binary entry point for the WinBolo Map
 *   Editor. Creates an SDL window/renderer and hands off
 *   to mapEditorRun().
 *********************************************************/

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../common/wb_log.h"
#include "../common/prefs.h"
#include "../server/threads.h"
#include "bolo_rand.h"
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "mapeditor.h"
#include "../third_party/stb/stb_image.h"

#define DEFAULT_WINDOW_W 1280
#define DEFAULT_WINDOW_H 800

/* Set the window icon (taskbar / title bar) from data/icons/mapeditor-icon.png.
 * macOS Cocoa ignores this — the .icns in the bundle drives the Dock icon. */
static void mapEditorSetWindowIcon(SDL_Window *window) {
    if (!window) return;

    SDL_IOStream *io = SDL_IOFromFile("data/icons/mapeditor-icon.png", "rb");
    if (!io) {
        const char *base = SDL_GetBasePath();
        if (!base) base = "./";
        char path[1024];
        SDL_snprintf(path, sizeof(path), "%sdata/icons/mapeditor-icon.png", base);
        io = SDL_IOFromFile(path, "rb");
    }
    if (!io) return;

    Sint64 fileSize = SDL_GetIOSize(io);
    if (fileSize <= 0) { SDL_CloseIO(io); return; }
    unsigned char *buf = (unsigned char *)SDL_malloc((size_t)fileSize);
    if (!buf) { SDL_CloseIO(io); return; }
    SDL_ReadIO(io, buf, (size_t)fileSize);
    SDL_CloseIO(io);

    int w, h, channels;
    unsigned char *pixels = stbi_load_from_memory(buf, (int)fileSize, &w, &h, &channels, 4);
    SDL_free(buf);
    if (!pixels) return;

    SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    if (surf) {
        SDL_SetWindowIcon(window, surf);
        SDL_DestroySurface(surf);
    }
    stbi_image_free(pixels);
}

int main(int argc, char *argv[]) {
    bolo_srand((uint64_t)time(NULL) ^ (uint64_t)getpid());

    const char *mapPath = NULL;
    if (argc > 1) {
        mapPath = argv[1];
    }

    fprintf(stderr, "WinBolo Map Editor\n");
    if (mapPath) {
        fprintf(stderr, "  Map: %s\n", mapPath);
    } else {
        fprintf(stderr, "  Map: (new blank map)\n");
    }

    /* Initialize SDL */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    threadsCreate(FALSE);

    /* Load the process-global preferences document (WinBolo.json) before
     * the editor reads its recent-files list. */
    {
        char prefsPath[1024];
        const char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
        if (prefDir) {
            SDL_snprintf(prefsPath, sizeof(prefsPath), "%sWinBolo.json", prefDir);
        } else {
            SDL_snprintf(prefsPath, sizeof(prefsPath), "%s", "WinBolo.json");
        }
        prefsInit(prefsPath);
    }

    /* Create window.
     *
     * HIGH_PIXEL_DENSITY: without it SDL sets the layer's contentsScale to 1
     * (SDL_cocoametalview.m), so on a Retina display the whole window is
     * rendered at point resolution and the compositor bilinear-upscales it —
     * text, chrome and canvas alike. NSHighResolutionCapable is already true
     * in the bundle plist; this flag is what makes SDL use the backing size.
     * Canvas code takes its viewport from SDL_GetRenderOutputSize and scales
     * pointer coordinates by SDL_GetWindowPixelDensity to match. */
    SDL_Window *window = SDL_CreateWindow("WinBolo Map Editor",
                                          DEFAULT_WINDOW_W, DEFAULT_WINDOW_H,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED |
                                          SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    mapEditorSetWindowIcon(window);

    /* Create renderer with vsync */
    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    /* Initialize SDL_ttf for TrueType font support */
    if (!TTF_Init()) {
        WB_LOG_WARN(WB_LOG_CAT_ASSET, "TTF_Init failed: %s", SDL_GetError());
        /* Continue — bitmap fonts still work, TTF just won't be available */
    }

    /* Run the editor */
    mapEditorRun(window, renderer, mapPath, false);

    /* Cleanup */
    TTF_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    threadsDestroy();
    SDL_Quit();
    return 0;
}
