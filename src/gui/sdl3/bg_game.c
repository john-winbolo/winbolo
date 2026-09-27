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
#include "gfx_settings.h"         /* gfxGetTextureFilter */
#include "../winbolo.h"            /* FRAME_RATE_* */
#include "../../common/wb_log.h"
#include "bolo_rand.h"
#include "global.h"
#include "everard_map.h"
#include "control_event.h"
#include "server_sim_lifecycle.h"  /* setTeam — bg_game holds a per-file
                                    * T2 grant for the bot-team assignment
                                    * in setup (see CMakeLists.txt). */
#include "../../server/server_lifecycle.h"
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

/* User zoom range, in whole zoom factors (16px tiles * zf). */
#define BG_MIN_ZOOM 1
#define BG_MAX_ZOOM 8

/* A display this size or smaller keeps the old 1x floor for the
 * fit-to-screen zoom; a bigger one is floored at 1.5x (see bgGameRender). */
#define BG_SMALL_DISPLAY_MAX_W 1024
#define BG_SMALL_DISPLAY_MAX_H 768

#define BG_TICK_INTERVAL_MS  20   /* 50 Hz — matches server tick rate */
#define BG_MAX_CATCHUP_TICKS 3    /* cap catch-up so a stall can't snowball */

/* Brain script path */
#define BG_BRAIN_PATH "Brains/GoalHunter_1.7/init.lua"

/* Find the brain script — try several paths.
 * Uses SDL_IOFromFile so it works with Android APK assets. */
static bool findBrainPath(char *out, size_t outLen) {
    const char *candidates[] = {
        "Brains/GoalHunter_1.7/init.lua",
        "brains/GoalHunter_1.7/init.lua",
        "data/Brains/GoalHunter_1.7/init.lua",
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

/* Drops the sprite atlas and its texture. Every place that drops tilesTex
 * calls this: the atlas is a copy of that sheet. destroyTex is false when
 * the renderer that owned the texture is already gone (see
 * bgGameEnsureTexture). */
static void bgGameDropSprites(BgGame *bg, bool destroyTex) {
    if (bg->spritesTex && destroyTex) SDL_DestroyTexture(bg->spritesTex);
    bg->spritesTex = NULL;
    tileLoaderFreeSpriteAtlas(bg->spriteAtlas);
    bg->spriteAtlas = NULL;
}

/* The padded copy of the sheet's sprites, built before the caller frees
 * the sheet. Not fatal when it fails: the sprites keep drawing from the
 * sheet. */
static void bgGameBuildSprites(BgGame *bg, SDL_Renderer *renderer,
                               SDL_Surface *sheet, GfxTextureFilter filter) {
    bg->spriteAtlas = tileLoaderBuildSpriteAtlas(sheet, 1);
    if (bg->spriteAtlas == NULL) return;
    bg->spritesTex = SDL_CreateTextureFromSurface(renderer,
                                                  bg->spriteAtlas->surface);
    tileLoaderSpriteAtlasDropSurface(bg->spriteAtlas);
    if (bg->spritesTex == NULL) {
        WB_LOG_WARN(WB_LOG_CAT_ASSET,
                    "[BgGame] the sprite atlas would not become a texture "
                    "(%s); sprites keep drawing from the sheet",
                    SDL_GetError());
        tileLoaderFreeSpriteAtlas(bg->spriteAtlas);
        bg->spriteAtlas = NULL;
        return;
    }
    SDL_SetTextureBlendMode(bg->spritesTex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(bg->spritesTex, sdl3DrawScaleModeForFilter(filter));
}

/* Drops the kept scene (see bgFrameRender). destroy is false when the
 * renderer that owned the textures is already gone. */
static void bgFrameDrop(BgGame *bg, bool destroy) {
    if (bg->frameTex && destroy) SDL_DestroyTexture(bg->frameTex);
    bg->frameTex = NULL;
    bg->frameValid = false;
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
    bgGameBuildSprites(bg, renderer, sheet, gfxGetTextureFilter());
    SDL_DestroySurface(sheet);
    if (bg->tilesTex) {
        SDL_SetTextureScaleMode(bg->tilesTex,
                                sdl3DrawScaleModeForFilter(gfxGetTextureFilter()));
    }
    if (!bg->tilesTex) {
        WB_LOG_ERROR(WB_LOG_CAT_ASSET, "[BgGame] SDL_CreateTextureFromSurface failed");
        bgGameDropSprites(bg, true);
        serverSimDestroy(bg->sim);
        return false;
    }
    bg->texRenderer = renderer;
    bg->tilesGeneration = sdl3DrawGetTilesGeneration();
    bg->tilesFilter = gfxGetTextureFilter();

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
            if (serverSimCreateBot(bg->sim, i, brainPath, name, aiFull, gameTournament,
                                   false, 0, NULL)) {
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
    bgGameDropSprites(bg, true);
    bgFrameDrop(bg, bg->frameRenderer == sdl3DrawGetRenderer());
    if (bg->valid) {
        serverSimDestroy(bg->sim);
        bg->valid = false;
    }
}

/* The draw places the camera and the tanks between the last two sim ticks
 * (see bgGameRender). A tank moving more than two tiles in one tick is a
 * respawn, not motion: gliding across it would fly over
 * the map, so the old position is dropped. */
#define BG_INTERP_SNAP_WU 512   /* two tiles, world units */

static bool bgInterpJumped(WORLD from, WORLD to) {
    int d = (int)to - (int)from;
    return d > BG_INTERP_SNAP_WU || d < -BG_INTERP_SNAP_WU;
}

/* Caller holds the sim mutex. A dead or empty slot is forgotten, so a
 * respawn starts from where it appears rather than where it died. */
static void bgInterpRecordTanks(BgGame *bg) {
    for (BYTE i = 0; i < MAX_TANKS; i++) {
        TankRenderInfo info;
        if (!serverSimGetTankRender(bg->sim, i, &info) || !info.alive) {
            bg->tankHave[i] = false;
            continue;
        }
        if (!bg->tankHave[i] ||
            bgInterpJumped(bg->tankCurX[i], info.world_x) ||
            bgInterpJumped(bg->tankCurY[i], info.world_y)) {
            bg->tankPrevX[i] = info.world_x;
            bg->tankPrevY[i] = info.world_y;
        } else {
            bg->tankPrevX[i] = bg->tankCurX[i];
            bg->tankPrevY[i] = bg->tankCurY[i];
        }
        bg->tankCurX[i] = info.world_x;
        bg->tankCurY[i] = info.world_y;
        bg->tankHave[i] = true;
    }
}

/* No jump test here: the camera only moves by the 1/8 ease, so after a far
 * respawn it would trip for several ticks in a row and step instead of
 * glide. bgGameCycleCamera snaps the drawn camera itself. */
static void bgInterpRecordCamera(BgGame *bg) {
    if (!bg->interpValid) {
        bg->camPrevX = bg->viewCenterX;
        bg->camPrevY = bg->viewCenterY;
    } else {
        bg->camPrevX = bg->camCurX;
        bg->camPrevY = bg->camCurY;
    }
    bg->camCurX = bg->viewCenterX;
    bg->camCurY = bg->viewCenterY;
    bg->interpValid = true;
}

/* One sim tick. False when it did not run (no bots, or hidden behind a
 * foreground game), so bgGameTickFixed only moves the draw's tick time
 * for a tick that really happened. */
static bool bgGameStep(BgGame *bg) {
    if (!bg || !bg->valid || bg->numBots == 0) return false;
    if (bg->hiddenByForeground) return false;

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
    bgInterpRecordTanks(bg);
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
    bgInterpRecordCamera(bg);
    return true;
}

/* Follow the next tank, wrapping past the last one back to the first.
 * Walks slots rather than counting to numBots: a bot whose create failed
 * leaves a gap, and serverSimGetTankRender is false for an empty slot, so
 * the search steps over both. cameraPlayer is also what bgGameRender
 * passes as selfPlayer, so the newly followed tank takes the self colour
 * too — the cycle is visible even on a tank sitting still. */
void bgGameCycleCamera(BgGame *bg) {
    if (!bg || !bg->valid) return;

    /* The same mutex contract bgGameStep honours: the bot pool runs the
     * brains inside it, so the tank array is only read with it held. */
    threadsWaitForMutex();
    for (BYTE step = 1; step <= MAX_TANKS; step++) {
        BYTE slot = (BYTE)((bg->cameraPlayer + step) % MAX_TANKS);
        TankRenderInfo info;
        if (!serverSimGetTankRender(bg->sim, slot, &info)) continue;
        bg->cameraPlayer = slot;
        /* Snap rather than let bgGameStep's 1/8 lerp glide there: across
         * a full map that is a second of flying over open ocean. */
        bg->viewCenterX = info.world_x;
        bg->viewCenterY = info.world_y;
        /* The drawn camera too, or it shows the old view until the next
         * tick. */
        bg->camPrevX = bg->camCurX = bg->viewCenterX;
        bg->camPrevY = bg->camCurY = bg->viewCenterY;
        WB_LOG_DEBUG(WB_LOG_CAT_GUI, "[BgGame] Camera now following slot %d",
                     (int)slot);
        break;
    }
    threadsReleaseMutex();
}

/* Step the background zoom. delta is in whole zoom factors, the same
 * units bgGameRender's fit produces, and the first step starts from the
 * fit the last render used — so + always means "one closer than what I am
 * looking at", whatever map got picked.
 *
 * Below the fit the map no longer fills the screen and the ocean around
 * the content shows; that is a fair thing to want to see, so BG_MIN_ZOOM
 * is 1 rather than the fit. */
void bgGameAdjustZoom(BgGame *bg, int delta) {
    if (!bg || !bg->valid || delta == 0) return;
    int base = (bg->zoomUser > 0) ? bg->zoomUser
             : (bg->lastZoom > 0) ? bg->lastZoom
             : 1;
    int want = base + delta;
    if (want < BG_MIN_ZOOM) want = BG_MIN_ZOOM;
    if (want > BG_MAX_ZOOM) want = BG_MAX_ZOOM;
    bg->zoomUser = want;
    WB_LOG_DEBUG(WB_LOG_CAT_GUI, "[BgGame] Zoom now %dx", want);
}

/* Drop the user zoom and go back to the fit-to-screen factor. */
void bgGameResetZoom(BgGame *bg) {
    if (!bg) return;
    bg->zoomUser = 0;
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

/* Rebuild bg->tilesTex when it no longer matches the live renderer or
 * the current tile atlas. A zoom change destroys and recreates the
 * renderer; a skin change keeps the renderer and rebuilds the atlas in
 * place, which the generation counter catches. Cheap fast-path: a
 * pointer compare and an int compare when neither has moved.
 *
 * The texture filter is checked here too, but it only needs setting on
 * the texture that is already there — no new sheet. */
static void bgGameEnsureTexture(BgGame *bg) {
    SDL_Renderer *cur = sdl3DrawGetRenderer();
    unsigned int gen = sdl3DrawGetTilesGeneration();
    GfxTextureFilter filter = gfxGetTextureFilter();
    if (bg->texRenderer == cur && bg->tilesGeneration == gen) {
        if (bg->tilesFilter != filter && bg->tilesTex) {
            SDL_ScaleMode mode = sdl3DrawScaleModeForFilter(filter);
            SDL_SetTextureScaleMode(bg->tilesTex, mode);
            if (bg->spritesTex) SDL_SetTextureScaleMode(bg->spritesTex, mode);
            bg->tilesFilter = filter;
        }
        return;
    }

    if (bg->texRenderer == cur) {
        /* Same renderer, new atlas: the texture is still live and ours
         * to destroy. */
        if (bg->tilesTex) SDL_DestroyTexture(bg->tilesTex);
    }
    /* Otherwise the previous renderer is gone — its textures are
     * already invalidated by SDL3 when SDL_DestroyRenderer ran. Calling
     * SDL_DestroyTexture on the stale handle is undefined behaviour,
     * so we elide the destroy and just NULL the field. */
    bg->tilesTex = NULL;
    bgGameDropSprites(bg, bg->texRenderer == cur);
    bg->texRenderer = cur;
    bg->tilesGeneration = gen;
    bg->tilesFilter = filter;
    if (cur == NULL) return;   /* No renderer to rebuild against yet. */

    static Uint64 sLastTexErrLogMs = 0;
    SDL_Surface *sheet = tileLoaderBuildSheet(16);
    if (!sheet) {
        Uint64 now = SDL_GetTicks();
        if (now - sLastTexErrLogMs > 5000) {
            WB_LOG_ERROR(WB_LOG_CAT_ASSET,
                         "[BgGame] tileLoaderBuildSheet failed during "
                         "tile texture rebuild");
            sLastTexErrLogMs = now;
        }
        return;
    }
    bg->tilesTex = SDL_CreateTextureFromSurface(cur, sheet);
    bgGameBuildSprites(bg, cur, sheet, filter);
    SDL_DestroySurface(sheet);
    if (bg->tilesTex) {
        SDL_SetTextureScaleMode(bg->tilesTex,
                                sdl3DrawScaleModeForFilter(filter));
    } else {
        bgGameDropSprites(bg, true);
        Uint64 now = SDL_GetTicks();
        if (now - sLastTexErrLogMs > 5000) {
            WB_LOG_ERROR(WB_LOG_CAT_ASSET,
                         "[BgGame] SDL_CreateTextureFromSurface failed "
                         "during tile texture rebuild");
            sLastTexErrLogMs = now;
        }
    }
}

/* One refresh period of the display the window is on, in ns. An unknown
 * or implausible rate counts as 60 Hz. */
static Sint64 bgFrameRefreshNs(SDL_Renderer *renderer) {
    float hz = 0.0f;
    SDL_Window *win = SDL_GetRenderWindow(renderer);
    SDL_DisplayID did = win ? SDL_GetDisplayForWindow(win) : 0;
    const SDL_DisplayMode *dm = did ? SDL_GetCurrentDisplayMode(did) : NULL;
    if (dm) hz = dm->refresh_rate;
    if (hz < 20.0f) hz = 60.0f;
    return (Sint64)(1000000000.0 / (double)hz);
}

/* The Frame Rate setting (Settings > Display) in Hz. frameRate holds a
 * FRAME_RATE_* code, not Hz. */
extern int frameRate;
static int bgFrameTargetHz(void) {
    switch (frameRate) {
        case FRAME_RATE_60: return 60;
        case FRAME_RATE_50: return 50;
        case FRAME_RATE_30: return 30;
        case FRAME_RATE_20: return 20;
        case FRAME_RATE_15: return 15;
        case FRAME_RATE_12: return 12;
        case FRAME_RATE_10: return 10;
        default:            return 30;   /* the game's default */
    }
}

/* Redraw the menu game every n refreshes: refresh / Frame Rate, rounded,
 * at least 1. At 60 Hz: 60->1 50->1 30->2 20->3 15->4 12->5 10->6. */
static int bgFrameEveryN(Sint64 periodNs) {
    double refreshHz = 1000000000.0 / (double)periodNs;
    int n = (int)(refreshHz / (double)bgFrameTargetHz() + 0.5);
    return n < 1 ? 1 : n;
}

/* How far this draw is between the last two ticks: 0 = the tick before,
 * 1 = the last tick. It is read off a clock that moves a whole number of
 * refresh periods per draw: the time since the last draw, rounded to whole
 * periods, at least one. With vsync that is `steps` every time, so every
 * shown frame moves the same amount; where a draw spans more refreshes
 * than that (a loop that sleeps, no real vsync) it still keeps up. The
 * clock follows the wall clock by 1/32 of the rest each draw so an inexact
 * refresh rate does not drift, and it resets when it runs more than two
 * draws ahead (draws faster than the refresh). The ticks still land on the
 * wall clock, so a quarter tick either side is allowed rather than a clamp
 * that would hitch; with no tick for two tick intervals (paused) it is
 * held to the last two ticks instead. */
static float bgInterpAlpha(BgGame *bg, Sint64 periodNs, int steps) {
    Uint64 nowNs  = SDL_GetTicksNS();
    Sint64 stepNs = periodNs * (Sint64)steps;
    if (bg->interpClockValid) {
        Sint64 k = ((Sint64)(nowNs - bg->interpClockNs) + periodNs / 2) / periodNs;
        if (k < 1) k = 1;
        Sint64 gap = (Sint64)nowNs - (Sint64)(bg->interpClockNs + (Uint64)(k * periodNs));
        if (gap < -2 * stepNs) {
            bg->interpClockNs = nowNs;
        } else {
            bg->interpClockNs += (Uint64)(k * periodNs + gap / 32);
        }
    } else {
        bg->interpClockNs = nowNs;
        bg->interpClockValid = true;
    }
    const double tickNs = BG_TICK_INTERVAL_MS * 1000000.0;
    Sint64 sinceNs = (Sint64)(bg->interpClockNs - bg->interpTickMs * 1000000u);
    float alpha = (float)((double)sinceNs / tickNs);
    float lo = -0.25f, hi = 1.25f;
    if ((double)(Sint64)(nowNs - bg->interpTickMs * 1000000u) > 2.0 * tickNs) {
        lo = 0.0f;
        hi = 1.0f;
    }
    if (alpha < lo) alpha = lo;
    if (alpha > hi) alpha = hi;
    return alpha;
}

/* The camera centre and tank positions for a draw that is `steps` refresh
 * periods after the last one. Before the first tick, the live camera and
 * the live tank positions. */
static void bgInterpFill(BgGame *bg, MapViewPreciseCam *pc,
                         Sint64 periodNs, int steps) {
    SDL_memset(pc, 0, sizeof(*pc));
    if (!bg->interpValid) {
        pc->centerWX = (float)bg->viewCenterX;
        pc->centerWY = (float)bg->viewCenterY;
        return;
    }
    float a = bgInterpAlpha(bg, periodNs, steps);
    pc->centerWX = (float)bg->camPrevX + ((float)bg->camCurX - (float)bg->camPrevX) * a;
    pc->centerWY = (float)bg->camPrevY + ((float)bg->camCurY - (float)bg->camPrevY) * a;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (!bg->tankHave[i]) continue;   /* drawn at its live position */
        pc->haveTank[i] = true;
        pc->tankWX[i] = (float)bg->tankPrevX[i] +
                        ((float)bg->tankCurX[i] - (float)bg->tankPrevX[i]) * a;
        pc->tankWY[i] = (float)bg->tankPrevY[i] +
                        ((float)bg->tankCurY[i] - (float)bg->tankPrevY[i]) * a;
    }
}

/* The camera eases after its tank every tick. Placed in whole game pixels
 * and moved only on ticks, it steps a game pixel at uneven moments, and
 * tile art dithered in a one-pixel checkerboard (the buildings) turns into
 * its own inverse on each step, so it flashes. Placed in whole screen
 * pixels between ticks instead, it glides; the tanks go the same way so
 * they keep pace with it. */
static void bgGameDrawScene(BgGame *bg, SDL_Renderer *renderer,
                            int screenW, int screenH, int zf,
                            Sint64 periodNs, int steps) {
    MapViewPreciseCam pc;
    bgInterpFill(bg, &pc, periodNs, steps);
    MapViewCtx ctx = { renderer, bg->tilesTex, zf, 1, (float)zf,
                       bg->spritesTex, bg->spriteAtlas, &pc };
    /* With precise set, the centre below is not used: pc carries it. */
    mapViewRenderCentered(&ctx, bg->sim,
                          bg->viewCenterX, bg->viewCenterY,
                          0, 0, screenW, screenH, bg->cameraPlayer);
}

/* Forget the kept scene of a renderer that is gone; drop it on a resize. */
static void bgFramePrepare(BgGame *bg, SDL_Renderer *renderer,
                           int screenW, int screenH) {
    if (bg->frameRenderer != renderer) {
        bgFrameDrop(bg, false);
        bg->frameRenderer = renderer;
        bg->frameBroken = false;
    }
    if (bg->frameW != screenW || bg->frameH != screenH) {
        bgFrameDrop(bg, true);
        bg->frameW = screenW;
        bg->frameH = screenH;
        bg->frameBroken = false;
    }
}

/* Draw the scene into the kept texture, texW x texH: the screen size, or
 * twice it at 1.5x. One texture is enough: the only step that can fail
 * before anything is drawn is setting the target, and then the texture
 * still holds the old scene. */
static bool bgFrameRedraw(BgGame *bg, SDL_Renderer *renderer,
                          int texW, int texH, int zf,
                          Sint64 periodNs, int n) {
    if (!bg->frameTex) {
        bg->frameTex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_TARGET,
                                         texW, texH);
        if (!bg->frameTex) {
            WB_LOG_WARN(WB_LOG_CAT_GUI,
                        "[BgGame] scene texture %dx%d failed (%s)",
                        texW, texH, SDL_GetError());
            bg->frameBroken = true;
            return false;
        }
        SDL_SetTextureBlendMode(bg->frameTex, SDL_BLENDMODE_NONE);
    }
    SDL_Texture *prev = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, bg->frameTex)) return false;
    SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
    SDL_RenderClear(renderer);
    bgGameDrawScene(bg, renderer, texW, texH, zf, periodNs, n);
    if (!SDL_SetRenderTarget(renderer, prev)) {
        SDL_SetRenderTarget(renderer, NULL);   /* put the window back regardless */
        return false;
    }
    bg->frameValid = true;
    bg->frameZoom = zf;
    bg->frameTilesGen = bg->tilesGeneration;
    bg->frameFilter = bg->tilesFilter;
    return true;
}

/* The Frame Rate setting for the menu game, n >= 2: the scene is drawn
 * offscreen on every n-th refresh and the refreshes between copy it. The
 * menu UI itself still draws and presents every refresh.
 *
 * The normal pattern is one redraw, then n - 1 copies. A late frame
 * decides by the wall clock: a copy frame n - 0.5 refresh periods or more
 * after the last redraw means copy slots were missed and this frame sits in
 * the redraw slot, so it redraws instead and the scene keeps changing every
 * n periods. A redraw frame always redraws, even late. So two redraws are
 * never on back-to-back refreshes, and one missed frame makes at most one
 * gap of n + 1 periods. The blend clock moves by the periods that really
 * passed, n with vsync.
 *
 * The scene is texW x texH and is copied to the window at screenW x
 * screenH with copyMode. At 1.5x the texture is twice the screen and n may
 * be 1, which redraws it every frame.
 *
 * False when no scene could be shown; the caller then draws direct. */
static bool bgFrameRender(BgGame *bg, SDL_Renderer *renderer,
                          int texW, int texH, int screenW, int screenH,
                          int zf, Sint64 periodNs, int n,
                          SDL_ScaleMode copyMode) {
    Uint64 nowNs    = SDL_GetTicksNS();
    int    since    = bg->frameSince + 1;
    bool redraw = !bg->frameValid ||                        /* first, or lost */
                  bg->frameZoom != zf ||
                  bg->frameTilesGen != bg->tilesGeneration ||
                  bg->frameFilter != bg->tilesFilter ||
                  since >= n ||                             /* every n-th     */
                  (Sint64)(nowNs - bg->frameRedrawNs) >=
                      (Sint64)(2 * n - 1) * periodNs / 2;   /* re-align       */
    if (redraw) {
        if (!bgFrameRedraw(bg, renderer, texW, texH, zf, periodNs, n)) {
            bg->frameValid = false;   /* try again next frame */
            return false;
        }
        bg->frameSince = 0;
        bg->frameRedrawNs = nowNs;
    } else {
        bg->frameSince = since;
    }
    SDL_SetTextureScaleMode(bg->frameTex, copyMode);
    SDL_FRect dst = { 0.0f, 0.0f, (float)screenW, (float)screenH };
    SDL_RenderTexture(renderer, bg->frameTex, NULL, &dst);
    return true;
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
    /* A fit of 1 is floored at 1.5x. At 1x each game pixel is one screen
     * pixel, and the one-pixel speckle in the grass tiles shimmers as the
     * camera scrolls over it; 2x shows too little of the map. 1.5x is not
     * a whole zoom, and drawn straight its game pixels would be 1 and 2
     * screen pixels wide by turns, which shimmers too. So the scene is
     * drawn at zoom 3 into the kept texture at twice the screen size and
     * copied to the window at half size with linear filtering: each screen
     * pixel is the average of 2x2 texture pixels (supersampled). A small
     * display keeps 1x, where 1.5x would show too little of the map. If
     * that texture cannot be made, zoom 2 until the renderer or the size
     * changes. A fit of 2 or more, or a user zoom, is drawn as before. */
    int ssScale = 1;   /* kept scene size / screen size: 2 at 1.5x */
    if (zf == 1 && bg->zoomUser == 0 && screenW > 0 && screenH > 0 &&
        !(screenW <= BG_SMALL_DISPLAY_MAX_W &&
          screenH <= BG_SMALL_DISPLAY_MAX_H)) {
        bool failed = bg->ssFailRenderer == renderer &&
                      bg->ssFailW == screenW && bg->ssFailH == screenH;
        if (failed) {
            zf = 2;
        } else {
            zf = 3;
            ssScale = 2;
        }
    }
    /* A user zoom replaces the fit, so the +/- keys keep their step on a
     * window resize instead of snapping back to whatever fits now. The
     * fit stays the starting point: it is what zoomUser is seeded from
     * on the first press, through lastZoom below. At 1.5x the fit counts
     * as 1, so + goes to 2x and - to 1x. */
    if (bg->zoomUser > 0) zf = bg->zoomUser;
    bg->lastZoom = (ssScale == 2) ? 1 : zf;

    /* Redraw every n refreshes by the Frame Rate setting; n = 1 draws
     * direct every frame, and so does a renderer that cannot keep the
     * scene offscreen. 1.5x cannot be drawn direct, so it always goes
     * through the kept scene; with n = 1 that is redrawn every frame. */
    Sint64 periodNs = bgFrameRefreshNs(renderer);
    int n = bgFrameEveryN(periodNs);
    int texW = screenW * ssScale;
    int texH = screenH * ssScale;
    bgFramePrepare(bg, renderer, texW, texH);
    if (n < 2 && ssScale == 1) bgFrameDrop(bg, true);
    if (bg->tilesTex != NULL) {
        bool shown;
        if (ssScale == 2) {
            shown = !bg->frameBroken &&
                    bgFrameRender(bg, renderer, texW, texH, screenW, screenH,
                                  zf, periodNs, n, SDL_SCALEMODE_LINEAR);
            if (!shown) {
                if (bg->frameBroken) {
                    bg->ssFailRenderer = renderer;
                    bg->ssFailW = screenW;
                    bg->ssFailH = screenH;
                    WB_LOG_WARN(WB_LOG_CAT_GUI,
                                "[BgGame] 1.5x scene texture %dx%d failed; "
                                "menu background uses zoom 2 at %dx%d",
                                texW, texH, screenW, screenH);
                }
                zf = 2;   /* this frame: direct at zoom 2 */
            }
        } else {
            shown = n >= 2 && !bg->frameBroken &&
                    screenW > 0 && screenH > 0 &&
                    bgFrameRender(bg, renderer, screenW, screenH,
                                  screenW, screenH, zf, periodNs, n,
                                  SDL_SCALEMODE_NEAREST);
        }
        if (!shown) {
            bgGameDrawScene(bg, renderer, screenW, screenH, zf, periodNs, 1);
        }
    }

    /* Draw "Map: <name>" next to play/pause button, fading out after 10 seconds */
    bgGameRenderMapName(bg, renderer, screenW, screenH);
}

void bgGameTickFixed(BgGame *bg, Uint64 *lastTickTime) {
    if (!bg || !bg->valid) return;
    Uint64 now = SDL_GetTicks();
    int ticks = 0;
    bool ran = false;
    while (now - *lastTickTime >= BG_TICK_INTERVAL_MS &&
           ticks < BG_MAX_CATCHUP_TICKS) {
        ran |= bgGameStep(bg);
        *lastTickTime += BG_TICK_INTERVAL_MS;
        ticks++;
    }
    /* If we hit the cap, advance lastTickTime so the next call doesn't
     * try to make up the dropped ticks — discarding sim time is the
     * right call for a decorative background sim. */
    if (now - *lastTickTime >= BG_TICK_INTERVAL_MS) {
        *lastTickTime = now;
    }
    /* The scheduled time of the tick just run, not the wall clock when it
     * ran, so the draw moves exactly one tick per 20 ms however the frames
     * fall. Only when a tick really ran: otherwise the draw holds the last
     * position rather than replaying the last step. */
    if (ran) {
        bg->interpTickMs = *lastTickTime;
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
