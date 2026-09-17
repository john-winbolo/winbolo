/*
 * main_wasm.c - Emscripten/WebAssembly entry point for LogViewer
 *
 * Copyright (c) 1998-2026 John Morrison.
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
#include "lv_global.h"
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
const char *lv_g_version_string = "1.01-wasm";

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
 *
 * The same rows in the same order as the desktop viewer's getDef, so a log
 * watched in a browser is coloured the way it is on the desktop. Team 1 green
 * and Team 2 red, the game's own ally / enemy reading.
 * -------------------------------------------------------------------------- */
static void getDef(char *dest, int index) {
    static const int defaults[] = { 2, 11, 12, 4, 16, 13, 8, 3, 6, 1, 5, 7, 9, 14, 15, 0 };
    int val = (index >= 0 && index < 16) ? defaults[index] : 0;
    snprintf(dest, 12, "%d", val);
}

/* --------------------------------------------------------------------------
 * lv_updateSpeed – sets timerSleep for the given speed value.
 * -------------------------------------------------------------------------- */
void lv_updateSpeed(BYTE spd, int updateSlider) {
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
void lv_frontEndPlaySound(sndEffects value) {
    if (isSoundsPlaying == TRUE) {
        lv_soundPlayEffect(value);
    }
}

void lv_windowDisableSound(void) {
    isSoundsPlaying = FALSE;
}

/* --------------------------------------------------------------------------
 * Item info – forward all data to the ImGui panel
 * -------------------------------------------------------------------------- */
void lv_updateItem(BYTE itemType, BYTE itemNumber, BYTE owner, BYTE x, BYTE y,
                BYTE armour, BYTE shells, BYTE mines, bool inTank) {
    lv_imgui_item_info_update(itemType, itemNumber, owner, x, y, armour, shells, mines, inTank);
}

/* --------------------------------------------------------------------------
 * Game information – forward to ImGui panel
 * -------------------------------------------------------------------------- */
void lv_frontEndSetGameInformation(bool clear, BYTE versionMajor, BYTE versionMinor,
                                BYTE versionRevision, char *mapName, BYTE gameType,
                                bool hiddenMines, BYTE aiType, int32_t startDelay,
                                int32_t timeLimit, BYTE *wbnKey, int32_t startTime) {
    lv_imgui_game_info_set(clear, versionMajor, versionMinor, versionRevision,
                        mapName, gameType, hiddenMines, aiType,
                        startDelay, timeLimit, wbnKey, startTime);
}

/* --------------------------------------------------------------------------
 * Events
 * -------------------------------------------------------------------------- */
void lv_windowAddEvent(int eventType, char *msg) {
    lv_imgui_events_add(eventType, msg);
}

void lv_windowAddHighlight(char *msg, uint32_t seekMs, int mapX, int mapY) {
    lv_imgui_events_add_highlight(msg, seekMs, mapX, mapY);
}

void lv_windowAddSummary(char *msg) {
    lv_imgui_events_add_summary(msg);
}

void lv_windowRemoveEventsAfter(uint32_t timeMs) {
    lv_imgui_events_remove_after(timeMs);
}

void lv_windowRemoveEvents(void) {
    lv_imgui_events_clear();
}

/* --------------------------------------------------------------------------
 * Control state – no-ops (ImGui reads globals directly)
 * -------------------------------------------------------------------------- */
void lv_controlEnable(int id, bool state, unsigned int menuId) {
    (void)id; (void)state; (void)menuId;
}

void lv_controlsEnable(int state) {
    (void)state;
}

/* --------------------------------------------------------------------------
 * Screen update / redraw
 * -------------------------------------------------------------------------- */
void lv_windowNeedRedraw(void) {
    wantScreenUpdate = TRUE;
}

/* --------------------------------------------------------------------------
 * Playback drawing
 * -------------------------------------------------------------------------- */
void lv_frontEndDrawMainScreen(screen *value, screenMines *mineView, screenTanks *tks,
                            screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms,
                            int32_t srtDelay, bool isPillView, int edgeX, int edgeY) {
    lv_drawMainScreen(value, mineView, tks, gs, sBullet, lgms,
                   FALSE, FALSE, srtDelay, isPillView, edgeX, edgeY, FALSE, 0, 0);
}

/* --------------------------------------------------------------------------
 * Playback control — no SDL timers, just set flags.
 * The main loop handles ticking based on playIsPlaying + timerSleep.
 * -------------------------------------------------------------------------- */
void lv_windowPlay(void) {
    playIsPlaying = TRUE;
    tickAccumulator = 0.0;
    lastFrameTime = emscripten_get_now();
}

void lv_windowPause(void) {
    playIsPlaying = FALSE;
}

void lv_windowStop(int corruptLog) {
    if (corruptLog == TRUE) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE,
                                 "Error: Corrupt Log File", NULL);
    }
    lv_windowPause();
    /* No SDL_Delay in WASM — just proceed */
    lv_frontEndSetGameInformation(TRUE, 0, 0, 0, NULL, 0, 0, 0, 0, 0, NULL, 0);
    lv_updateItem(0, 0, 0, 0, 0, 0, 0, 0, FALSE);
    lv_screenCloseLog();
    isLoaded = FALSE;
    lv_imgui_events_clear();
    lv_windowNeedRedraw();
}

void lv_windowFastForward(void) {
    lv_drawDirtyScreen();
    lv_screenFastForward();
    lv_drawDirtyScreen();
    lv_screenUpdate(redraw);
}

void lv_windowRewind(void) {
    lv_drawDirtyScreen();
    lv_screenRewind();
    lv_drawDirtyScreen();
    lv_screenUpdate(redraw);
    if (playIsPlaying == FALSE) {
        wantScreenUpdate = TRUE;
    }
}

void lv_windowResize(void) {
    lv_drawResizeRenderTarget();
}

/* --------------------------------------------------------------------------
 * End-of-log / start-of-log callbacks from backend
 * -------------------------------------------------------------------------- */
void lv_finished(void) {
    playIsPlaying = FALSE;
}

void lv_startOfLog(void) {
    /* ImGui controls panel reads isLoaded each frame */
}

/* --------------------------------------------------------------------------
 * Open file — called from JS via file upload
 * The file is written to Emscripten's virtual FS before calling this.
 * -------------------------------------------------------------------------- */
void lv_windowOpenFile(char *cmdLine) {
    char fileName[FILENAME_MAX];
    int  dlgResult;

    lv_windowStop(FALSE);
    fileName[0] = '\0';

    if (cmdLine == NULL) {
        /* In WASM, file open is triggered from JS — should not reach here */
        dlgResult = lv_platform_dialog_open_file(
            "Open File...", "WinBolo Log Files", "*.wbv", "wbv",
            fileName, sizeof(fileName), NULL);
    } else {
        dlgResult = PLATFORM_DIALOG_OK;
        strncpy(fileName, cmdLine, sizeof(fileName) - 1);
        fileName[sizeof(fileName) - 1] = '\0';
    }

    if (dlgResult == PLATFORM_DIALOG_OK) {
        if (lv_screenLoadMap(fileName, 4) == FALSE) {
            lv_platform_dialog_error(DIALOG_BOX_TITLE, "Could not open log file");
            isLoaded = FALSE;
        } else {
            isLoaded = TRUE;
            lv_imgui_events_clear();
            lv_windowNeedRedraw();
        }
    }
}

void lv_windowSaveMap(void) {
    /* Save not yet implemented in WASM build */
    lv_platform_dialog_message("WinBolo Log Viewer", "Save Map is not yet available in the web version.");
}

/* --------------------------------------------------------------------------
 * Called from JavaScript when user uploads a .wbv file
 * -------------------------------------------------------------------------- */
EMSCRIPTEN_KEEPALIVE
void wasm_open_log_file(const char *filename) {
    lv_windowOpenFile((char *)filename);
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
    lv_platform_config_get_string("LOGVIEWER", "Tank Centred", "No", line, sizeof(line));
    lv_screenTankCentred(tolower((unsigned char)line[0]) == 'y' ? TRUE : FALSE);

    /* Sound Effects */
    lv_platform_config_get_string("LOGVIEWER", "Sounds", "Yes", line, sizeof(line));
    isSoundsPlaying = (tolower((unsigned char)line[0]) == 'y') ? TRUE : FALSE;

    /* DNS — disabled in WASM */

    /* Use Team Colours */
    lv_platform_config_get_string("LOGVIEWER", "Use Team Colours", "Yes", line, sizeof(line));
    useTeamColours = (line[0] == '\0' || tolower((unsigned char)line[0]) == 'y') ? TRUE : FALSE;

    /* Team colours */
    count = 0;
    while (count < MAX_TANKS) {
        snprintf(line, sizeof(line), "Team Colour %d", count + 1);
        getDef(def, count);
        lv_platform_config_get_string("LOGVIEWER", line, def, val, sizeof(val));
        tc[count] = (BYTE)atoi(val);
        if (tc[count] > 16) tc[count] = count;
        count++;
    }
    lv_platform_config_get_string("LOGVIEWER", "Neutral Colour", "10", val, sizeof(val));
    tc[16] = (BYTE)atoi(val);
    if (tc[16] > 16) tc[16] = 16;

    /* Playback Speed */
    lv_platform_config_get_string("LOGVIEWER", "Playback Speed", "1", val, sizeof(val));
    speed = (BYTE)atoi(val);
    if (speed < 1 || speed > 9) speed = 1;
    lv_updateSpeed(speed, 0);
}

static void mainSavePreferences(void) {
    char line[256];
    char val[64];
    BYTE count;

    lv_imgui_main_menu_save();
    lv_imgui_dialogs_save();

    lv_platform_config_set_string("LOGVIEWER", "Sounds", isSoundsPlaying ? "Yes" : "No");
    lv_platform_config_set_string("LOGVIEWER", "Use Team Colours", useTeamColours ? "Yes" : "No");

    snprintf(line, sizeof(line), "%d", lv_screenGetSizeX());
    lv_platform_config_set_string("LOGVIEWER", "ScreenSizeX", line);
    snprintf(line, sizeof(line), "%d", lv_screenGetSizeY());
    lv_platform_config_set_string("LOGVIEWER", "ScreenSizeY", line);

    count = 0;
    while (count < MAX_TANKS) {
        snprintf(line, sizeof(line), "Team Colour %d", count + 1);
        snprintf(val, sizeof(val), "%d", tc[count]);
        lv_platform_config_set_string("LOGVIEWER", line, val);
        count++;
    }
    snprintf(val, sizeof(val), "%d", tc[16]);
    lv_platform_config_set_string("LOGVIEWER", "Neutral Colour", val);

    snprintf(val, sizeof(val), "%d", speed);
    lv_platform_config_set_string("LOGVIEWER", "Playback Speed", val);

    lv_platform_config_set_string("LOGVIEWER", "PreferencesSaved", "Yes");
    lv_platform_config_save();
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
        lv_imgui_context_handle_event(&sdlEvent);

        if (sdlEvent.type == SDL_EVENT_RENDER_TARGETS_RESET ||
            sdlEvent.type == SDL_EVENT_RENDER_DEVICE_RESET) {
            lv_drawDirtyScreen();
        }
        if (sdlEvent.type == SDL_EVENT_WINDOW_EXPOSED ||
            sdlEvent.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
            lv_drawDirtyScreen();
            wantScreenUpdate = TRUE;
        }
        if (sdlEvent.type == SDL_EVENT_WINDOW_RESIZED) {
            int w = sdlEvent.window.data1;
            int h = sdlEvent.window.data2;
            int menuH = (int)lv_imgui_get_menu_bar_height();

            int tileW = ((w + TILE_SIZE_X / 2) / TILE_SIZE_X) * TILE_SIZE_X;
            int tileH = (((h - menuH) + TILE_SIZE_Y / 2) / TILE_SIZE_Y) * TILE_SIZE_Y;
            if (tileW < TILE_SIZE_X) tileW = TILE_SIZE_X;
            if (tileH < TILE_SIZE_Y) tileH = TILE_SIZE_Y;
            if (tileW > 255 * TILE_SIZE_X) tileW = 255 * TILE_SIZE_X;
            if (tileH > 255 * TILE_SIZE_Y) tileH = 255 * TILE_SIZE_Y;

            SDL_SetWindowSize(lv_drawGetSDLWindow(), tileW, tileH + menuH);
            {
                BYTE newTilesX = (BYTE)(tileW / TILE_SIZE_X);
                BYTE newTilesY = (BYTE)(tileH / TILE_SIZE_Y);
                lv_screenSetSizeX(newTilesX);
                lv_screenSetSizeY(newTilesY);
                if (isLoaded) {
                    BYTE ox, oy;
                    lv_screenGetOffsets(&ox, &oy);
                    if ((int)ox + newTilesX > 255) ox = (BYTE)(255 - newTilesX);
                    if ((int)oy + newTilesY > 255) oy = (BYTE)(255 - newTilesY);
                    lv_screenSetOffset(ox, oy);
                }
            }
            lv_drawResizeRenderTarget();
            lv_drawDirtyScreen();
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
            lv_screenLogTick();
            if (doubleSpeed == TRUE) {
                lv_screenLogTick();
                lv_screenLogTick();
            }
        }
        wantScreenUpdate = TRUE;
    }

    /* --- Render --- */
    SDL_SetRenderDrawColor(lv_drawGetSDLRenderer(), 0, 0, 0, 255);
    SDL_RenderClear(lv_drawGetSDLRenderer());

    if (isLoaded == FALSE) {
        lv_drawSplashForImGui();
    } else {
        if (wantScreenUpdate == TRUE) {
            lv_drawDirtyScreen();
            lv_screenUpdate(redraw);
            wantScreenUpdate = FALSE;
        } else {
            lv_drawBlitGameTexture();
        }
    }

    /* Render ImGui UI */
    lv_imgui_context_newframe();
    lv_imgui_main_menu_bar();
    lv_imgui_cache_menu_bar_height();
    lv_imgui_controls_window();
    lv_imgui_game_info_window();
    lv_imgui_events_window();
    lv_imgui_item_info_window();
    lv_g_reset_window_positions = false;
    lv_imgui_dialogs_render();
    lv_imgui_context_render();

    lv_imgui_game_viewport_process_input();

    SDL_RenderPresent(lv_drawGetSDLRenderer());
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
    lv_platform_config_init("WinBolo");
    lv_platform_dialogs_init();

    isLoaded        = FALSE;
    isSoundsPlaying = TRUE;

    lv_clientMutexCreate();

    /* Load screen size before creating window */
    lv_platform_config_get_string("LOGVIEWER", "ScreenSizeX", "50", line, sizeof(line));
    sizeX = atoi(line);
    if (sizeX < 5 || sizeX > 99) sizeX = 50;

    lv_platform_config_get_string("LOGVIEWER", "ScreenSizeY", "38", line, sizeof(line));
    sizeY = atoi(line);
    if (sizeY < 5 || sizeY > 99) sizeY = 38;

    lv_screenSetSizeX((BYTE)sizeX);
    lv_screenSetSizeY((BYTE)sizeY);

    /* Tell SDL3 which canvas element to use */
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_CANVAS_SELECTOR, "#canvas");

    printf("[WASM] Starting lv_drawSetup...\n");
    if (lv_drawSetup() == FALSE) {
        printf("[WASM] lv_drawSetup FAILED\n");
        return 1;
    }
    printf("[WASM] lv_drawSetup OK\n");

    /* SDL3's Emscripten backend sets the canvas element size to 1x1 during
     * probing, then relies on CSS for display size when external_size=true.
     * But the rendering buffer (element size) needs to match.
     * Force both the SDL window and canvas buffer to our desired size. */
    {
        int w = lv_screenGetSizeX() * TILE_SIZE_X;
        int h = lv_screenGetSizeY() * TILE_SIZE_Y + 25; /* +25 for menu bar */
        printf("[WASM] Forcing canvas to: %d x %d\n", w, h);
        emscripten_set_canvas_element_size("#canvas", w, h);
        SDL_SetWindowSize(lv_drawGetSDLWindow(), w, h);
    }

    lv_platform_dialogs_set_window(lv_drawGetSDLWindow());

    printf("[WASM] Starting lv_imgui_context_init...\n");
    if (lv_imgui_context_init(lv_drawGetSDLWindow(), lv_drawGetSDLRenderer()) == 0) {
        printf("[WASM] lv_imgui_context_init FAILED\n");
        lv_drawCleanup();
        return 1;
    }
    printf("[WASM] lv_imgui_context_init OK\n");

    lv_imgui_main_menu_init();
    lv_imgui_controls_init();
    lv_imgui_game_info_init();
    lv_imgui_events_init();
    lv_imgui_item_info_init();
    lv_imgui_dialogs_init();
    lv_imgui_game_viewport_init();

    /* Sound (non-fatal if it fails) */
    if (lv_soundSetup() == FALSE) {
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
    lv_windowStop(FALSE);
    lv_imgui_context_shutdown();
    lv_drawCleanupSplash();
    lv_soundCleanup();
    lv_drawCleanup();
    SDL_Quit();
    lv_clientMutexDestroy();
    lv_platform_config_shutdown();
    lv_platform_dialogs_shutdown();
    return 0;
}
