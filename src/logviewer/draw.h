/*
 * $Id$
 *
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
*Name:          Draw
*Filename:      draw.h
*Author:        John Morrison
*Creation Date: 13/12/98
*Last Modified: 28/5/00
*Purpose:
*  System Specific Drawing routines (Uses SDL3)
*********************************************************/

#ifndef _DRAW_H
#define _DRAW_H

#include "lv_global.h"
#include "backend.h"

/* Size of the message string on the screen */
#define MESSAGE_STRING_SIZE 68

/* Length of the kills/Deaths string */
#define STRING_SIZE 3

/* An Null Charector */
#define EMPTY_CHAR '\0'


/*********************************************************
*NAME:          lv_drawSetup
*AUTHOR:        John Morrison
*CREATION DATE: 13/10/98
*LAST MODIFIED:  29/4/00
*PURPOSE:
*  Sets up drawing systems, direct draw structures etc.
*  Returns whether the operation was successful or not
*
*ARGUMENTS:
* appInst - Handle to the application (Required to 
*           load bitmaps from resources)
* appWnd  - Main Window Handle (Required for clipper)
*********************************************************/
BYTE lv_drawSetup(void);

/*********************************************************
*NAME:          lv_drawCleanup
*AUTHOR:        John Morrison
*CREATION DATE: 13/12/98
*LAST MODIFIED: 13/2/98
*PURPOSE:
*  Destroys and cleans up drawing systems, direct draw 
*  structures etc.
*
*ARGUMENTS:
*
*********************************************************/
void lv_drawCleanup(void);

/*********************************************************
*NAME:          lv_drawMainScreen
*AUTHOR:        John Morrison
*CREATION DATE: 31/10/98
*LAST MODIFIED: 27/05/00
*PURPOSE:
*  Updates the Main Window View
*
*ARGUMENTS:
*  value    - Pointer to the sceen structure
*  mineView - Pointer to the screen mines structure
*  tks      - Pointer to the screen tank structure
*  gs       - Pointer to the screen gunsight structure
*  sBullets - The screen Bullets structure
*  lgms     - Screen Builder structure
*  showPillLabels - Show the pillbox labels?
*  showBaseLabels - Show the base labels?
*  srtDelay       - The start delay in ticks.
*                  If greater then 0 should draw countdown
*  isPillView     - TRUE if we are in pillbox view
*  edgeX          - Edge X offset for smooth scrolling
*  edgeY          - Edge Y offset for smooth scrolling
*  useCursor      - True if to draw the cursor
*  cursorLeft     - Cursor left position
*  cursorTop      - Cursor Top position
*********************************************************/
void lv_drawMainScreen(screen *value, screenMines *mineView, screenTanks *tks, screenGunsight *gs, screenBullets *sBullets, screenLgm *lgms, BYTE showPillLabels, BYTE showBaseLabels, int32_t srtDelay, BYTE isPillView, int edgeX, int edgeY, BYTE useCursor, BYTE cursorLeft, BYTE cursorTop);

/*********************************************************
*NAME:          lv_drawGetFrameRate
*AUTHOR:        John Morrison
*CREATION DATE: 16/12/98
*LAST MODIFIED: 16/12/98
*PURPOSE:
*  Returns the frame rate being achieved
*
*ARGUMENTS:
*
*********************************************************/
int lv_drawGetFrameRate(void);

/*********************************************************
*NAME:          lv_drawShells
*AUTHOR:        John Morrison
*CREATION DATE: 26/12/98
*LAST MODIFIED: 26/12/98
*PURPOSE:
*  Draws shells and explosions on the backbuffer.
*
*ARGUMENTS:
*  sBullets - The screen Bullets data structure 
*********************************************************/
void lv_drawShells(screenBullets *sBullets);

/*********************************************************
*NAME:          lv_drawTanks
*AUTHOR:        John Morrison
*CREATION DATE: 6/1/99
*LAST MODIFIED: 2/2/99
*PURPOSE:
*  Draws tanks on the backbuffer.
*
*ARGUMENTS:
*  tks - The screen Tanks data structure 
*********************************************************/
void lv_drawTanks(screenTanks *tks);

/*********************************************************
*NAME:          lv_drawLGMs
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 17/1/99
*PURPOSE:
*  Draws the builder
*
*ARGUMENTS:
*  lgms - The screenLgm data structure 
*********************************************************/
void lv_drawLGMs(screenLgm *lgms);

/*********************************************************
*NAME:          lv_drawTankLabel
*AUTHOR:        John Morrison
*CREATION DATE: 2/2/98
*LAST MODIFIED: 2/2/98
*PURPOSE:
*  Draws the tank label if required.
*
*ARGUMENTS:
*  str - The string identifer of the tank
*  mx  - Tank map X position
*  my  - Tank map Y position
*  px  - Tank pixel offset
*  py  - Tank pixel offset
*********************************************************/
void lv_drawTankLabel(char *str, int mx, int my, BYTE px, BYTE py);

void lv_drawDirtyScreen(void);

/*********************************************************
*NAME:          lv_drawSplashForImGui
*PURPOSE:
*  Draws the splash screen for the ImGui rendering loop.
*  This function clears the screen and draws the splash,
*  but does NOT present. ImGui will present after drawing UI.
*
*  Called from the main loop BEFORE ImGui renders.
*********************************************************/
void lv_drawSplashForImGui(void);

/*********************************************************
*NAME:          lv_drawCleanupSplash
*PURPOSE:
*  Cleans up the cached splash texture
*********************************************************/
void lv_drawCleanupSplash(void);

/*********************************************************
*NAME:          lv_drawGetSDLWindow
*AUTHOR:        ImGui Migration
*PURPOSE:
*  Returns the SDL window handle for ImGui integration
*
*ARGUMENTS:
*  None
*RETURNS:
*  SDL_Window* pointer or NULL if not initialized
*********************************************************/
struct SDL_Window* lv_drawGetSDLWindow(void);

/*********************************************************
*NAME:          lv_drawGetSDLRenderer
*AUTHOR:        ImGui Migration
*PURPOSE:
*  Returns the SDL renderer handle for ImGui integration
*
*ARGUMENTS:
*  None
*RETURNS:
*  SDL_Renderer* pointer or NULL if not initialized
*********************************************************/
struct SDL_Renderer* lv_drawGetSDLRenderer(void);

/*********************************************************
*NAME:          lv_drawGetGameTexture
*AUTHOR:        ImGui Migration
*PURPOSE:
*  Returns the game render texture for ImGui viewport display
*
*ARGUMENTS:
*  None
*RETURNS:
*  SDL_Texture* pointer or NULL if not initialized
*********************************************************/
struct SDL_Texture* lv_drawGetGameTexture(void);

/*********************************************************
*NAME:          lv_drawSetEmbedded / lv_drawSetEmbedViewport
*PURPOSE:
*  Embed mode. When enabled, lv_drawMainScreen and
*  lv_drawBlitGameTexture paint only into the render target
*  and never blit it to the framebuffer (the host draws the
*  texture inside its own ImGui frame), the menu-bar offset
*  is 0 at every site, and lv_drawApplyZoomStep sizes the
*  tile grid from the viewport set here instead of the
*  window.
*********************************************************/
void lv_drawSetEmbedded(int enabled);
void lv_drawSetEmbedViewport(int w, int h);

/*********************************************************
*NAME:          lv_drawGetGameTargetSize
*PURPOSE:
*  Returns the render target's pixel dimensions, which are
*  (sizeX+1, sizeY+1) tiles — one tile larger than the
*  visible slice. An embedding host needs them to turn the
*  visible slice into texture UVs. Out-params are optional.
*********************************************************/
void lv_drawGetGameTargetSize(int *outW, int *outH);

/*********************************************************
*NAME:          lv_drawGetTilesTexture / lv_drawGetSheetScale
*PURPOSE:
*  Expose the unified SVG/PNG/BMP tile atlas (and its build
*  scale) so the standalone game-view path (Phase D of
*  plans/ctrailer.md) can construct a MapViewCtx pointing at
*  the same atlas this module already maintains. Atlas scale
*  is always 1.
*********************************************************/
struct SDL_Texture* lv_drawGetTilesTexture(void);
int lv_drawGetSheetScale(void);

/*********************************************************
*NAME:          lv_drawScnMarker
*PURPOSE:
*  Draws one scenario map marker the way the game does: the
*  square outlined in the marker's palette colour with a
*  pointer above it, both breathing on the wall clock.
*  (cx, cy) is the centre of the square in the renderer's
*  current coordinates and tileW/tileH its size there. A
*  colour the palette draws nothing for draws nothing.
*  Both the overview and the game view call this, so the
*  two cannot disagree about how a marker looks.
*********************************************************/
void lv_drawScnMarker(struct SDL_Renderer *renderer, BYTE colour,
                      float cx, float cy, float tileW, float tileH,
                      uint32_t nowMs);

/*********************************************************
*NAME:          lv_drawRegionName / lv_drawRegion
*PURPOSE:
*  A declared region as Options -> Regions shows it.
*  lv_drawRegionName renders a region's name in font, in the
*  regions' colour, and returns the texture (the caller
*  destroys it) with its size in *outW, *outH; NULL for an
*  empty name or a failed render. lv_drawRegion outlines the
*  rectangle (x, y, w, h), in the renderer's current
*  coordinates, with a stroke sized from tileH, and draws the
*  name texture, when there is one, just inside its top-left
*  corner on a dark box. Both the overview and the game view
*  call these, so the two draw a region the same way.
*********************************************************/
struct TTF_Font;
struct SDL_Texture *lv_drawRegionName(struct SDL_Renderer *renderer,
                                      struct TTF_Font *font, const char *name,
                                      int *outW, int *outH);
void lv_drawRegion(struct SDL_Renderer *renderer, float x, float y,
                   float w, float h, float tileH,
                   struct SDL_Texture *name, int nameW, int nameH);

/*********************************************************
*NAME:          lv_drawBlitGameTexture
*PURPOSE:
*  Blits the game render texture to the screen without
*  re-rendering the game. Used when the game state hasn't
*  changed but we need to display the last frame.
*********************************************************/
void lv_drawBlitGameTexture(void);

/*********************************************************
*NAME:          lv_drawResizeRenderTarget
*PURPOSE:
*  Recreates the render target texture when screen size changes.
*  This is critical for correct mouse coordinate mapping -
*  if the texture size doesn't match the screen size, SDL
*  will scale the texture, causing coordinate drift.
*********************************************************/
void lv_drawResizeRenderTarget(void);

/*********************************************************
*NAME:          lv_drawSetupWithHandles
*PURPOSE:
*  Sets up drawing using externally-provided window/renderer
*  (embedded mode). Does NOT create SDL window/renderer.
*  Loads textures, render target, and font.
*RETURNS:
*  TRUE on success, FALSE on failure
*********************************************************/
BYTE lv_drawSetupWithHandles(struct SDL_Window *window, struct SDL_Renderer *renderer);

/*********************************************************
*NAME:          lv_drawGetTargetWidth / lv_drawGetTargetHeight
*PURPOSE:
*  Returns the current render target dimensions
*********************************************************/
int lv_drawGetTargetWidth(void);
int lv_drawGetTargetHeight(void);

/*********************************************************
*NAME:          lv_drawZoomIn / lv_drawZoomOut
*PURPOSE:
*  Step the user zoom level up/down by one entry in the
*  stepped zoom table (0.5x .. 16x). The map tile under
*  (mouseScreenX, mouseScreenY) — window-pixel coordinates,
*  same space SDL events report — stays anchored under the
*  cursor across the change.
*********************************************************/
void lv_drawZoomIn(int mouseScreenX, int mouseScreenY);
void lv_drawZoomOut(int mouseScreenX, int mouseScreenY);

/*********************************************************
*NAME:          lv_drawGetZoomLevel
*PURPOSE:
*  Returns the current zoom multiplier (1.0 == native).
*********************************************************/
float lv_drawGetZoomLevel(void);

/*********************************************************
*NAME:          lv_drawGetZoomStepIndex
*PURPOSE:
*  Returns the current zoom step index into the zoom table.
*********************************************************/
int lv_drawGetZoomStepIndex(void);

/*********************************************************
*NAME:          lv_drawGetZoomStepCount
*PURPOSE:
*  Returns the total number of zoom steps available.
*********************************************************/
int lv_drawGetZoomStepCount(void);

/*********************************************************
*NAME:          lv_drawGetZoomStepValue
*PURPOSE:
*  Returns the zoom multiplier for a given step index.
*********************************************************/
float lv_drawGetZoomStepValue(int index);

/*********************************************************
*NAME:          lv_drawSetZoomStep
*PURPOSE:
*  Sets zoom to a specific step index, anchored on the
*  given screen coordinates.
*********************************************************/
void lv_drawSetZoomStep(int stepIndex, int mouseScreenX, int mouseScreenY);

/*********************************************************
*NAME:          lv_drawSetZoomStepIndexRaw
*PURPOSE:
*  Sets the zoom step alone — no viewport resize, no cursor
*  anchoring, no screen-state writes. For saving and
*  restoring the step around an embedded session, where the
*  anchored path would resize the tile grid to the embed's
*  rect and, on restore, write through decoder state that is
*  already gone.
*********************************************************/
void lv_drawSetZoomStepIndexRaw(int index);

#endif
