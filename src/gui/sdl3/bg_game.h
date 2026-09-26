/*
 * Copyright (c) 1998-2026 John Morrison.
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
 * Name:          bg_game.h
 * Purpose:
 *   Background game rendering for the welcome screen.
 *   Loads a map via ServerSim, runs brain bots, and
 *   renders live game state behind the ImGui welcome UI.
 *********************************************************/

#ifndef BG_GAME_H
#define BG_GAME_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include "server_sim.h"
#include "gfx_settings.h"
#include "sprite_atlas.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BgGame {
    ServerSim   *sim;
    bool         valid;         /* true if sim was loaded successfully */
    SDL_Texture *tilesTex;      /* Own tile atlas for bg rendering */
    /* Renderer the texture was created against. SDL3 invalidates child
     * textures when its renderer is destroyed, so the next bgGameRender
     * checks this against sdl3DrawGetRenderer() and rebuilds tilesTex
     * when they differ. NULL = uninitialised (no texture has been
     * built yet). */
    SDL_Renderer *texRenderer;
    /* Tile-atlas generation tilesTex was built from. A skin change
     * rebuilds the shared atlas without touching the renderer, so this
     * is what tells this copy it is stale. */
    unsigned int  tilesGeneration;
    /* Texture filter tilesTex was last set to. A change to the setting
     * only needs the scale mode re-applied, not a new sheet, so this is
     * checked separately from the generation. */
    GfxTextureFilter tilesFilter;
    /* The padded copy of the moving sprites, off the same sheet as
     * tilesTex and on the same renderer, so a filtered sample does not
     * blend in the sprite packed next door (sprite_atlas.h). Built and
     * dropped with tilesTex; both NULL draws the sprites from the sheet. */
    SDL_Texture *spritesTex;
    SpriteAtlas *spriteAtlas;
    BYTE         cameraPlayer;  /* Player slot to follow with camera */
    int          zoomUser;      /* User zoom factor from the +/- keys.
                                 * 0 = follow bgGameRender's fit-to-screen
                                 * pick, which is the startup state. */
    int          lastZoom;      /* Zoom factor the last render actually
                                 * used, so the first +/- press can step
                                 * from the fit the viewer is looking at. */
    WORLD        viewCenterX;   /* Camera world position */
    WORLD        viewCenterY;
    BYTE         numBots;       /* Number of bots added */
    BYTE         numTeams;      /* Number of teams (0 = FFA) */
    bool         paused;        /* User-toggled pause state (persists across dialogs) */
    bool         hiddenByForeground;  /* true while a foreground SP/host game is active —
                                       * the bg tick early-returns so the bg doesn't dispatch
                                       * brains to the shared worker pool. Independent of
                                       * the user-pause flag (paused), which only drives
                                       * the map-name overlay fade. */
    Uint64       createdTicks;  /* SDL_GetTicks() at creation, for map name fade */
    Uint64       mapNameFadeStartMs;   /* 0 = use initial 10s timer; nonzero = pause-driven fade from this tick */
    Uint8        mapNameFadeFromAlpha; /* Starting alpha for the active pause-driven fade */
    /* Bounding box of map content (map coordinates) */
    int          mapMinX, mapMinY, mapMaxX, mapMaxY;
    /* Camera centre and tank positions after the last two sim ticks, and
     * the scheduled time (SDL_GetTicks ms) of the last one, so
     * bgGameRender can draw in between. interpValid is false until the
     * first tick records anything. */
    bool         interpValid;
    Uint64       interpTickMs;
    WORLD        camPrevX, camPrevY, camCurX, camCurY;
    bool         tankHave[MAX_TANKS];       /* alive at the last tick */
    WORLD        tankPrevX[MAX_TANKS], tankPrevY[MAX_TANKS];
    WORLD        tankCurX[MAX_TANKS], tankCurY[MAX_TANKS];
} BgGame;

bool bgGameCreate(BgGame *bg, const char *mapFile, SDL_Renderer *renderer);
void bgGameDestroy(BgGame *bg);
void bgGameRender(BgGame *bg, SDL_Renderer *renderer, int screenW, int screenH);

/* Convenience: tick at fixed 50 Hz rate using a running timestamp */
void bgGameTickFixed(BgGame *bg, Uint64 *lastTickTime);

/* Convenience: render background game + semi-transparent dark overlay */
void bgGameRenderWithOverlay(BgGame *bg, SDL_Renderer *renderer, int screenW, int screenH);

/* Toggle pause; also kicks off a fade-in (paused) or fade-out (unpaused)
 * of the map-name label, starting from its current visible alpha. */
void bgGameTogglePause(BgGame *bg);

/* Mark bg as hidden by a foreground game (SP or host). While hidden,
 * bgGameTickFixed runs no sim ticks. Independent of bgGameTogglePause. */
void bgGameSetHiddenByForeground(BgGame *bg, bool hidden);

/* Point the camera at the next occupied tank slot, wrapping from the last
 * back to the first. No-op when the sim holds no tanks. */
void bgGameCycleCamera(BgGame *bg);

/* Step the user zoom by delta whole zoom factors (clamped), starting from
 * the fit-to-screen factor the last render used. bgGameResetZoom puts it
 * back on the fit. */
void bgGameAdjustZoom(BgGame *bg, int delta);
void bgGameResetZoom(BgGame *bg);

/* Shared background game instance used across all pre-game dialogs */
void bgGameSetShared(BgGame *bg);
BgGame *bgGameGetShared(void);

#ifdef __cplusplus
}
#endif

#endif /* BG_GAME_H */
