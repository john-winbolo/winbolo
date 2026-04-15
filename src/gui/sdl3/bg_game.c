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
 * Name:          bg_game.c
 * Purpose:
 *   Background game rendering for the welcome screen.
 *   Loads a map, runs brain bots, and renders terrain,
 *   pillboxes, bases, tanks, shells, and explosions.
 *********************************************************/

#include "bg_game.h"
#include "mapview.h"
#include "tileloader.h"
#include "../../bolo/global.h"
#include "../../bolo/everard_map.h"
#include "../../bolo/tank.h"
#include "../../bolo/bot_manager.h"
#include "../../bolo/players.h"
#include "../../bolo/bolo_map.h"
#include "../../bolo/pillbox.h"
#include "../../bolo/bases.h"
#include "../../bolo/starts.h"
#include "../../bolo/allience.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Shared background game instance for pre-game dialogs */
static BgGame *sharedBg = NULL;

void bgGameSetShared(BgGame *bg) { sharedBg = bg; }
BgGame *bgGameGetShared(void) { return sharedBg; }

/* Bot player count range */
#define BG_MIN_BOTS 2
#define BG_MAX_BOTS 2

/* Brain script path */
#define BG_BRAIN_PATH "Brains/NewAutopilot/init.lua"

/* Find the brain script — try several paths.
 * Uses SDL_IOFromFile so it works with Android APK assets. */
static bool findBrainPath(char *out, size_t outLen) {
    const char *candidates[] = {
        "Brains/NewAutopilot/init.lua",
        "brains/NewAutopilot/init.lua",
        "data/Brains/NewAutopilot/init.lua",
    };
    for (int i = 0; i < (int)(sizeof(candidates)/sizeof(candidates[0])); i++) {
        SDL_IOStream *io = SDL_IOFromFile(candidates[i], "r");
        if (io) {
            SDL_CloseIO(io);
            SDL_snprintf(out, outLen, "%s", candidates[i]);
            return true;
        }
    }
    return false;
}

bool bgGameCreate(BgGame *bg, const char *mapFile, SDL_Renderer *renderer) {
    SDL_memset(bg, 0, sizeof(*bg));
    bg->valid = false;

    SDL_Log("[BgGame] Creating with map: %s", mapFile);

    /* Try loading the map file. On Android, fopen can't read APK assets,
     * so use SDL_LoadFile to extract to a temp file, then use the proven
     * fopen-based serverSimCreate on the temp file. */
    bool mapLoaded = false;
    if (mapFile && mapFile[0]) {
        /* First try direct fopen-based load (works on desktop) */
        if (serverSimCreate(&bg->sim, (char *)mapFile, gameTournament, false, 0, -1)) {
            mapLoaded = true;
        } else {
            /* Direct load failed — try SDL_LoadFile (Android APK assets) → temp file */
            size_t fileSize = 0;
            void *fileData = SDL_LoadFile(mapFile, &fileSize);
            if (fileData && fileSize > 0) {
                const char *tmpDir = SDL_GetPrefPath("WinBolo", "WinBolo");
                char tmpPath[512];
                SDL_snprintf(tmpPath, sizeof(tmpPath), "%s_bg_temp.map", tmpDir ? tmpDir : "");
                FILE *fp = fopen(tmpPath, "wb");
                if (fp) {
                    fwrite(fileData, 1, fileSize, fp);
                    fclose(fp);
                    mapLoaded = serverSimCreate(&bg->sim, tmpPath, gameTournament, false, 0, -1);
                    if (!mapLoaded) {
                        SDL_Log("[BgGame] serverSimCreate failed for temp file '%s'", tmpPath);
                    }
                    remove(tmpPath);
                }
                SDL_free(fileData);
            } else {
                SDL_Log("[BgGame] SDL_LoadFile failed for '%s'", mapFile);
            }
        }
    }
    if (!mapLoaded) {
        /* Fall back to embedded Everard Island */
        BYTE emap[6000] = E_MAP;
        SDL_Log("[BgGame] Falling back to embedded Everard Island");
        if (!serverSimCreateCompressed(&bg->sim, emap, 5097, gameTournament, false, 0, -1)) {
            SDL_Log("[BgGame] serverSimCreateCompressed also failed");
            return false;
        }
        strncpy(bg->sim.mapName, "Everard Island", MAP_STR_SIZE - 1);
        bg->sim.mapName[MAP_STR_SIZE - 1] = '\0';
    }
    /* bg_game is a local headless sim — no lobby, run immediately */
    bg->sim.lobbyEnabled = false;
    bg->sim.state = serverStateRunning;

    SDL_Surface *sheet = tileLoaderBuildSheet(16);
    if (!sheet) {
        SDL_Log("[BgGame] tileLoaderBuildSheet failed");
        serverSimDestroy(&bg->sim);
        return false;
    }
    bg->tilesTex = SDL_CreateTextureFromSurface(renderer, sheet);
    SDL_DestroySurface(sheet);
    if (bg->tilesTex) {
        SDL_SetTextureScaleMode(bg->tilesTex, SDL_SCALEMODE_NEAREST);
    }
    if (!bg->tilesTex) {
        SDL_Log("[BgGame] SDL_CreateTextureFromSurface failed");
        serverSimDestroy(&bg->sim);
        return false;
    }

    bg->valid = true;
    bg->createdTicks = SDL_GetTicks();

    /* Compute bounding box of map content (non-ocean terrain + pills/bases/starts) */
    {
        GameSim *gs = &bg->sim.sim;
        int minX = 255, minY = 255, maxX = 0, maxY = 0;

        /* Scan terrain */
        for (int y = 0; y < MAP_ARRAY_SIZE; y++) {
            for (int x = 0; x < MAP_ARRAY_SIZE; x++) {
                if (mapGetPos(&gs->mp, (BYTE)x, (BYTE)y) != DEEP_SEA) {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
            }
        }
        /* Include pillboxes */
        BYTE np = pillsGetNumPills(&gs->pb);
        for (BYTE i = 1; i <= np; i++) {
            pillbox p;
            pillsGetPill(&gs->pb, &p, i);
            if (p.x < minX) minX = p.x;
            if (p.x > maxX) maxX = p.x;
            if (p.y < minY) minY = p.y;
            if (p.y > maxY) maxY = p.y;
        }
        /* Include bases */
        BYTE nb = basesGetNumBases(&gs->bs);
        for (BYTE i = 1; i <= nb; i++) {
            base b;
            basesGetBase(&gs->bs, &b, i);
            if (b.x < minX) minX = b.x;
            if (b.x > maxX) maxX = b.x;
            if (b.y < minY) minY = b.y;
            if (b.y > maxY) maxY = b.y;
        }
        /* Include starts */
        BYTE ns = startsGetNumStarts(&gs->ss);
        for (BYTE i = 1; i <= ns; i++) {
            start st;
            startsGetStartStruct(&gs->ss, &st, i);
            if (st.x < minX) minX = st.x;
            if (st.x > maxX) maxX = st.x;
            if (st.y < minY) minY = st.y;
            if (st.y > maxY) maxY = st.y;
        }
        /* Add padding (a few tiles of ocean around the content) */
        int pad = 5;
        if (minX > pad) minX -= pad; else minX = 0;
        if (minY > pad) minY -= pad; else minY = 0;
        if (maxX + pad < 255) maxX += pad; else maxX = 255;
        if (maxY + pad < 255) maxY += pad; else maxY = 255;

        bg->mapMinX = minX;
        bg->mapMinY = minY;
        bg->mapMaxX = maxX;
        bg->mapMaxY = maxY;

        SDL_Log("[BgGame] Map bounds: (%d,%d)-(%d,%d) = %dx%d tiles",
                minX, minY, maxX, maxY, maxX - minX + 1, maxY - minY + 1);
    }

    /* Add brain bots with randomized count and teams */
    botManagerInit();
    char brainPath[512];
    if (findBrainPath(brainPath, sizeof(brainPath))) {
        SDL_Log("[BgGame] Found brain: %s", brainPath);
        int numBots = BG_MIN_BOTS + (rand() % (BG_MAX_BOTS - BG_MIN_BOTS + 1));
        for (BYTE i = 0; i < numBots; i++) {
            char name[32];
            SDL_snprintf(name, sizeof(name), "Bot %d", i + 1);
            if (botManagerAddBot(&bg->sim, i, brainPath, name, aiFull, gameTournament, false)) {
                bg->numBots++;
            }
        }
        SDL_Log("[BgGame] Added %d bots", bg->numBots);

        /* Randomize teams: 0 = no teams, 2-4 = number of teams */
        int numTeams = 0;
        {
            int r = rand() % 5;  /* 0 = no teams, 1-4 maps to 2-4 teams */
            if (r >= 1 && r <= 2) numTeams = 2;
            else if (r == 3) numTeams = 3;
            else if (r == 4) numTeams = 4;
        }
        if (numTeams > 0 && bg->numBots >= 2) {
            if (numTeams > bg->numBots) numTeams = bg->numBots;
            /* Assign each bot to a team and set mutual alliances */
            BYTE teamOf[MAX_TANKS];
            for (BYTE i = 0; i < bg->numBots; i++) {
                teamOf[i] = i % numTeams;
            }
            players *plrs = &bg->sim.sim.plyrs;
            for (BYTE i = 0; i < bg->numBots; i++) {
                for (BYTE j = 0; j < bg->numBots; j++) {
                    if (i != j && teamOf[i] == teamOf[j]) {
                        allienceAdd(&((*plrs)->item[i].allie), j);
                    }
                }
            }
            bg->numTeams = (BYTE)numTeams;
            SDL_Log("[BgGame] Set up %d teams for %d bots", numTeams, bg->numBots);
        } else {
            bg->numTeams = 0;
            SDL_Log("[BgGame] Free-for-all (no teams)");
        }
    } else {
        SDL_Log("[BgGame] No brain script found");
    }

    /* Set camera to center of map content, follow first bot */
    bg->cameraPlayer = 0;
    bg->viewCenterX = ((bg->mapMinX + bg->mapMaxX) / 2) << 8;
    bg->viewCenterY = ((bg->mapMinY + bg->mapMaxY) / 2) << 8;

    return true;
}

void bgGameDestroy(BgGame *bg) {
    if (!bg) return;
    if (bg->valid) {
        botManagerDestroy(&bg->sim);
    }
    if (bg->tilesTex) {
        SDL_DestroyTexture(bg->tilesTex);
        bg->tilesTex = NULL;
    }
    if (bg->valid) {
        serverSimDestroy(&bg->sim);
        bg->valid = false;
    }
}

void bgGameTick(BgGame *bg) {
    if (!bg || !bg->valid || bg->numBots == 0) return;

    /* Run brain AI then tick the simulation.
     * Bot brains run every other tick (game tick, not keys tick). */
    botManagerTick(&bg->sim, aiFull);
    serverSimTick(&bg->sim);

    /* Update camera to follow the tracked player (freeze while dead) */
    if (bg->cameraPlayer < MAX_TANKS &&
        bg->sim.sim.tanks[bg->cameraPlayer] != NULL &&
        tankGetDeathWait(&bg->sim.sim.tanks[bg->cameraPlayer]) == 0) {
        WORLD wx, wy;
        if (serverSimGetTankState(&bg->sim, bg->cameraPlayer, &wx, &wy)) {
            /* Smooth camera: lerp toward tank position */
            bg->viewCenterX = bg->viewCenterX + ((int)wx - (int)bg->viewCenterX) / 8;
            bg->viewCenterY = bg->viewCenterY + ((int)wy - (int)bg->viewCenterY) / 8;
        }
    }
}

#define MAP_NAME_DISPLAY_MS  10000  /* Show map name for 10 seconds */
#define MAP_NAME_FADE_MS      2000  /* Fade out over last 2 seconds */
#define MAP_NAME_SCALE           1  /* 1x scale for debug text (8px tall) */
#define MAP_NAME_MAX_ALPHA     180  /* Slightly transparent */

static void bgGameRenderMapName(BgGame *bg, SDL_Renderer *renderer, int screenW, int screenH) {
    if (!bg->sim.mapName[0]) return;

    Uint64 elapsed = SDL_GetTicks() - bg->createdTicks;
    if (elapsed >= MAP_NAME_DISPLAY_MS) return;

    /* Compute alpha: semi-transparent then fade over last 2 seconds */
    Uint8 alpha = MAP_NAME_MAX_ALPHA;
    if (elapsed > MAP_NAME_DISPLAY_MS - MAP_NAME_FADE_MS) {
        Uint64 fadeElapsed = elapsed - (MAP_NAME_DISPLAY_MS - MAP_NAME_FADE_MS);
        alpha = (Uint8)(MAP_NAME_MAX_ALPHA - (MAP_NAME_MAX_ALPHA * fadeElapsed / MAP_NAME_FADE_MS));
    }

    /* Build map name without .map extension */
    char mapName[MAP_STR_SIZE];
    SDL_snprintf(mapName, sizeof(mapName), "%s", bg->sim.mapName);
    size_t mnLen = SDL_strlen(mapName);
    if (mnLen > 4 && SDL_strcasecmp(mapName + mnLen - 4, ".map") == 0) {
        mapName[mnLen - 4] = '\0';
    }

    char label[256];
    if (bg->numTeams > 0) {
        SDL_snprintf(label, sizeof(label), "%s  |  %d players, %d teams", mapName, bg->numBots, bg->numTeams);
    } else {
        SDL_snprintf(label, sizeof(label), "%s  |  %d players, FFA", mapName, bg->numBots);
    }

    /* Position to the right of the play/pause button (30px + 12px margin + gap).
     * SDL_RenderDebugText draws 8x8 chars; at 1x scale each char is 8px. */
    float pad = 6.0f;
    float charW = 8.0f * MAP_NAME_SCALE;
    float charH = 8.0f * MAP_NAME_SCALE;
    float textW = (float)SDL_strlen(label) * charW;
    float textXpx = 56.0f;
    float textYpx = (float)screenH - 12.0f - 30.0f + (30.0f - charH) * 0.5f;

    /* Faint dark background pill behind the text */
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    float bgAlpha = (float)alpha * 0.4f;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)bgAlpha);
    SDL_FRect bgRect = {
        textXpx - pad,
        textYpx - pad,
        textW + pad * 2.0f,
        charH + pad * 2.0f
    };
    SDL_RenderFillRect(renderer, &bgRect);

    /* Draw the text */
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, alpha);
    SDL_SetRenderScale(renderer, (float)MAP_NAME_SCALE, (float)MAP_NAME_SCALE);
    SDL_RenderDebugText(renderer, textXpx / MAP_NAME_SCALE, textYpx / MAP_NAME_SCALE, label);
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
}

void bgGameRender(BgGame *bg, SDL_Renderer *renderer, int screenW, int screenH) {
    if (!bg || !bg->valid) return;

    /* Temporarily set the sim's "self" player to the camera player so
     * basesGetAlliancePos / pillsGetScreenHealth colour bases and pills
     * correctly from this player's perspective (own = good, enemy = evil). */
    BYTE prevSelf = playersGetSelf(&bg->sim.sim.plyrs);
    playersSetSelfNum(&bg->sim.sim.plyrs, bg->cameraPlayer);

    /* Pick zoom factor so the map content area fits the screen.
     * mapTilesW/H = number of tiles in the bounding box.
     * We want mapTilesW * 16 * zf >= screenW (and same for height).
     * Pick the smaller of the two so everything fits, minimum 1. */
    int mapTilesW = bg->mapMaxX - bg->mapMinX + 1;
    int mapTilesH = bg->mapMaxY - bg->mapMinY + 1;
    int zfW = (mapTilesW > 0) ? (screenW + mapTilesW * 16 - 1) / (mapTilesW * 16) : 1;
    int zfH = (mapTilesH > 0) ? (screenH + mapTilesH * 16 - 1) / (mapTilesH * 16) : 1;
    int zf = zfW < zfH ? zfW : zfH;
    if (zf < 1) zf = 1;

    MapViewCtx ctx = { renderer, bg->tilesTex, zf, 1 };
    mapViewRenderCentered(&ctx, &bg->sim.sim,
                          bg->viewCenterX, bg->viewCenterY,
                          0, 0, screenW, screenH, bg->cameraPlayer);

    playersSetSelfNum(&bg->sim.sim.plyrs, prevSelf);

    /* Draw "Map: <name>" next to play/pause button, fading out after 10 seconds */
    bgGameRenderMapName(bg, renderer, screenW, screenH);
}

#define BG_TICK_INTERVAL_MS 10  /* 100 Hz — matches normal game tick rate */

void bgGameTickFixed(BgGame *bg, Uint64 *lastTickTime) {
    if (!bg || !bg->valid) return;
    Uint64 now = SDL_GetTicks();
    while (now - *lastTickTime >= BG_TICK_INTERVAL_MS) {
        bgGameTick(bg);
        *lastTickTime += BG_TICK_INTERVAL_MS;
    }
}

void bgGameRenderWithOverlay(BgGame *bg, SDL_Renderer *renderer, int screenW, int screenH) {
    if (!bg || !bg->valid) return;
    bgGameRender(bg, renderer, screenW, screenH);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 140);
    SDL_FRect overlay = { 0, 0, (float)screenW, (float)screenH };
    SDL_RenderFillRect(renderer, &overlay);
    /* Draw map name on top of the overlay so it's visible */
    bgGameRenderMapName(bg, renderer, screenW, screenH);
}
