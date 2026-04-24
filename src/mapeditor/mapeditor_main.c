/*
 * Copyright (c) 1998-2008 John Morrison.
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
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "mapeditor.h"

#define DEFAULT_WINDOW_W 1280
#define DEFAULT_WINDOW_H 800

int main(int argc, char *argv[]) {
    srand((unsigned int)(time(NULL) ^ getpid()));

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

    /* Create window */
    SDL_Window *window = SDL_CreateWindow("WinBolo Map Editor",
                                          DEFAULT_WINDOW_W, DEFAULT_WINDOW_H,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

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
        SDL_Log("Warning: TTF_Init failed: %s", SDL_GetError());
        /* Continue — bitmap fonts still work, TTF just won't be available */
    }

    /* Run the editor */
    mapEditorRun(window, renderer, mapPath, false);

    /* Cleanup */
    TTF_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
