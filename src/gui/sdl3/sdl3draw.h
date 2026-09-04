/*
 * $Id$
 *
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
#include "global.h"
#include "viewport_types.h"  /* screen, screenMines, screenGunsight */
#include "client_enums.h"    /* buildSelect */
#include "screentank.h"      /* screenTanks, tank */
#include "screenbullet.h"    /* screenBullets */
#include "screenlgm.h"       /* screenLgm */
#include "client_sim.h"
/* Status / message / HUD renderers extracted in Phase C of
 * plans/ctrailer.md. sdl3draw.h re-exposes them transparently so
 * existing callers compile unchanged. */
#include "sdl3draw_status.h"
/* Pure geometry — no SDL, no ImGui — so both the drawing side and the ImGui
 * side can work from the same rectangles. */
#include "overview_hud_layout.h"
#include "gfx_settings.h"    /* GfxTextureFilter */

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

/* In-window Map Overview mode: the overview fills the game window and the
   classic 15x15 view and chrome are not drawn. */
void sdl3DrawSetOverviewInWindow(bool active);
bool sdl3DrawIsOverviewInWindow(void);
struct OverviewView *sdl3DrawOverviewInWindowView(void);
/* Where the overview was last blitted, in renderer coordinates — which are
   ImGui's coordinates in every mode we ship. False when the mode is off or
   nothing has been drawn yet. */
bool sdl3DrawGetOverviewInWindowRect(float *outX, float *outY,
                                     float *outW, float *outH);
/* The HUD geometry the last in-window frame used, so the ImGui side can put
   its hit-testing on the same rectangles the blit used. False when the mode
   is off or no HUD was laid out. */
bool sdl3DrawGetOverviewHudLayout(OverviewHudLayout *out);

/* Counter bumped every time the tile atlas is rebuilt. A caller that
 * builds its own sheet from tileLoaderBuildSheet can hold the value it
 * last saw and rebuild when it no longer matches — that is how a skin
 * change reaches an atlas the renderer does not own. */
unsigned int sdl3DrawGetTilesGeneration(void);

/* TRUE if the caller is on the thread that created the renderer. The SDL
 * renderer / Metal command queue must only be touched from that thread, so
 * sim-tick front-end callbacks that would otherwise draw defer to the
 * per-frame render pass and use this to gate any direct GPU work. */
bool sdl3DrawOnRenderThread(void);

/* Live game-render destination rect + scale, used by UI overlays to
 * pin themselves to the actual on-screen game viewport (which can be
 * letterboxed / pillarboxed / non-integer scaled in custom zoom). Any
 * pointer may be NULL. */
void sdl3DrawGetGameRect(float *destX, float *destY,
                          float *destW, float *destH, float *scale);

/* Atlas sheet scale used when blitting from the texture returned by
 * sdl3DrawGetTilesTexture. Sprite source coords stored in the legacy
 * tables (e.g. mapViewPosX/Y) are at scale 1; multiply by this factor
 * before passing to SDL_RenderTexture. Returns 1 when the atlas has
 * not been loaded. */
int sdl3DrawGetSheetScale(void);

/* Build the gunsight crosshair from data/crosshairs_17x17.png — a 17x17
 * sprite whose centre pixel (8,8) sits on the aim point. NULL when the
 * asset is missing or the decode fails; the caller owns the texture and
 * destroys it. A second window has to make its own copy: an SDL texture
 * only works on the renderer that created it. */
SDL_Texture *sdl3DrawCreateCrosshairTexture(SDL_Renderer *r);

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
*NAME:          sdl3DrawReconfigureZoom
*PURPOSE:
*  Reconfigures all zoom-dependent assets (tiles, fonts,
*  status panel, man-status and game render targets) in
*  place against the live renderer.  explicitZoom >= 1
*  uses that integer render zoom; 0 (Custom mode) derives
*  it from the current window size.  Does not touch the
*  renderer or window.
*********************************************************/
void sdl3DrawReconfigureZoom(int explicitZoom);

/*********************************************************
*NAME:          sdl3DrawReloadTiles
*PURPOSE:
*  Rebuilds only the tile atlas in place for a skin
*  change, re-reading the skin assets from disk.  Does
*  not touch the renderer, window, fonts, or zoom.
*********************************************************/
void sdl3DrawReloadTiles(void);

/*********************************************************
*NAME:          sdl3DrawReloadBackground
*PURPOSE:
*  Drops the cached game background for a skin change so
*  the next frame reads it again.  Destroys and clears the
*  texture only; the draw sites reload it lazily.
*********************************************************/
void sdl3DrawReloadBackground(void);

/*********************************************************
*NAME:          sdl3DrawScaleModeForFilter
*PURPOSE:
*  Turns the player's texture filter setting into the SDL
*  scale mode that matches it.  Anything unrecognised maps
*  to nearest.  Lives here because gfx_settings.h has no
*  SDL of its own.
*********************************************************/
SDL_ScaleMode sdl3DrawScaleModeForFilter(GfxTextureFilter filter);

/*********************************************************
*NAME:          sdl3DrawSetTilesScaleMode
*PURPOSE:
*  Sets how the tile sheet is sampled.  Applies to the
*  current sheet when there is one, and is kept for every
*  sheet built afterwards, so calling it before the first
*  build is fine.  The status bar draws from the same
*  texture object and follows without a call of its own.
*********************************************************/
void sdl3DrawSetTilesScaleMode(SDL_ScaleMode mode);

/*********************************************************
*NAME:          sdl3DrawSetReconfigureGuard
*PURPOSE:
*  Marks the start (true) / end (false) of an in-place
*  zoom or skin reconfigure so sdl3DrawCleanup can assert
*  the renderer/window is never torn down during one.
*********************************************************/
void sdl3DrawSetReconfigureGuard(bool active);

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
                        int32_t srtDelay, bool isItemView, int edgeX, int edgeY,
                        bool useCursor, BYTE cursorLeft, BYTE cursorTop);

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
*NAME:          sdl3DrawReturningToLobby
*PURPOSE:
*  Same chrome + black playfield as sdl3DrawDownloadScreen
*  with justBlack=true, plus a centred "Returning to lobby"
*  caption. Rendered during the brief post-game window
*  where the server has sent CTRL_GAME_PHASE_GAME_OVER
*  but inLobby has not yet flipped.
*********************************************************/
void sdl3DrawReturningToLobby(ClientSim *cs);

/*********************************************************
*NAME:          sdl3DrawMainScreenBlack
*PURPOSE:
*  Mirror of drawMainScreenBlack — clears SDL window.
*********************************************************/
void sdl3DrawMainScreenBlack(RECT *rcWindow);

/* -------------------------------------------------------
 * Phase 4 — Status panel render-target functions
 * ------------------------------------------------------- */

/* sdl3DrawSetBasesStatusClear / sdl3DrawStatusBase / sdl3DrawCopyBasesStatus
 * and the pillbox / tank / *bars equivalents declared via
 * sdl3draw_status.h (#included above). */

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
*  clientSimGetNetStatus(cs)==netFailed so the overlay is drawn
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
*NAME:          sdl3DrawItemInView
*PURPOSE:
*  Names the current item view at the bottom of the game
*  area — "Pillbox View", "Base View", or "Allied Tank
*  View" with the ally's player name. Draws nothing in the
*  tank view.
*********************************************************/
void sdl3DrawItemInView(ClientSim *cs);

/*********************************************************
*NAME:          sdl3DrawGetItemViewLabel
*PURPOSE:
*  Fills out with the name of the current item view and
*  returns true.  Returns false, writing nothing, in the
*  tank view and for a NULL cs.  The one place the three
*  view names are spelled: the classic corner label and the
*  full screen map's caption both take their text from here.
*********************************************************/
bool sdl3DrawGetItemViewLabel(ClientSim *cs, char *out, size_t outLen);

/* sdl3DrawResetCachedText / sdl3DrawMessages / sdl3DrawKillsDeaths /
 * sdl3DrawTankLabel declared via sdl3draw_status.h (#included above). */

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

/* When true, the build-mode cursor reticle is drawn at 50% alpha (used to
   show a locked build target while build/cursor mode is off). Solid when
   false. Set each frame before sdl3DrawMainScreen. */
void sdl3DrawSetCursorFaint(bool faint);

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

/* sdl3DrawGetCachedMessages declared via sdl3draw_status.h (#included
 * above). The tablet overlay uses it to render messages via ImGui. */

void sdl3DrawGetCachedTankStats(BYTE *shells, BYTE *mines, BYTE *armour, BYTE *trees);
void sdl3DrawGetCachedBaseStats(BYTE *shells, BYTE *mines, BYTE *armour, bool *hasBase);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SDL3DRAW_H */
