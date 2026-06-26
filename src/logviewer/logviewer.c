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
#include "spectator_drain.h"   /* dep-free seam: drive/drain the spectator feed */
#include "lv_global.h"
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
#include "imgui/imgui_game_view.h"
#include "imgui/imgui_logviewer_menu.h"
#include "game_view.h"

/* Platform abstraction */
#include "platform/platform_config.h"
#include "platform/platform_dialogs.h"
#include "../gui/sdl3/macos_pinch.h"
#include "../gui/ui_mode.h"
#ifdef __APPLE__
#include "platform/mac_menubar.h"
#endif

/* Version string referenced by imgui_dialogs.cpp */
const char *lv_g_version_string = "1.01";

/* File-scope pointer to the current viewer session state */
static LogViewerState *g_lv = NULL;

/* Game-view accessors (Phase D of plans/ctrailer.md). game_view.c includes
 * the bolo-side mapview.h headers and so cannot include backend.h (the two
 * share type names but use different layouts). These accessors expose
 * pointers/values from g_lv as void* /scalar so game_view.c can drive the
 * frame assembly without seeing the LogViewerState struct definition. */
void *lv_gameViewGetScreen(void)         { return &g_lv->view;     }
void *lv_gameViewGetMineView(void)       { return &g_lv->mineView; }
void *lv_gameViewGetBases(void)          { return &g_lv->bs;       }
void *lv_gameViewGetPills(void)          { return &g_lv->pb;       }
BYTE  lv_gameViewGetCameraSlot(void)     { return g_lv->cameraSlot; }
bool  lv_gameViewIsHudAlive(BYTE slot)   {
  if (slot >= MAX_TANKS) return false;
  return g_lv->gameViewHud[slot].alive;
}
uint16_t lv_gameViewGetKills(BYTE slot)  {
  if (slot >= MAX_TANKS) return 0;
  return g_lv->kills[slot];
}
uint16_t lv_gameViewGetDeaths(BYTE slot) {
  if (slot >= MAX_TANKS) return 0;
  return g_lv->deaths[slot];
}
uint32_t lv_gameViewGetDeathTimeMs(BYTE slot) {
  if (slot >= MAX_TANKS) return 0;
  return g_lv->gameViewHud[slot].deathTimeMs;
}
void lv_gameViewGetInventory(BYTE slot, BYTE *shells, BYTE *mines, BYTE *armour, BYTE *trees) {
  if (slot >= MAX_TANKS) {
    if (shells) *shells = 0;
    if (mines)  *mines  = 0;
    if (armour) *armour = 0;
    if (trees)  *trees  = 0;
    return;
  }
  if (shells) *shells = g_lv->tankInv[slot].shells;
  if (mines)  *mines  = g_lv->tankInv[slot].mines;
  if (armour) *armour = g_lv->tankInv[slot].armour;
  if (trees)  *trees  = g_lv->tankInv[slot].trees;
}

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
    /* Spectator live-DVR runs in the foreground spectatorRun loop, which drives
       the decoder itself — never start the background replay timer here (it would
       double-drive lv_screenLogTick and race the live append). playIsPlaying is
       the orthogonal play/freeze flag the loop reads. */
    if (lv_screenSpecIsLiveMode()) {
        g_lv->playIsPlaying = TRUE;
        return;
    }
    if (g_lv->playIsPlaying == FALSE) {
        g_lv->timerGameID  = SDL_AddTimer(20,  lv_windowTimer,      NULL);
        g_lv->timerFrameID = SDL_AddTimer(50,  lv_windowFrameTimer, NULL);
    }
    g_lv->playIsPlaying = TRUE;
}

void lv_windowPause(void) {
    if (lv_screenSpecIsLiveMode()) {
        g_lv->playIsPlaying = FALSE;
        return;
    }
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
    /* Spectator: the panel's Stop button freezes the feed rather than closing the
       live log out from under the foreground host loop. */
    if (lv_screenSpecIsLiveMode()) {
        g_lv->playIsPlaying = FALSE;
        return;
    }
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

    /* Sound Volume (0-100, mirrors main game) */
    lv_platform_config_get_string("LOGVIEWER", "Sound Volume", "50", line, sizeof(line));
    g_lv->soundVolume = atoi(line);
    if (g_lv->soundVolume < 0) g_lv->soundVolume = 0;
    if (g_lv->soundVolume > 100) g_lv->soundVolume = 100;
    lv_soundSetVolume(g_lv->soundVolume);

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

    /* Sound Volume */
    snprintf(val, sizeof(val), "%d", g_lv->soundVolume);
    lv_platform_config_set_string("LOGVIEWER", "Sound Volume", val);

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
 * lvHostSetup -- Shared host bring-up for the log viewer and the spectator.
 *
 * Allocates g_lv, brings up the platform layer / mutex / draw / ImGui / sound,
 * loads preferences and sizes the screen to the window. The window/renderer are
 * borrowed when non-NULL (embedded) or created here when NULL (standalone).
 * Returns TRUE on success; on any failure it unwinds whatever it brought up and
 * returns FALSE with g_lv cleared. Pure extraction from logViewerRun.
 * -------------------------------------------------------------------------- */
static int lvHostSetup(SDL_Window *window, SDL_Renderer *renderer,
                       bool fromMainMenu) {
    char line[256];
    int  sizeX, sizeY;

    /* Allocate central logviewer state */
    g_lv = lv_decoderCreate(fromMainMenu);
    if (g_lv == NULL) {
        return FALSE;
    }

    /* Platform abstraction init */
    lv_platform_config_init("WinBolo");
    lv_platform_dialogs_init();

    if (lv_clientMutexCreate() == FALSE) {
        lv_platform_dialog_error("WinBolo Log Viewer", "Could not create mutex");
        if (window == NULL) { lv_platform_config_shutdown(); lv_platform_dialogs_shutdown(); }
        free(g_lv);
        g_lv = NULL;
        return FALSE;
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
            return FALSE;
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
            return FALSE;
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
        return FALSE;
    }
    lv_imgui_main_menu_init(g_lv);
#ifdef __APPLE__
    /* Install the native NSMenu after ImGui + window are up. The shim's
     * save-on-install / restore-on-uninstall stack swaps WinBolo's menu
     * out when embedded, and builds a Log Viewer app menu when standalone
     * (no previous mainMenu to save). */
    lv_mac_menubar_install(g_lv->window, g_lv);
#endif
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

    return TRUE;
}

/* --------------------------------------------------------------------------
 * lvHostTeardown -- Shared host shutdown for the log viewer and the spectator.
 *
 * Stops the decoder, tears down ImGui / draw / sound / mutex / DNS and (when
 * standalone) the platform layer, then frees g_lv. The caller restores any
 * active game view and saves preferences first. Pure extraction.
 * -------------------------------------------------------------------------- */
static void lvHostTeardown(void) {
    lv_windowStop(FALSE);
#ifdef __APPLE__
    /* Restore the previously-installed NSMenu (WinBolo's, when embedded;
     * empty stub when standalone since the process is exiting). */
    lv_mac_menubar_uninstall();
#endif
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
 * lvHostHandleGameViewKey -- Shared game-view input: grave-key game-view
 * toggle, Tab camera cycle, 1-4 zoom, arrow-key pan, ,-. time seek. Returns
 * TRUE if the event was consumed (the caller should skip its other handlers).
 * allowToggle gates the grave-key on/off toggle and allowSeek gates the ,-.
 * seek; the spectator host (always game-view, live feed) suppresses both.
 * Pure extraction from logViewerRun (the two gates are the only additions;
 * the viewer passes both TRUE, preserving its behaviour). */
static bool lvHostHandleGameViewKey(SDL_Event sdlEvent, bool allowToggle,
                                    bool allowSeek) {
    /* Game-view-mode toggle. Must run BEFORE ImGui sees the event so
     * ImGui doesn't swallow the backtick keypress. Default zoom 3×.
     * Allowed in both the standalone viewer and the embedded
     * "Watch a Log" flow — game-view borrows whichever window is
     * active and restores it on toggle-off. */
    if (allowToggle && sdlEvent.type == SDL_EVENT_KEY_DOWN &&
        sdlEvent.key.key == SDLK_GRAVE && g_lv->isLoaded) {
        if (!g_lv->gameView) {
            g_lv->savedUseTeamColours = g_lv->useTeamColours;
            g_lv->useTeamColours = FALSE;
            lv_drawGameViewSetup(/* zoom */ 3);
            /* Centre on the camera tank so the first frame after
             * activation isn't a partial viewport into the corner of
             * the map. Subsequent frames use the dead-zone tracker. */
            lv_imgui_game_view_init_camera(g_lv);
        } else {
            lv_drawGameViewTeardown();
            g_lv->useTeamColours = g_lv->savedUseTeamColours;
        }
        g_lv->gameView = !g_lv->gameView;
        lv_drawDirtyScreen();
        g_lv->wantScreenUpdate = TRUE;
        return TRUE;
    }

    /* Tab cycles cameraSlot to the next in-use player. Run BEFORE
     * the ImGui handler so ImGui's widget-focus traversal doesn't
     * eat the keypress while in game view. */
    if (g_lv->gameView && sdlEvent.type == SDL_EVENT_KEY_DOWN &&
        sdlEvent.key.key == SDLK_TAB) {
        BYTE start = g_lv->cameraSlot;
        BYTE found = start;
        BYTE i;
        for (i = 1; i <= MAX_TANKS; i++) {
            BYTE next = (BYTE)((start + i) % MAX_TANKS);
            if (lv_playersIsInUse(next)) {
                found = next;
                break;
            }
        }
        g_lv->cameraSlot = found;
        /* Re-centre on the new camera tank — it may be far outside
         * the previous viewport, and the dead-zone tracker would
         * otherwise scroll one tile per frame to catch up. */
        lv_imgui_game_view_init_camera(g_lv);
        g_lv->wantScreenUpdate = TRUE;
        return TRUE;
    }

    /* Phase E: zoom hotkeys 1/2/3/4 — only meaningful while
     * game view is active. Tear down and re-set up at the
     * new zoom; preserves cameraSlot since that's on
     * LogViewerState, not in game_view.c's statics. */
    if (g_lv->gameView && sdlEvent.type == SDL_EVENT_KEY_DOWN) {
        int newZoom = 0;
        switch (sdlEvent.key.key) {
            case SDLK_1: newZoom = 1; break;
            case SDLK_2: newZoom = 2; break;
            case SDLK_3: newZoom = 3; break;
            case SDLK_4: newZoom = 4; break;
            default: break;
        }
        if (newZoom > 0 && newZoom != lv_drawGameViewGetZoom()) {
            lv_drawGameViewTeardown();
            lv_drawGameViewSetup(newZoom);
            g_lv->wantScreenUpdate = TRUE;
            return TRUE;
        }
    }

    /* Manual arrow-key viewport scroll while game view is active.
     * Pans by kArrowStep unzoomed pixels per key event (SDL fires
     * SDL_EVENT_KEY_DOWN repeatedly while held), going through
     * lv_screenPanToTotalPixels so the pan accumulates at sub-tile
     * granularity into (xOffset,subPxX) instead of snapping a
     * whole tile per repeat. The dead-zone tracker on the next
     * frame may ease the camera back if the pan moved the tank
     * out of the safe zone. */
    if (g_lv->gameView && sdlEvent.type == SDL_EVENT_KEY_DOWN) {
        int dx = 0, dy = 0;
        switch (sdlEvent.key.key) {
            case SDLK_LEFT:  dx = -1; break;
            case SDLK_RIGHT: dx =  1; break;
            case SDLK_UP:    dy = -1; break;
            case SDLK_DOWN:  dy =  1; break;
            default: break;
        }
        if (dx || dy) {
            const int kViewTiles  = 16; /* mirrors GV_SCREEN_TILES */
            const int kTilePx     = 16; /* TILE_SIZE_X */
            const int kArrowStep  = 8;  /* unzoomed px per repeat */
            int totalPxX = (int)g_lv->xOffset * kTilePx + g_lv->subPxX
                         + dx * kArrowStep;
            int totalPxY = (int)g_lv->yOffset * kTilePx + g_lv->subPxY
                         + dy * kArrowStep;
            if (lv_playersIsInUse(g_lv->cameraSlot)) {
                BYTE camMx, camMy, camPx, camPy, camFr;
                bool camBoat;
                lv_playersGetTankDetails(g_lv->cameraSlot,
                    &camMx, &camMy, &camPx, &camPy, &camFr, &camBoat);
                int tankWorldPxX = (int)camMx * kTilePx + (int)camPx;
                int tankWorldPxY = (int)camMy * kTilePx + (int)camPy;
                int viewportPx   = kViewTiles * kTilePx;
                if      (tankWorldPxX <  totalPxX)              totalPxX = tankWorldPxX;
                else if (tankWorldPxX >= totalPxX + viewportPx) totalPxX = tankWorldPxX - viewportPx + 1;
                if      (tankWorldPxY <  totalPxY)              totalPxY = tankWorldPxY;
                else if (tankWorldPxY >= totalPxY + viewportPx) totalPxY = tankWorldPxY - viewportPx + 1;
            }
            lv_screenPanToTotalPixels(totalPxX, totalPxY);
            g_lv->wantScreenUpdate = TRUE;
            return TRUE;
        }
    }

    /* ',' / '<' jump back, '.' / '>' jump forward, 10s each.
     * SDL3 reports key.key as the unmodified keycode by default,
     * so Shift+',' arrives as SDLK_COMMA — match both physical
     * keys regardless of shift state. Re-centres the camera
     * after the seek because a 10s jump can leave the followed
     * tank far outside the viewport, and the dead-zone tracker
     * would otherwise scroll one tile per frame to catch up.
     * Suppressed (allowSeek=FALSE) on the live spectator feed. */
    if (allowSeek && g_lv->gameView && sdlEvent.type == SDL_EVENT_KEY_DOWN &&
        g_lv->isLoaded &&
        (sdlEvent.key.key == SDLK_COMMA  || sdlEvent.key.key == SDLK_LESS ||
         sdlEvent.key.key == SDLK_PERIOD || sdlEvent.key.key == SDLK_GREATER)) {
        size_t curPos, totSize;
        uint32_t curTime, totTime;
        lv_screenGetLogProgress(&curPos, &totSize, &curTime, &totTime);
        if (totTime > 0) {
            const uint32_t kStepMs = 10000;
            bool back = (sdlEvent.key.key == SDLK_COMMA ||
                         sdlEvent.key.key == SDLK_LESS);
            uint32_t targetMs;
            if (back) {
                targetMs = (curTime > kStepMs) ? curTime - kStepMs : 0;
            } else {
                targetMs = curTime + kStepMs;
                if (targetMs > totTime) targetMs = totTime;
            }
            float ratio = (float)targetMs / (float)totTime;
            unsigned char wasPlaying = g_lv->playIsPlaying;
            if (wasPlaying) lv_windowPause();
            lv_clientMutexWaitFor();
            lv_drawDirtyScreen();
            lv_screenSeekToPosition(ratio);
            lv_drawDirtyScreen();
            lv_clientMutexRelease();
            lv_imgui_game_view_init_camera(g_lv);
            lv_windowNeedRedraw();
            if (wasPlaying) lv_windowPlay();
            g_lv->wantScreenUpdate = TRUE;
        }
        return TRUE;
    }

    return FALSE;
}

/* --------------------------------------------------------------------------
 * lvHostRenderFrame -- Shared per-frame scene + ImGui render dispatch.
 *
 * Renders the current mode: the block-grid overview when !gameView, the
 * followed game view when gameView (with the cameraSlot setSelf wrap so tank
 * sprite colours track the camera), then runs the ImGui pass for that mode and
 * presents. Pure extraction of logViewerRun's frame body, shared with
 * spectatorRun.
 *
 * overlay, when non-NULL, is drawn centred just before the ImGui render (the
 * live spectator's "connection lost" message); the standalone viewer passes
 * NULL.
 *
 * Live-spectator additions are gated on lv_screenSpecIsLiveMode(), which only
 * spectatorRun ever sets: the DVR scrubber is kept reachable in the game view,
 * and the block-grid pass renders a safe scrubber + event-feed + game-info
 * subset instead of the viewer's menu bar and full panel set -- the menu bar's
 * File/Action items (and Ctrl shortcuts) would tear down the live session, and
 * the comments panel needs a finished log's WBN key. The viewer is never in
 * live mode, so its behaviour is unchanged. */
static void lvHostRenderFrame(const char *overlay) {
    /* Clear the frame */
    SDL_SetRenderDrawColor(g_lv->renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_lv->renderer);

    if (g_lv->isLoaded == FALSE) {
        lv_drawSplashForImGui();
    } else {
        /* In game-view mode, recentre the camera on the spectated tank
         * BEFORE lv_screenUpdate runs so the tile renderer reads the
         * fresh offsets this frame. */
        if (g_lv->gameView) {
            lv_imgui_game_view_update_camera(g_lv);
        }
        /* Game view renders directly to the framebuffer (no
         * textureTarget caching) so the lv_drawBlitGameTexture
         * fallback would just blit a stale logviewer frame on top of
         * a freshly-rendered game-view frame. Force the full update
         * path while game view is active. */
        if (g_lv->wantScreenUpdate == TRUE || g_lv->gameView) {
            BYTE savedSelf = 0;
            bool didSetSelf = FALSE;
            if (g_lv->gameView) {
                /* Set self to the spectated tank BEFORE lv_screenUpdate
                 * builds screenTanks. Tank sprite frame indices
                 * (TANK_SELF_* / GOOD_* / EVIL_*) are baked from
                 * lv_playersGetSelf at update time — without this, map
                 * sprite colours would lock to whatever self was when the
                 * log started and wouldn't follow Tab. */
                savedSelf = lv_playersGetSelf();
                lv_playersSetSelf(g_lv->cameraSlot);
                didSetSelf = TRUE;
            }
            lv_clientMutexWaitFor();
            lv_drawDirtyScreen();
            lv_screenUpdate(redraw);
            lv_clientMutexRelease();
            g_lv->wantScreenUpdate = FALSE;
            if (didSetSelf) {
                lv_playersSetSelf(savedSelf);
            }
        } else {
            lv_drawBlitGameTexture();
        }
    }

    /* Render ImGui UI */
    lv_imgui_context_newframe();
#ifdef __APPLE__
    /* Marshal in-window menu state into the native NSMenu once per
     * frame. Cheap walk over cached NSMenuItem pointers; checkmarks
     * and enable states mirror the ImGui menu's predicates. */
    {
        struct LvMenuState lvms;
        memset(&lvms, 0, sizeof(lvms));
        lvms.isLoaded         = g_lv->isLoaded ? true : false;
        lvms.playIsPlaying    = g_lv->playIsPlaying ? true : false;
        lvms.modeInformation  = lv_imgui_get_mode_information() ? true : false;
        lvms.useTeamColours   = g_lv->useTeamColours ? true : false;
        lvms.gameViewActive   = g_lv->gameView ? true : false;
        lvms.tankCentred      = lv_imgui_get_tank_centred() ? true : false;
        lvms.soundEffects     = g_lv->isSoundsPlaying ? true : false;
        lvms.soundVolume      = g_lv->soundVolume;
        lvms.dnsLookups       = lv_imgui_get_dns_lookups() ? true : false;
        lvms.showControls     = lv_g_show_controls_window;
        lvms.showEvents       = lv_g_show_events_window;
        lvms.showGameInfo     = lv_g_show_game_info_window;
        lvms.showItemInfo     = lv_g_show_item_info_window;
        lvms.showComments     = lv_g_show_comments_window;
        lvms.zoomStepIndex    = lv_drawGetZoomStepIndex();
        lvms.zoomStepCount    = lv_drawGetZoomStepCount();
        lvms.fromMainMenu     = g_lv->fromMainMenu ? true : false;
        lv_mac_menubar_refresh(&lvms);
    }
#endif
    if (g_lv->gameView) {
        lv_imgui_render_game_menu_bar(g_lv);
        lv_imgui_render_game_view(g_lv);
        /* Live spectator keeps the DVR scrubber reachable in game view; the
         * standalone viewer's game view shows no scrubber (never live mode). */
        if (lv_screenSpecIsLiveMode()) {
            lv_g_show_controls_window = true;
            lv_imgui_controls_window();
        }
        lv_g_reset_window_positions = false;
        if (overlay != NULL) {
            lv_imgui_center_message(overlay);
        }
        lv_imgui_dialogs_render();
        lv_imgui_context_render();
    } else if (lv_screenSpecIsLiveMode()) {
        /* Live spectator block-grid overview: DVR scrubber + live event feed +
         * game info (map name / type / settings, decoded from the seed). The
         * viewer's menu bar and comments panel stay gated off (see the function
         * banner). */
        lv_g_show_controls_window = true;
        lv_imgui_controls_window();
        lv_g_show_events_window = true;
        lv_imgui_events_window();
        lv_g_show_game_info_window = true;
        lv_imgui_game_info_window();
        lv_g_reset_window_positions = false;
        if (overlay != NULL) {
            lv_imgui_center_message(overlay);
        }
        lv_imgui_dialogs_render();
        lv_imgui_context_render();

        lv_imgui_game_viewport_process_input();
    } else {
        bool compact = (uiModeIsTablet() || uiModeIsSteamDeck());
        if (compact) {
            /* Start opens the popup menu — replaces the desktop top
             * menu bar in tablet/Deck compact mode. */
            lvMenuPollOpenInput();
            /* Each panel _window() early-outs on its lv_g_show_* flag
             * (desktop-side visibility prefs).  In compact mode there
             * is no menu to toggle those, so save/force/restore around
             * the calls — never persisting any change. */
            bool save_ctrl  = lv_g_show_controls_window;
            bool save_evt   = lv_g_show_events_window;
            bool save_gi    = lv_g_show_game_info_window;
            bool save_ii    = lv_g_show_item_info_window;
            bool save_cmt   = lv_g_show_comments_window;
            lv_g_show_controls_window  = true;
            lv_g_show_events_window    = (lvCompactGetActivePanel() == LV_PANEL_EVENTS);
            lv_g_show_game_info_window = (lvCompactGetActivePanel() == LV_PANEL_GAME_INFO);
            lv_g_show_item_info_window = (lvCompactGetActivePanel() == LV_PANEL_ITEM_INFO);
            lv_g_show_comments_window  = (lvCompactGetActivePanel() == LV_PANEL_COMMENTS);

            /* Toolbar (controls window) is always visible in compact;
             * it IS the toolbar (playback + speed + seek). */
            lv_imgui_controls_window();
            /* Single content panel, switched via the popup menu. */
            switch (lvCompactGetActivePanel()) {
                case LV_PANEL_GAME_INFO: lv_imgui_game_info_window(); break;
                case LV_PANEL_ITEM_INFO: lv_imgui_item_info_window(); break;
                case LV_PANEL_COMMENTS:  lv_imgui_comments_window();  break;
                case LV_PANEL_EVENTS:
                default:                 lv_imgui_events_window();    break;
            }

            lv_g_show_controls_window  = save_ctrl;
            lv_g_show_events_window    = save_evt;
            lv_g_show_game_info_window = save_gi;
            lv_g_show_item_info_window = save_ii;
            lv_g_show_comments_window  = save_cmt;
        } else {
            lv_imgui_main_menu_bar();
            lv_imgui_cache_menu_bar_height();
            lv_imgui_controls_window();
            lv_imgui_game_info_window();
            lv_imgui_events_window();
            lv_imgui_item_info_window();
            lv_imgui_comments_window();
        }
        lv_g_reset_window_positions = false;
        /* Popup stacks over panels, under blocking modal dialogs. */
        if (compact) {
            lvMenuRender(g_lv);
        }
        lv_imgui_dialogs_render();
        lv_imgui_context_render();

        lv_imgui_game_viewport_process_input();
    }

    SDL_RenderPresent(g_lv->renderer);
}

/* --------------------------------------------------------------------------
 * logViewerRun -- Core viewer loop.
 *
 * Takes ownership of the event loop until the user exits.
 * Window and renderer are borrowed, not owned.
 * -------------------------------------------------------------------------- */
void logViewerRun(SDL_Window *window, SDL_Renderer *renderer,
                  const char *logPath, bool fromMainMenu) {
    /* Bring up the decoder, platform layer, draw/ImGui/sound and preferences,
     * and size the screen to the window (shared with spectatorRun). */
    if (lvHostSetup(window, renderer, fromMainMenu) == FALSE) {
        return;
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
            /* Game-view input (grave toggle / Tab cycle / 1-4 zoom / arrow pan
             * / ,-. seek), shared with spectatorRun. The viewer allows the
             * toggle and the seek; the spectator host suppresses both. */
            if (lvHostHandleGameViewKey(sdlEvent, /* allowToggle */ TRUE,
                                        /* allowSeek */ TRUE)) {
                continue;
            }

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
                /* Game view drives a fixed window size (515*zf × 325*zf);
                 * leave it alone here. Phase E swaps in a real zoom
                 * selector — until then, manual resize is just disabled. */
                if (g_lv->gameView) {
                    continue;
                }
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
                 * in window coordinates (mouse_x / mouse_y). Suppressed in
                 * game-view mode — the trailer window size is fixed and
                 * mouse-wheel zoom would invalidate the saved teardown
                 * dimensions. */
                if (!lv_imgui_want_capture_mouse() && !g_lv->gameView) {
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

        /* Trackpad pinch-to-zoom (macOS) — anchor on current mouse position.
         * Suppressed in game-view mode; trailer window is fixed-size. */
        if (!g_lv->gameView) {
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

        /* Render the current mode (block-grid / game view) and present. */
        lvHostRenderFrame(NULL);
    }

    /* -----------------------------------------------------------------------
     * Shutdown
     * ----------------------------------------------------------------------- */
    /* If the user quit while game view was active, tear it down first so the
     * screen size restored to LogViewerState matches the pre-game-view value
     * — otherwise savePreferences would persist the game-view's 16×16 size
     * as the default for next launch. */
    if (g_lv->gameView) {
        lv_drawGameViewTeardown();
        g_lv->useTeamColours = g_lv->savedUseTeamColours;
        g_lv->gameView = FALSE;
    }
    savePreferences();
    lvHostTeardown();
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

/* --------------------------------------------------------------------------
 * spectatorRun -- Modal host for the live delayed spectator feed.
 *
 * Borrows the caller's window/renderer (never NULL here) and runs a self-driven
 * decoder loop: it pumps the spectator transport, drains the captured seed and
 * forward records through the decoder, forces the game view on and renders the
 * delayed feed each frame, then tears down. Unlike logViewerRun it never calls
 * lv_windowPlay — there is no log file and no background replay timer; this loop
 * advances the decoder itself from the drained records.
 *
 * cs is the bolo-world ClientSim as an opaque handle: the transport pump and the
 * seed/record drain go through the dependency-free spectator_drain.h seam so this
 * logviewer-world TU never includes client_sim.h (its screenObj conflicts with
 * backend.h). The ClientSim's lifetime is the caller's — spectatorRun does not
 * disconnect or free it.
 * -------------------------------------------------------------------------- */
void spectatorRun(SDL_Window *window, SDL_Renderer *renderer, void *cs) {
    if (lvHostSetup(window, renderer, /* fromMainMenu */ TRUE) == FALSE) {
        return;
    }

    /* Stalled-feed watchdog. A spectator sends no input, so the transport's
     * normal liveness timeout never fires — the host times the wait itself.
     * lastProgressMs is bumped on any seam progress (a fresh countdown value, a
     * drained record); kSpecStallMs of dead air surfaces a recoverable
     * "connection lost" the user can exit from. */
    const uint32_t kSpecStallMs = 8000;   /* ~8s with no feed progress */
    uint32_t lastProgressMs = SDL_GetTicks();
    uint32_t lastCountdown  = 0;
    bool     haveCountdown  = false;
    bool     connectionLost = false;

    /* Await the captured seed: pump the transport and service events until the
     * seed is ready or the user quits. Show a "spectating begins in N" overlay
     * once the cold-start countdown arrives, a "connecting" placeholder before
     * it, or "connection lost" if the feed stalls. When the feed stalls the seed
     * never arrives, so this loop keeps showing the message until the user
     * quits. */
    while (!g_lv->quit && !specDrainSeedReady(cs)) {
        SDL_Event sdlEvent;
        uint32_t remaining = 0;
        char overlay[64];

        specDrainPump(cs);
        while (SDL_PollEvent(&sdlEvent)) {
            lv_imgui_context_handle_event(&sdlEvent);
            /* Esc is the back affordance: leave the spectator view (including
             * a stalled "connection lost" overlay) and return to the caller. */
            if (sdlEvent.type == SDL_EVENT_QUIT ||
                (sdlEvent.type == SDL_EVENT_KEY_DOWN &&
                 sdlEvent.key.key == SDLK_ESCAPE)) {
                g_lv->quit = TRUE;
                break;
            }
        }

        /* A changed countdown value (or its first arrival) is feed progress. */
        if (specDrainCountdown(cs, &remaining) &&
            (!haveCountdown || remaining != lastCountdown)) {
            haveCountdown  = true;
            lastCountdown  = remaining;
            lastProgressMs = SDL_GetTicks();
        }
        if (!connectionLost && (SDL_GetTicks() - lastProgressMs) > kSpecStallMs) {
            connectionLost = true;
        }

        SDL_SetRenderDrawColor(g_lv->renderer, 0, 0, 0, 255);
        SDL_RenderClear(g_lv->renderer);
        lv_imgui_context_newframe();
        if (connectionLost) {
            lv_imgui_center_message("Connection lost");
        } else if (haveCountdown) {
            /* ~50 ticks/sec; round up so the last second shows "1", not "0". */
            uint32_t secs = (lastCountdown + 49) / 50;
            if (secs == 0) {
                lv_imgui_center_message("Spectating begins now");
            } else {
                snprintf(overlay, sizeof(overlay),
                         "Spectating begins in %u", (unsigned)secs);
                lv_imgui_center_message(overlay);
            }
        } else {
            lv_imgui_center_message("Connecting...");
        }
        lv_imgui_context_render();
        SDL_RenderPresent(g_lv->renderer);
    }

    /* Seed the decoder and open in the block-grid overview. The seed load is
     * guarded: a failed/absent seed leaves isLoaded FALSE and the steady loop
     * below is skipped — spectatorRun falls through to a clean teardown.
     * gameView stays FALSE; the grave key drives the on-demand game-view
     * setup/teardown once the spectator is running. */
    if (!g_lv->quit) {
        uint8_t *seed = NULL;
        uint32_t seedLen = 0;
        if (specDrainTakeSeed(cs, &seed, &seedLen)) {
            /* The spectator never receives the lobby-settings packet on the
             * wire, but the seed's control snapshot carries that same event;
             * decode it for the synthesized header's map name / game settings.
             * sgi outlives the load call below, which copies mapName into the
             * header. Fields the event doesn't carry stay zero (panel defaults). */
            SpecSeedInfo sgi;
            LvSpecSeedInfo info;
            const LvSpecSeedInfo *infoPtr = NULL;
            if (specSeedDecodeInfo(seed, seedLen, &sgi) && sgi.haveInfo) {
                memset(&info, 0, sizeof(info));
                info.mapName          = sgi.mapName;
                info.gameType         = sgi.gameType;
                info.allowHiddenMines = sgi.allowHiddenMines;
                info.ai               = sgi.ai;
                infoPtr = &info;
            }
            if (lv_specSeedLoad(infoPtr, seed, seedLen)) {
                g_lv->isLoaded = TRUE;
                lv_imgui_events_clear();
            }
            free(seed);
        }

        if (g_lv->isLoaded) {
            lv_drawDirtyScreen();
            g_lv->wantScreenUpdate = TRUE;
            /* A delivered seed means the feed is alive: clear any await-phase
             * stall and restart the watchdog for the record stream. */
            connectionLost = false;
            lastProgressMs = SDL_GetTicks();
            /* Enter live-DVR mode: the stream pump now appends only and this loop
             * drives the advance, so the scrubber can rewind the growing buffer. */
            lv_screenSpecSetLiveMode(TRUE);
        }
    }

    /* Steady loop: pump the transport, drain every record queued this frame into
     * the decoder, pump once more, then render the current mode. The grave key
     * toggles between the block-grid overview and the followed game view; the
     * loop runs off !quit so a mode toggle never exits it. The server sends ~one
     * record per delayed tick, so draining all queued records each frame needs
     * no host-side throttle. Entered only once a seed loaded; a stalled feed
     * overlays "connection lost" until exit. */
    while (!g_lv->quit && g_lv->isLoaded) {
        SDL_Event sdlEvent;
        SpecDrainRecord rec;
        bool drainedAny = false;

        specDrainPump(cs);
        while (SDL_PollEvent(&sdlEvent)) {
            /* Reuse the viewer's game-view input. The grave key toggles
             * between the block-grid overview and the game view (allowToggle),
             * and ,-. seek parks the live feed like the scrubber (allowSeek). */
            if (lvHostHandleGameViewKey(sdlEvent, /* allowToggle */ TRUE,
                                        /* allowSeek */ TRUE)) {
                continue;
            }
            /* End / L jumps back to the live head and resumes following it. */
            if (sdlEvent.type == SDL_EVENT_KEY_DOWN &&
                (sdlEvent.key.key == SDLK_END || sdlEvent.key.key == SDLK_L)) {
                lv_screenSpecJumpToLive();
                continue;
            }
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
            /* Mouse-wheel zoom for the block-grid overview, anchored on the
             * cursor — the spectator's menu-bar zoom path is gated off, so the
             * wheel is the overview's only zoom. Skipped while ImGui owns the
             * mouse (cursor over the scrubber) and in game view (fixed size;
             * 1-4 keys zoom there). Mirrors logViewerRun's wheel handling. */
            if (sdlEvent.type == SDL_EVENT_MOUSE_WHEEL) {
                if (!lv_imgui_want_capture_mouse() && !g_lv->gameView) {
                    int mx = (int)sdlEvent.wheel.mouse_x;
                    int my = (int)sdlEvent.wheel.mouse_y;
                    if (sdlEvent.wheel.y > 0.0f) {
                        lv_drawZoomIn(mx, my);
                    } else if (sdlEvent.wheel.y < 0.0f) {
                        lv_drawZoomOut(mx, my);
                    }
                }
            }
            /* Esc exits the live feed back to the caller — the game-view key
             * handler above leaves it unconsumed, so it falls through here. */
            if (sdlEvent.type == SDL_EVENT_QUIT ||
                (sdlEvent.type == SDL_EVENT_KEY_DOWN &&
                 sdlEvent.key.key == SDLK_ESCAPE)) {
                g_lv->quit = TRUE;
                break;
            }
        }

        /* Drain all records queued this frame. In live-DVR mode lv_specRecordPump
         * only appends (the stream pump no longer auto-advances); the frame update
         * below drives the decoder forward per follow-live / parked mode. Track the
         * latest record tick so the parked head-time can extrapolate. */
        while (specDrainPopRecord(cs, &rec)) {
            lv_specRecordPump(rec.isKeyframe, rec.payload, rec.payloadLen);
            lv_screenSpecNoteHeadTick(rec.gameTick);
            if (rec.payload != NULL) {
                free(rec.payload);
            }
            drainedAny = true;
        }
        specDrainPump(cs);

        if (drainedAny) {
            lastProgressMs = SDL_GetTicks();
        }
        if (!connectionLost && (SDL_GetTicks() - lastProgressMs) > kSpecStallMs) {
            connectionLost = true;
        }

        /* Advance the decoder: follow-live slams to the head, parked plays forward
         * at real time from the scrub point; pause (playIsPlaying) freezes either.
         * Keeps g_lv->totalTimeMs pointed at the growing live head for the scrubber. */
        lv_screenSpecFrameUpdate(SDL_GetTicks());

        g_lv->wantScreenUpdate = TRUE;

        /* Render the current mode via the shared dispatch. In game view it
         * keeps the DVR scrubber; in the block-grid overview it renders the
         * scrubber + live event feed (the live-mode gate suppresses the
         * viewer's menu bar and game-info / comments panels). A stalled feed
         * overlays "connection lost" on the frozen last frame until exit. */
        lvHostRenderFrame(connectionLost ? "Connection lost" : NULL);
    }

    /* Leave live-DVR mode so the stream pump / transport revert to standalone
     * behaviour for any later log session in this process. */
    lv_screenSpecSetLiveMode(FALSE);

    /* Teardown: if the spectator exited while in the game view, restore its
     * window size/tile counts and team-colour state (the grave toggle set them
     * on entry), run the shared shutdown, and drop the seed control stash. The
     * ClientSim/transport belongs to the caller — do not disconnect it here. */
    if (g_lv->gameView) {
        lv_drawGameViewTeardown();
        g_lv->useTeamColours = g_lv->savedUseTeamColours;
        g_lv->gameView = FALSE;
    }
    lvHostTeardown();
    lv_specSeedControlClear();
}
