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

#include "global.h"
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

#endif
