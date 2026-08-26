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
#include "spectator_input.h"   /* dep-free seam: read the controller for pan/zoom */
#include "lv_global.h"
#include "clientmutex.h"
#include "draw.h"
#include "sound.h"
#include "dns.h"
#include "positions.h"
#include "tiles.h"
#include "logviewer.h"
#include "lv_host.h"
#include "lv_stats.h"

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
#include "../gui/lang.h"
#ifdef __APPLE__
#include "platform/mac_menubar.h"
#endif

/* Version string referenced by imgui_dialogs.cpp */
const char *lv_g_version_string = "1.01";

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

void lv_windowAddHighlight(char *msg, uint32_t seekMs, int mapX, int mapY) {
    lv_imgui_events_add_highlight(msg, seekMs, mapX, mapY);
}

void lv_windowAddSummary(char *msg) {
    lv_imgui_events_add_summary(msg);
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
 * Playback control
 * -------------------------------------------------------------------------- */
/* Jump the scrubber to a highlight moment and centre the view on its cell.
 * The seek walks the log and rebuilds the world, the same state lv_windowTimer
 * ticks on the SDL timer thread under lv_clientMutex, so playback is stopped
 * and the lock held across it exactly as the scrubber and keyboard step do.
 * Without that, a click during playback lands lv_shellsAddItem and
 * lv_shellsDestroy on the same list from two threads and corrupts the heap. */
void lv_windowSeekToHighlight(uint32_t ms, int mapX, int mapY) {
    unsigned char wasPlaying = g_lv->playIsPlaying;
    if (wasPlaying) {
        lv_windowPause();
    }
    lv_clientMutexWaitFor();
    lv_drawDirtyScreen();
    lv_screenSeekToTimeMs(ms);
    lv_screenCentreOnCell(mapX, mapY);
    lv_drawDirtyScreen();
    lv_clientMutexRelease();
    lv_imgui_game_view_init_camera(g_lv);
    lv_windowNeedRedraw();
    if (wasPlaying) {
        lv_windowPlay();
    }
    g_lv->wantScreenUpdate = TRUE;
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
 * Start-of-log callback from backend
 * -------------------------------------------------------------------------- */
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

    /* A live spectator must not tear down the delayed feed by opening another
       log. On macOS the native menu still dispatches File > Open even though the
       in-window menu is suppressed in live mode, so guard at the action itself.
       Standalone playback and the command-line open are never in live mode. */
    if (lv_screenSpecIsLiveMode()) {
        return;
    }

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
            lvStatsEmitRoundSummary();
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

    /* The live spectator never saves the delayed feed to a .wbv. The native
       macOS menu can still fire File > Save Map, so no-op in live mode (it would
       otherwise freeze the feed via the pause below). Standalone is unaffected. */
    if (lv_screenSpecIsLiveMode()) {
        return;
    }

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
 * The ImGui-free half (lvHostSetupCore) plus the viewer's own ImGui context and
 * panels, sound, preferences, and the screen sizing that fills the window.
 * Returns TRUE on success; on any failure it unwinds whatever it brought up and
 * returns FALSE with g_lv cleared.
 * -------------------------------------------------------------------------- */
static int lvHostSetup(SDL_Window *window, SDL_Renderer *renderer,
                       bool fromMainMenu) {
    if (lvHostSetupCore(window, renderer, fromMainMenu) == FALSE) {
        return FALSE;
    }

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
 * active game view and saves preferences first.
 * -------------------------------------------------------------------------- */
static void lvHostTeardown(void) {
    lvHostTeardownCommon(true);
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
 * lvHostHandleResize -- Shared SDL_EVENT_WINDOW_RESIZED handler.
 *
 * Snaps the window to the nearest zoom-effective tile boundary, updates the
 * screen tile counts and clamps the scroll offset, then recreates the
 * render-target texture (without which mouse-to-tile coordinates drift) and
 * marks the screen dirty. Game view drives a fixed window size, so a resize
 * there is ignored. Shared by logViewerRun and spectatorRun so the live
 * spectator reflows its overview exactly like the standalone viewer.
 * -------------------------------------------------------------------------- */
static void lvHostHandleResize(const SDL_Event *e) {
    if (g_lv->gameView) {
        return;
    }
    int w = e->window.data1;
    int h = e->window.data2;
    int menuH = (int)lv_imgui_get_menu_bar_height();
    float zoom = lv_drawGetZoomLevel();
    if (zoom <= 0.0f) zoom = 1.0f;

    /* Snap to the nearest zoom-effective tile boundary so the blit fills the
     * window cleanly at the current zoom. */
    float tilePxX = (float)TILE_SIZE_X * zoom;
    float tilePxY = (float)TILE_SIZE_Y * zoom;
    int newTilesX = (int)(((float)w + tilePxX * 0.5f) / tilePxX);
    int newTilesY = (int)((((float)(h - menuH)) + tilePxY * 0.5f) / tilePxY);
    if (newTilesX < 1) newTilesX = 1;
    if (newTilesY < 1) newTilesY = 1;
    if (newTilesX > 255) newTilesX = 255;
    if (newTilesY > 255) newTilesY = 255;

    /* A maximized or fullscreen window can't be resized: SDL_SetWindowSize is
     * a no-op there, so snapping would leave the window at its full size while
     * the tile counts and render target below shrink to the snapped size. That
     * window/render-target/tile mismatch desyncs the cursor-anchored zoom
     * (lv_drawApplyZoomStep clamps the anchor against the render target), which
     * is why wheel/controller zoom wedges once the window is maximized. In that
     * state, fit the tile counts to the actual window (the event's data1/data2
     * are the real maximized dimensions) and leave the window alone. A normal
     * windowed resize still snaps to a clean tile boundary. */
    SDL_WindowFlags wflags = SDL_GetWindowFlags(g_lv->window);
    if (!(wflags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) {
        int snappedW = (int)(newTilesX * tilePxX + 0.5f);
        int snappedH = (int)(newTilesY * tilePxY + 0.5f);
        SDL_SetWindowSize(g_lv->window, snappedW, snappedH + menuH);
    }
    lv_screenSetSizeX((BYTE)newTilesX);
    lv_screenSetSizeY((BYTE)newTilesY);
    /* Clamp scroll offset so the viewport stays within the 255x255 map, and
     * reset sub-pixel pan so the resized viewport snaps cleanly to tile
     * boundaries — there's no in-flight drag state to preserve across a
     * window resize. */
    if (g_lv->isLoaded) {
        BYTE ox, oy;
        lv_screenGetOffsets(&ox, &oy);
        if ((int)ox + newTilesX > 255) ox = (BYTE)(255 - newTilesX);
        if ((int)oy + newTilesY > 255) oy = (BYTE)(255 - newTilesY);
        lv_screenSetOffset(ox, oy);
        lv_screenSetSubOffset(0, 0);
    }
    lv_drawResizeRenderTarget();
    lv_drawDirtyScreen();
    g_lv->wantScreenUpdate = TRUE;
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
            /* Overview repaint is incremental: the per-tile dirty cache
             * (lv_drawLast) combined with each sprite's lv_drawMarkRedraw
             * (tanks/shells/LGMs/labels) erases moving overlays, so only
             * changed tiles repaint into the persistent render target.
             * Forcing a full-screen dirty every frame instead repaints every
             * visible tile — at a zoomed-out 255x255 viewport (e.g. fullscreen
             * on a 4K display) that is ~65k tile blits per frame and stalls the
             * render. Pan/zoom/resize already dirty the whole screen on demand.
             * Game view renders directly (no render-target cache) and needs the
             * full update each frame. */
            if (g_lv->gameView) {
                lv_drawDirtyScreen();
            }
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
        lvms.hideLobby        = lv_screenGetHideLobby() ? true : false;
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
            lv_imgui_spectator_badge(g_lv->gamePhase);
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
        lv_imgui_spectator_badge(g_lv->gamePhase);
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
            lvStatsEmitRoundSummary();
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
                lvHostHandleResize(&sdlEvent);
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

/* Apply a decoded seed/keyframe's lobby roster to the viewer's player table:
 * name every present lobby slot (silently, via the quiet setter — no newswire
 * spam) and adopt the snapshot's phase. The quiet setter touches only inUse +
 * playerName, never a tank position, so a positionless lobby-only slot stays
 * suppressed by the (0,0,0,0) guard in lv_playersMakeScreenTanks and a slot
 * holding a real forward-established tank keeps its position (only its name is
 * re-set to the same value). Safe to call on every keyframe. */
static void lvSpecApplyRoster(const SpecSeedInfo *sgi) {
    BYTE i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (sgi->lobbyPresent[i]) {
            lv_playersSetPlayerNameQuiet((BYTE)i, sgi->lobbyName[i]);
        }
    }
    g_lv->gamePhase = sgi->specPhase;
}

/* Set the borrowed window's title for the live session: host:port, the map name
 * decoded from the current seed, and a phase tag ([Lobby] / [Live] / [Game Over],
 * omitted for an unknown phase). Re-applied on every (re)seed so the map name and
 * phase refresh when the round or phase changes. The caller restores the app
 * title on return. */
static void lvSpecApplyTitle(const char *serverHost, uint16_t serverPort) {
    char specTitle[176];
    char phaseTag[64];
    /* Localized phase tag: the bracketed badge wraps the already-localized
       phase string (UNKNOWN keeps an empty tag). */
    switch (g_lv->gamePhase) {
        case SPEC_PHASE_LOBBY:
        case SPEC_PHASE_COUNTDOWN:
            snprintf(phaseTag, sizeof(phaseTag), " [%s]",
                     langGetText(STR_LV_SPEC_PHASE_LOBBY));
            break;
        case SPEC_PHASE_RUNNING:
            snprintf(phaseTag, sizeof(phaseTag), " [%s]",
                     langGetText(STR_LV_SPEC_PHASE_LIVE));
            break;
        case SPEC_PHASE_GAMEOVER:
            snprintf(phaseTag, sizeof(phaseTag), " [%s]",
                     langGetText(STR_LV_SPEC_PHASE_GAMEOVER));
            break;
        default:
            phaseTag[0] = '\0';
            break;
    }
    if (g_lv->mapName[0] != '\0') {
        /* phaseTag is the localized " [Lobby]"/" [Live]"/... tag carried in
           {string3}; the host and map flow through {string1}/{string2}. */
        MessageArgs args = {0};
        snprintf(args.string1, sizeof(args.string1), "%s", serverHost ? serverHost : "");
        args.number = (int)serverPort;
        snprintf(args.string2, sizeof(args.string2), "%s", g_lv->mapName);
        snprintf(args.string3, sizeof(args.string3), "%s", phaseTag);
        snprintf(specTitle, sizeof(specTitle), "%s",
                 langGetTextFmt(STR_LV_SPEC_WINDOW_TITLE_FMT, &args));
    } else {
        MessageArgs args = {0};
        snprintf(args.string1, sizeof(args.string1), "%s", serverHost ? serverHost : "");
        args.number = (int)serverPort;
        snprintf(args.string3, sizeof(args.string3), "%s", phaseTag);
        snprintf(specTitle, sizeof(specTitle), "%s",
                 langGetTextFmt(STR_LV_SPEC_WINDOW_TITLE_NOMAP_FMT, &args));
    }
    SDL_SetWindowTitle(g_lv->window, specTitle);
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
bool spectatorRun(SDL_Window *window, SDL_Renderer *renderer, void *cs,
                  const char *serverHost, uint16_t serverPort) {
    /* True when the loop exits because the server put the spectator back into
     * live-lobby mode (its delayed game drained to the lobby); false when the
     * user left or the feed never loaded. The caller's dual-mode loop re-enters
     * the live lobby on true and exits on false. */
    bool liveResumed = false;

    if (lvHostSetup(window, renderer, /* fromMainMenu */ TRUE) == FALSE) {
        return false;
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
            lv_imgui_center_message(langGetText(STR_LV_SPEC_CONNECTION_LOST));
        } else if (haveCountdown) {
            /* ~50 ticks/sec; round up so the last second shows "1", not "0". */
            uint32_t secs = (lastCountdown + 49) / 50;
            if (secs == 0) {
                lv_imgui_center_message(langGetText(STR_LV_SPEC_BEGINS_NOW));
            } else {
                MessageArgs args = {0};
                args.number = (int)secs;
                snprintf(overlay, sizeof(overlay), "%s",
                         langGetTextFmt(STR_LV_SPEC_BEGINS_IN_FMT, &args));
                lv_imgui_center_message(overlay);
            }
        } else {
            lv_imgui_center_message(langGetText(STR_LV_SPEC_CONNECTING));
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
            /* Phase is populated by the decode walk regardless of haveInfo
               (specSeedDecodeInfo zeroes sgi first), so read it unconditionally. */
            g_lv->gamePhase = sgi.specPhase;
            if (lv_specSeedLoad(infoPtr, seed, seedLen)) {
                g_lv->isLoaded = TRUE;
                lv_imgui_events_clear();
                /* Resolve lobby-chat sender names for players who hold no tank
                   (and so never appear in the world snapshot): inject the names
                   the seed's lobby-slot roster carried into the viewer roster.
                   sgi was zeroed by specSeedDecodeInfo, so absent slots are
                   simply skipped. Silent (quiet setter) — no newswire spam. */
                lvSpecApplyRoster(&sgi);
            }
            free(seed);
        }

        if (g_lv->isLoaded) {
            lvSpecApplyTitle(serverHost, serverPort);

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

        /* The server re-subscribed this spectator to the live lobby control bus
         * (its delayed game drained back across the game→lobby boundary), so
         * live lobby control has begun arriving — return to the caller, which
         * re-enters the live read-only lobby against the now-live ClientSim.
         * This is distinct from the in-stream new-game keyframe handled below,
         * which keeps playing delayed and must not return. */
        if (specDrainLiveResumed(cs)) {
            liveResumed = true;
            break;
        }

        /* Controller input is gated on the leave-confirm modal. While it is
         * open ImGui owns the gamepad (A confirms / B cancels the prompt), so
         * map pan/zoom and our own B-to-leave stand down. Drive the gamepad-nav
         * flag the same way — off in the pannable overview so the stick/d-pad
         * pan instead of moving ImGui focus, on for the modal and the game view
         * — and set it before lvHostRenderFrame's newframe reads it. */
        bool specPopupOpen = lv_imgui_any_popup_open() != 0;
        lv_imgui_set_gamepad_nav((specPopupOpen || g_lv->gameView) ? 1 : 0);

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
            /* Snap + reflow the overview on a drag-resize/maximize, matching
             * the standalone viewer (game view is fixed-size and ignored). */
            if (sdlEvent.type == SDL_EVENT_WINDOW_RESIZED) {
                lvHostHandleResize(&sdlEvent);
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
            /* Window-X / OS quit leaves immediately — never trap the OS close.
             * Esc instead arms the "Leave spectating?" confirm modal (rendered
             * by lvHostRenderFrame in both overview and game view); the future
             * controller B-button arms it through the same entry point. The
             * game-view key handler above leaves Esc unconsumed, so it falls
             * through here. */
            if (sdlEvent.type == SDL_EVENT_QUIT) {
                g_lv->quit = TRUE;
                break;
            }
            if (sdlEvent.type == SDL_EVENT_KEY_DOWN &&
                sdlEvent.key.key == SDLK_ESCAPE) {
                lv_imgui_spectator_leave_request();
            }
            /* Controller B (East) mirrors Esc: arm the "Leave spectating?"
             * confirm. Suppressed while the modal is open so ImGui's own
             * East = cancel resolves it rather than this re-arming it. */
            if (!specPopupOpen &&
                sdlEvent.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                sdlEvent.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
                lv_imgui_spectator_leave_request();
            }
        }

        /* Controller pan/zoom for the block-grid overview (game view is
         * fixed-size and ignored; the modal owns input while open). Pan mirrors
         * the mouse-drag conversion — screen-pixel deltas map to 1/zoom zoom-1
         * native pixels — but adds the stick delta to the absolute pan (stick
         * right/down moves the view right/down), the inverse of grab-drag,
         * which subtracts. Zoom is centered on the window, reusing the same
         * lv_drawZoomIn/Out the mouse wheel drives. */
        if (!g_lv->gameView && !specPopupOpen) {
            float panX = 0.0f, panY = 0.0f;
            int   zoomDir = 0;
            if (specInputPollPan(&panX, &panY)) {
                float zoom = lv_drawGetZoomLevel();
                if (zoom <= 0.0f) zoom = 1.0f;
                /* Per-frame pan reach in screen pixels at full deflection. */
                const float kPanSpeedPx = 12.0f;
                unsigned char ox = 0, oy = 0;
                int sx = 0, sy = 0;
                lv_screenGetOffsets(&ox, &oy);
                lv_screenGetSubOffset(&sx, &sy);
                int curX = (int)ox * TILE_SIZE_X + sx;
                int curY = (int)oy * TILE_SIZE_Y + sy;
                int newX = curX + (int)(panX * kPanSpeedPx / zoom);
                int newY = curY + (int)(panY * kPanSpeedPx / zoom);
                lv_drawDirtyScreen();
                lv_screenPanToTotalPixels(newX, newY);
            }
            if (specInputPollZoom(&zoomDir)) {
                int w = 0, h = 0;
                SDL_GetWindowSize(g_lv->window, &w, &h);
                if (zoomDir > 0) {
                    lv_drawZoomIn(w / 2, h / 2);
                } else {
                    lv_drawZoomOut(w / 2, h / 2);
                }
            }
        }

        /* The confirm modal resolves on a click: Yes leaves the feed, No keeps
         * watching (the modal closed itself). */
        if (lv_imgui_spectator_leave_confirmed()) {
            g_lv->quit = TRUE;
        }

        /* Drain all records queued this frame. In live-DVR mode lv_specRecordPump
         * only appends (the stream pump no longer auto-advances); the frame update
         * below drives the decoder forward per follow-live / parked mode. Track the
         * latest record tick so the parked head-time can extrapolate. */
        while (specDrainPopRecord(cs, &rec)) {
            /* A world reset (new lobby/map) regresses the game tick below the
               tracked head and arrives as a forced keyframe. Reset the DVR to the
               new segment — drop the old map's scroll-back range and restart
               head-time tracking — then re-seed from this keyframe so the decoder
               re-syncs on a fresh buffer, exactly as on the initial seed. The
               re-seed rebuilds the live buffer + decoder; lv_screenSpecResetSegment
               clears the seek index and DVR state the re-seed leaves alone. */
            if (rec.isKeyframe && lv_screenSpecIsLiveMode() &&
                lv_screenSpecHeadTick() > 0 &&
                rec.gameTick < lv_screenSpecHeadTick()) {
                SpecSeedInfo sgi;
                LvSpecSeedInfo info;
                const LvSpecSeedInfo *infoPtr = NULL;
                lv_screenSpecResetSegment();
                /* The new segment's keyframe carries its own lobby-settings
                   control slice, so recover the new map name / settings from it
                   the same way the initial seed does. */
                if (specSeedDecodeInfo(rec.payload, rec.payloadLen, &sgi) &&
                    sgi.haveInfo) {
                    memset(&info, 0, sizeof(info));
                    info.mapName          = sgi.mapName;
                    info.gameType         = sgi.gameType;
                    info.allowHiddenMines = sgi.allowHiddenMines;
                    info.ai               = sgi.ai;
                    infoPtr = &info;
                }
                /* New segment's phase (populated regardless of haveInfo). */
                g_lv->gamePhase = sgi.specPhase;
                if (!lv_specSeedLoad(infoPtr, rec.payload, rec.payloadLen)) {
                    /* A failed re-seed left no decoder; exit to a clean teardown
                       rather than render against a torn-down buffer. */
                    g_lv->isLoaded = FALSE;
                } else {
                    /* Match the initial-seed path: drop the previous segment's
                       cached overview tiles and stale event feed so a lobby map
                       change fully redraws to the new map instead of lingering
                       on the old one. */
                    lv_drawDirtyScreen();
                    g_lv->wantScreenUpdate = TRUE;
                    lv_imgui_events_clear();
                    /* Refresh lobby-chat sender names + phase from the new
                       segment's roster (same silent injection as the initial
                       seed). */
                    lvSpecApplyRoster(&sgi);
                    /* Refresh title + map + phase tag for the new segment. */
                    lvSpecApplyTitle(serverHost, serverPort);
                }
            } else {
                /* A mid-segment keyframe still carries the current control
                   snapshot, so refresh the lobby roster + phase from it: a
                   player who joins the lobby while we are already spectating is
                   named within a keyframe interval instead of waiting for the
                   next segment re-seed. Silent (quiet setter) — no event spam.
                   Decode fills roster/phase regardless of return value
                   (specSeedDecodeInfo zeroes kf first); a non-keyframe forward
                   record carries no snapshot and is pumped unchanged. */
                if (rec.isKeyframe) {
                    SpecSeedInfo kf;
                    int prevPhase = g_lv->gamePhase;
                    specSeedDecodeInfo(rec.payload, rec.payloadLen, &kf);
                    lvSpecApplyRoster(&kf);
                    /* Only retitle when the phase actually changed, so we don't
                       SDL_SetWindowTitle on every keyframe. */
                    if (g_lv->gamePhase != prevPhase) {
                        lvSpecApplyTitle(serverHost, serverPort);
                    }
                }
                lv_specRecordPump(rec.isKeyframe, rec.payload, rec.payloadLen);
            }
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
        lvHostRenderFrame(connectionLost ? langGetText(STR_LV_SPEC_CONNECTION_LOST) : NULL);
    }

    /* Leave live-DVR mode so the stream pump / transport revert to standalone
     * behaviour for any later log session in this process. */
    lv_screenSpecSetLiveMode(FALSE);

    /* Restore the default gamepad UI nav the overview disabled while panning,
     * so any later log session in this process keeps controller focus nav. */
    lv_imgui_set_gamepad_nav(1);

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
    return liveResumed;
}
