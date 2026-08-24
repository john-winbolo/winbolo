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
 * Name:          bg_game.c
 * Purpose:
 *   Background game rendering for the welcome screen.
 *   Loads a map, runs brain bots, and renders terrain,
 *   pillboxes, bases, tanks, shells, and explosions.
 *********************************************************/

#include "bg_game.h"
#include "mapview.h"
#include "tileloader.h"
#include "sdl3draw.h"             /* sdl3DrawGetRenderer */
#include "../../common/wb_log.h"
#include "bolo_rand.h"
#include "global.h"
#include "everard_map.h"
#include "control_event.h"
#include "server_sim_lifecycle.h"  /* setTeam — bg_game holds a per-file
                                    * T2 grant for the bot-team assignment
                                    * in setup (see CMakeLists.txt). */
#include "../../server/server_lifecycle.h"
#include "../../server/scenario.h"   /* scenarioShutdown — splash sim runs plain */
#include "../../server/threads.h"   /* threadsWaitForMutex / threadsReleaseMutex */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Shared background game instance for pre-game dialogs */
static BgGame *sharedBg = NULL;

void bgGameSetShared(BgGame *bg) { sharedBg = bg; }
BgGame *bgGameGetShared(void) { return sharedBg; }

/* Bot player count range */
#define BG_MIN_BOTS 2
#define BG_MAX_BOTS 16

/* Brain script path */
#define BG_BRAIN_PATH "Brains/GoalHunter_1.6/init.lua"

/* Find the brain script — try several paths.
 * Uses SDL_IOFromFile so it works with Android APK assets. */
static bool findBrainPath(char *out, size_t outLen) {
    const char *candidates[] = {
        "Brains/GoalHunter_1.6/init.lua",
        "brains/GoalHunter_1.6/init.lua",
        "data/Brains/GoalHunter_1.6/init.lua",
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

    WB_LOG_INFO(WB_LOG_CAT_GUI, "[BgGame] Creating with map: %s", mapFile);

    /* Try loading the map file. On Android, fopen can't read APK assets,
     * so use SDL_LoadFile to extract to a temp file, then use the proven
     * fopen-based serverSimCreate on the temp file. */
    bool mapLoaded = false;
    if (mapFile && mapFile[0]) {
        /* First try direct fopen-based load (works on desktop) */
        bg->sim = serverSimCreate((char *)mapFile, gameTournament, false, 0, -1);
        if (bg->sim != NULL) {
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
                    bg->sim = serverSimCreate(tmpPath, gameTournament, false, 0, -1);
                    mapLoaded = (bg->sim != NULL);
                    if (!mapLoaded) {
                        WB_LOG_WARN(WB_LOG_CAT_GUI, "[BgGame] serverSimCreate failed for temp file '%s'", tmpPath);
                    }
                    remove(tmpPath);
                }
                SDL_free(fileData);
            } else {
                WB_LOG_WARN(WB_LOG_CAT_GUI, "[BgGame] SDL_LoadFile failed for '%s'", mapFile);
            }
        }
    }
    if (!mapLoaded) {
        /* Fall back to embedded Everard Island */
        BYTE emap[6000] = E_MAP;
        WB_LOG_INFO(WB_LOG_CAT_GUI, "[BgGame] Falling back to embedded Everard Island");
        bg->sim = serverSimCreateCompressed(emap, 5097, "Everard Island", gameTournament, false, 0, -1);
        if (bg->sim == NULL) {
            WB_LOG_ERROR(WB_LOG_CAT_GUI, "[BgGame] serverSimCreateCompressed also failed");
            return false;
        }
    }
    /* The welcome-screen game is pure eye candy — if the picked map
     * happens to carry a scenario sidecar, drop the script so no waves
     * spawn, no messages broadcast and no scripted round-end fires
     * behind the splash screen. */
    scenarioShutdown(bg->sim);

    /* Embedded sim: silence its console messages — no server console. */
    serverSimSetQuiet(bg->sim, true);
    /* bg_game is a local headless sim — no lobby, run immediately.
     * acceptRemoteClients=false short-circuits UDP/WBN/tracker/NAT
     * inside serverInstanceStartup; cfg.skipLobby transitions the
     * sim to running state so the subsequent serverSimCreateBot
     * calls hit the running-state branch. */
    {
        ServerInstanceConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.acceptRemoteClients = false;
        cfg.skipLobby           = true;
        serverInstanceStartup(bg->sim, &cfg);
    }

    SDL_Surface *sheet = tileLoaderBuildSheet(16);
    if (!sheet) {
        WB_LOG_ERROR(WB_LOG_CAT_ASSET, "[BgGame] tileLoaderBuildSheet failed");
        serverSimDestroy(bg->sim);
        return false;
    }
    bg->tilesTex = SDL_CreateTextureFromSurface(renderer, sheet);
    SDL_DestroySurface(sheet);
    if (bg->tilesTex) {
        SDL_SetTextureScaleMode(bg->tilesTex, SDL_SCALEMODE_NEAREST);
    }
    if (!bg->tilesTex) {
        WB_LOG_ERROR(WB_LOG_CAT_ASSET, "[BgGame] SDL_CreateTextureFromSurface failed");
        serverSimDestroy(bg->sim);
        return false;
    }
    bg->texRenderer = renderer;

    bg->valid = true;
    bg->createdTicks = SDL_GetTicks();

    /* Compute bounding box of map content (non-ocean terrain + pills/bases/starts) */
    {
        int minX = 255, minY = 255, maxX = 0, maxY = 0;

        /* Scan terrain */
        for (int y = 0; y < MAP_ARRAY_SIZE; y++) {
            for (int x = 0; x < MAP_ARRAY_SIZE; x++) {
                if (serverSimGetMapTerrain(bg->sim, (BYTE)x, (BYTE)y) != DEEP_SEA) {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
            }
        }
        /* Include pillboxes */
        BYTE np = serverSimGetPillCount(bg->sim);
        for (BYTE i = 1; i <= np; i++) {
            BYTE px, py;
            if (!serverSimGetPill(bg->sim, i, &px, &py, NULL, NULL, NULL)) continue;
            if (px < minX) minX = px;
            if (px > maxX) maxX = px;
            if (py < minY) minY = py;
            if (py > maxY) maxY = py;
        }
        /* Include bases */
        BYTE nb = serverSimGetBaseCount(bg->sim);
        for (BYTE i = 1; i <= nb; i++) {
            BYTE bx, by;
            if (!serverSimGetBase(bg->sim, i, &bx, &by, NULL)) continue;
            if (bx < minX) minX = bx;
            if (bx > maxX) maxX = bx;
            if (by < minY) minY = by;
            if (by > maxY) maxY = by;
        }
        /* Include starts */
        BYTE ns = serverSimGetStartCount(bg->sim);
        for (BYTE i = 1; i <= ns; i++) {
            BYTE sx, sy;
            if (!serverSimGetStart(bg->sim, i, &sx, &sy, NULL)) continue;
            if (sx < minX) minX = sx;
            if (sx > maxX) maxX = sx;
            if (sy < minY) minY = sy;
            if (sy > maxY) maxY = sy;
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

        WB_LOG_DEBUG(WB_LOG_CAT_GUI, "[BgGame] Map bounds: (%d,%d)-(%d,%d) = %dx%d tiles",
                minX, minY, maxX, maxY, maxX - minX + 1, maxY - minY + 1);
    }

    /* Add brain bots with randomized count and teams */
    char brainPath[512];
    if (findBrainPath(brainPath, sizeof(brainPath))) {
        WB_LOG_INFO(WB_LOG_CAT_GUI, "[BgGame] Found brain: %s", brainPath);
        int numBots = BG_MIN_BOTS + (int)bolo_rand_below((uint32_t)(BG_MAX_BOTS - BG_MIN_BOTS + 1));
        for (BYTE i = 0; i < numBots; i++) {
            char name[32];
            SDL_snprintf(name, sizeof(name), "Bot %d", i + 1);
            if (serverSimCreateBot(bg->sim, i, brainPath, name, aiFull, gameTournament, false)) {
                bg->numBots++;
            }
        }
        WB_LOG_INFO(WB_LOG_CAT_GUI, "[BgGame] Added %d bots", bg->numBots);

        /* Randomize teams: 0 = no teams, 2-4 = number of teams */
        int numTeams = 0;
        {
            int r = (int)bolo_rand_below(5);  /* 0 = no teams, 1-4 maps to 2-4 teams */
            if (r >= 1 && r <= 2) numTeams = 2;
            else if (r == 3) numTeams = 3;
            else if (r == 4) numTeams = 4;
        }
        if (numTeams > 0 && bg->numBots >= 2) {
            if (numTeams > bg->numBots) numTeams = bg->numBots;
            for (BYTE i = 0; i < bg->numBots; i++) {
                serverSimSetTeamBatch(bg->sim, i, (BYTE)((i % numTeams) + 1));
            }
            /* serverInstanceStartup ran the team-alliance pass before
             * any bot existed, so plrs->item[].allie is empty. Rebake
             * it now that teamNumber is populated. */
            serverSimReapplyTeamAlliances(bg->sim);
            bg->numTeams = (BYTE)numTeams;
            WB_LOG_INFO(WB_LOG_CAT_GUI, "[BgGame] Set up %d teams for %d bots", numTeams, bg->numBots);
        } else {
            bg->numTeams = 0;
            WB_LOG_INFO(WB_LOG_CAT_GUI, "[BgGame] Free-for-all (no teams)");
        }
    } else {
        WB_LOG_INFO(WB_LOG_CAT_GUI, "[BgGame] No brain script found");
    }

    /* Set camera to center of map content, follow first bot */
    bg->cameraPlayer = 0;
    bg->viewCenterX = ((bg->mapMinX + bg->mapMaxX) / 2) << 8;
    bg->viewCenterY = ((bg->mapMinY + bg->mapMaxY) / 2) << 8;

    return true;
}

void bgGameDestroy(BgGame *bg) {
    if (!bg) return;
    if (bg->tilesTex) {
        SDL_DestroyTexture(bg->tilesTex);
        bg->tilesTex = NULL;
    }
    if (bg->valid) {
        serverSimDestroy(bg->sim);
        bg->valid = false;
    }
}

void bgGameTick(BgGame *bg) {
    if (!bg || !bg->valid || bg->numBots == 0) return;
    if (bg->hiddenByForeground) return;

    /* serverSimApplyCommand asserts threadsCurrentlyHoldsMutex() — the
     * bot-pool drain inside botManagerTick dispatches CMD_CHAT through
     * that path when a bot's brain queues outbound chat, so the bg
     * demo has to honour the same mutex contract the timer-callback
     * tick path does. Pre-3701635 this path was contract-free; the
     * dispatcher is now mutex-owning. */
    threadsWaitForMutex();
    /* One bot pass per 20ms frame produces input for both halves of the
     * frame; serverSimTick internally runs the keys + game half-steps. */
    serverSimBotTick(bg->sim, aiFull);
    serverSimTick(bg->sim);
    threadsReleaseMutex();

    /* Update camera to follow the tracked player (freeze while dead) */
    if (bg->cameraPlayer < MAX_TANKS) {
        TankRenderInfo info;
        if (serverSimGetTankRender(bg->sim, bg->cameraPlayer, &info) && info.alive) {
            /* Smooth camera: lerp toward tank position */
            bg->viewCenterX = bg->viewCenterX + ((int)info.world_x - (int)bg->viewCenterX) / 8;
            bg->viewCenterY = bg->viewCenterY + ((int)info.world_y - (int)bg->viewCenterY) / 8;
        }
    }
}

#define MAP_NAME_DISPLAY_MS  10000  /* Show map name for 10 seconds */
#define MAP_NAME_FADE_MS      2000  /* Fade out over last 2 seconds */
#define MAP_NAME_SCALE           1  /* 1x scale for debug text (8px tall) */
#define MAP_NAME_MAX_ALPHA     180  /* Slightly transparent */

/* Current visible alpha for the map-name label. 0 = hidden. */
static Uint8 bgGameMapNameAlpha(const BgGame *bg, Uint64 nowMs) {
    if (bg->mapNameFadeStartMs != 0) {
        /* Pause-driven transition: lerp from the captured start alpha to
         * the target (full when paused, 0 when not) over MAP_NAME_FADE_MS. */
        Uint8 target = bg->paused ? MAP_NAME_MAX_ALPHA : 0;
        Uint64 fadeElapsed = nowMs - bg->mapNameFadeStartMs;
        if (fadeElapsed >= MAP_NAME_FADE_MS) return target;
        int from = (int)bg->mapNameFadeFromAlpha;
        int delta = ((int)target - from) * (int)fadeElapsed / (int)MAP_NAME_FADE_MS;
        int a = from + delta;
        if (a < 0) a = 0;
        if (a > 255) a = 255;
        return (Uint8)a;
    }
    /* Initial display: full alpha for 10s, then fade over the last 2s. */
    Uint64 elapsed = nowMs - bg->createdTicks;
    if (elapsed >= MAP_NAME_DISPLAY_MS) return 0;
    if (elapsed > MAP_NAME_DISPLAY_MS - MAP_NAME_FADE_MS) {
        Uint64 fadeElapsed = elapsed - (MAP_NAME_DISPLAY_MS - MAP_NAME_FADE_MS);
        return (Uint8)(MAP_NAME_MAX_ALPHA - (MAP_NAME_MAX_ALPHA * fadeElapsed / MAP_NAME_FADE_MS));
    }
    return MAP_NAME_MAX_ALPHA;
}

void bgGameSetHiddenByForeground(BgGame *bg, bool hidden) {
    if (bg) bg->hiddenByForeground = hidden;
}

void bgGameTogglePause(BgGame *bg) {
    if (!bg) return;
    Uint64 now = SDL_GetTicks();
    /* Capture the alpha we are currently rendering so the fade starts
     * from there (avoids a snap when toggling mid-transition or while
     * the initial display is still on screen). */
    bg->mapNameFadeFromAlpha = bgGameMapNameAlpha(bg, now);
    bg->paused = !bg->paused;
    bg->mapNameFadeStartMs = now;
}

static void bgGameRenderMapName(BgGame *bg, SDL_Renderer *renderer, int screenW, int screenH) {
    if (!serverSimGetMapName(bg->sim)[0]) return;

    Uint8 alpha = bgGameMapNameAlpha(bg, SDL_GetTicks());
    if (alpha == 0) return;

    /* Build map name without .map extension */
    char mapName[MAP_STR_SIZE];
    SDL_snprintf(mapName, sizeof(mapName), "%s", serverSimGetMapName(bg->sim));
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

/* If the SDL renderer has been destroyed and recreated (zoom change,
 * skin reload) since bg->tilesTex was built, rebuild the texture
 * against the current renderer. Cheap fast-path: a single pointer
 * compare when the renderer is unchanged. */
static void bgGameEnsureTexture(BgGame *bg) {
    SDL_Renderer *cur = sdl3DrawGetRenderer();
    if (bg->texRenderer == cur) return;

    /* The previous renderer is gone — its textures are already
     * invalidated by SDL3 when SDL_DestroyRenderer ran. Calling
     * SDL_DestroyTexture on the stale handle is undefined behaviour,
     * so we elide the destroy and just NULL the field. */
    bg->tilesTex = NULL;
    bg->texRenderer = cur;
    if (cur == NULL) return;   /* No renderer to rebuild against yet. */

    static Uint64 sLastTexErrLogMs = 0;
    SDL_Surface *sheet = tileLoaderBuildSheet(16);
    if (!sheet) {
        Uint64 now = SDL_GetTicks();
        if (now - sLastTexErrLogMs > 5000) {
            WB_LOG_ERROR(WB_LOG_CAT_ASSET,
                         "[BgGame] tileLoaderBuildSheet failed during "
                         "renderer-recreate rebuild");
            sLastTexErrLogMs = now;
        }
        return;
    }
    bg->tilesTex = SDL_CreateTextureFromSurface(cur, sheet);
    SDL_DestroySurface(sheet);
    if (bg->tilesTex) {
        SDL_SetTextureScaleMode(bg->tilesTex, SDL_SCALEMODE_NEAREST);
    } else {
        Uint64 now = SDL_GetTicks();
        if (now - sLastTexErrLogMs > 5000) {
            WB_LOG_ERROR(WB_LOG_CAT_ASSET,
                         "[BgGame] SDL_CreateTextureFromSurface failed "
                         "during renderer-recreate rebuild");
            sLastTexErrLogMs = now;
        }
    }
}

void bgGameRender(BgGame *bg, SDL_Renderer *renderer, int screenW, int screenH) {
    if (!bg || !bg->valid) return;
    bgGameEnsureTexture(bg);

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

    if (bg->tilesTex != NULL) {
        MapViewCtx ctx = { renderer, bg->tilesTex, zf, 1 };
        mapViewRenderCentered(&ctx, bg->sim,
                              bg->viewCenterX, bg->viewCenterY,
                              0, 0, screenW, screenH, bg->cameraPlayer);
    }

    /* Draw "Map: <name>" next to play/pause button, fading out after 10 seconds */
    bgGameRenderMapName(bg, renderer, screenW, screenH);
}

#define BG_TICK_INTERVAL_MS  20   /* 50 Hz — matches server tick rate */
#define BG_MAX_CATCHUP_TICKS 3    /* cap catch-up so a stall can't snowball */

void bgGameTickFixed(BgGame *bg, Uint64 *lastTickTime) {
    if (!bg || !bg->valid) return;
    Uint64 now = SDL_GetTicks();
    int ticks = 0;
    while (now - *lastTickTime >= BG_TICK_INTERVAL_MS &&
           ticks < BG_MAX_CATCHUP_TICKS) {
        bgGameTick(bg);
        *lastTickTime += BG_TICK_INTERVAL_MS;
        ticks++;
    }
    /* If we hit the cap, advance lastTickTime so the next call doesn't
     * try to make up the dropped ticks — discarding sim time is the
     * right call for a decorative background sim. */
    if (now - *lastTickTime >= BG_TICK_INTERVAL_MS) {
        *lastTickTime = now;
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
