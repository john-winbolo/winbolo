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

/*********************************************************
*Name:          SDL3 Draw Mirror
*Filename:      sdl3draw.h
*Purpose:
*  Phase 1-4 SDL3 mirror renderer. Opens a second window
*  alongside the Win32/DirectDraw window. Functions are
*  prefixed sdl3Draw* to avoid symbol collision while
*  both renderers coexist. Will be renamed draw* once
*  the Win32 path is removed.
*
*  Phase 4 adds sprites (shells/tanks/LGMs), status
*  panel render targets, bar textures and man-status.
*********************************************************/

#ifndef SDL3DRAW_H
#define SDL3DRAW_H

#include <SDL3/SDL.h>

/* Game view dimensions (logical pixels before zoom). */
#define SDL3_SCREEN_W 515
#define SDL3_SCREEN_H 325

#ifdef __cplusplus
extern "C" {
#endif
#include "../../bolo/global.h"
#include "../../bolo/screen.h"
#include "../../bolo/client_sim.h"

/* Portable RECT when not compiling on Windows */
#ifndef _WIN32
#ifndef _RECT_DEFINED
#define _RECT_DEFINED
typedef struct { long left, top, right, bottom; } RECT;
#endif
#endif

/*********************************************************
*NAME:          sdl3DrawSetup
*PURPOSE:
*  Opens the SDL3 mirror window and renderer.
*  Also creates all render-target textures used by
*  the status panels, bars and man-status circle.
*  Returns TRUE on success.
*********************************************************/
/*********************************************************
*NAME:          sdl3DrawGetZoomFactor
*PURPOSE:
*  Returns the current zoom factor (1, 2, or 4).
*********************************************************/
int sdl3DrawGetZoomFactor(void);
SDL_Window *sdl3DrawGetWindow(void);
SDL_Renderer *sdl3DrawGetRenderer(void);
SDL_Texture *sdl3DrawGetTilesTexture(void);

/* Atlas sheet scale used when blitting from the texture returned by
 * sdl3DrawGetTilesTexture. Sprite source coords stored in the legacy
 * tables (e.g. mapViewPosX/Y) are at scale 1; multiply by this factor
 * before passing to SDL_RenderTexture. Returns 1 when the atlas has
 * not been loaded. */
int sdl3DrawGetSheetScale(void);

SDL_Texture *sdl3DrawGetManStatusTexture(bool *ready);
bool sdl3DrawGetManStatusState(bool *isDead, TURNTYPE *angle);

/* Disable/restore render logical presentation for ImGui dialogs.
 * On Android (non-tablet), the game uses logical presentation which
 * causes a coordinate mismatch with ImGui touch input. */
void sdl3DrawDisableLogicalPresentation(void);
void sdl3DrawRestoreLogicalPresentation(void);

/*********************************************************
*NAME:          sdl3DrawHandleEvent
*PURPOSE:
*  Handle SDL events that affect draw-layer state —
*  currently mouse motion (cursor shape) and window
*  focus/leave (cursor restore).  Call this for every
*  event drained from the SDL queue on the main thread.
*********************************************************/
void sdl3DrawHandleEvent(ClientSim *cs, SDL_Event *ev);

bool sdl3DrawSetup(int zoomFactor);

/*********************************************************
*NAME:          sdl3DrawCleanup
*PURPOSE:
*  Destroys the SDL3 window, renderer and all textures.
*********************************************************/
void sdl3DrawCleanup(void);

/*********************************************************
*NAME:          sdl3DrawMainScreen
*PURPOSE:
*  Mirror of drawMainScreen. Draws background chrome,
*  scrolling map tiles, mine overlays, gunsight,
*  build-mode cursor, shells, tanks and LGMs.
*  After the clip rect is removed it calls
*  sdl3RenderStatusPanels() and then SDL_RenderPresent.
*********************************************************/
void sdl3DrawMainScreen(ClientSim *cs, screen *value, screenMines *mineView, screenTanks *tks,
                        screenGunsight *gs, screenBullets *sBullets, screenLgm *lgms,
                        RECT *rcWindow, bool showPillLabels, bool showBaseLabels,
                        int32_t srtDelay, bool isPillView, int edgeX, int edgeY,
                        bool useCursor, BYTE cursorLeft, BYTE cursorTop, tank *tank);

/*********************************************************
*NAME:          sdl3DrawRedrawAll
*PURPOSE:
*  Mirror of drawRedrawAll.  Redraws background chrome
*  and overlays all persistent status panels.
*********************************************************/
void sdl3DrawRedrawAll(ClientSim *cs, buildSelect value, RECT *rcWindow,
                       bool showPillsStatus, bool showBasesStatus);

/*********************************************************
*NAME:          sdl3DrawDownloadScreen
*PURPOSE:
*  Blacks the window and optionally shows a
*  "Downloading map data..." message (Phase 5).
*********************************************************/
void sdl3DrawDownloadScreen(ClientSim *cs, RECT *rcWindow, bool justBlack);

/*********************************************************
*NAME:          sdl3DrawMainScreenBlack
*PURPOSE:
*  Mirror of drawMainScreenBlack — clears SDL window.
*********************************************************/
void sdl3DrawMainScreenBlack(RECT *rcWindow);

/* -------------------------------------------------------
 * Phase 4 — Status panel render-target functions
 * ------------------------------------------------------- */

/*********************************************************
*NAME:          sdl3DrawSetBasesStatusClear
*PURPOSE:
*  Clears the bases status render target to black and
*  draws the centre base icon (BASE_GOOD tile).
*********************************************************/
void sdl3DrawSetBasesStatusClear(void);

/*********************************************************
*NAME:          sdl3DrawStatusBase
*PURPOSE:
*  Draws a single base status icon into the bases render
*  target at the grid position for baseNum (1-16).
*  ba selects the tile source; labels is ignored (Phase 5).
*********************************************************/
void sdl3DrawStatusBase(BYTE baseNum, baseAlliance ba, bool labels);

/*********************************************************
*NAME:          sdl3DrawCopyBasesStatus
*PURPOSE:
*  Blits the bases render target to the SDL3 window at
*  the correct zoomed position.  x/y are ignored.
*********************************************************/
void sdl3DrawCopyBasesStatus(int x, int y);

/*********************************************************
*NAME:          sdl3DrawSetPillsStatusClear
*PURPOSE:
*  Clears the pills status render target to black and
*  draws the centre pillbox icon (PILL_GOOD15 tile).
*********************************************************/
void sdl3DrawSetPillsStatusClear(void);

/*********************************************************
*NAME:          sdl3DrawStatusPillbox
*PURPOSE:
*  Draws a single pillbox status icon into the pills
*  render target at the grid position for pillNum (1-16).
*********************************************************/
void sdl3DrawStatusPillbox(BYTE pillNum, pillAlliance pa, bool labels);

/*********************************************************
*NAME:          sdl3DrawCopyPillsStatus
*PURPOSE:
*  Blits the pills render target to the SDL3 window.
*  x/y are ignored.
*********************************************************/
void sdl3DrawCopyPillsStatus(int x, int y);

/*********************************************************
*NAME:          sdl3DrawSetTanksStatusClear
*PURPOSE:
*  Clears the tanks status render target to black and
*  draws the centre tank icon (TANK_SELF_0 tile).
*********************************************************/
void sdl3DrawSetTanksStatusClear(void);

/*********************************************************
*NAME:          sdl3DrawStatusTank
*PURPOSE:
*  Draws a single tank status icon into the tanks render
*  target at the grid position for tankNum (1-16).
*********************************************************/
void sdl3DrawStatusTank(BYTE tankNum, tankAlliance ta);

/*********************************************************
*NAME:          sdl3DrawCopyTanksStatus
*PURPOSE:
*  Blits the tanks render target to the SDL3 window.
*  x/y are ignored.
*********************************************************/
void sdl3DrawCopyTanksStatus(int x, int y);

/*********************************************************
*NAME:          sdl3DrawStatusTankBars
*PURPOSE:
*  Draws green resource bars (shells/mines/armour/trees)
*  into the tank-bars render target.
*  x/y are ignored; the bars are drawn at the positions
*  defined by STATUS_TANK_BARS_* constants.
*********************************************************/
void sdl3DrawStatusTankBars(int x, int y,
                             BYTE shells, BYTE mines, BYTE armour, BYTE trees);

/*********************************************************
*NAME:          sdl3DrawCopyTankStatusBars
*PURPOSE:
*  Blits the tank-bars render target to the SDL3 window.
*  x/y are ignored.
*********************************************************/
void sdl3DrawCopyTankStatusBars(int x, int y);

/*********************************************************
*NAME:          sdl3DrawStatusBaseBars
*PURPOSE:
*  Draws green resource bars (shells/mines/armour) into
*  the base-bars render target.
*  x/y and redraw are ignored.
*********************************************************/
void sdl3DrawStatusBaseBars(int x, int y,
                             BYTE shells, BYTE mines, BYTE armour, bool redraw);

/*********************************************************
*NAME:          sdl3DrawCopyBasesStatusBars
*PURPOSE:
*  Blits the base-bars render target to the SDL3 window.
*  x/y are ignored.
*********************************************************/
void sdl3DrawCopyBasesStatusBars(int x, int y);

/*********************************************************
*NAME:          sdl3DrawSetManClear
*PURPOSE:
*  Clears the man-status render target to black.
*********************************************************/
void sdl3DrawSetManClear(void);

/*********************************************************
*NAME:          sdl3DrawSetManStatus
*PURPOSE:
*  Draws the LGM direction circle on the man-status
*  render target.  If isDead the circle is filled red;
*  otherwise an outline is drawn with an arrow indicating
*  angle.  Copies the result to the screen immediately.
*  x/y are ignored (positions use MAN_STATUS_* constants).
*********************************************************/
void sdl3DrawSetManStatus(int x, int y, bool isDead, TURNTYPE angle);

/*********************************************************
*NAME:          sdl3DrawCopyManStatus
*PURPOSE:
*  Blits the man-status render target to the SDL3 window
*  at the zoomed MAN_STATUS_X/Y position.
*  x/y are ignored.
*********************************************************/
void sdl3DrawCopyManStatus(int x, int y);

/*********************************************************
*NAME:          sdl3DrawSelectIndentsOn
*PURPOSE:
*  Records the current build selection so that
*  sdl3RenderStatusPanels can draw the INDENT_ON tile
*  over the correct button.  x/y are ignored.
*********************************************************/
void sdl3DrawSelectIndentsOn(buildSelect value, int x, int y);

/*********************************************************
*NAME:          sdl3DrawSelectIndentsOff
*PURPOSE:
*  No-op. background.bmp already shows all buttons in
*  the off (un-indented) state.
*********************************************************/
void sdl3DrawSelectIndentsOff(buildSelect value, int x, int y);

/*********************************************************
*NAME:          sdl3DrawStartDelay
*PURPOSE:
*  Blacks out only the game-area rectangle while the
*  start-delay countdown is active.
*  srtDelay is used as a guard (no-op when <= 0).
*********************************************************/
void sdl3DrawStartDelay(RECT *rcWindow, int32_t srtDelay);

/*********************************************************
*NAME:          sdl3DrawSetNetFailed
*PURPOSE:
*  Call with v=true before sdl3DrawMainScreen when
*  cs->netStat==netFailed so the overlay is drawn
*  inside the same frame.  Call with v=false otherwise.
*********************************************************/
void sdl3DrawSetNetFailed(bool v);

/*********************************************************
*NAME:          sdl3DrawNetFailed
*PURPOSE:
*  Renders "Network Failed - Resyncing" text inside the
*  game area.  Only draws when sdl3DrawSetNetFailed(true)
*  was called this frame.
*********************************************************/
void sdl3DrawNetFailed(void);

/*********************************************************
*NAME:          sdl3DrawPillInView
*PURPOSE:
*  Renders "Pillbox View" text at the bottom of the game
*  area while the player is spectating a pillbox.
*********************************************************/
void sdl3DrawPillInView(void);

/*********************************************************
*NAME:          sdl3DrawResetCachedText
*PURPOSE:
*  Clears cached message strings, kills/deaths, and
*  destroys their texture caches.  Call on game start so
*  stale text from the previous round is not displayed.
*********************************************************/
void sdl3DrawResetCachedText(void);

/*********************************************************
*NAME:          sdl3DrawMessages
*PURPOSE:
*  Renders the two-line scrolling message box at the
*  bottom of the screen using SDL_ttf.
*  x/y are the window origin (ignored — SDL3 uses 0,0).
*********************************************************/
void sdl3DrawMessages(int x, int y, char *top, char *bottom);

/*********************************************************
*NAME:          sdl3DrawKillsDeaths
*PURPOSE:
*  Renders the kills and deaths numeric counters in the
*  status area using SDL_ttf.
*  x/y are the window origin (ignored — SDL3 uses 0,0).
*********************************************************/
void sdl3DrawKillsDeaths(int x, int y, int kills, int deaths);

/*********************************************************
*NAME:          sdl3DrawTankLabel
*PURPOSE:
*  Renders the player name above the tank sprite using
*  SDL_ttf (Phase 5).  A cached SDL_Texture is kept per
*  player slot and rebuilt only when the name changes.
*  Uses gCurrentEdgeX/Y set by sdl3DrawMainScreen so
*  the label scrolls with the map.
*
*ARGUMENTS:
*  str       - Player name string
*  playerNum - Player slot (0-15)
*  mx, my    - Map tile coordinates
*  px, py    - Pixel offsets within the tile
*********************************************************/
void sdl3DrawTankLabel(char *str, BYTE playerNum,
                       BYTE mx, BYTE my, BYTE px, BYTE py);

/*********************************************************
*NAME:          sdl3DrawGetTabletViewport
*PURPOSE:
*  Returns the last computed tablet viewport bounds and
*  effective zoom.  Returns zeros if not in tablet mode.
*********************************************************/
void sdl3DrawGetTabletViewport(int *x, int *y, int *w, int *h, int *zoom);

/*********************************************************
*NAME:          sdl3DrawSetDragOffset / GetDragOffset
*PURPOSE:
*  Set/get the drag-scroll pixel offset for smooth sub-tile
*  scrolling in tablet mode. Values are in zoomed pixels.
*********************************************************/
void sdl3DrawSetDragOffset(int dx, int dy);
void sdl3DrawGetDragOffset(int *dx, int *dy);

/*********************************************************
*NAME:          sdl3DrawSetStatusPanelOrigins
*PURPOSE:
*  Override the screen positions of the tanks/pills/bases
*  status grids. Pass -1 for any value to use the desktop
*  default. Positions are in zoomed pixel coordinates.
*********************************************************/
void sdl3DrawSetStatusPanelOrigins(float tanksX, float tanksY,
                                    float pillsX, float pillsY,
                                    float basesX, float basesY);

/*********************************************************
*NAME:          sdl3DrawTabletStatusGrids
*PURPOSE:
*  Renders status grids (tanks/pills/bases) for tablet mode
*  using the same SDL rendering as desktop (BLENDMODE_NONE).
*  Call after sdl3DrawSetStatusPanelOrigins() has set the
*  gutter positions.
*********************************************************/
void sdl3DrawTabletStatusGrids(struct ClientSim *cs);

/*********************************************************
*NAME:          sdl3DrawGetCachedMessages
*PURPOSE:
*  Returns pointers to the cached top and bottom message
*  strings (set each frame by sdl3DrawMessages).
*  Used by the tablet overlay to render messages via ImGui.
*********************************************************/
void sdl3DrawGetCachedMessages(const char **top, const char **bottom);

void sdl3DrawGetCachedTankStats(BYTE *shells, BYTE *mines, BYTE *armour, BYTE *trees);
void sdl3DrawGetCachedBaseStats(BYTE *shells, BYTE *mines, BYTE *armour, bool *hasBase);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SDL3DRAW_H */
