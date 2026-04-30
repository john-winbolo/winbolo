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
 * Name:          BrainTest
 * Filename:      braintest_main.c
 * Purpose:
 *   Standalone brain debug viewer. Runs a ServerSim with
 *   bot(s) and renders the game with debug overlays for
 *   the brain's internal state: influence grid, danger
 *   grid, front line, and pathfinder route.
 *
 *   Usage:
 *     BrainTest [options]
 *
 *   Options:
 *     -brain PATH      Brain script (default: brains/NewAutopilot)
 *     -noplayers N     Number of bot players (default: 1)
 *     -map PATH        Map file (default: built-in Everard Island)
 *     -follow N        Follow bot N with camera (default: 0)
 *
 *   Controls:
 *     Arrow keys       Scroll the map (free camera)
 *     Space            Pause / unpause
 *     Tab              Cycle followed bot
 *     1                Toggle influence overlay
 *     2                Toggle danger overlay
 *     3                Toggle front line overlay
 *     4                Toggle path overlay
 *     0                Toggle all overlays off
 *     +/-              Zoom in/out
 *     Scroll wheel     Zoom in/out
 *     F                Toggle free camera / follow mode
 *     Left click       Show A* cost + path to clicked tile (magenta)
 *     Right click      Clear click path overlay
 *********************************************************/

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "../bolo/global.h"
#include "../bolo/everard_map.h"
#include "../bolo/tank.h"
#include "../bolo/bot_manager.h"
#include "../bolo/players.h"
#include "../bolo/bolo_map.h"
#include "../bolo/pillbox.h"
#include "../bolo/bases.h"
#include "../bolo/starts.h"
#include "../bolo/allience.h"
#include "../bolo/brain_pathfinder.h"
#include "../bolo/gui_message.h"
#include "../server/server_sim.h"
#include "../gui/sdl3/mapview.h"
#include "../gui/sdl3/tileloader.h"
#include "../gui/sdl3/luabrainshandler.h"
#include "../gui/clientmutex.h"
#include "../gui/lang.h"

/* ------------------------------------------------------------------ */
/* Globals needed by the engine                                        */
/* ------------------------------------------------------------------ */
bool isInMenu = FALSE;

/* ------------------------------------------------------------------ */
/* gameFront* stubs                                                    */
/* ------------------------------------------------------------------ */
void gameFrontGetPassword(char *pword) { pword[0] = '\0'; }
void gameFrontGetPlayerName(char *pn) { strcpy(pn, "BrainTest"); }
void gameFrontSetPlayerName(char *pn) { (void)pn; }
void gameFrontSetAIType(aiType ait) { (void)ait; }
void gameFrontEnableRejoin(void) {}

time_t windowsGetTicks(void) { return (time_t)winboloTimer(); }
time_t serverMainGetTicks(void) { return (time_t)winboloTimer(); }

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define TICK_INTERVAL_MS   20   /* 50 Hz game tick */
#define DEFAULT_WINDOW_W  1280
#define DEFAULT_WINDOW_H   800
#define SCROLL_SPEED        4   /* map tiles per second of held arrow key */
#define MIN_ZOOM            1
#define MAX_ZOOM           16

/* ------------------------------------------------------------------ */
/* App state                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    ServerSim    sim;
    bool         simValid;
    SDL_Window  *window;
    SDL_Renderer*renderer;
    SDL_Texture *tilesTex;

    /* Camera (world coords) */
    WORLD        viewCenterX;
    WORLD        viewCenterY;
    bool         freeCamera;     /* true = arrow keys, false = follow bot */
    BYTE         followBot;      /* bot slot to follow */

    /* Map bounds */
    int          mapMinX, mapMinY, mapMaxX, mapMaxY;

    /* Simulation */
    BYTE         numBots;
    bool         paused;
    int          zoomFactor;

    /* Overlay toggles */
    bool         showInfluence;
    bool         showDanger;
    bool         showFrontLine;
    bool         showPath;
    bool         showHUD;          /* status bar + legend (toggle with H) */

    /* Overlay texture (256x256 RGBA, updated once per game tick) */
    SDL_Texture *overlayTex;
    uint32_t     overlayTick;   /* sim tick when overlay was last updated */
    bool         overlayDirty;  /* force update on toggle change */

    /* Cached path (tracePath only works when A* status==1) */
    int          cachedPath_x[2048];
    int          cachedPath_y[2048];
    int          cachedPathLen;

    /* Debug pathfinder (for click-to-cost, independent of brain's PF) */
    BrainPathfinder *debugPF;

    /* Mouse / drag-to-pan state */
    bool         dragging;              /* middle or right drag active */
    bool         rightDown;             /* right button is held */
    bool         rightDragged;          /* right button moved (drag vs click) */
    float        dragLastX, dragLastY;  /* last drag screen position */

    /* Mouse / click-to-cost state */
    float        mouseX, mouseY;        /* screen coords */
    int          hoverMX, hoverMY;      /* map tile under cursor */
    bool         hoverValid;

    bool         clickActive;           /* a click-cost query is active */
    int          clickMX, clickMY;      /* clicked tile */
    float        clickCost;             /* A* or estimate cost tank → click */
    float        clickEstCost;          /* linear estimate cost (same as brain uses) */
    int          clickPath_x[2048];     /* traced path to clicked tile */
    int          clickPath_y[2048];
    int          clickPathLen;

    /* Goal info (refreshed per tick) */
    BrainGoalInfo goalInfo;
    bool         goalInfoValid;
    float        targetCost;            /* A* cost to current goal target */
    float        targetEstCost;         /* linear estimate (what brain uses for ranking) */
} BrainTestApp;

static volatile bool appQuit = FALSE;

static void signalHandler(int sig) {
    (void)sig;
    appQuit = TRUE;
}

/* ------------------------------------------------------------------ */
/* Command-line parsing                                                */
/* ------------------------------------------------------------------ */

static char optBrain[512] = "brains/NewAutopilot";
static char optMap[512]   = "";
static int  optNumPlayers = 1;
static int  optFollow     = 0;
static aiType  optAI      = aiFull;
static gameType optGame   = gameOpen;

static void printUsage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -brain PATH      Brain script directory (default: brains/NewAutopilot)\n"
        "  -noplayers N     Number of bot players (default: 1)\n"
        "  -map PATH        Map file (default: built-in Everard Island)\n"
        "  -follow N        Follow bot N with camera (default: 0)\n"
        "  -ai TYPE         AI type: none, yes, advantage, full (default: full)\n"
        "  -game TYPE       Game type: open, tournament, strict (default: open)\n"
        "\n"
        "Controls:\n"
        "  Arrows           Scroll map (switches to free camera)\n"
        "  Space            Pause / unpause\n"
        "  Tab              Cycle followed bot\n"
        "  1-4              Toggle overlays (influence/danger/frontline/path)\n"
        "  0                All overlays off\n"
        "  +/-/scroll       Zoom in/out\n"
        "  F                Toggle free camera / follow mode\n"
        "  Left click       Show A* cost + path to tile (magenta)\n"
        "  Right click      Clear click overlay\n",
        prog);
}

static bool parseArgs(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-brain") == 0 || strcmp(argv[i], "--brain") == 0) && i + 1 < argc) {
            strncpy(optBrain, argv[++i], sizeof(optBrain) - 1);
        } else if ((strcmp(argv[i], "-noplayers") == 0 || strcmp(argv[i], "--noplayers") == 0) && i + 1 < argc) {
            optNumPlayers = atoi(argv[++i]);
            if (optNumPlayers < 1) optNumPlayers = 1;
            if (optNumPlayers > 16) optNumPlayers = 16;
        } else if ((strcmp(argv[i], "-map") == 0 || strcmp(argv[i], "--map") == 0) && i + 1 < argc) {
            strncpy(optMap, argv[++i], sizeof(optMap) - 1);
        } else if ((strcmp(argv[i], "-follow") == 0 || strcmp(argv[i], "--follow") == 0) && i + 1 < argc) {
            optFollow = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-ai") == 0 || strcmp(argv[i], "--ai") == 0) && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "none") == 0)           optAI = aiNone;
            else if (strcmp(v, "yes") == 0)        optAI = aiYes;
            else if (strcmp(v, "advantage") == 0)  optAI = aiYesAdvantage;
            else if (strcmp(v, "full") == 0)        optAI = aiFull;
            else { fprintf(stderr, "Unknown AI type: %s\n", v); return FALSE; }
        } else if ((strcmp(argv[i], "-game") == 0 || strcmp(argv[i], "--game") == 0) && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "open") == 0)             optGame = gameOpen;
            else if (strcmp(v, "tournament") == 0)  optGame = gameTournament;
            else if (strcmp(v, "strict") == 0)      optGame = gameStrictTournament;
            else { fprintf(stderr, "Unknown game type: %s\n", v); return FALSE; }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            printUsage(argv[0]);
            return FALSE;
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Find brain script                                                   */
/* ------------------------------------------------------------------ */

static bool findBrainScript(const char *base, char *out, size_t outLen) {
    /* Try: base/init.lua, base.lua, base as-is */
    char tryPath[1024];

    SDL_snprintf(tryPath, sizeof(tryPath), "%s/init.lua", base);
    SDL_IOStream *io = SDL_IOFromFile(tryPath, "r");
    if (io) { SDL_CloseIO(io); SDL_snprintf(out, outLen, "%s", tryPath); return true; }

    SDL_snprintf(tryPath, sizeof(tryPath), "%s.lua", base);
    io = SDL_IOFromFile(tryPath, "r");
    if (io) { SDL_CloseIO(io); SDL_snprintf(out, outLen, "%s", tryPath); return true; }

    io = SDL_IOFromFile(base, "r");
    if (io) { SDL_CloseIO(io); SDL_snprintf(out, outLen, "%s", base); return true; }

    /* Try Brains/ prefix variants (including ../brains/ for build dirs) */
    const char *prefixes[] = { "Brains/", "brains/", "../brains/", "data/Brains/" };
    for (int i = 0; i < 4; i++) {
        SDL_snprintf(tryPath, sizeof(tryPath), "%s%s/init.lua", prefixes[i], base);
        io = SDL_IOFromFile(tryPath, "r");
        if (io) { SDL_CloseIO(io); SDL_snprintf(out, outLen, "%s", tryPath); return true; }
    }

    return false;
}

/* ------------------------------------------------------------------ */
/* Map bounds calculation                                              */
/* ------------------------------------------------------------------ */

static void calcMapBounds(BrainTestApp *app) {
    GameSim *gs = &app->sim.sim;
    int minX = 255, minY = 255, maxX = 0, maxY = 0;

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
    BYTE np = pillsGetNumPills(&gs->pb);
    for (BYTE i = 1; i <= np; i++) {
        pillbox p; pillsGetPill(&gs->pb, &p, i);
        if (p.x < minX) minX = p.x; if (p.x > maxX) maxX = p.x;
        if (p.y < minY) minY = p.y; if (p.y > maxY) maxY = p.y;
    }
    BYTE nb = basesGetNumBases(&gs->bs);
    for (BYTE i = 1; i <= nb; i++) {
        base b; basesGetBase(&gs->bs, &b, i);
        if (b.x < minX) minX = b.x; if (b.x > maxX) maxX = b.x;
        if (b.y < minY) minY = b.y; if (b.y > maxY) maxY = b.y;
    }
    BYTE ns = startsGetNumStarts(&gs->ss);
    for (BYTE i = 1; i <= ns; i++) {
        start st; startsGetStartStruct(&gs->ss, &st, i);
        if (st.x < minX) minX = st.x; if (st.x > maxX) maxX = st.x;
        if (st.y < minY) minY = st.y; if (st.y > maxY) maxY = st.y;
    }

    int pad = 5;
    if (minX > pad) minX -= pad; else minX = 0;
    if (minY > pad) minY -= pad; else minY = 0;
    if (maxX + pad < 255) maxX += pad; else maxX = 255;
    if (maxY + pad < 255) maxY += pad; else maxY = 255;

    app->mapMinX = minX; app->mapMinY = minY;
    app->mapMaxX = maxX; app->mapMaxY = maxY;
}

/* ------------------------------------------------------------------ */
/* Screen ↔ map coordinate conversion                                  */
/* ------------------------------------------------------------------ */

static bool screenToMap(BrainTestApp *app, float sx, float sy,
                        int screenW, int screenH, int *outMX, int *outMY) {
    int zf = app->zoomFactor;
    int tilePixels = 16 * zf;
    int centerMX = app->viewCenterX >> 8;
    int centerMY = app->viewCenterY >> 8;
    float scx = screenW / 2.0f;
    float scy = screenH / 2.0f;
    float mapOriginX = scx - centerMX * tilePixels
                       - (float)(app->viewCenterX & 0xFF) * tilePixels / 256.0f;
    float mapOriginY = scy - centerMY * tilePixels
                       - (float)(app->viewCenterY & 0xFF) * tilePixels / 256.0f;

    int mx = (int)floorf((sx - mapOriginX) / tilePixels);
    int my = (int)floorf((sy - mapOriginY) / tilePixels);
    if (mx < 0 || mx > 255 || my < 0 || my > 255) return false;
    *outMX = mx;
    *outMY = my;
    return true;
}

/* ------------------------------------------------------------------ */
/* Debug pathfinder sync & click-cost computation                      */
/* ------------------------------------------------------------------ */

/* Copy terrain/danger/influence/config from brain's pathfinder to debug PF */
static void syncDebugPathfinder(BrainTestApp *app) {
    BrainPathfinder *src = botManagerGetBrainPathfinder(app->followBot);
    BrainPathfinder *dst = app->debugPF;
    if (!src || !dst) return;

    dst->map = src->map;
    memcpy(dst->danger_grid, src->danger_grid, sizeof(src->danger_grid));
    memcpy(dst->influence_grid, src->influence_grid, sizeof(src->influence_grid));
    memcpy(dst->overlay_grid, src->overlay_grid, sizeof(src->overlay_grid));
    memcpy(dst->terrain_cost_table, src->terrain_cost_table, sizeof(src->terrain_cost_table));
    memcpy(dst->terrain_cost_boat_table, src->terrain_cost_boat_table, sizeof(src->terrain_cost_boat_table));
    memcpy(dst->terrain_speed_table, src->terrain_speed_table, sizeof(src->terrain_speed_table));
    dst->turn_cost = src->turn_cost;
    dst->wall_shoot_cost = src->wall_shoot_cost;
    dst->wall_shoot_shells = src->wall_shoot_shells;
    dst->shell_reserve = src->shell_reserve;
    dst->road_build_cost = src->road_build_cost;
    dst->tree_reserve = src->tree_reserve;
    dst->mine_penalty = src->mine_penalty;
    dst->estimate_samples = src->estimate_samples;
    dst->water_drain_rate = src->water_drain_rate;
    dst->shell_loss_cost = src->shell_loss_cost;
    dst->mine_loss_cost = src->mine_loss_cost;
    dst->armour_drain_rate = src->armour_drain_rate;
    dst->min_shells = src->min_shells;
    dst->min_mines = src->min_mines;
    dst->min_armour = src->min_armour;
}

/* Run a full A* from tank to (dmx,dmy) on the debug pathfinder */
static void computeClickPath(BrainTestApp *app, int dmx, int dmy) {
    BrainPathfinder *dpf = app->debugPF;
    if (!dpf || !dpf->map) return;

    WORLD twx, twy;
    if (!serverSimGetTankState(&app->sim, app->followBot, &twx, &twy)) return;
    int smx = twx >> 8;
    int smy = twy >> 8;
    int in_boat = (app->sim.sim.tanks[app->followBot] != NULL &&
                   tankIsOnBoat(&app->sim.sim.tanks[app->followBot])) ? 1 : 0;

    /* Reset the debug PF search state by changing destination */
    dpf->status = 0;
    dpf->dest_x = -1;
    dpf->dest_y = -1;

    /* Run A* to completion with a generous budget */
    int nx, ny;
    int status = 0;
    for (int iter = 0; iter < 20 && status == 0; iter++) {
        status = brainPathfinderPathTo(dpf, smx, smy, dmx, dmy,
                                        in_boat, 40, 20, 20, 40,
                                        100000, &nx, &ny);
    }

    app->clickCost = (status == 1)
        ? dpf->g_cost[dmy * 256 + dmx]
        : brainPathfinderEstimateCost(dpf, smx, smy, dmx, dmy, in_boat);
    app->clickEstCost = brainPathfinderEstimateCost(dpf, smx, smy, dmx, dmy, in_boat);

    /* Trace the path */
    if (status == 1) {
        app->clickPathLen = brainPathfinderTracePath(dpf,
            app->clickPath_x, app->clickPath_y, 2048);
    } else {
        app->clickPathLen = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Debug overlay rendering                                             */
/* ------------------------------------------------------------------ */

/* RGBA pixel helpers */
static inline void setPixel(uint8_t *pixels, int pitch, int x, int y,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint8_t *p = pixels + y * pitch + x * 4;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

static inline void blendPixel(uint8_t *pixels, int pitch, int x, int y,
                               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint8_t *p = pixels + y * pitch + x * 4;
    /* Simple additive alpha: keep max alpha, blend color */
    uint8_t oldA = p[3];
    if (a > oldA) {
        p[0] = r; p[1] = g; p[2] = b; p[3] = a;
    } else if (oldA > 0) {
        /* Blend towards new color */
        int blend = a * 255 / (oldA + a);
        p[0] = (uint8_t)(p[0] + (r - p[0]) * blend / 255);
        p[1] = (uint8_t)(p[1] + (g - p[1]) * blend / 255);
        p[2] = (uint8_t)(p[2] + (b - p[2]) * blend / 255);
        p[3] = (uint8_t)(oldA + (255 - oldA) * a / 255);
    }
}

static void updateOverlayTexture(BrainTestApp *app) {
    BrainPathfinder *pf = botManagerGetBrainPathfinder(app->followBot);
    if (!pf) return;
    if (!app->showInfluence && !app->showDanger && !app->showFrontLine) return;

    /* Only rebuild when the sim has ticked or overlays were toggled */
    if (app->sim.tick == app->overlayTick && !app->overlayDirty) return;
    app->overlayTick = app->sim.tick;
    app->overlayDirty = false;

    /* Lock the 256x256 overlay texture */
    SDL_Surface *surf = NULL;
    if (!SDL_LockTextureToSurface(app->overlayTex, NULL, &surf)) return;

    uint8_t *pixels = (uint8_t *)surf->pixels;
    int pitch = surf->pitch;

    /* Clear to transparent */
    memset(pixels, 0, 256 * pitch);

    /* Influence overlay: blue (friendly +) / red (hostile -) */
    if (app->showInfluence) {
        for (int y = 0; y < 256; y++) {
            for (int x = 0; x < 256; x++) {
                int16_t inf = brainPathfinderInfluenceAt(pf, x, y);
                if (inf == 0) continue;
                if (inf > 0) {
                    /* Friendly = blue, intensity proportional to value */
                    int v = inf > 100 ? 100 : inf;
                    uint8_t a = (uint8_t)(60 + v * 160 / 100);
                    blendPixel(pixels, pitch, x, y, 40, 80, 255, a);
                } else {
                    /* Hostile = red */
                    int v = -inf > 100 ? 100 : -inf;
                    uint8_t a = (uint8_t)(60 + v * 160 / 100);
                    blendPixel(pixels, pitch, x, y, 255, 40, 40, a);
                }
            }
        }
    }

    /* Danger overlay: red/orange intensity */
    if (app->showDanger) {
        for (int y = 0; y < 256; y++) {
            for (int x = 0; x < 256; x++) {
                float d = brainPathfinderDangerAt(pf, x, y);
                if (d <= 0.0f) continue;
                int v = (int)(d * 8.0f);
                if (v > 200) v = 200;
                uint8_t a = (uint8_t)(60 + v * 180 / 200);
                blendPixel(pixels, pitch, x, y, 255, 120, 0, a);
            }
        }
    }

    /* Front line overlay: bright yellow points */
    if (app->showFrontLine) {
        int flx[4096], fly[4096];
        int n = brainPathfinderFindFrontLine(pf, flx, fly, 4096);
        for (int i = 0; i < n; i++) {
            if (flx[i] >= 0 && flx[i] < 256 && fly[i] >= 0 && fly[i] < 256) {
                setPixel(pixels, pitch, flx[i], fly[i], 255, 255, 0, 220);
            }
        }
    }

    SDL_UnlockTexture(app->overlayTex);
}

/* Cache the brain's A* path for screen-space rendering */
static void updateCachedPath(BrainTestApp *app) {
    if (!app->showPath) return;
    BrainPathfinder *pf = botManagerGetBrainPathfinder(app->followBot);
    if (!pf) return;
    int n = brainPathfinderTracePath(pf, app->cachedPath_x, app->cachedPath_y, 2048);
    if (n > 0) {
        app->cachedPathLen = n;
    }
}

/* ------------------------------------------------------------------ */
/* Render the debug overlay on top of the map                          */
/* ------------------------------------------------------------------ */

/* Convert map tile coords to screen center of that tile */
static void mapToScreen(BrainTestApp *app, int mx, int my,
                         int screenW, int screenH,
                         float *outSX, float *outSY) {
    int zf = app->zoomFactor;
    int tilePixels = 16 * zf;
    int centerMX = app->viewCenterX >> 8;
    int centerMY = app->viewCenterY >> 8;
    float scx = screenW / 2.0f;
    float scy = screenH / 2.0f;
    float mapOriginX = scx - centerMX * tilePixels
                       - (float)(app->viewCenterX & 0xFF) * tilePixels / 256.0f;
    float mapOriginY = scy - centerMY * tilePixels
                       - (float)(app->viewCenterY & 0xFF) * tilePixels / 256.0f;
    *outSX = mapOriginX + ((float)mx + 0.5f) * tilePixels;
    *outSY = mapOriginY + ((float)my + 0.5f) * tilePixels;
}

/* Draw a path as connected screen-space lines */
static void renderPathLines(BrainTestApp *app, int screenW, int screenH,
                             int *path_x, int *path_y, int pathLen,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (pathLen < 2) return;
    SDL_SetRenderDrawColor(app->renderer, r, g, b, a);
    for (int i = 0; i < pathLen - 1; i++) {
        float x1, y1, x2, y2;
        mapToScreen(app, path_x[i], path_y[i], screenW, screenH, &x1, &y1);
        mapToScreen(app, path_x[i+1], path_y[i+1], screenW, screenH, &x2, &y2);
        SDL_RenderLine(app->renderer, x1, y1, x2, y2);
    }
}

static void renderOverlay(BrainTestApp *app, int screenW, int screenH) {
    /* Update cached path data (independent of grid overlays) */
    updateCachedPath(app);

    bool hasGridOverlay = app->showInfluence || app->showDanger || app->showFrontLine;
    bool hasPathOverlay = (app->showPath && app->cachedPathLen > 0) ||
                          (app->clickActive && app->clickPathLen > 0);

    if (!hasGridOverlay && !hasPathOverlay) return;

    /* Grid overlays (influence/danger/frontline) via texture */
    if (hasGridOverlay) {
        updateOverlayTexture(app);

        int zf = app->zoomFactor;
        int tilePixels = 16 * zf;
        int centerMX = app->viewCenterX >> 8;
        int centerMY = app->viewCenterY >> 8;
        float scx = screenW / 2.0f;
        float scy = screenH / 2.0f;
        float mapOriginX = scx - centerMX * tilePixels
                           - (float)(app->viewCenterX & 0xFF) * tilePixels / 256.0f;
        float mapOriginY = scy - centerMY * tilePixels
                           - (float)(app->viewCenterY & 0xFF) * tilePixels / 256.0f;

        SDL_FRect destRect = {
            mapOriginX, mapOriginY,
            256.0f * tilePixels, 256.0f * tilePixels
        };

        SDL_SetTextureBlendMode(app->overlayTex, SDL_BLENDMODE_BLEND);
        SDL_RenderTexture(app->renderer, app->overlayTex, NULL, &destRect);
    }

    /* Path overlays as screen-space lines */
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);

    if (app->showPath && app->cachedPathLen > 0) {
        renderPathLines(app, screenW, screenH,
                        app->cachedPath_x, app->cachedPath_y, app->cachedPathLen,
                        0, 255, 80, 240);
    }

    if (app->clickActive && app->clickPathLen > 0) {
        renderPathLines(app, screenW, screenH,
                        app->clickPath_x, app->clickPath_y, app->clickPathLen,
                        255, 100, 255, 220);
        /* Click destination marker — small cross */
        float cx, cy;
        mapToScreen(app, app->clickMX, app->clickMY, screenW, screenH, &cx, &cy);
        SDL_SetRenderDrawColor(app->renderer, 255, 255, 255, 255);
        SDL_RenderLine(app->renderer, cx - 4, cy, cx + 4, cy);
        SDL_RenderLine(app->renderer, cx, cy - 4, cx, cy + 4);
    }
}

/* ------------------------------------------------------------------ */
/* HUD text rendering                                                  */
/* ------------------------------------------------------------------ */

/* Helper: draw a line of debug text at 2x scale */
static float drawHudLine(SDL_Renderer *r, float x, float y, float scale,
                          uint8_t cr, uint8_t cg, uint8_t cb, const char *text) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
    SDL_SetRenderScale(r, scale, scale);
    SDL_RenderDebugText(r, x / scale, y / scale, text);
    SDL_SetRenderScale(r, 1.0f, 1.0f);
    return y + 8.0f * scale + 2.0f;
}

static void renderHUD(BrainTestApp *app, int screenW, int screenH) {
    BrainPathfinder *pf = botManagerGetBrainPathfinder(app->followBot);
    char line[256];

    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);

    /* Tick counter — show brain tick (sim.tick/2) to match JSONL log */
    SDL_snprintf(line, sizeof(line), "Tick: %u", app->sim.tick / 2);
    float tickScale = 3.0f;
    float tickW = (float)strlen(line) * 8.0f * tickScale;
    float tickH = 8.0f * tickScale + 12.0f;
    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 180);
    SDL_FRect tickBg = { 0, 0, tickW + 16.0f, tickH };
    SDL_RenderFillRect(app->renderer, &tickBg);
    SDL_SetRenderDrawColor(app->renderer, 255, 255, 255, 255);
    SDL_SetRenderScale(app->renderer, tickScale, tickScale);
    SDL_RenderDebugText(app->renderer, 6.0f / tickScale, 6.0f / tickScale, line);
    SDL_SetRenderScale(app->renderer, 1.0f, 1.0f);

    /* Status bar below the tick counter (toggle with H) */
    if (app->showHUD) {
        float statusScale = 2.0f;
        float statusY = tickH;
        float statusH = 8.0f * statusScale + 8.0f;
        SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 160);
        SDL_FRect bg = { 0, statusY, (float)screenW, statusH };
        SDL_RenderFillRect(app->renderer, &bg);

        SDL_SetRenderDrawColor(app->renderer, 200, 200, 200, 255);

        SDL_snprintf(line, sizeof(line),
            "Bot:%d %s | %s | Zoom:%d | [1]Inf%s [2]Dng%s [3]FL%s [4]Path%s | [Space]%s [F]%s [H]HUD [Tab]Next",
            app->followBot,
            app->freeCamera ? "FREE" : "FOLLOW",
            pf ? "PF:ok" : "PF:none",
            app->zoomFactor,
            app->showInfluence ? "*" : "",
            app->showDanger ? "*" : "",
            app->showFrontLine ? "*" : "",
            app->showPath ? "*" : "",
            app->paused ? "PAUSED" : "running",
            app->freeCamera ? "follow" : "free");

        SDL_SetRenderScale(app->renderer, statusScale, statusScale);
        SDL_RenderDebugText(app->renderer, 4.0f / statusScale, (statusY + 4.0f) / statusScale, line);
        SDL_SetRenderScale(app->renderer, 1.0f, 1.0f);
    }

    /* ── Goal info panel (top-right) ── */
    if (app->goalInfoValid) {
        float ps = 1.0f;  /* panel text scale */
        float lineH = 8.0f * ps + 1.0f;
        float panelW = 480.0f;
        /* Clamp panel to screen */
        if (panelW > (float)screenW - 8.0f) panelW = (float)screenW - 8.0f;
        float panelX = (float)screenW - panelW - 2.0f;
        float py = 2.0f;

        /* Count lines for background height */
        int numLines = 2;  /* goal + cost */
        if (app->goalInfo.substate[0]) numLines++;
        int maxCandidates = app->goalInfo.num_candidates;
        /* Clamp candidates to fit screen */
        float availH = (float)screenH - py - 8.0f;
        int maxFit = (int)((availH - lineH * (float)numLines - 4.0f) / lineH) - 1;
        if (maxFit < 0) maxFit = 0;
        if (maxCandidates > maxFit) maxCandidates = maxFit;
        if (maxCandidates > 0) numLines += 1 + maxCandidates; /* header + items */

        float panelH = lineH * (float)numLines + 4.0f;
        SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 180);
        SDL_FRect panelBg = { panelX, py, panelW, panelH };
        SDL_RenderFillRect(app->renderer, &panelBg);

        /* Current goal */
        if (app->goalInfo.target_id >= 0) {
            SDL_snprintf(line, sizeof(line), "Goal: %s #%d (%d,%d)",
                         app->goalInfo.kind, app->goalInfo.target_id,
                         app->goalInfo.mx, app->goalInfo.my);
        } else {
            SDL_snprintf(line, sizeof(line), "Goal: %s (%d,%d)",
                         app->goalInfo.kind, app->goalInfo.mx, app->goalInfo.my);
        }
        py = drawHudLine(app->renderer, panelX + 3.0f, py + 1.0f, ps,
                         100, 255, 100, line);

        /* Substate if present */
        if (app->goalInfo.substate[0]) {
            SDL_snprintf(line, sizeof(line), " sub: %s", app->goalInfo.substate);
            py = drawHudLine(app->renderer, panelX + 3.0f, py, ps,
                             180, 180, 180, line);
        }

        /* Target cost + pathfinder status */
        {
            const char *pfSt = pf ? (pf->status == 1 ? "done" :
                                     pf->status == 0 ? "run" : "fail") : "-";
            if (pf && pf->status == 1 &&
                pf->dest_x == app->goalInfo.mx && pf->dest_y == app->goalInfo.my) {
                SDL_snprintf(line, sizeof(line), "A*=%.0f  est=%.0f  PF:%s",
                             app->targetCost, app->targetEstCost, pfSt);
            } else if (pf) {
                SDL_snprintf(line, sizeof(line), "est=%.0f  PF:%s",
                             app->targetEstCost, pfSt);
            } else {
                SDL_snprintf(line, sizeof(line), "cost:- PF:%s", pfSt);
            }
        }
        py = drawHudLine(app->renderer, panelX + 3.0f, py, ps,
                         200, 200, 100, line);

        /* Candidate pool */
        if (maxCandidates > 0) {
            py = drawHudLine(app->renderer, panelX + 3.0f, py, ps,
                             140, 140, 140, "-- candidates --");
            for (int i = 0; i < maxCandidates; i++) {
                SDL_snprintf(line, sizeof(line), "%s%-30s %5.0f x%.1f",
                             app->goalInfo.candidates[i].winner ? ">" : " ",
                             app->goalInfo.candidates[i].desc,
                             app->goalInfo.candidates[i].cost,
                             app->goalInfo.candidates[i].phase_weight);
                uint8_t cr = app->goalInfo.candidates[i].winner ? 100 : 150;
                uint8_t cg = app->goalInfo.candidates[i].winner ? 255 : 150;
                uint8_t cb = app->goalInfo.candidates[i].winner ? 100 : 150;
                py = drawHudLine(app->renderer, panelX + 3.0f, py, ps,
                                 cr, cg, cb, line);
            }
        }
    }

    /* ── Click cost display (bottom-right) ── */
    if (app->clickActive) {
        float cs = 2.0f;
        float lineH2 = 8.0f * cs + 2.0f;
        char line2[256];
        SDL_snprintf(line, sizeof(line), "Click (%d,%d) A*=%.0f  est=%.0f",
                     app->clickMX, app->clickMY, app->clickCost, app->clickEstCost);
        /* Show which cost the brain uses for goal selection */
        SDL_snprintf(line2, sizeof(line2), "brain uses est for goal ranking");
        float lw1 = (float)strlen(line) * 8.0f * cs;
        float lw2 = (float)strlen(line2) * 8.0f * cs;
        float cw = (lw1 > lw2 ? lw1 : lw2) + 12.0f;
        float ch = lineH2 * 2.0f + 8.0f;
        float cx = (float)screenW - cw - 4.0f;
        float cy = (float)screenH - ch - 4.0f;
        SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 180);
        SDL_FRect cbg = { cx, cy, cw, ch };
        SDL_RenderFillRect(app->renderer, &cbg);
        drawHudLine(app->renderer, cx + 4.0f, cy + 4.0f, cs, 255, 100, 255, line);
        drawHudLine(app->renderer, cx + 4.0f, cy + 4.0f + lineH2, cs, 180, 180, 100, line2);
    }

    /* ── Hover tile coords (bottom center) ── */
    if (app->hoverValid) {
        float hs = 1.5f;
        SDL_snprintf(line, sizeof(line), "Tile (%d,%d)", app->hoverMX, app->hoverMY);
        float hw = (float)strlen(line) * 8.0f * hs + 8.0f;
        float hh = 8.0f * hs + 6.0f;
        float hx = ((float)screenW - hw) / 2.0f;
        float hy = (float)screenH - hh - 4.0f;
        SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 140);
        SDL_FRect hbg = { hx, hy, hw, hh };
        SDL_RenderFillRect(app->renderer, &hbg);
        drawHudLine(app->renderer, hx + 4.0f, hy + 3.0f, hs, 200, 200, 200, line);
    }

    /* Overlay legend at bottom-left (toggle with H) */
    if (app->showHUD && (app->showInfluence || app->showDanger || app->showFrontLine
        || app->showPath || app->clickActive)) {
        float legScale = 2.0f;
        float legH = 8.0f * legScale + 8.0f;
        float legY = (float)screenH - legH;

        char legend[256] = "";
        if (app->showInfluence) strcat(legend, "Blue=friendly Red=hostile  ");
        if (app->showDanger)    strcat(legend, "Orange=danger  ");
        if (app->showFrontLine) strcat(legend, "Yellow=frontline  ");
        if (app->showPath)      strcat(legend, "Green=path  ");
        if (app->clickActive)   strcat(legend, "Magenta=click  ");

        float legW = (float)strlen(legend) * 8.0f * legScale + 12.0f;
        SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 160);
        SDL_FRect legendBg = { 0, legY, legW, legH };
        SDL_RenderFillRect(app->renderer, &legendBg);

        SDL_SetRenderDrawColor(app->renderer, 200, 200, 200, 255);
        SDL_SetRenderScale(app->renderer, legScale, legScale);
        SDL_RenderDebugText(app->renderer, 4.0f / legScale, (legY + 4.0f) / legScale, legend);
        SDL_SetRenderScale(app->renderer, 1.0f, 1.0f);
    }
}

/* ------------------------------------------------------------------ */
/* Tick and render                                                     */
/* ------------------------------------------------------------------ */

static void appTick(BrainTestApp *app) {
    if (!app->simValid || app->numBots == 0) return;

    /* Propagate frontend state into the sim struct */
    app->sim.sim.isInMenu = isInMenu;

    /* Run brain AI then tick the simulation (two ticks per brain call) */
    botManagerTick(&app->sim, optAI);
    serverSimTick(&app->sim);
    {
        GameEvent savedEvents[MAX_SNAPSHOT_EVENTS];
        uint8_t savedCount = app->sim.eventCount;
        if (savedCount > 0) {
            memcpy(savedEvents, app->sim.events,
                   savedCount * sizeof(GameEvent));
        }
        serverSimTick(&app->sim);
        if (savedCount > 0 && savedCount + app->sim.eventCount <= MAX_SNAPSHOT_EVENTS) {
            memmove(app->sim.events + savedCount, app->sim.events,
                    app->sim.eventCount * sizeof(GameEvent));
            memcpy(app->sim.events, savedEvents,
                   savedCount * sizeof(GameEvent));
            app->sim.eventCount += savedCount;
        }
    }

    /* Update camera if following a bot */
    if (!app->freeCamera && app->followBot < MAX_TANKS) {
        WORLD wx, wy;
        if (serverSimGetTankState(&app->sim, app->followBot, &wx, &wy)) {
            /* Smooth lerp */
            app->viewCenterX = app->viewCenterX + ((int)wx - (int)app->viewCenterX) / 4;
            app->viewCenterY = app->viewCenterY + ((int)wy - (int)app->viewCenterY) / 4;
        }
    }

    /* Sync debug pathfinder grids */
    syncDebugPathfinder(app);
}

static void refreshGoalInfo(BrainTestApp *app) {
    app->goalInfoValid = botManagerGetGoalInfo(app->followBot, &app->goalInfo);

    BrainPathfinder *pf = botManagerGetBrainPathfinder(app->followBot);
    if (pf && app->goalInfoValid && app->goalInfo.mx > 0) {
        WORLD twx, twy;
        bool hasTank = serverSimGetTankState(&app->sim, app->followBot, &twx, &twy);
        /* Always compute the estimate (this is what the brain uses for ranking) */
        if (hasTank) {
            app->targetEstCost = brainPathfinderEstimateCost(pf,
                twx >> 8, twy >> 8,
                app->goalInfo.mx, app->goalInfo.my,
                pf->in_boat);
        }
        if (pf->status == 1 &&
            pf->dest_x == app->goalInfo.mx && pf->dest_y == app->goalInfo.my) {
            app->targetCost = pf->g_cost[app->goalInfo.my * 256 + app->goalInfo.mx];
        } else if (hasTank) {
            app->targetCost = app->targetEstCost;
        }
    }
}

static void appRender(BrainTestApp *app) {
    int screenW, screenH;
    SDL_GetWindowSize(app->window, &screenW, &screenH);

    /* Refresh goal info every frame (works while paused too) */
    refreshGoalInfo(app);

    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
    SDL_RenderClear(app->renderer);

    if (app->simValid) {
        /* Set perspective to followed bot for correct coloring */
        BYTE prevSelf = app->sim.sim.viewPlayer;
        app->sim.sim.viewPlayer = app->followBot;

        MapViewCtx ctx = { app->renderer, app->tilesTex, app->zoomFactor, 1 };
        mapViewRenderCentered(&ctx, &app->sim.sim,
                              app->viewCenterX, app->viewCenterY,
                              0, 0, screenW, screenH, app->followBot);

        app->sim.sim.viewPlayer = prevSelf;

        /* Debug overlays */
        renderOverlay(app, screenW, screenH);
    }

    /* HUD */
    renderHUD(app, screenW, screenH);

    SDL_RenderPresent(app->renderer);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    srand((unsigned int)(time(NULL) ^ getpid()));

    BrainTestApp app;
    memset(&app, 0, sizeof(app));
    app.zoomFactor = 2;
    app.freeCamera = false;
    app.showPath = true;
    app.showHUD = false;

    if (!parseArgs(argc, argv)) return 1;
    app.followBot = (BYTE)optFollow;

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    static const char *aiNames[]   = { "none", "yes", "advantage", "full" };
    static const char *gameNames[] = { "?", "open", "tournament", "strict" };
    fprintf(stderr, "BrainTest - Brain Debug Viewer\n");
    fprintf(stderr, "  Brain:   %s\n", optBrain);
    fprintf(stderr, "  Players: %d\n", optNumPlayers);
    fprintf(stderr, "  Map:     %s\n", optMap[0] ? optMap : "(built-in Everard Island)");
    fprintf(stderr, "  AI:      %s\n", aiNames[optAI]);
    fprintf(stderr, "  Game:    %s\n", gameNames[optGame]);

    /* Initialize SDL */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    initWinboloTimer();
    clientMutexCreate();
    langSetup();

    /* Create window */
    app.window = SDL_CreateWindow("BrainTest",
                                  DEFAULT_WINDOW_W, DEFAULT_WINDOW_H,
                                  SDL_WINDOW_RESIZABLE);
    if (!app.window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    app.renderer = SDL_CreateRenderer(app.window, NULL);
    if (!app.renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(app.window);
        return 1;
    }
    SDL_SetRenderVSync(app.renderer, 1);

    /* Initialize map view lookup tables */
    mapViewInit();

    /* Build tile atlas */
    SDL_Surface *sheet = tileLoaderBuildSheet(16);
    if (!sheet) {
        fprintf(stderr, "tileLoaderBuildSheet failed\n");
        SDL_DestroyRenderer(app.renderer);
        SDL_DestroyWindow(app.window);
        return 1;
    }
    app.tilesTex = SDL_CreateTextureFromSurface(app.renderer, sheet);
    SDL_DestroySurface(sheet);
    if (app.tilesTex) {
        SDL_SetTextureScaleMode(app.tilesTex, SDL_SCALEMODE_NEAREST);
    }

    /* Create overlay texture (256x256 RGBA) */
    app.overlayTex = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STREAMING, 256, 256);

    /* Load map */
    bool mapLoaded = false;
    if (optMap[0]) {
        mapLoaded = serverSimCreate(&app.sim, optMap, optGame, false, 0, -1);
        if (!mapLoaded) {
            fprintf(stderr, "Failed to load map '%s'\n", optMap);
        }
    }
    if (!mapLoaded) {
        BYTE emap[6000] = E_MAP;
        if (!serverSimCreateCompressed(&app.sim, emap, 5097, optGame, false, 0, -1)) {
            fprintf(stderr, "serverSimCreateCompressed failed\n");
            return 1;
        }
        strncpy(app.sim.mapName, "Everard Island", MAP_STR_SIZE - 1);
    }
    app.sim.lobbyEnabled = false;
    app.sim.state = serverStateRunning;
    app.simValid = true;

    /* Calculate map bounds */
    calcMapBounds(&app);
    app.viewCenterX = ((app.mapMinX + app.mapMaxX) / 2) << 8;
    app.viewCenterY = ((app.mapMinY + app.mapMaxY) / 2) << 8;

    /* Add bots */
    botManagerInit();
    char brainPath[1024];
    if (findBrainScript(optBrain, brainPath, sizeof(brainPath))) {
        fprintf(stderr, "  Brain script: %s\n", brainPath);
        for (int i = 0; i < optNumPlayers; i++) {
            char name[32];
            SDL_snprintf(name, sizeof(name), "Bot %d", i);
            if (botManagerAddBot(&app.sim, (BYTE)i, brainPath, name, optAI, optGame, false)) {
                app.numBots++;
            }
        }
        fprintf(stderr, "  Added %d bots\n", app.numBots);
    } else {
        fprintf(stderr, "  WARNING: Brain script not found: %s\n", optBrain);
    }

    /* Create debug pathfinder for click-to-cost queries */
    app.debugPF = brainPathfinderCreate();
    if (!app.debugPF) {
        fprintf(stderr, "WARNING: failed to create debug pathfinder\n");
    }

    fprintf(stderr, "  Map bounds: (%d,%d)-(%d,%d)\n",
            app.mapMinX, app.mapMinY, app.mapMaxX, app.mapMaxY);

    /* Main loop */
    Uint64 lastTickTime = SDL_GetTicks();
    const bool *keystate = SDL_GetKeyboardState(NULL);
    bool autoPauseDone = false;

    while (!appQuit) {
        /* Process events */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                appQuit = TRUE;
                break;

            case SDL_EVENT_KEY_DOWN:
                if (ev.key.repeat) break;
                switch (ev.key.key) {
                case SDLK_SPACE:
                    app.paused = !app.paused;
                    break;
                case SDLK_TAB:
                    /* Cycle to next active bot */
                    for (int tries = 0; tries < MAX_TANKS; tries++) {
                        app.followBot = (app.followBot + 1) % MAX_TANKS;
                        if (botManagerIsBot(app.followBot)) break;
                    }
                    break;
                case SDLK_F:
                    app.freeCamera = !app.freeCamera;
                    break;
                case SDLK_H:
                    app.showHUD = !app.showHUD;
                    break;
                case SDLK_1:
                    app.showInfluence = !app.showInfluence;
                    app.overlayDirty = true;
                    break;
                case SDLK_2:
                    app.showDanger = !app.showDanger;
                    app.overlayDirty = true;
                    break;
                case SDLK_3:
                    app.showFrontLine = !app.showFrontLine;
                    app.overlayDirty = true;
                    break;
                case SDLK_4:
                    app.showPath = !app.showPath;
                    app.overlayDirty = true;
                    break;
                case SDLK_0:
                    app.showInfluence = app.showDanger = app.showFrontLine = app.showPath = false;
                    app.overlayDirty = true;
                    break;
                case SDLK_EQUALS:
                case SDLK_KP_PLUS:
                    if (app.zoomFactor < MAX_ZOOM) app.zoomFactor++;
                    break;
                case SDLK_MINUS:
                case SDLK_KP_MINUS:
                    if (app.zoomFactor > MIN_ZOOM) app.zoomFactor--;
                    break;
                case SDLK_ESCAPE:
                    appQuit = TRUE;
                    break;
                default:
                    break;
                }
                break;

            case SDL_EVENT_MOUSE_MOTION:
                app.mouseX = ev.motion.x;
                app.mouseY = ev.motion.y;
                {
                    int sw, sh;
                    SDL_GetWindowSize(app.window, &sw, &sh);
                    app.hoverValid = screenToMap(&app, app.mouseX, app.mouseY,
                                                  sw, sh,
                                                  &app.hoverMX, &app.hoverMY);
                }
                /* Drag-to-pan */
                if (app.rightDown) app.rightDragged = true;
                if (app.dragging) {
                    float dx = ev.motion.x - app.dragLastX;
                    float dy = ev.motion.y - app.dragLastY;
                    app.dragLastX = ev.motion.x;
                    app.dragLastY = ev.motion.y;
                    int tilePixels = 16 * app.zoomFactor;
                    /* Convert screen pixel delta to world units */
                    int wmoveX = (int)(dx * 256.0f / (float)tilePixels);
                    int wmoveY = (int)(dy * 256.0f / (float)tilePixels);
                    int cx = (int)app.viewCenterX - wmoveX;
                    int cy = (int)app.viewCenterY - wmoveY;
                    if (cx < 0) cx = 0;
                    if (cy < 0) cy = 0;
                    if (cx > 65535) cx = 65535;
                    if (cy > 65535) cy = 65535;
                    app.viewCenterX = (WORLD)cx;
                    app.viewCenterY = (WORLD)cy;
                    app.freeCamera = true;
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button == SDL_BUTTON_MIDDLE) {
                    app.dragging = true;
                    app.dragLastX = ev.button.x;
                    app.dragLastY = ev.button.y;
                    app.freeCamera = true;
                } else if (ev.button.button == SDL_BUTTON_LEFT) {
                    int sw, sh;
                    SDL_GetWindowSize(app.window, &sw, &sh);
                    int cmx, cmy;
                    if (screenToMap(&app, ev.button.x, ev.button.y,
                                     sw, sh, &cmx, &cmy)) {
                        app.clickActive = true;
                        app.clickMX = cmx;
                        app.clickMY = cmy;
                        app.overlayDirty = true;
                        syncDebugPathfinder(&app);
                        computeClickPath(&app, cmx, cmy);
                    }
                } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                    app.rightDown = true;
                    app.rightDragged = false;
                    app.dragging = true;
                    app.dragLastX = ev.button.x;
                    app.dragLastY = ev.button.y;
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.button == SDL_BUTTON_MIDDLE) {
                    app.dragging = false;
                } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                    if (!app.rightDragged) {
                        /* Right-click (no drag) clears the click overlay */
                        app.clickActive = false;
                        app.clickPathLen = 0;
                        app.overlayDirty = true;
                    }
                    app.rightDown = false;
                    app.dragging = false;
                }
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                if (ev.wheel.y > 0) {
                    if (app.zoomFactor < MAX_ZOOM) app.zoomFactor++;
                } else if (ev.wheel.y < 0) {
                    if (app.zoomFactor > MIN_ZOOM) app.zoomFactor--;
                }
                break;
            }
        }

        /* Arrow key scrolling (continuous while held) */
        if (keystate[SDL_SCANCODE_LEFT] || keystate[SDL_SCANCODE_RIGHT] ||
            keystate[SDL_SCANCODE_UP] || keystate[SDL_SCANCODE_DOWN]) {
            app.freeCamera = true;
            int scrollAmount = SCROLL_SPEED * 256 / 3;  /* world units per frame (~60fps) */
            int cx = (int)app.viewCenterX;
            int cy = (int)app.viewCenterY;
            if (keystate[SDL_SCANCODE_LEFT])  cx -= scrollAmount;
            if (keystate[SDL_SCANCODE_RIGHT]) cx += scrollAmount;
            if (keystate[SDL_SCANCODE_UP])    cy -= scrollAmount;
            if (keystate[SDL_SCANCODE_DOWN])  cy += scrollAmount;
            /* Clamp to valid WORLD range */
            if (cx < 0) cx = 0;
            if (cy < 0) cy = 0;
            if (cx > 65535) cx = 65535;
            if (cy > 65535) cy = 65535;
            app.viewCenterX = (WORLD)cx;
            app.viewCenterY = (WORLD)cy;
        }

        /* Game ticks at 50 Hz */
        if (!app.paused) {
            Uint64 now = SDL_GetTicks();
            while (now - lastTickTime >= TICK_INTERVAL_MS) {
                appTick(&app);
                lastTickTime += TICK_INTERVAL_MS;
                /* Auto-pause after first brain tick so the initial
                 * goal decision and path are visible immediately */
                if (!autoPauseDone && app.sim.tick >= 4) {
                    app.paused = true;
                    autoPauseDone = true;
                    break;
                }
            }
        } else {
            lastTickTime = SDL_GetTicks();
        }

        /* Render */
        appRender(&app);

        /* Cap at ~60fps (vsync should handle this but just in case) */
        SDL_Delay(1);
    }

    /* Cleanup */
    fprintf(stderr, "Shutting down after tick %u\n", app.sim.tick);
    botManagerDestroy(&app.sim);
    if (app.debugPF) brainPathfinderDestroy(app.debugPF);
    if (app.overlayTex) SDL_DestroyTexture(app.overlayTex);
    if (app.tilesTex) SDL_DestroyTexture(app.tilesTex);
    tileLoaderCleanup();
    if (app.simValid) serverSimDestroy(&app.sim);
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(app.window);
    langCleanup();
    clientMutexDestroy();
    endWinboloTimer();
    SDL_Quit();

    return 0;
}
