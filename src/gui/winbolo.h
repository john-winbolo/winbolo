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
*Name:          WinBolo
*Filename:      WinBolo.h
*Author:        John Morrison
*Creation Date: 31/10/98
*Last Modified:  31/7/00
*Purpose:
*  Provides the front end for window/frontend
*********************************************************/

#ifndef _WINBOLO_H
#define _WINBOLO_H

#include "global.h"
#include "input.h"

struct ClientSim;

/* The name of the main window Class */
#define WIND_CLASSNAME "WinBolo"
#define WIND_TITLE "WinBolo"

#define DIALOG_BOX_TITLE "WinBolo"

#define BOLO_SOUNDS_DLL "BoloSounds.bsd"

/* Frame Rates */
/* Numbers slightly off because of timer innacuracies */
#define FRAME_RATE_10 19
#define FRAME_RATE_12 23
#define FRAME_RATE_15 28
#define FRAME_RATE_20 37
#define FRAME_RATE_30 55
#define FRAME_RATE_50 82
#define FRAME_RATE_60 111

/* Zoom Factors */
#define ZOOM_FACTOR_NORMAL 1
#define ZOOM_FACTOR_DOUBLE 2
#define ZOOM_FACTOR_TRIPLE 3
#define ZOOM_FACTOR_QUAD 4
#define ZOOM_FACTOR_CUSTOM 0  /* Resizable window, renders at ceiling integer zoom */

/* Height of the ImGui menu bar rendered inside the client area */
#ifdef __APPLE__
#define MENU_BAR_HEIGHT 0
#else
#define MENU_BAR_HEIGHT 22
#endif

/* The size of the main window EXCLUDING Menus and Toolbar */
#define SCREEN_SIZE_X 515
#define SCREEN_SIZE_Y 325

#define TOTAL_WINDOW_SIZE_X (SCREEN_SIZE_X)
#define TOTAL_WINDOW_SIZE_Y (SCREEN_SIZE_Y)

/* Nothing has been selected */
#define NO_SELECT -1

/* Defines the number of milliseconds in a second */
/* Used for frame rate counting */
#define MILLISECONDS 1000


/* Open Dialog Box Stuff */
#define OPEN_FILE_FILTERS "Map Files\0*.MAP\0All Files\0*.*\0\0"
#define OPEN_FILE_TITLE "Open File...\0"
#define DEFAULT_FILE_EXTENSION "map\0"

/* Stuff to write in players menu if there are no tanks there */
#define STR_01 "1\0"
#define STR_02 "2\0"
#define STR_03 "3\0"
#define STR_04 "4\0"
#define STR_05 "5\0"
#define STR_06 "6\0"
#define STR_07 "7\0"
#define STR_08 "8\0"
#define STR_09 "9\0"
#define STR_10 "10\0"
#define STR_11 "11\0"
#define STR_12 "12\0"
#define STR_13 "13\0"
#define STR_14 "14\0"
#define STR_15 "15\0"
#define STR_16 "16\0"
#define STR_PLAYER_LEN 2

/* Window show request types */
typedef enum {
  wsrOpen, /* Window request to be open */
  wsrClose /* Window has closed */
} windowShowRequest;

/* Are we in a menu or not */
extern bool isInMenu;

/*********************************************************
*NAME:          windowKeyPressed
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/98
*LAST MODIFIED: 31/1/99
*PURPOSE:
* When a key is pressed it determines what combination
* should be passed when the game is updated
*
*ARGUMENTS:
*  keyCode - The key code of the button that was pressed
*********************************************************/
void windowKeyPressed(struct ClientSim *cs, int keyCode);

/*********************************************************
*NAME:          windowButtonAdd
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/98
*LAST MODIFIED: 26/11/98
*PURPOSE:
* When a tank button (accelerate/decelerate/left/right)
* is pressed it updates the state of the button to pass
*
*ARGUMENTS:
*  keyCode - The key code of the button that was pressed
*********************************************************/
void windowButtonAdd(int keyCode);

/*********************************************************
*NAME:          windowButtonRemove
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/98
*LAST MODIFIED: 26/11/98
*PURPOSE:
* When a tank button (accelerate/decelerate/left/right)
* is released it updates the state of the button to pass
*
*ARGUMENTS:
*  keyCode - The key code of the button that was pressed
*********************************************************/
void windowButtonRemove(int keyCode);

/*********************************************************
*NAME:          windowMouseClick
*AUTHOR:        John Morrison
*CREATION DATE: 21/12/98
*LAST MODIFIED: 21/12/98
*PURPOSE:
* The mouse button is pressed. Check to see if selected
* building things are changed
*
*ARGUMENTS:
*  xWin - X Co-ord of top position of window
*  yWin - Y Co-ord of top position of window
*  xPos - X Position of the mouse (relative to the window)
*  yPos - Y Position of the mouse (relative to the window)
*********************************************************/
void windowMouseClick(int xWin, int yWin, int xPos, int yPos);

/*********************************************************
*NAME:          windowSetZoomFactor
*AUTHOR:        John Morrison
*CREATION DATE: 21/1/99
*LAST MODIFIED: 21/1/99
*PURPOSE:
*  Sets the windows zoom factor
*
*ARGUMENTS:
*  amount - New Zoom Factor
*********************************************************/
void windowSetZoomFactor(BYTE amount);

/*********************************************************
*NAME:          windowZoomChange
*AUTHOR:        John Morrison
*CREATION DATE: 21/1/99
*LAST MODIFIED: 21/1/99
*PURPOSE:
*  The menu items have been clicked. Change the check mark
*  and then update the zoom factor
*
*ARGUMENTS:
*  amount - New Zoom Factor
*********************************************************/
void windowZoomChange(BYTE amount, bool fromDragResize);

/*********************************************************
*NAME:          windowGetZoomFactor
*AUTHOR:        John Morrison
*CREATION DATE: 21/1/99
*LAST MODIFIED: 21/1/99
*PURPOSE:
*  Gets the windows zoom factor
*
*ARGUMENTS:
*
*********************************************************/
BYTE windowGetZoomFactor(void);

/*********************************************************
*NAME:          windowGetSavedPosition / windowSetSavedPosition
*PURPOSE:
*  Get/set the saved window position for preferences.
*********************************************************/
void windowGetSavedPosition(int *x, int *y);
void windowSetSavedPosition(int x, int y);

/*********************************************************
*NAME:          windowGetCustomSize / windowSetCustomSize
*PURPOSE:
*  Get/set the saved custom window size for preferences.
*********************************************************/
void windowGetCustomSize(int *w, int *h);
void windowSetCustomSize(int w, int h);

/*********************************************************
*NAME:          windowSaveCurrentPosition
*PURPOSE:
*  Save the current window position to preferences state.
*********************************************************/
void windowSaveCurrentPosition(void);

/*********************************************************
*NAME:          windowComputeAspectCorrectSize
*PURPOSE:
*  Given actual window size (including menu bar), compute the
*  largest aspect-correct (SCREEN_SIZE_X:SCREEN_SIZE_Y) size
*  that fits within it. Returns corrected size and centered
*  position.
*********************************************************/
void windowComputeAspectCorrectSize(int actualW, int actualH, int actualX, int actualY,
                                     int *outW, int *outH, int *outX, int *outY);

/*********************************************************
*NAME:          windowGetDrawTime
*AUTHOR:        John Morrison
*CREATION DATE: 30/1/99
*LAST MODIFIED: 30/1/99
*PURPOSE:
*  Returns number of miliseconds spent drawing
*  last second
*
*ARGUMENTS:
*
*********************************************************/
int windowGetDrawTime(void);

/*********************************************************
*NAME:          windowGetNetTime
*AUTHOR:        John Morrison
*CREATION DATE: 23/2/99
*LAST MODIFIED: 23/2/99
*PURPOSE:
*  Returns number of miliseconds spent doing network work
*  last second
*
*ARGUMENTS:
*
*********************************************************/
int windowGetNetTime(void);

/*********************************************************
*NAME:          windowGetAiTime
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/99
*LAST MODIFIED: 26/11/99
*PURPOSE:
*  Returns number of miliseconds spent doing ai work
*  last second
*
*ARGUMENTS:
*
*********************************************************/
int windowGetAiTime(void);

/*********************************************************
*NAME:          windowGetSimTime
*AUTHOR:        John Morrison
*CREATION DATE: 30/1/99
*LAST MODIFIED: 30/1/99
*PURPOSE:
*  Returns number of miliseconds spent doing sim modelling
*  last second
*
*ARGUMENTS:
*
*********************************************************/
int windowGetSimTime(void);

/*********************************************************
*NAME:          windowGetKeys
*AUTHOR:        John Morrison
*CREATION DATE: 31/1/99
*LAST MODIFIED: 31/1/99
*PURPOSE:
* Gets a copy of the keys
*
*ARGUMENTS:
*  value - Pointer to hold the copy of the keys
*********************************************************/
void windowGetKeys(keyItems *value);

/*********************************************************
*NAME:          windowSetKeys
*AUTHOR:        John Morrison
*CREATION DATE: 31/1/99
*LAST MODIFIED: 31/1/99
*PURPOSE:
* Sets the keys to be that held in value
*
*ARGUMENTS:
*  value - Pointer to hold the copy of the keys
*********************************************************/
void windowSetKeys(keyItems *value);

/*********************************************************
*NAME:          windowSaveMap
*AUTHOR:        John Morrison
*CREATION DATE: 10/2/99
*LAST MODIFIED: 10/2/99
*PURPOSE:
* Handles saving of the map (Displaying the dialog
* box etc)
*
*ARGUMENTS:
*
*********************************************************/
void windowSaveMap(struct ClientSim *cs);

/*********************************************************
*NAME:          windowShowGameInfo
*AUTHOR:        John Morrison
*CREATION DATE: 14/2/99
*LAST MODIFIED: 14/2/99
*PURPOSE:
* Handles opening (preventing multiple openings) of the
* game info dialog box
*
*ARGUMENTS:
*  req - type of window open/close request
*********************************************************/
void windowShowGameInfo(windowShowRequest req);

/*********************************************************
*NAME:          windowShowSysInfo
*AUTHOR:        John Morrison
*CREATION DATE: 14/2/99
*LAST MODIFIED: 14/2/99
*PURPOSE:
* Handles opening (preventing multiple openings) of the
* sys info dialog box
*
*ARGUMENTS:
*  req - type of window open/close request
*********************************************************/
void windowShowSysInfo(windowShowRequest req);

/*********************************************************
*NAME:          windowShowSetPlayerName
*AUTHOR:        John Morrison
*CREATION DATE: 14/2/99
*LAST MODIFIED: 14/2/99
*PURPOSE:
* Handles opening (preventing multiple openings) of the
* set player name dialog box
*
*ARGUMENTS:
*  req - type of window open/close request
*********************************************************/
void windowShowSetPlayerName(windowShowRequest req);

/*********************************************************
*NAME:          windowShowSendMessages
*AUTHOR:        John Morrison
*CREATION DATE: 14/2/99
*LAST MODIFIED: 14/2/99
*PURPOSE:
* Handles opening (preventing multiple openings) of the
* send message dialog box
*
*ARGUMENTS:
*  req - type of window open/close request
*********************************************************/
void windowShowSendMessages(windowShowRequest req);

/*********************************************************
*NAME:          windowShowSetKeys
*AUTHOR:        John Morrison
*CREATION DATE: 14/2/99
*LAST MODIFIED: 14/2/99
*PURPOSE:
* Handles opening (preventing multiple openings) of the
* set keys dialog box
*
*ARGUMENTS:
*  req - type of window open/close request
*********************************************************/
void windowShowSetKeys(windowShowRequest req);

/*********************************************************
*NAME:          windowShowNetInfo
*AUTHOR:        John Morrison
*CREATION DATE: 3/3/99
*LAST MODIFIED: 3/3/99
*PURPOSE:
* Handles opening (preventing multiple openings) of the
* net info dialog box
*
*ARGUMENTS:
*  req - type of window open/close request
*********************************************************/
void windowShowNetInfo(windowShowRequest req);

/*********************************************************
*NAME:          windowRedrawAll
*AUTHOR:        John Morrison
*CREATION DATE: 24/3/99
*LAST MODIFIED: 24/3/99
*PURPOSE:
* We need to redraw all screen items. Do so here
*
*ARGUMENTS:
*
*********************************************************/
void windowRedrawAll(struct ClientSim *cs);

/*********************************************************
*NAME:          windowGetBackgroundSound
*AUTHOR:        John Morrison
*CREATION DATE: 27/3/99
*LAST MODIFIED: 27/3/99
*PURPOSE:
* Returns whether we are using background sound or not
*
*ARGUMENTS:
*
*********************************************************/
bool windowGetBackgroundSound(void);

/*********************************************************
*NAME:          windowDisableSound
*AUTHOR:        John Morrison
*CREATION DATE: 28/12/98
*LAST MODIFIED: 28/12/98
*PURPOSE:
* Disables the sound Effects Menu Entries.
*
*ARGUMENTS:
*
*********************************************************/
void windowDisableSound(void);

/*********************************************************
*NAME:          windowSoundKeepalive
*AUTHOR:        John Morrison
*CREATION DATE: 29/12/98
*LAST MODIFIED: 29/12/98
*PURPOSE:
* Switches the state of the Sound keepalive Menu Item
*
*ARGUMENTS:
*
*********************************************************/
void windowSoundKeepalive(void);

/*********************************************************
*NAME:          windowReCreate
*AUTHOR:        John Morrison
*CREATION DATE: 31/10/98
*LAST MODIFIED: 16/12/98
*PURPOSE:
*  Sets up the main window.
*
*ARGUMENTS:
*
*********************************************************/
void windowReCreate(void);

/*********************************************************
*NAME:          windowApplyMenuChecks
*AUTHOR:        John Morrison
*CREATION DATE: 31/10/98
*LAST MODIFIED: 16/12/98
*PURPOSE:
*  Applys our menu settings to the main window
*
*ARGUMENTS:
*
*********************************************************/
void windowApplyMenuChecks(struct ClientSim *cs);

/*********************************************************
*NAME:          windowShowAboutBox
*AUTHOR:        John Morrison
*CREATION DATE: 31/10/98
*LAST MODIFIED: 31/10/98
*PURPOSE:
* Creates and shows the about box
*
*ARGUMENTS:
*
*********************************************************/
void windowShowAboutBox(void);

/*********************************************************
*NAME:          windowShowAllianceRequest
*AUTHOR:        John Morrison
*CREATION DATE: 01/09/02
*LAST MODIFIED: 01/09/02
*PURPOSE:
* Returns if we should show this alliance request or not
*
*ARGUMENTS:
*********************************************************/
bool windowShowAllianceRequest(void);

/*********************************************************
*NAME:          windowAllowPlayerNameChange
*PURPOSE:
* Enable/disable the ability to change the player name.
*
*ARGUMENTS:
* allow - TRUE to allow name changes, FALSE to disallow
*********************************************************/
void windowAllowPlayerNameChange(bool allow);

#endif
