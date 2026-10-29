/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 * Name:          lv_host_dock.c
 * Purpose:
 *   The map beside the sidebars, and the first view of a
 *   log. Moved out of logviewer.c so that the web viewer
 *   (wasm/main_wasm.c), which has a main loop of its own,
 *   runs the same code as the desktop viewer and the live
 *   spectator. See lv_host_dock.h.
 *********************************************************/

#include <stdio.h>
#include "backend.h"
#include "lv_global.h"
#include "clientmutex.h"
#include "draw.h"
#include "tiles.h"
#include "logviewer.h"
#include "lv_host.h"
#include "lv_host_dock.h"
#include "lv_dock_fit.h"
#include "lv_dock_clear.h"
#include "lv_first_view.h"   /* where the map is when a log is first shown */

#include <SDL3/SDL.h>

#include "imgui/imgui_main_menu.h"   /* lv_imgui_get_menu_bar_height */
#include "imgui/imgui_dock.h"

/* What the sidebars took the last time the map's tile counts were fitted
 * beside them. lvHostDockSync refits when their answer differs from this. */
static LvDockMargins s_lvDockApplied = { 0, 0, 0, 0 };
/* What the sidebars took of the window when the map was last fitted, clear
 * ones too. The same now with s_lvDockApplied different says a sidebar's
 * option "Make unused sidebar portions transparent" was turned on or off. */
static LvDockMargins s_lvDockFull = { 0, 0, 0, 0 };

void lvHostDockReset(void) {
    LvDockMargins none = { 0, 0, 0, 0 };
    s_lvDockApplied = none;
    s_lvDockFull = none;
}

void lvHostDockNoteFitted(const LvDockMargins *dock) {
    s_lvDockApplied = *dock;
    lv_imgui_dock_margins(&s_lvDockFull.left, &s_lvDockFull.top, &s_lvDockFull.right,
                          &s_lvDockFull.bottom);
}

/* The part of the map's view no solid sidebar hides, each way: where it
 * starts, in points from the view's own start on the screen, and how long
 * it is. It is the window less the menu bar and the solid sidebars; a
 * sidebar whose unused part is clear counts as map. When the middle of that
 * is under a panel of a clear sidebar, the answer is the part no sidebar is
 * on at all, clear or solid. */
void lvHostVisibleMap(float *startX, float *startY, float *lenX, float *lenY) {
    LvDockMargins open = { 0, 0, 0, 0 };
    LvDockMargins view = { 0, 0, 0, 0 };
    int winW = 0, winH = 0;
    int menuH = (int)lv_imgui_get_menu_bar_height();

    SDL_GetWindowSize(g_lv->window, &winW, &winH);
    /* Where the view starts on the screen. */
    lv_imgui_dock_map_margins(&view.left, &view.top, &view.right, &view.bottom);
    lv_imgui_dock_open_margins(&open.left, &open.top, &open.right, &open.bottom);
    if (!lv_imgui_dock_point_on_map(
            (float)open.left + (float)(winW - open.left - open.right) * 0.5f,
            (float)(menuH + open.top) + (float)(winH - menuH - open.top - open.bottom) * 0.5f)) {
        lv_imgui_dock_margins(&open.left, &open.top, &open.right, &open.bottom);
    }
    *startX = (float)(open.left - view.left);
    *startY = (float)(open.top - view.top);
    *lenX = (float)(winW - open.left - open.right);
    *lenY = (float)(winH - menuH - open.top - open.bottom);
    if (*startX < 0.0f) *startX = 0.0f;
    if (*startY < 0.0f) *startY = 0.0f;
    if (*lenX < 1.0f) *lenX = 1.0f;
    if (*lenY < 1.0f) *lenY = 1.0f;
}

/* The part of the window the map shows in, in window pixels: below the menu
 * bar and inside the sidebars that are open. The spectator's badge and its
 * "connection lost" message go here, so no sidebar is over them. */
void lvHostMapArea(float *x, float *y, float *w, float *h) {
    LvDockMargins open = { 0, 0, 0, 0 };
    int winW = 0, winH = 0;
    int menuH = (int)lv_imgui_get_menu_bar_height();

    SDL_GetWindowSize(g_lv->window, &winW, &winH);
    lv_imgui_dock_open_margins(&open.left, &open.top, &open.right, &open.bottom);
    *x = (float)open.left;
    *y = (float)(menuH + open.top);
    *w = (float)(winW - open.left - open.right);
    *h = (float)(winH - menuH - open.top - open.bottom);
    if (*w < 1.0f) *w = 1.0f;
    if (*h < 1.0f) *h = 1.0f;
}

/* --------------------------------------------------------------------------
 * lvHostDockSync -- Keep the map's size in step with the sidebars.
 *
 * Runs first thing each frame, before the map is drawn. It lets each sidebar
 * come in or go out for the frame, then, when what the sidebars take has
 * changed (one turned on or off, or its edge dragged), refits the number of
 * map squares across and down to the rest of the window. Everything that
 * places the map -- the centring, the pan limits, centre-on-click, the
 * mouse-to-square sum -- works from those numbers, so nothing of the map is
 * lost behind a sidebar.
 *
 * The window itself is left alone here (the user did not resize it), so the
 * fit rounds up: the last square may run under a sidebar's edge by less than
 * a square. The live spectator and the web viewer have the same sidebars as
 * the viewer, so this runs for all three.
 *
 * A sidebar whose unused part is clear takes nothing from the map: the map
 * is fitted as if that sidebar were not there and is drawn under it. How
 * much of the view is under one is told to the screen each frame
 * (lv_screenSetViewInset), so the middle of the view stays the middle of
 * the map that shows. When the option is turned on or off the map's corner
 * moves on the screen; the scroll position is moved the other way by as
 * much, so the map itself stays where it is.
 *
 * Game view sets its own fixed size and puts the overview's
 * back when it ends; it is left alone, and the comparison catches up
 * afterwards.
 *
 * On a live feed the timer thread decodes into the same screen, so the
 * view inset and the grid size are changed under the client mutex. The
 * lock is held only for these few calls, and most frames change nothing.
 * -------------------------------------------------------------------------- */
static void lvHostDockSyncLocked(void);

void lvHostDockSync(void) {
    lv_imgui_dock_pre_frame();
    lv_clientMutexWaitFor();
    lvHostDockSyncLocked();
    lv_clientMutexRelease();
}

static void lvHostDockSyncLocked(void) {
    LvDockMargins dock = { 0, 0, 0, 0 };
    LvDockMargins full = { 0, 0, 0, 0 };
    LvDockMargins was;
    int winW = 0, winH = 0, menuH;
    bool acrossChanged, downChanged, resized = FALSE, optionOnly;
    BYTE oldOffX = 0, oldOffY = 0;
    int oldSubX = 0, oldSubY = 0;
    float zoom;

    if (g_lv->gameView) {
        lv_screenSetViewInset(0, 0, 0, 0);
        return;
    }
    lv_imgui_dock_margins(&full.left, &full.top, &full.right, &full.bottom);
    lv_imgui_dock_map_margins(&dock.left, &dock.top, &dock.right, &dock.bottom);
    zoom = lv_drawGetZoomLevel();
    if (zoom <= 0.0f) zoom = 1.0f;
    lv_screenSetViewInset((int)((float)(full.left - dock.left) / zoom + 0.5f),
                          (int)((float)(full.top - dock.top) / zoom + 0.5f),
                          (int)((float)(full.right - dock.right) / zoom + 0.5f),
                          (int)((float)(full.bottom - dock.bottom) / zoom + 0.5f));
    optionOnly = full.left == s_lvDockFull.left && full.top == s_lvDockFull.top &&
                 full.right == s_lvDockFull.right && full.bottom == s_lvDockFull.bottom;
    s_lvDockFull = full;
    if (dock.left == s_lvDockApplied.left && dock.top == s_lvDockApplied.top &&
        dock.right == s_lvDockApplied.right && dock.bottom == s_lvDockApplied.bottom) {
        return;
    }
    was = s_lvDockApplied;
    if (g_lv->logLoaded) {
        lv_screenGetOffsets(&oldOffX, &oldOffY);
        lv_screenGetSubOffset(&oldSubX, &oldSubY);
    }
    /* Each way is refitted only when its own sidebars changed, so a viewer
     * with none is never touched here, as it never was. */
    acrossChanged = (dock.left + dock.right) !=
                    (s_lvDockApplied.left + s_lvDockApplied.right);
    downChanged = (dock.top + dock.bottom) !=
                  (s_lvDockApplied.top + s_lvDockApplied.bottom);
    s_lvDockApplied = dock;

    SDL_GetWindowSize(g_lv->window, &winW, &winH);
    if (winW <= 0 || winH <= 0) {
        return;
    }
    menuH = (int)lv_imgui_get_menu_bar_height();
    if (acrossChanged) {
        int newTilesX = lvFitTilesBesideDock(winW, dock.left + dock.right,
                                             (float)TILE_SIZE_X * zoom);
        if ((BYTE)newTilesX != lv_screenGetSizeX()) {
            lv_screenSetSizeX((BYTE)newTilesX);
            resized = TRUE;
        }
    }
    if (downChanged) {
        int newTilesY = lvFitTilesBesideDock(winH - menuH, dock.top + dock.bottom,
                                             (float)TILE_SIZE_Y * zoom);
        if ((BYTE)newTilesY != lv_screenGetSizeY()) {
            lv_screenSetSizeY((BYTE)newTilesY);
            resized = TRUE;
        }
    }
    if (resized) {
        lv_screenSetSubOffset(0, 0);
        lv_drawResizeRenderTarget();
    }
    if (optionOnly && g_lv->logLoaded) {
        /* Only the option changed: the map's corner moved on the screen, so
         * the scroll position goes the other way and the map stays put. */
        lv_screenPanToTotalPixels(
            (int)oldOffX * TILE_SIZE_X + oldSubX +
                lvDockClearScrollShift(was.left, dock.left, zoom),
            (int)oldOffY * TILE_SIZE_Y + oldSubY +
                lvDockClearScrollShift(was.top, dock.top, zoom));
    }
    /* A sidebar that only moved the map (the left one growing as the right
     * one shrinks) still needs the whole window drawn again. */
    lv_drawDirtyScreen();
    g_lv->wantScreenUpdate = TRUE;
}

/* TRUE in the hosts that place the first view of a log: the log viewer's own
 * run (logViewerRun), the live spectator (spectatorRun) and the web viewer
 * (wasm/main_wasm.c). The reel in the game keeps the view the load gave it. */
static bool s_lvFirstViewHost = FALSE;

void lvHostFirstViewSetHost(bool on) {
    s_lvFirstViewHost = on;
}

/* --------------------------------------------------------------------------
 * lvHostFirstView -- Put the map where the first look at a log is worth it.
 *
 * A load opens the view with its corner at square 127,127, which is the
 * middle of the world: the land of a map is round that square, so three
 * quarters of it were off the view. Once for each load this puts the middle
 * of the box round everything that is not deep sea (lv_screenLandBox) in the
 * middle of the map that shows (lvHostVisibleMap), and zooms out when the
 * box does not go in at the zoom the viewer has (lvFirstViewZoomStep).
 *
 * It waits for a frame, so the sidebars are in place, and for a map: a log
 * that opens in the lobby has none until the round starts. Anything that
 * moves the view before then ends the wait (lv_screenFirstViewCancel), so
 * the view is never taken from the user. With "Tank Centred" on the zoom is
 * left alone and the view goes back to the tank on the next tick.
 * -------------------------------------------------------------------------- */
void lvHostFirstView(void) {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int has;
    float startX, startY, lenX, lenY;

    if (!s_lvFirstViewHost || g_lv->isLoaded == FALSE) {
        return;
    }
    if (g_lv->gameView) {
        lv_screenFirstViewCancel();
        return;
    }
    if (!lv_screenFirstViewDue()) {
        return;
    }
    lv_clientMutexWaitFor();
    has = lv_screenLandBox(&x0, &y0, &x1, &y1);
    lv_clientMutexRelease();
    if (!has) {
        return;
    }
    lv_screenFirstViewCancel();
    lvHostVisibleMap(&startX, &startY, &lenX, &lenY);
    if (g_lv->centredTank == FALSE) {
        float steps[32];
        int count = lv_drawGetZoomStepCount();
        int current = lv_drawGetZoomStepIndex();
        int i, step;
        if (count > (int)(sizeof(steps) / sizeof(steps[0]))) {
            count = (int)(sizeof(steps) / sizeof(steps[0]));
        }
        for (i = 0; i < count; i++) {
            steps[i] = lv_drawGetZoomStepValue(i);
        }
        step = lvFirstViewZoomStep(steps, count, current, x1 - x0 + 1, y1 - y0 + 1,
                                   lenX, lenY, TILE_SIZE_X);
        if (step != current) {
            int winW = 0, winH = 0;
            SDL_GetWindowSize(g_lv->window, &winW, &winH);
            lv_drawSetZoomStep(step, winW / 2, winH / 2);
        }
    }
    lv_clientMutexWaitFor();
    lv_drawDirtyScreen();
    lv_screenCentrePxIn(lvLandBoxMidPx(x0, x1, TILE_SIZE_X), lvLandBoxMidPx(y0, y1, TILE_SIZE_Y),
                        startX, startY, lenX, lenY, lv_drawGetZoomLevel());
    lv_clientMutexRelease();
    lv_windowNeedRedraw();
    g_lv->wantScreenUpdate = TRUE;
}
