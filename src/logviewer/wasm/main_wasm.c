/*
 * main_wasm.c - Emscripten/WebAssembly entry point for LogViewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * The browser build's host, in place of logviewer.c. It replaces the
 * blocking loop in logViewerRun with emscripten_set_main_loop. The state
 * lives in g_lv, and the half of the host every build shares -- bring-up,
 * teardown and the replay driver with its SDL timer pair -- comes from
 * lv_embed.c, as it does on the desktop.
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
#include "logviewer.h"
#include "lv_host.h"

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
#include "imgui/imgui_scenario_panel.h"

/* Platform abstraction */
#include "platform/platform_config.h"
#include "platform/platform_dialogs.h"

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
 * Sound
 * -------------------------------------------------------------------------- */
void lv_frontEndPlaySound(sndEffects value) {
    if (g_lv->isSoundsPlaying == TRUE) {
        lv_soundPlayEffect(value);
    }
}

void lv_windowDisableSound(void) {
    g_lv->isSoundsPlaying = FALSE;
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
 * Control state – no-ops (ImGui reads the state directly)
 * -------------------------------------------------------------------------- */
void lv_controlEnable(int id, bool state, unsigned int menuId) {
    (void)id; (void)state; (void)menuId;
}

void lv_controlsEnable(int state) {
    (void)state;
}

/* --------------------------------------------------------------------------
 * Playback control. Play, pause and stop come from lv_embed.c.
 * -------------------------------------------------------------------------- */
/* Jump the scrubber to a highlight moment and centre the view on its cell.
 * Playback stops across the seek, as it does on the desktop. */
void lv_windowSeekToHighlight(uint32_t ms, int mapX, int mapY) {
    bool wasPlaying = g_lv->playIsPlaying;
    if (wasPlaying) {
        lv_windowPause();
    }
    lv_drawDirtyScreen();
    lv_screenSeekToTimeMs(ms);
    lv_screenCentreOnCell(mapX, mapY);
    lv_drawDirtyScreen();
    lv_windowNeedRedraw();
    if (wasPlaying) {
        lv_windowPlay();
    }
}

/* File > Quit. A browser tab has no application to end. */
void logViewerRequestAppQuit(void) {
}

void lv_windowFastForward(void) {
    lv_drawDirtyScreen();
    lv_screenFastForward();
    lv_drawDirtyScreen();
    g_lv->wantScreenUpdate = TRUE;
}

void lv_windowRewind(void) {
    lv_drawDirtyScreen();
    lv_screenRewind();
    lv_drawDirtyScreen();
    g_lv->wantScreenUpdate = TRUE;
}

void lv_windowResize(void) {
    lv_drawResizeRenderTarget();
}

/* --------------------------------------------------------------------------
 * Start-of-log callback from backend
 * -------------------------------------------------------------------------- */
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
            g_lv->isLoaded = FALSE;
        } else {
            g_lv->isLoaded = TRUE;
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
    g_lv->isSoundsPlaying = (tolower((unsigned char)line[0]) == 'y') ? TRUE : FALSE;

    /* DNS — disabled in WASM */

    /* Use Team Colours */
    lv_platform_config_get_string("LOGVIEWER", "Use Team Colours", "Yes", line, sizeof(line));
    g_lv->useTeamColours = (line[0] == '\0' || tolower((unsigned char)line[0]) == 'y') ? TRUE : FALSE;

    /* Team colours */
    count = 0;
    while (count < MAX_TANKS) {
        snprintf(line, sizeof(line), "Team Colour %d", count + 1);
        getDef(def, count);
        lv_platform_config_get_string("LOGVIEWER", line, def, val, sizeof(val));
        g_lv->tc[count] = (BYTE)atoi(val);
        if (g_lv->tc[count] > 16) g_lv->tc[count] = count;
        count++;
    }
    lv_platform_config_get_string("LOGVIEWER", "Neutral Colour", "10", val, sizeof(val));
    g_lv->tc[16] = (BYTE)atoi(val);
    if (g_lv->tc[16] > 16) g_lv->tc[16] = 16;

    /* Playback Speed */
    lv_platform_config_get_string("LOGVIEWER", "Playback Speed", "1", val, sizeof(val));
    {
        BYTE spd = (BYTE)atoi(val);
        if (spd < 1 || spd > 9) spd = 1;
        lv_updateSpeed(spd, 0);
    }
}

static void mainSavePreferences(void) {
    char line[256];
    char val[64];
    BYTE count;

    lv_imgui_main_menu_save();
    lv_imgui_dialogs_save();

    lv_platform_config_set_string("LOGVIEWER", "Sounds", g_lv->isSoundsPlaying ? "Yes" : "No");
    lv_platform_config_set_string("LOGVIEWER", "Use Team Colours", g_lv->useTeamColours ? "Yes" : "No");

    snprintf(line, sizeof(line), "%d", lv_screenGetSizeX());
    lv_platform_config_set_string("LOGVIEWER", "ScreenSizeX", line);
    snprintf(line, sizeof(line), "%d", lv_screenGetSizeY());
    lv_platform_config_set_string("LOGVIEWER", "ScreenSizeY", line);

    count = 0;
    while (count < MAX_TANKS) {
        snprintf(line, sizeof(line), "Team Colour %d", count + 1);
        snprintf(val, sizeof(val), "%d", g_lv->tc[count]);
        lv_platform_config_set_string("LOGVIEWER", line, val);
        count++;
    }
    snprintf(val, sizeof(val), "%d", g_lv->tc[16]);
    lv_platform_config_set_string("LOGVIEWER", "Neutral Colour", val);

    snprintf(val, sizeof(val), "%d", g_lv->speed);
    lv_platform_config_set_string("LOGVIEWER", "Playback Speed", val);

    lv_platform_config_set_string("LOGVIEWER", "PreferencesSaved", "Yes");
    lv_platform_config_save();
}

/* --------------------------------------------------------------------------
 * Fit the tile grid to a window of w x h points at the current zoom.
 *
 * The canvas fills the page, so the page sets the window's size and the
 * window is never resized to a whole number of tiles: the grid is the
 * nearest whole number of tiles, the way the desktop viewer fits a maximized
 * window, and the blit clips what is left over.
 * -------------------------------------------------------------------------- */
static void fitTilesToWindow(int w, int h) {
    int menuH = (int)lv_imgui_get_menu_bar_height();
    float zoom = lv_drawGetZoomLevel();
    float tilePxX, tilePxY;
    int tilesX, tilesY;

    if (zoom <= 0.0f) zoom = 1.0f;
    tilePxX = (float)TILE_SIZE_X * zoom;
    tilePxY = (float)TILE_SIZE_Y * zoom;
    tilesX = (int)(((float)w + tilePxX * 0.5f) / tilePxX);
    tilesY = (int)(((float)(h - menuH) + tilePxY * 0.5f) / tilePxY);
    if (tilesX < 1) tilesX = 1;
    if (tilesY < 1) tilesY = 1;
    if (tilesX > 255) tilesX = 255;
    if (tilesY > 255) tilesY = 255;

    /* The size setters keep the offset inside the map for the new grid. */
    lv_screenSetSizeX((BYTE)tilesX);
    lv_screenSetSizeY((BYTE)tilesY);
    lv_screenSetSubOffset(0, 0);
    lv_drawResizeRenderTarget();
    lv_drawDirtyScreen();
    g_lv->wantScreenUpdate = TRUE;
}

/* --------------------------------------------------------------------------
 * Step the decoder by the time that has passed since the last frame.
 *
 * The desktop steps it from an SDL timer every timerSleep ms. In the browser
 * that timer is a setTimeout chain on the page's one thread, and each tick
 * only books the next after it has run and waited behind the frame being
 * drawn, so it lands late every time and the log plays at about half speed.
 * Here the main loop counts the ticks owed by the clock and runs them, so the
 * log keeps to time however long a frame takes. A tab in the background gets
 * no frames, so a long gap is dropped rather than replayed all at once.
 * -------------------------------------------------------------------------- */
#define MAX_TICKS_PER_FRAME 100

static void runElapsedTicks(void) {
    static Uint64 lastMs = 0;
    static Uint64 owedMs = 0;
    Uint64 now = SDL_GetTicks();
    Uint64 sleepMs, ticks, i;

    if (g_lv->playIsPlaying == FALSE || g_lv->isLoaded == FALSE) {
        lastMs = 0;
        owedMs = 0;
        return;
    }
    if (lastMs == 0) {
        lastMs = now;
        return;
    }
    owedMs += now - lastMs;
    lastMs = now;

    sleepMs = (g_lv->timerSleep > 0) ? (Uint64)g_lv->timerSleep : 20;
    ticks = owedMs / sleepMs;
    owedMs -= ticks * sleepMs;
    if (ticks > MAX_TICKS_PER_FRAME) {
        ticks = MAX_TICKS_PER_FRAME;
        owedMs = 0;
    }
    if (ticks == 0) {
        return;
    }

    lv_clientMutexWaitFor();
    for (i = 0; i < ticks; i++) {
        lv_screenLogTick();
        if (g_lv->doubleSpeed == TRUE) {
            lv_screenLogTick();
            lv_screenLogTick();
        }
    }
    lv_clientMutexRelease();
    g_lv->wantScreenUpdate = TRUE;
}

/* --------------------------------------------------------------------------
 * Main loop body — called by emscripten_set_main_loop. The decoder is
 * ticked by lv_embed.c's SDL timers, which SDL runs on the browser's own
 * event loop under Emscripten.
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
            g_lv->wantScreenUpdate = TRUE;
        }
        if (sdlEvent.type == SDL_EVENT_WINDOW_RESIZED) {
            fitTilesToWindow(sdlEvent.window.data1, sdlEvent.window.data2);
        }
        if (sdlEvent.type == SDL_EVENT_MOUSE_WHEEL) {
            /* Stepped zoom anchored on the cursor, as on the desktop. Left to
             * ImGui while the cursor is over a panel, so panels still scroll. */
            if (!lv_imgui_want_capture_mouse()) {
                int mx = (int)sdlEvent.wheel.mouse_x;
                int my = (int)sdlEvent.wheel.mouse_y;
                if (sdlEvent.wheel.y > 0.0f) {
                    lv_drawZoomIn(mx, my);
                } else if (sdlEvent.wheel.y < 0.0f) {
                    lv_drawZoomOut(mx, my);
                }
            }
        }
        /* Note: SDL_EVENT_QUIT is not meaningful in browser */
    }

    runElapsedTicks();

    /* --- Render --- */
    SDL_SetRenderDrawColor(g_lv->renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_lv->renderer);

    /* With no log loaded the page shows only the menu bar: no splash, so
     * nothing sits behind the page's progress bars while a log downloads. */
    if (g_lv->isLoaded == TRUE) {
        if (g_lv->wantScreenUpdate == TRUE) {
            lv_drawDirtyScreen();
            lv_screenUpdate(redraw);
            g_lv->wantScreenUpdate = FALSE;
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
    lv_imgui_scenario_panel_window(g_lv->isLoaded ? true : false);
    lv_g_reset_window_positions = false;
    lv_imgui_dialogs_render();
    lv_imgui_context_render();

    lv_imgui_game_viewport_process_input();

    SDL_RenderPresent(g_lv->renderer);
}

/* --------------------------------------------------------------------------
 * main — Emscripten entry point
 * -------------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /* Tell SDL3 which canvas element to use */
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_CANVAS_SELECTOR, "#canvas");

    /* The decoder state, platform layer, mutex, screen size and draw module.
     * A NULL window makes it create its own, as the standalone viewer does. */
    printf("[WASM] Starting lvHostSetupCore...\n");
    if (lvHostSetupCore(NULL, NULL, FALSE) == FALSE) {
        printf("[WASM] lvHostSetupCore FAILED\n");
        return 1;
    }
    printf("[WASM] lvHostSetupCore OK\n");
    /* The main loop steps the decoder (runElapsedTicks), not the SDL timer. */
    lv_windowSetHostRunsTicks(true);

    g_lv->isSoundsPlaying = TRUE;

    printf("[WASM] Starting lv_imgui_context_init...\n");
    if (lv_imgui_context_init(g_lv->window, g_lv->renderer) == 0) {
        printf("[WASM] lv_imgui_context_init FAILED\n");
        lvHostTeardownCommon(false);
        free(g_lv);
        g_lv = NULL;
        return 1;
    }
    printf("[WASM] lv_imgui_context_init OK\n");

    lv_imgui_main_menu_init(g_lv);
    lv_imgui_controls_init(g_lv);
    lv_imgui_game_info_init();
    lv_imgui_events_init();
    lv_imgui_item_info_init();
    lv_imgui_dialogs_init(g_lv);
    lv_imgui_game_viewport_init(g_lv);

    /* The canvas covers the page and follows the browser window's size, as
     * the game's does. The grid is fitted to it now and on every resize. */
    SDL_SetWindowFillDocument(g_lv->window, true);
    /* SDL moves everything else in <body> into a hidden <div> when the canvas
     * takes the page. The page's progress bars, its drop outline and the File
     * > Open input still have to work, so they go back on top of the canvas. */
    emscripten_run_script(
        "['loading', 'drop', 'wbv-file-input'].forEach(function(id) {"
        "  var el = document.getElementById(id);"
        "  if (el) document.body.appendChild(el);"
        "});");
    {
        int w = 0, h = 0;
        SDL_GetWindowSize(g_lv->window, &w, &h);
        fitTilesToWindow(w, h);
    }

    /* Sound (non-fatal if it fails) */
    if (lv_soundSetup() == FALSE) {
        g_lv->isSoundsPlaying = FALSE;
    }

    /* Load preferences */
    mainLoadPreferences();

    printf("[WASM] Starting main loop\n");

    /* Tell the page the viewer is set up, so a log it downloaded first can
     * be opened now. Emscripten's onRuntimeInitialized comes before main(),
     * when g_lv does not exist yet. */
    emscripten_run_script("if (window.wbLogViewerReady) window.wbLogViewerReady();");

    /* Hand control to the browser's event loop.
     * 0 = use requestAnimationFrame (typically 60fps).
     * 1 = simulate_infinite_loop (don't return from main). */
    emscripten_set_main_loop(main_loop_iteration, 0, 1);

    /* This code is never reached when simulate_infinite_loop=1,
     * but if we ever switch to 0, cleanup goes here. */
    mainSavePreferences();
    lvHostTeardownCommon(true);
    free(g_lv);
    g_lv = NULL;
    SDL_Quit();
    return 0;
}
