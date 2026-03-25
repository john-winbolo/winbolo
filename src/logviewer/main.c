/*
 * $Id$
 *
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

/* Phase 5: Win32 window/dialog/menu code removed. SDL + ImGui only. */

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

/* Must be included after global.h to avoid bool type conflict.
 * SDL_MAIN_HANDLED tells SDL3 we supply our own WinMain on Windows. */
#ifdef _WIN32
#  define SDL_MAIN_HANDLED
#endif
#include <SDL3/SDL.h>

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
const char *g_version_string = "1.01";

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

/* SDL timers */
SDL_TimerID timerGameID  = 0;
SDL_TimerID timerFrameID = 0;


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
 * Called from imgui_controls.cpp via extern, and from mainLoadPreferences.
 * -------------------------------------------------------------------------- */
void updateSpeed(BYTE spd, int updateSlider) {
    (void)updateSlider; /* ImGui slider reads the 'speed' global directly */
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

/* Remove events that are ahead of the current playback position (during rewind).
 * ImGui events window doesn't support partial removal, so clear all. */
void windowRemoveEvents(void) {
    imgui_events_clear();
}

/* --------------------------------------------------------------------------
 * Control state – ImGui panels read globals directly each frame,
 * so these functions are no-ops.
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
 * SDL timer callbacks (called on background threads – only set flags)
 * -------------------------------------------------------------------------- */
Uint32 SDLCALL windowFrameTimer(void *userdata, SDL_TimerID timerID, Uint32 interval) {
    (void)userdata; (void)timerID; (void)interval;
    wantScreenUpdate = TRUE;
    if (playIsPlaying == TRUE) {
        return 50;
    }
    return 0;
}

Uint32 SDLCALL windowTimer(void *userdata, SDL_TimerID timerID, Uint32 interval) {
    (void)userdata; (void)timerID; (void)interval;
    clientMutexWaitFor();
    screenLogTick();
    if (doubleSpeed == TRUE) {
        screenLogTick();
        screenLogTick();
    }
    clientMutexRelease();
    if (playIsPlaying == TRUE) {
        return (Uint32)timerSleep;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Playback control
 * -------------------------------------------------------------------------- */
void windowPlay(void) {
    if (playIsPlaying == FALSE) {
        timerGameID  = SDL_AddTimer(20,  windowTimer,      NULL);
        timerFrameID = SDL_AddTimer(50,  windowFrameTimer, NULL);
    }
    playIsPlaying = TRUE;
}

void windowPause(void) {
    clientMutexWaitFor();
    if (playIsPlaying == TRUE) {
        SDL_RemoveTimer(timerGameID);
        SDL_RemoveTimer(timerFrameID);
        timerGameID  = 0;
        timerFrameID = 0;
    }
    playIsPlaying = FALSE;
    clientMutexRelease();
}

void windowStop(int corruptLog) {
    if (corruptLog == TRUE) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE,
                                 "Error: Corrupt Log File", NULL);
    }
    windowPause();
    SDL_Delay(500);
    clientMutexWaitFor();
    frontEndSetGameInformation(TRUE, 0, 0, 0, NULL, 0, 0, 0, 0, 0, NULL, 0);
    updateItem(0, 0, 0, 0, 0, 0, 0, 0, FALSE);
    screenCloseLog();
    isLoaded = FALSE;
    imgui_events_clear();
    clientMutexRelease();
    windowNeedRedraw();
}

void windowFastForward(void) {
    clientMutexWaitFor();
    drawDirtyScreen();
    screenFastForward();
    drawDirtyScreen();
    screenUpdate(redraw);
    clientMutexRelease();
}

void windowRewind(void) {
    char line[64];
    clientMutexWaitFor();
    drawDirtyScreen();
    screenRewind();
    drawDirtyScreen();
    screenUpdate(redraw);
    screenGetTime(line);
    clientMutexRelease();
    if (playIsPlaying == TRUE) {
        controlsEnable(1 /* STATE_PLAYING */);
    } else {
        wantScreenUpdate = TRUE;
    }
}

void windowResize(void) {
    clientMutexWaitFor();
    drawResizeRenderTarget();
    clientMutexRelease();
}

/* --------------------------------------------------------------------------
 * End-of-log / start-of-log callbacks from backend
 * -------------------------------------------------------------------------- */
void finished(void) {
    playIsPlaying = FALSE;
    /* ImGui controls panel reads playIsPlaying each frame */
}

void startOfLog(void) {
    /* ImGui controls panel reads isLoaded each frame */
}

/* --------------------------------------------------------------------------
 * Open / Save file
 * -------------------------------------------------------------------------- */
void windowOpenFile(char *cmdLine) {
    char fileName[FILENAME_MAX];
    char memoryBuff[32];
    int  dlgResult;

    windowStop(FALSE);
    fileName[0] = '\0';

    if (cmdLine == NULL) {
        dlgResult = platform_dialog_open_file(
            "Open File...", "WinBolo Log Files", "*.wbv", "wbv",
            fileName, sizeof(fileName), NULL);
    } else {
        dlgResult = PLATFORM_DIALOG_OK;
        strncpy(fileName, cmdLine, sizeof(fileName) - 1);
        fileName[sizeof(fileName) - 1] = '\0';
    }

    if (dlgResult == PLATFORM_DIALOG_OK) {
        platform_config_get_string("LOGVIEWER", "Memory Buffer", "4",
                                   memoryBuff, sizeof(memoryBuff));
        if (screenLoadMap(fileName, atoi(memoryBuff)) == FALSE) {
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
    char fileName[FILENAME_MAX];
    char currentTime[FILENAME_MAX];
    int  dlgResult;
    int  count = 0;
    int  len;

    windowPause();
    fileName[0] = '\0';

    clientMutexWaitFor();
    screenGetMapName(fileName);
    screenGetTime(currentTime);
    clientMutexRelease();

    /* Replace ':' in time string so it can appear in filenames */
    len = (int)strlen(currentTime);
    while (count < len) {
        if (currentTime[count] == ':') currentTime[count] = '.';
        count++;
    }

    strncat(fileName, " - ", sizeof(fileName) - strlen(fileName) - 1);
    strncat(fileName, currentTime, sizeof(fileName) - strlen(fileName) - 1);

    dlgResult = platform_dialog_save_file(
        "Save Map File...", "WinBolo Map Files", "*.MAP", "map",
        fileName, fileName, sizeof(fileName), NULL);

    if (dlgResult == PLATFORM_DIALOG_OK) {
        if (screenSaveMap(fileName, FALSE) == FALSE) {
            platform_dialog_error(DIALOG_BOX_TITLE, "Error Saving Map File");
        }
    }
}

/* --------------------------------------------------------------------------
 * Preferences
 * -------------------------------------------------------------------------- */
void mainLoadPreferences(void) {
    char line[FILENAME_MAX];
    char def[FILENAME_MAX];
    char val[FILENAME_MAX];
    bool dns;
    BYTE count;

    /* Tank Centred */
    platform_config_get_string("LOGVIEWER", "Tank Centred", "No", line, sizeof(line));
    if (tolower((unsigned char)line[0]) == 'y') {
        screenTankCentred(TRUE);
    } else {
        screenTankCentred(FALSE);
    }

    /* Sound Effects */
    platform_config_get_string("LOGVIEWER", "Sounds", "Yes", line, sizeof(line));
    isSoundsPlaying = (tolower((unsigned char)line[0]) == 'y') ? TRUE : FALSE;

    /* DNS Lookups */
    platform_config_get_string("LOGVIEWER", "DNS Lookups", "No", line, sizeof(line));
    dns = (tolower((unsigned char)line[0]) == 'y') ? TRUE : FALSE;
    if (dns == TRUE) {
        dns = (bool)dnsSetEnabled(TRUE);
    }

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

void mainSavePrefernces(void) {
    char line[256];
    char val[64];
    BYTE count;

    /* ImGui module save (mode, dns, tank centred, window visibility) */
    imgui_main_menu_save();
    imgui_dialogs_save();

    /* Sound Effects */
    platform_config_set_string("LOGVIEWER", "Sounds", isSoundsPlaying ? "Yes" : "No");

    /* Use Team Colours */
    platform_config_set_string("LOGVIEWER", "Use Team Colours", useTeamColours ? "Yes" : "No");

    /* Screen Size */
    snprintf(line, sizeof(line), "%d", screenGetSizeX());
    platform_config_set_string("LOGVIEWER", "ScreenSizeX", line);
    snprintf(line, sizeof(line), "%d", screenGetSizeY());
    platform_config_set_string("LOGVIEWER", "ScreenSizeY", line);

    /* Team colours */
    count = 0;
    while (count < MAX_TANKS) {
        snprintf(line, sizeof(line), "Team Colour %d", count + 1);
        snprintf(val, sizeof(val), "%d", tc[count]);
        platform_config_set_string("LOGVIEWER", line, val);
        count++;
    }
    snprintf(val, sizeof(val), "%d", tc[16]);
    platform_config_set_string("LOGVIEWER", "Neutral Colour", val);

    /* Playback speed */
    snprintf(val, sizeof(val), "%d", speed);
    platform_config_set_string("LOGVIEWER", "Playback Speed", val);

    platform_config_set_string("LOGVIEWER", "PreferencesSaved", "Yes");
    platform_config_save();
}

/* --------------------------------------------------------------------------
 * main – SDL-only event loop, no Win32 window/dialog infrastructure
 * -------------------------------------------------------------------------- */
int main(int argc, char *argv[]);

#ifdef _WIN32
/* WinMain entry point required by /SUBSYSTEM:WINDOWS. Delegates to main(). */
int __stdcall WinMain(void *hInst, void *hPrev, char *lpCmd, int nShow) {
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;
    return main(__argc, __argv);
}
#endif

int main(int argc, char *argv[]) {
    bool done = FALSE;
    char line[256];
    int  sizeX, sizeY;

    /* Platform abstraction init */
    platform_config_init("WinBolo");
    platform_dialogs_init();

    isLoaded      = FALSE;
    isSoundsPlaying = TRUE;

    if (clientMutexCreate() == FALSE) {
        platform_dialog_error("Log Viewer", "Could not create mutex");
        platform_config_shutdown();
        platform_dialogs_shutdown();
        return 0;
    }

    /* Load screen size before creating window */
    platform_config_get_string("LOGVIEWER", "ScreenSizeX", "50", line, sizeof(line));
    sizeX = atoi(line);
    if (sizeX < 5 || sizeX > 99) sizeX = 50;

    platform_config_get_string("LOGVIEWER", "ScreenSizeY", "38", line, sizeof(line));
    sizeY = atoi(line);
    if (sizeY < 5 || sizeY > 99) sizeY = 38;

    screenSetSizeX((BYTE)sizeX);
    screenSetSizeY((BYTE)sizeY);

    if (drawSetup() == FALSE) {
        clientMutexDestroy();
        platform_config_shutdown();
        platform_dialogs_shutdown();
        return 0;
    }

    /* Associate the window with dialogs (needed on Linux/Wayland for portal) */
    platform_dialogs_set_window(drawGetSDLWindow());

    /* Initialize ImGui */
    if (imgui_context_init(drawGetSDLWindow(), drawGetSDLRenderer()) == 0) {
        drawCleanup();
        clientMutexDestroy();
        platform_config_shutdown();
        platform_dialogs_shutdown();
        return 0;
    }
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

    /* Open file from command line if provided */
    if (argc > 1 && argv[1] && strlen(argv[1]) > 0) {
        windowOpenFile(argv[1]);
    }

    /* -----------------------------------------------------------------------
     * Main event loop
     * ----------------------------------------------------------------------- */
    while (done == FALSE) {
        SDL_Event sdlEvent;

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

                /* Snap width and height (minus menu bar) to nearest 16-pixel tile boundary */
                int tileW = ((w + TILE_SIZE_X / 2) / TILE_SIZE_X) * TILE_SIZE_X;
                int tileH = (((h - menuH) + TILE_SIZE_Y / 2) / TILE_SIZE_Y) * TILE_SIZE_Y;
                if (tileW < TILE_SIZE_X) tileW = TILE_SIZE_X;
                if (tileH < TILE_SIZE_Y) tileH = TILE_SIZE_Y;
                /* Cap to map boundary (255 tiles * 16px) */
                if (tileW > 255 * TILE_SIZE_X) tileW = 255 * TILE_SIZE_X;
                if (tileH > 255 * TILE_SIZE_Y) tileH = 255 * TILE_SIZE_Y;

                SDL_SetWindowSize(drawGetSDLWindow(), tileW, tileH + menuH);
                {
                    BYTE newTilesX = (BYTE)(tileW / TILE_SIZE_X);
                    BYTE newTilesY = (BYTE)(tileH / TILE_SIZE_Y);
                    screenSetSizeX(newTilesX);
                    screenSetSizeY(newTilesY);
                    /* Clamp scroll offset so the viewport stays within the 255x255 map */
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
            if (sdlEvent.type == SDL_EVENT_QUIT) {
                done = TRUE;
                break;
            }
        }

        /* Clear the frame */
        SDL_SetRenderDrawColor(drawGetSDLRenderer(), 0, 0, 0, 255);
        SDL_RenderClear(drawGetSDLRenderer());

        if (isLoaded == FALSE) {
            drawSplashForImGui();
        } else {
            if (wantScreenUpdate == TRUE) {
                clientMutexWaitFor();
                drawDirtyScreen();
                screenUpdate(redraw);
                clientMutexRelease();
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

    /* -----------------------------------------------------------------------
     * Shutdown
     * ----------------------------------------------------------------------- */
    mainSavePrefernces();
    windowStop(FALSE);
    imgui_context_shutdown();
    drawCleanupSplash();
    soundCleanup();
    drawCleanup();
    SDL_Quit();
    clientMutexDestroy();
    dnsShutdown();
    platform_config_shutdown();
    platform_dialogs_shutdown();
    return 0;
}
