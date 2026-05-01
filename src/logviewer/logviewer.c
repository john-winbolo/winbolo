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
 * Name:          logviewer.c
 * Purpose:
 *   Core log viewer loop. Takes ownership of the event loop
 *   until the user exits. Window and renderer are borrowed,
 *   not owned. Follows the same pattern as mapEditorRun().
 *********************************************************/

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
#include "logviewer.h"

#include <SDL3/SDL.h>

/* ImGui integration headers */
#include "imgui/imgui_context.h"
#include "imgui/imgui_main_menu.h"
#include "imgui/imgui_controls.h"
#include "imgui/imgui_game_info.h"
#include "imgui/imgui_events.h"
#include "imgui/imgui_item_info.h"
#include "imgui/imgui_comments.h"
#include "imgui/imgui_dialogs.h"
#include "imgui/imgui_game_viewport.h"

/* Platform abstraction */
#include "platform/platform_config.h"
#include "platform/platform_dialogs.h"
#include "../gui/sdl3/macos_pinch.h"

/* Version string referenced by imgui_dialogs.cpp */
const char *lv_g_version_string = "1.01";

/* File-scope pointer to the current viewer session state */
static LogViewerState *g_lv = NULL;

/* Pending memory-based log load (set by logViewerRunFromMemory) */
static uint8_t *s_pendingZipData = NULL;
static size_t   s_pendingZipLen  = 0;

/* --------------------------------------------------------------------------
 * Helper: default team colour value for index
 * -------------------------------------------------------------------------- */
static void getDef(char *dest, int index) {
    static const int defaults[] = { 11, 12, 2, 4, 16, 13, 8, 3, 6, 1, 5, 7, 9, 14, 15, 0 };
    int val = (index >= 0 && index < 16) ? defaults[index] : 0;
    snprintf(dest, 12, "%d", val);
}

/* --------------------------------------------------------------------------
 * lv_updateSpeed -- sets timerSleep for the given speed value.
 * Called from imgui_controls.cpp via extern, and from mainLoadPreferences.
 * -------------------------------------------------------------------------- */
void lv_updateSpeed(BYTE spd, int updateSlider) {
    (void)updateSlider; /* ImGui slider reads g_lv->speed directly */
    int ts;
    switch (spd) {
    case 2: ts = 18; break;
    case 3: ts = 16; break;
    case 4: ts = 14; break;
    case 5: ts = 12; break;
    case 6: ts = 10; break;
    case 7: ts =  8; break;
    case 8: ts =  6; break;
    case 9: ts =  1; break;
    default:
        ts = 20;
        spd = 1;
        break;
    }
    if (g_lv != NULL) {
        g_lv->speed = spd;
        g_lv->timerSleep = ts;
    }
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
 * Item info -- forward all data to the ImGui panel
 * -------------------------------------------------------------------------- */
void lv_updateItem(BYTE itemType, BYTE itemNumber, BYTE owner, BYTE x, BYTE y,
                BYTE armour, BYTE shells, BYTE mines, bool inTank) {
    lv_imgui_item_info_update(itemType, itemNumber, owner, x, y, armour, shells, mines, inTank);
}

/* --------------------------------------------------------------------------
 * Game information -- forward to ImGui panel
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

/* Remove events that are ahead of the given playback time (during rewind/seek). */
void lv_windowRemoveEventsAfter(uint32_t timeMs) {
    lv_imgui_events_remove_after(timeMs);
}

void lv_windowRemoveEvents(void) {
    lv_imgui_events_clear();
}

/* --------------------------------------------------------------------------
 * Control state -- ImGui panels read state directly each frame,
 * so these functions are no-ops.
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
    g_lv->wantScreenUpdate = TRUE;
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
 * SDL timer callbacks (called on background threads -- only set flags)
 * -------------------------------------------------------------------------- */
Uint32 SDLCALL lv_windowFrameTimer(void *userdata, SDL_TimerID timerID, Uint32 interval) {
    (void)userdata; (void)timerID; (void)interval;
    g_lv->wantScreenUpdate = TRUE;
    if (g_lv->playIsPlaying == TRUE) {
        return 50;
    }
    return 0;
}

Uint32 SDLCALL lv_windowTimer(void *userdata, SDL_TimerID timerID, Uint32 interval) {
    (void)userdata; (void)timerID; (void)interval;
    lv_clientMutexWaitFor();
    lv_screenLogTick();
    if (g_lv->doubleSpeed == TRUE) {
        lv_screenLogTick();
        lv_screenLogTick();
    }
    lv_clientMutexRelease();
    if (g_lv->playIsPlaying == TRUE) {
        return (Uint32)g_lv->timerSleep;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Playback control
 * -------------------------------------------------------------------------- */
void lv_windowPlay(void) {
    if (g_lv->playIsPlaying == FALSE) {
        g_lv->timerGameID  = SDL_AddTimer(20,  lv_windowTimer,      NULL);
        g_lv->timerFrameID = SDL_AddTimer(50,  lv_windowFrameTimer, NULL);
    }
    g_lv->playIsPlaying = TRUE;
}

void lv_windowPause(void) {
    lv_clientMutexWaitFor();
    if (g_lv->playIsPlaying == TRUE) {
        SDL_RemoveTimer(g_lv->timerGameID);
        SDL_RemoveTimer(g_lv->timerFrameID);
        g_lv->timerGameID  = 0;
        g_lv->timerFrameID = 0;
    }
    g_lv->playIsPlaying = FALSE;
    lv_clientMutexRelease();
}

void lv_windowStop(int corruptLog) {
    if (corruptLog == TRUE) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE,
                                 "Error: Corrupt Log File", NULL);
    }
    lv_windowPause();
    SDL_Delay(500);
    lv_clientMutexWaitFor();
    lv_frontEndSetGameInformation(TRUE, 0, 0, 0, NULL, 0, 0, 0, 0, 0, NULL, 0);
    lv_updateItem(0, 0, 0, 0, 0, 0, 0, 0, FALSE);
    lv_screenCloseLog();
    g_lv->isLoaded = FALSE;
    lv_imgui_events_clear();
    lv_clientMutexRelease();
    lv_windowNeedRedraw();
}

void lv_windowFastForward(void) {
    lv_clientMutexWaitFor();
    lv_drawDirtyScreen();
    lv_screenFastForward();
    lv_drawDirtyScreen();
    lv_screenUpdate(redraw);
    lv_clientMutexRelease();
}

void lv_windowRewind(void) {
    char line[64];
    lv_clientMutexWaitFor();
    lv_drawDirtyScreen();
    lv_screenRewind();
    lv_drawDirtyScreen();
    lv_screenUpdate(redraw);
    lv_screenGetTime(line);
    lv_clientMutexRelease();
    if (g_lv->playIsPlaying == TRUE) {
        lv_controlsEnable(1 /* STATE_PLAYING */);
    } else {
        g_lv->wantScreenUpdate = TRUE;
    }
}

void lv_windowResize(void) {
    lv_clientMutexWaitFor();
    lv_drawResizeRenderTarget();
    lv_clientMutexRelease();
}

/* --------------------------------------------------------------------------
 * End-of-log / start-of-log callbacks from backend
 * -------------------------------------------------------------------------- */
void lv_finished(void) {
    g_lv->playIsPlaying = FALSE;
}

void lv_startOfLog(void) {
    /* ImGui controls panel reads isLoaded each frame */
}

/* --------------------------------------------------------------------------
 * Open / Save file
 * -------------------------------------------------------------------------- */
void lv_windowOpenFile(char *cmdLine) {
    char fileName[FILENAME_MAX];
    char memoryBuff[32];
    int  dlgResult;

    lv_windowStop(FALSE);
    fileName[0] = '\0';

    if (cmdLine == NULL) {
        dlgResult = lv_platform_dialog_open_file(
            "Open File...", "WinBolo Log Files", "*.wbv", "wbv",
            fileName, sizeof(fileName), NULL);
    } else {
        dlgResult = PLATFORM_DIALOG_OK;
        strncpy(fileName, cmdLine, sizeof(fileName) - 1);
        fileName[sizeof(fileName) - 1] = '\0';
    }

    if (dlgResult == PLATFORM_DIALOG_OK) {
        lv_platform_config_get_string("LOGVIEWER", "Memory Buffer", "4",
                                   memoryBuff, sizeof(memoryBuff));
        if (lv_screenLoadMap(fileName, atoi(memoryBuff)) == FALSE) {
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
    char fileName[FILENAME_MAX];
    char currentTime[FILENAME_MAX];
    int  dlgResult;
    int  count = 0;
    int  len;

    lv_windowPause();
    fileName[0] = '\0';

    lv_clientMutexWaitFor();
    lv_screenGetMapName(fileName);
    lv_screenGetTime(currentTime);
    lv_clientMutexRelease();

    /* Replace ':' in time string so it can appear in filenames */
    len = (int)strlen(currentTime);
    while (count < len) {
        if (currentTime[count] == ':') currentTime[count] = '.';
        count++;
    }

    strncat(fileName, " - ", sizeof(fileName) - strlen(fileName) - 1);
    strncat(fileName, currentTime, sizeof(fileName) - strlen(fileName) - 1);

    dlgResult = lv_platform_dialog_save_file(
        "Save Map File...", "WinBolo Map Files", "*.MAP", "map",
        fileName, fileName, sizeof(fileName), NULL);

    if (dlgResult == PLATFORM_DIALOG_OK) {
        if (lv_screenSaveMap(fileName, FALSE) == FALSE) {
            lv_platform_dialog_error(DIALOG_BOX_TITLE, "Error Saving Map File");
        }
    }
}

/* --------------------------------------------------------------------------
 * Preferences
 * -------------------------------------------------------------------------- */
static void loadPreferences(void) {
    char line[FILENAME_MAX];
    char def[FILENAME_MAX];
    char val[FILENAME_MAX];
    bool dns;
    BYTE count;

    /* Tank Centred */
    lv_platform_config_get_string("LOGVIEWER", "Tank Centred", "No", line, sizeof(line));
    if (tolower((unsigned char)line[0]) == 'y') {
        lv_screenTankCentred(TRUE);
    } else {
        lv_screenTankCentred(FALSE);
    }

    /* Sound Effects */
    lv_platform_config_get_string("LOGVIEWER", "Sounds", "Yes", line, sizeof(line));
    g_lv->isSoundsPlaying = (tolower((unsigned char)line[0]) == 'y') ? TRUE : FALSE;

    /* DNS Lookups */
    lv_platform_config_get_string("LOGVIEWER", "DNS Lookups", "No", line, sizeof(line));
    dns = (tolower((unsigned char)line[0]) == 'y') ? TRUE : FALSE;
    if (dns == TRUE) {
        dns = (bool)lv_dnsSetEnabled(TRUE);
    }

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

static void savePreferences(void) {
    char line[256];
    char val[64];
    BYTE count;

    /* ImGui module save (mode, dns, tank centred, window visibility) */
    lv_imgui_main_menu_save();
    lv_imgui_dialogs_save();

    /* Sound Effects */
    lv_platform_config_set_string("LOGVIEWER", "Sounds", g_lv->isSoundsPlaying ? "Yes" : "No");

    /* Use Team Colours */
    lv_platform_config_set_string("LOGVIEWER", "Use Team Colours", g_lv->useTeamColours ? "Yes" : "No");

    /* Screen Size */
    snprintf(line, sizeof(line), "%d", lv_screenGetSizeX());
    lv_platform_config_set_string("LOGVIEWER", "ScreenSizeX", line);
    snprintf(line, sizeof(line), "%d", lv_screenGetSizeY());
    lv_platform_config_set_string("LOGVIEWER", "ScreenSizeY", line);

    /* Team colours */
    count = 0;
    while (count < MAX_TANKS) {
        snprintf(line, sizeof(line), "Team Colour %d", count + 1);
        snprintf(val, sizeof(val), "%d", g_lv->tc[count]);
        lv_platform_config_set_string("LOGVIEWER", line, val);
        count++;
    }
    snprintf(val, sizeof(val), "%d", g_lv->tc[16]);
    lv_platform_config_set_string("LOGVIEWER", "Neutral Colour", val);

    /* Playback speed */
    snprintf(val, sizeof(val), "%d", g_lv->speed);
    lv_platform_config_set_string("LOGVIEWER", "Playback Speed", val);

    lv_platform_config_set_string("LOGVIEWER", "PreferencesSaved", "Yes");
    lv_platform_config_save();
}

/* --------------------------------------------------------------------------
 * logViewerRun -- Core viewer loop.
 *
 * Takes ownership of the event loop until the user exits.
 * Window and renderer are borrowed, not owned.
 * -------------------------------------------------------------------------- */
void logViewerRun(SDL_Window *window, SDL_Renderer *renderer,
                  const char *logPath, bool fromMainMenu) {
    char line[256];
    int  sizeX, sizeY;

    /* Allocate central logviewer state */
    g_lv = (LogViewerState *)calloc(1, sizeof(LogViewerState));
    if (g_lv == NULL) {
        return;
    }
    g_lv->fromMainMenu = fromMainMenu;
    g_lv->screenSizeX = MAIN_SCREEN_SIZE_X + 15; /* default 30 */
    g_lv->screenSizeY = MAIN_SCREEN_SIZE_Y + 15; /* default 30 */
    g_lv->isLoaded = FALSE;
    g_lv->isSoundsPlaying = TRUE;
    lv_screenSetState(g_lv);

    /* Platform abstraction init */
    lv_platform_config_init("WinBolo");
    lv_platform_dialogs_init();

    if (lv_clientMutexCreate() == FALSE) {
        lv_platform_dialog_error("Log Viewer", "Could not create mutex");
        if (window == NULL) { lv_platform_config_shutdown(); lv_platform_dialogs_shutdown(); }
        free(g_lv);
        g_lv = NULL;
        return;
    }

    /* Load screen size before setting up draw */
    lv_platform_config_get_string("LOGVIEWER", "ScreenSizeX", "50", line, sizeof(line));
    sizeX = atoi(line);
    if (sizeX < 5 || sizeX > 99) sizeX = 50;

    lv_platform_config_get_string("LOGVIEWER", "ScreenSizeY", "38", line, sizeof(line));
    sizeY = atoi(line);
    if (sizeY < 5 || sizeY > 99) sizeY = 38;

    lv_screenSetSizeX((BYTE)sizeX);
    lv_screenSetSizeY((BYTE)sizeY);

    if (window == NULL) {
        /* Standalone mode: create own window/renderer */
        g_lv->ownsWindow = TRUE;
        if (lv_drawSetup() == FALSE) {
            lv_clientMutexDestroy();
            lv_platform_config_shutdown();
            lv_platform_dialogs_shutdown();
            free(g_lv);
            g_lv = NULL;
            return;
        }
        g_lv->window = lv_drawGetSDLWindow();
        g_lv->renderer = lv_drawGetSDLRenderer();
        /* Standalone: init audio subsystem (embedded gets it from main app) */
        SDL_InitSubSystem(SDL_INIT_AUDIO);
    } else {
        /* Embedded mode: borrow caller's window/renderer */
        g_lv->window = window;
        g_lv->renderer = renderer;
        g_lv->ownsWindow = FALSE;
        if (lv_drawSetupWithHandles(window, renderer) == FALSE) {
            lv_clientMutexDestroy();
            free(g_lv);
            g_lv = NULL;
            return;
        }
    }

    /* Associate the window with dialogs (needed on Linux/Wayland for portal) */
    lv_platform_dialogs_set_window(g_lv->window);

    /* macOS trackpad pinch-to-zoom (no-op if already initialised or non-macOS) */
    macOSPinchZoomInit();

    /* Initialize ImGui */
    if (lv_imgui_context_init(g_lv->window, g_lv->renderer) == 0) {
        lv_drawCleanup();
        lv_clientMutexDestroy();
        free(g_lv);
        g_lv = NULL;
        return;
    }
    lv_imgui_main_menu_init(g_lv);
    lv_imgui_controls_init(g_lv);
    lv_imgui_game_info_init();
    lv_imgui_events_init();
    lv_imgui_item_info_init();
    lv_imgui_comments_init();
    lv_imgui_dialogs_init(g_lv);
    lv_imgui_game_viewport_init(g_lv);

    /* Sound (non-fatal if it fails) */
    if (lv_soundSetup() == FALSE) {
        g_lv->isSoundsPlaying = FALSE;
    }

    /* Load preferences */
    loadPreferences();

    /* Sync screen tile size to actual window dimensions so the game view
     * fills the window on first frame (not just after a manual resize).
     * Tile counts are sized against the zoom-effective tile pixel
     * size (TILE_SIZE * zoom), so this stays correct if a zoom level
     * was restored from preferences. */
    {
        int initW, initH;
        SDL_GetWindowSize(g_lv->window, &initW, &initH);
        if (initW > 0 && initH > 0) {
            int menuH = (int)lv_imgui_get_menu_bar_height();
            float zoom = lv_drawGetZoomLevel();
            if (zoom <= 0.0f) zoom = 1.0f;
            float tilePxX = (float)TILE_SIZE_X * zoom;
            float tilePxY = (float)TILE_SIZE_Y * zoom;
            int newTilesX = (int)(((float)initW + tilePxX * 0.5f) / tilePxX);
            int newTilesY = (int)((((float)(initH - menuH)) + tilePxY * 0.5f) / tilePxY);
            if (newTilesX < 1) newTilesX = 1;
            if (newTilesY < 1) newTilesY = 1;
            if (newTilesX > 255) newTilesX = 255;
            if (newTilesY > 255) newTilesY = 255;

            if ((BYTE)newTilesX != lv_screenGetSizeX() || (BYTE)newTilesY != lv_screenGetSizeY()) {
                lv_screenSetSizeX((BYTE)newTilesX);
                lv_screenSetSizeY((BYTE)newTilesY);
                lv_drawResizeRenderTarget();
                lv_drawDirtyScreen();
            }
        }
    }

    /* Open file from command line / caller if provided */
    if (logPath != NULL && strlen(logPath) > 0) {
        lv_windowOpenFile((char *)logPath);
    }

    /* Load from pending memory buffer if set (from logViewerRunFromMemory) */
    if (s_pendingZipData != NULL) {
        if (lv_screenLoadMapFromMemory(s_pendingZipData, s_pendingZipLen) == FALSE) {
            /* lv_screenLoadMapFromMemory took ownership even on failure */
            g_lv->isLoaded = FALSE;
        } else {
            g_lv->isLoaded = TRUE;
            lv_imgui_events_clear();
            lv_windowNeedRedraw();
        }
        s_pendingZipData = NULL;
        s_pendingZipLen  = 0;
    }

    /* -----------------------------------------------------------------------
     * Main event loop
     * ----------------------------------------------------------------------- */
    while (!g_lv->quit) {
        SDL_Event sdlEvent;

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
                int w = sdlEvent.window.data1;
                int h = sdlEvent.window.data2;
                int menuH = (int)lv_imgui_get_menu_bar_height();
                float zoom = lv_drawGetZoomLevel();
                if (zoom <= 0.0f) zoom = 1.0f;

                /* Snap to the nearest zoom-effective tile boundary so the
                 * blit fills the window cleanly at the current zoom. */
                float tilePxX = (float)TILE_SIZE_X * zoom;
                float tilePxY = (float)TILE_SIZE_Y * zoom;
                int newTilesX = (int)(((float)w + tilePxX * 0.5f) / tilePxX);
                int newTilesY = (int)((((float)(h - menuH)) + tilePxY * 0.5f) / tilePxY);
                if (newTilesX < 1) newTilesX = 1;
                if (newTilesY < 1) newTilesY = 1;
                if (newTilesX > 255) newTilesX = 255;
                if (newTilesY > 255) newTilesY = 255;

                int snappedW = (int)(newTilesX * tilePxX + 0.5f);
                int snappedH = (int)(newTilesY * tilePxY + 0.5f);
                SDL_SetWindowSize(g_lv->window, snappedW, snappedH + menuH);
                {
                    lv_screenSetSizeX((BYTE)newTilesX);
                    lv_screenSetSizeY((BYTE)newTilesY);
                    /* Clamp scroll offset so the viewport stays within the 255x255 map.
                     * Reset sub-pixel pan so the resized viewport snaps cleanly to
                     * tile boundaries — there's no "in-flight drag" state to preserve
                     * across a window resize. */
                    if (g_lv->isLoaded) {
                        BYTE ox, oy;
                        lv_screenGetOffsets(&ox, &oy);
                        if ((int)ox + newTilesX > 255) ox = (BYTE)(255 - newTilesX);
                        if ((int)oy + newTilesY > 255) oy = (BYTE)(255 - newTilesY);
                        lv_screenSetOffset(ox, oy);
                        lv_screenSetSubOffset(0, 0);
                    }
                }
                lv_drawResizeRenderTarget();
                lv_drawDirtyScreen();
                g_lv->wantScreenUpdate = TRUE;
            }
            if (sdlEvent.type == SDL_EVENT_MOUSE_WHEEL) {
                /* Stepped zoom anchored on the cursor. Skip when ImGui is
                 * using the wheel (cursor over a panel) so panel scrolling
                 * still works. The wheel event carries the mouse position
                 * in window coordinates (mouse_x / mouse_y). */
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
            if (sdlEvent.type == SDL_EVENT_QUIT) {
                g_lv->quit = TRUE;
                break;
            }
        }

        /* Trackpad pinch-to-zoom (macOS) — anchor on current mouse position */
        {
            float pinch = macOSPinchZoomConsume();
            if (pinch != 0.0f) {
                static float pinchAccum = 0.0f;
                pinchAccum += pinch;
                float mx, my;
                SDL_GetMouseState(&mx, &my);
                while (pinchAccum > 0.15f) {
                    lv_drawZoomIn((int)mx, (int)my);
                    pinchAccum -= 0.15f;
                }
                while (pinchAccum < -0.15f) {
                    lv_drawZoomOut((int)mx, (int)my);
                    pinchAccum += 0.15f;
                }
            }
        }

        /* Clear the frame */
        SDL_SetRenderDrawColor(g_lv->renderer, 0, 0, 0, 255);
        SDL_RenderClear(g_lv->renderer);

        if (g_lv->isLoaded == FALSE) {
            lv_drawSplashForImGui();
        } else {
            if (g_lv->wantScreenUpdate == TRUE) {
                lv_clientMutexWaitFor();
                lv_drawDirtyScreen();
                lv_screenUpdate(redraw);
                lv_clientMutexRelease();
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
        lv_imgui_comments_window();
        lv_g_reset_window_positions = false;
        lv_imgui_dialogs_render();
        lv_imgui_context_render();

        lv_imgui_game_viewport_process_input();

        SDL_RenderPresent(g_lv->renderer);
    }

    /* -----------------------------------------------------------------------
     * Shutdown
     * ----------------------------------------------------------------------- */
    savePreferences();
    lv_windowStop(FALSE);
    lv_imgui_comments_shutdown();
    lv_imgui_context_shutdown();
    lv_drawCleanupSplash();
    lv_soundCleanup();
    {
        bool standalone = g_lv->ownsWindow;
        lv_drawCleanup();
        lv_clientMutexDestroy();
        lv_dnsShutdown();
        if (standalone) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            lv_platform_config_shutdown();
            lv_platform_dialogs_shutdown();
        }
    }
    free(g_lv);
    g_lv = NULL;
}

/* --------------------------------------------------------------------------
 * logViewerRunFromMemory -- Load a log from an in-memory zip buffer.
 *
 * Identical to logViewerRun but loads from memory instead of file.
 * Takes ownership of zipData (freed when log viewer closes).
 * -------------------------------------------------------------------------- */
void logViewerRunFromMemory(SDL_Window *window, SDL_Renderer *renderer,
                            uint8_t *zipData, size_t zipLen, bool fromMainMenu) {
    /* Use static pending vars that logViewerRun checks after init */
    s_pendingZipData = zipData;
    s_pendingZipLen  = zipLen;
    logViewerRun(window, renderer, NULL, fromMainMenu);
}
