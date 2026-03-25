/*
 * main_wasm.c - Emscripten/WebAssembly entry point for LogViewer
 *
 * Copyright (c) 1998-2008 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This is an alternate main() for the WASM build. It replaces the
 * blocking while-loop in main.c with emscripten_set_main_loop(),
 * and replaces SDL_AddTimer with frame-based tick counting.
 * The original main.c is NOT modified.
 */

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <stdio.h>
#include "backend.h"
#include "global.h"
#include "clientmutex.h"
#include "draw.h"
#include "sound.h"
#include "dns.h"
#include "positions.h"
#include "tiles.h"

#include <SDL3/SDL.h>
#include <emscripten.h>
#include <emscripten/html5.h>

/* ImGui integration headers */
#include "imgui/imgui_context.h"
#include "imgui/imgui_main_menu.h"
#include "imgui/imgui_controls.h"
#include "imgui/imgui_game_info.h"
#include "imgui/imgui_events.h"
#include "imgui/imgui_item_info.h"
#include "imgui/imgui_dialogs.h"
#include "imgui/imgui_game_viewport.h"

/* Platform abstraction */
#include "platform/platform_config.h"
#include "platform/platform_dialogs.h"

/* Version string referenced by imgui_dialogs.cpp */
const char *g_version_string = "1.01-wasm";

/* Team Colours array */
BYTE tc[17]; /* MAX_PLAYERS + 1 */

/* Playback state */
bool playIsPlaying = FALSE;
bool isLoaded      = FALSE;
bool isSoundsPlaying;
bool useTeamColours = FALSE;
bool wantScreenUpdate = FALSE;
bool doubleSpeed = FALSE;
BYTE speed;
int  timerSleep;

/* Frame-based tick accumulator (replaces SDL timers) */
static double tickAccumulator = 0.0;
static double lastFrameTime   = 0.0;
static const double FRAME_INTERVAL_MS = 50.0; /* ~20 FPS for screen updates */

/* --------------------------------------------------------------------------
 * Helper: default team colour value for index
 * -------------------------------------------------------------------------- */
static void getDef(char *dest, int index) {
    static const int defaults[] = { 11, 12, 2, 4, 16, 13, 8, 3, 6, 1, 5, 7, 9, 14, 15, 0 };
    int val = (index >= 0 && index < 16) ? defaults[index] : 0;
    snprintf(dest, 12, "%d", val);
}

/* --------------------------------------------------------------------------
 * updateSpeed – sets timerSleep for the given speed value.
 * -------------------------------------------------------------------------- */
void updateSpeed(BYTE spd, int updateSlider) {
    (void)updateSlider;
    switch (spd) {
    case 2: timerSleep = 18; break;
    case 3: timerSleep = 16; break;
    case 4: timerSleep = 14; break;
    case 5: timerSleep = 12; break;
    case 6: timerSleep = 10; break;
    case 7: timerSleep =  8; break;
    case 8: timerSleep =  6; break;
    case 9: timerSleep =  1; break;
    default:
        timerSleep = 20;
        spd = 1;
        break;
    }
    speed = spd;
}

/* --------------------------------------------------------------------------
 * Sound
 * -------------------------------------------------------------------------- */
void frontEndPlaySound(sndEffects value) {
    if (isSoundsPlaying == TRUE) {
        soundPlayEffect(value);
    }
}

void windowDisableSound(void) {
    isSoundsPlaying = FALSE;
}

/* --------------------------------------------------------------------------
 * Item info – forward all data to the ImGui panel
 * -------------------------------------------------------------------------- */
void updateItem(BYTE itemType, BYTE itemNumber, BYTE owner, BYTE x, BYTE y,
                BYTE armour, BYTE shells, BYTE mines, bool inTank) {
    imgui_item_info_update(itemType, itemNumber, owner, x, y, armour, shells, mines, inTank);
}

/* --------------------------------------------------------------------------
 * Game information – forward to ImGui panel
 * -------------------------------------------------------------------------- */
void frontEndSetGameInformation(bool clear, BYTE versionMajor, BYTE versionMinor,
                                BYTE versionRevision, char *mapName, BYTE gameType,
                                bool hiddenMines, BYTE aiType, int32_t startDelay,
                                int32_t timeLimit, BYTE *wbnKey, int32_t startTime) {
    imgui_game_info_set(clear, versionMajor, versionMinor, versionRevision,
                        mapName, gameType, hiddenMines, aiType,
                        startDelay, timeLimit, wbnKey, startTime);
}

/* --------------------------------------------------------------------------
 * Events
 * -------------------------------------------------------------------------- */
void windowAddEvent(int eventType, char *msg) {
    imgui_events_add(eventType, msg);
}

void windowRemoveEvents(void) {
    imgui_events_clear();
}

/* --------------------------------------------------------------------------
 * Control state – no-ops (ImGui reads globals directly)
 * -------------------------------------------------------------------------- */
void controlEnable(int id, bool state, unsigned int menuId) {
    (void)id; (void)state; (void)menuId;
}

void controlsEnable(int state) {
    (void)state;
}

/* --------------------------------------------------------------------------
 * Screen update / redraw
 * -------------------------------------------------------------------------- */
void windowNeedRedraw(void) {
    wantScreenUpdate = TRUE;
}

/* --------------------------------------------------------------------------
 * Playback drawing
 * -------------------------------------------------------------------------- */
void frontEndDrawMainScreen(screen *value, screenMines *mineView, screenTanks *tks,
                            screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms,
                            int32_t srtDelay, bool isPillView, int edgeX, int edgeY) {
    drawMainScreen(value, mineView, tks, gs, sBullet, lgms,
                   FALSE, FALSE, srtDelay, isPillView, edgeX, edgeY, FALSE, 0, 0);
}

/* --------------------------------------------------------------------------
 * Playback control — no SDL timers, just set flags.
 * The main loop handles ticking based on playIsPlaying + timerSleep.
 * -------------------------------------------------------------------------- */
void windowPlay(void) {
    playIsPlaying = TRUE;
    tickAccumulator = 0.0;
    lastFrameTime = emscripten_get_now();
}

void windowPause(void) {
    playIsPlaying = FALSE;
}

void windowStop(int corruptLog) {
    if (corruptLog == TRUE) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE,
                                 "Error: Corrupt Log File", NULL);
    }
    windowPause();
    /* No SDL_Delay in WASM — just proceed */
    frontEndSetGameInformation(TRUE, 0, 0, 0, NULL, 0, 0, 0, 0, 0, NULL, 0);
    updateItem(0, 0, 0, 0, 0, 0, 0, 0, FALSE);
    screenCloseLog();
    isLoaded = FALSE;
    imgui_events_clear();
    windowNeedRedraw();
}

void windowFastForward(void) {
    drawDirtyScreen();
    screenFastForward();
    drawDirtyScreen();
    screenUpdate(redraw);
}

void windowRewind(void) {
    drawDirtyScreen();
    screenRewind();
    drawDirtyScreen();
    screenUpdate(redraw);
    if (playIsPlaying == FALSE) {
        wantScreenUpdate = TRUE;
    }
}

void windowResize(void) {
    drawResizeRenderTarget();
}

/* --------------------------------------------------------------------------
 * End-of-log / start-of-log callbacks from backend
 * -------------------------------------------------------------------------- */
void finished(void) {
    playIsPlaying = FALSE;
}

void startOfLog(void) {
    /* ImGui controls panel reads isLoaded each frame */
}

/* --------------------------------------------------------------------------
 * Open file — called from JS via file upload
 * The file is written to Emscripten's virtual FS before calling this.
 * -------------------------------------------------------------------------- */
void windowOpenFile(char *cmdLine) {
    char fileName[FILENAME_MAX];
    int  dlgResult;

    windowStop(FALSE);
    fileName[0] = '\0';

    if (cmdLine == NULL) {
        /* In WASM, file open is triggered from JS — should not reach here */
        dlgResult = platform_dialog_open_file(
            "Open File...", "WinBolo Log Files", "*.wbv", "wbv",
            fileName, sizeof(fileName), NULL);
    } else {
        dlgResult = PLATFORM_DIALOG_OK;
        strncpy(fileName, cmdLine, sizeof(fileName) - 1);
        fileName[sizeof(fileName) - 1] = '\0';
    }

    if (dlgResult == PLATFORM_DIALOG_OK) {
        if (screenLoadMap(fileName, 4) == FALSE) {
            platform_dialog_error(DIALOG_BOX_TITLE, "Could not open log file");
            isLoaded = FALSE;
        } else {
            isLoaded = TRUE;
            imgui_events_clear();
            windowNeedRedraw();
        }
    }
}

void windowSaveMap(void) {
    /* Save not yet implemented in WASM build */
    platform_dialog_message("Log Viewer", "Save Map is not yet available in the web version.");
}

/* --------------------------------------------------------------------------
 * Called from JavaScript when user uploads a .wbv file
 * -------------------------------------------------------------------------- */
EMSCRIPTEN_KEEPALIVE
void wasm_open_log_file(const char *filename) {
    windowOpenFile((char *)filename);
}

/* --------------------------------------------------------------------------
 * Preferences
 * -------------------------------------------------------------------------- */
static void mainLoadPreferences(void) {
    char line[FILENAME_MAX];
    char def[FILENAME_MAX];
    char val[FILENAME_MAX];
    BYTE count;

    /* Tank Centred */
    platform_config_get_string("LOGVIEWER", "Tank Centred", "No", line, sizeof(line));
    screenTankCentred(tolower((unsigned char)line[0]) == 'y' ? TRUE : FALSE);

    /* Sound Effects */
    platform_config_get_string("LOGVIEWER", "Sounds", "Yes", line, sizeof(line));
    isSoundsPlaying = (tolower((unsigned char)line[0]) == 'y') ? TRUE : FALSE;

    /* DNS — disabled in WASM */

    /* Use Team Colours */
    platform_config_get_string("LOGVIEWER", "Use Team Colours", "Yes", line, sizeof(line));
    useTeamColours = (line[0] == '\0' || tolower((unsigned char)line[0]) == 'y') ? TRUE : FALSE;

    /* Team colours */
    count = 0;
    while (count < MAX_TANKS) {
        snprintf(line, sizeof(line), "Team Colour %d", count + 1);
        getDef(def, count);
        platform_config_get_string("LOGVIEWER", line, def, val, sizeof(val));
        tc[count] = (BYTE)atoi(val);
        if (tc[count] > 16) tc[count] = count;
        count++;
    }
    platform_config_get_string("LOGVIEWER", "Neutral Colour", "10", val, sizeof(val));
    tc[16] = (BYTE)atoi(val);
    if (tc[16] > 16) tc[16] = 16;

    /* Playback Speed */
    platform_config_get_string("LOGVIEWER", "Playback Speed", "1", val, sizeof(val));
    speed = (BYTE)atoi(val);
    if (speed < 1 || speed > 9) speed = 1;
    updateSpeed(speed, 0);
}

static void mainSavePreferences(void) {
    char line[256];
    char val[64];
    BYTE count;

    imgui_main_menu_save();
    imgui_dialogs_save();

    platform_config_set_string("LOGVIEWER", "Sounds", isSoundsPlaying ? "Yes" : "No");
    platform_config_set_string("LOGVIEWER", "Use Team Colours", useTeamColours ? "Yes" : "No");

    snprintf(line, sizeof(line), "%d", screenGetSizeX());
    platform_config_set_string("LOGVIEWER", "ScreenSizeX", line);
    snprintf(line, sizeof(line), "%d", screenGetSizeY());
    platform_config_set_string("LOGVIEWER", "ScreenSizeY", line);

    count = 0;
    while (count < MAX_TANKS) {
        snprintf(line, sizeof(line), "Team Colour %d", count + 1);
        snprintf(val, sizeof(val), "%d", tc[count]);
        platform_config_set_string("LOGVIEWER", line, val);
        count++;
    }
    snprintf(val, sizeof(val), "%d", tc[16]);
    platform_config_set_string("LOGVIEWER", "Neutral Colour", val);

    snprintf(val, sizeof(val), "%d", speed);
    platform_config_set_string("LOGVIEWER", "Playback Speed", val);

    platform_config_set_string("LOGVIEWER", "PreferencesSaved", "Yes");
    platform_config_save();
}

/* --------------------------------------------------------------------------
 * Main loop body — called by emscripten_set_main_loop
 * -------------------------------------------------------------------------- */
static int frameCount = 0;

static void main_loop_iteration(void) {
    SDL_Event sdlEvent;

    if (frameCount < 3) {
        printf("[WASM] Frame %d\n", frameCount);
    }
    frameCount++;

    /* Process SDL events */
    while (SDL_PollEvent(&sdlEvent)) {
        imgui_context_handle_event(&sdlEvent);

        if (sdlEvent.type == SDL_EVENT_RENDER_TARGETS_RESET ||
            sdlEvent.type == SDL_EVENT_RENDER_DEVICE_RESET) {
            drawDirtyScreen();
        }
        if (sdlEvent.type == SDL_EVENT_WINDOW_EXPOSED ||
            sdlEvent.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
            drawDirtyScreen();
            wantScreenUpdate = TRUE;
        }
        if (sdlEvent.type == SDL_EVENT_WINDOW_RESIZED) {
            int w = sdlEvent.window.data1;
            int h = sdlEvent.window.data2;
            int menuH = (int)imgui_get_menu_bar_height();

            int tileW = ((w + TILE_SIZE_X / 2) / TILE_SIZE_X) * TILE_SIZE_X;
            int tileH = (((h - menuH) + TILE_SIZE_Y / 2) / TILE_SIZE_Y) * TILE_SIZE_Y;
            if (tileW < TILE_SIZE_X) tileW = TILE_SIZE_X;
            if (tileH < TILE_SIZE_Y) tileH = TILE_SIZE_Y;
            if (tileW > 255 * TILE_SIZE_X) tileW = 255 * TILE_SIZE_X;
            if (tileH > 255 * TILE_SIZE_Y) tileH = 255 * TILE_SIZE_Y;

            SDL_SetWindowSize(drawGetSDLWindow(), tileW, tileH + menuH);
            {
                BYTE newTilesX = (BYTE)(tileW / TILE_SIZE_X);
                BYTE newTilesY = (BYTE)(tileH / TILE_SIZE_Y);
                screenSetSizeX(newTilesX);
                screenSetSizeY(newTilesY);
                if (isLoaded) {
                    BYTE ox, oy;
                    screenGetOffsets(&ox, &oy);
                    if ((int)ox + newTilesX > 255) ox = (BYTE)(255 - newTilesX);
                    if ((int)oy + newTilesY > 255) oy = (BYTE)(255 - newTilesY);
                    screenSetOffset(ox, oy);
                }
            }
            drawResizeRenderTarget();
            drawDirtyScreen();
            wantScreenUpdate = TRUE;
        }
        /* Note: SDL_EVENT_QUIT is not meaningful in browser */
    }

    /* --- Game logic ticks (replaces SDL_AddTimer) --- */
    if (playIsPlaying && isLoaded) {
        double now = emscripten_get_now();
        double elapsed = now - lastFrameTime;
        lastFrameTime = now;
        tickAccumulator += elapsed;

        double tickInterval = (double)timerSleep;
        if (tickInterval < 1.0) tickInterval = 1.0;

        while (tickAccumulator >= tickInterval) {
            tickAccumulator -= tickInterval;
            screenLogTick();
            if (doubleSpeed == TRUE) {
                screenLogTick();
                screenLogTick();
            }
        }
        wantScreenUpdate = TRUE;
    }

    /* --- Render --- */
    SDL_SetRenderDrawColor(drawGetSDLRenderer(), 0, 0, 0, 255);
    SDL_RenderClear(drawGetSDLRenderer());

    if (isLoaded == FALSE) {
        drawSplashForImGui();
    } else {
        if (wantScreenUpdate == TRUE) {
            drawDirtyScreen();
            screenUpdate(redraw);
            wantScreenUpdate = FALSE;
        } else {
            drawBlitGameTexture();
        }
    }

    /* Render ImGui UI */
    imgui_context_newframe();
    imgui_main_menu_bar();
    imgui_cache_menu_bar_height();
    imgui_controls_window();
    imgui_game_info_window();
    imgui_events_window();
    imgui_item_info_window();
    imgui_dialogs_render();
    imgui_context_render();

    imgui_game_viewport_process_input();

    SDL_RenderPresent(drawGetSDLRenderer());
}

/* --------------------------------------------------------------------------
 * main — Emscripten entry point
 * -------------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    char line[256];
    int  sizeX, sizeY;

    (void)argc;
    (void)argv;

    /* Platform abstraction init */
    platform_config_init("WinBolo");
    platform_dialogs_init();

    isLoaded        = FALSE;
    isSoundsPlaying = TRUE;

    clientMutexCreate();

    /* Load screen size before creating window */
    platform_config_get_string("LOGVIEWER", "ScreenSizeX", "50", line, sizeof(line));
    sizeX = atoi(line);
    if (sizeX < 5 || sizeX > 99) sizeX = 50;

    platform_config_get_string("LOGVIEWER", "ScreenSizeY", "38", line, sizeof(line));
    sizeY = atoi(line);
    if (sizeY < 5 || sizeY > 99) sizeY = 38;

    screenSetSizeX((BYTE)sizeX);
    screenSetSizeY((BYTE)sizeY);

    /* Tell SDL3 which canvas element to use */
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_CANVAS_SELECTOR, "#canvas");

    printf("[WASM] Starting drawSetup...\n");
    if (drawSetup() == FALSE) {
        printf("[WASM] drawSetup FAILED\n");
        return 1;
    }
    printf("[WASM] drawSetup OK\n");

    /* SDL3's Emscripten backend sets the canvas element size to 1x1 during
     * probing, then relies on CSS for display size when external_size=true.
     * But the rendering buffer (element size) needs to match.
     * Force both the SDL window and canvas buffer to our desired size. */
    {
        int w = screenGetSizeX() * TILE_SIZE_X;
        int h = screenGetSizeY() * TILE_SIZE_Y + 25; /* +25 for menu bar */
        printf("[WASM] Forcing canvas to: %d x %d\n", w, h);
        emscripten_set_canvas_element_size("#canvas", w, h);
        SDL_SetWindowSize(drawGetSDLWindow(), w, h);
    }

    platform_dialogs_set_window(drawGetSDLWindow());

    printf("[WASM] Starting imgui_context_init...\n");
    if (imgui_context_init(drawGetSDLWindow(), drawGetSDLRenderer()) == 0) {
        printf("[WASM] imgui_context_init FAILED\n");
        drawCleanup();
        return 1;
    }
    printf("[WASM] imgui_context_init OK\n");

    imgui_main_menu_init();
    imgui_controls_init();
    imgui_game_info_init();
    imgui_events_init();
    imgui_item_info_init();
    imgui_dialogs_init();
    imgui_game_viewport_init();

    /* Sound (non-fatal if it fails) */
    if (soundSetup() == FALSE) {
        isSoundsPlaying = FALSE;
    }

    /* Load preferences */
    mainLoadPreferences();

    /* Initialize frame timing */
    lastFrameTime = emscripten_get_now();

    printf("[WASM] Starting main loop\n");

    /* Hand control to the browser's event loop.
     * 0 = use requestAnimationFrame (typically 60fps).
     * 1 = simulate_infinite_loop (don't return from main). */
    emscripten_set_main_loop(main_loop_iteration, 0, 1);

    /* This code is never reached when simulate_infinite_loop=1,
     * but if we ever switch to 0, cleanup goes here. */
    mainSavePreferences();
    windowStop(FALSE);
    imgui_context_shutdown();
    drawCleanupSplash();
    soundCleanup();
    drawCleanup();
    SDL_Quit();
    clientMutexDestroy();
    platform_config_shutdown();
    platform_dialogs_shutdown();
    return 0;
}
