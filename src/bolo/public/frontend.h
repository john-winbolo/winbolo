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
*Name:          FrontEnd
*Filename:      frontEnd.h
*Author:        John Morrison
*Creation Date: 3/1/99
*Last Modified: 4/1/00
*Purpose:
*  Functions called by the back end
*********************************************************/

#ifndef _FRONTEND_H
#define _FRONTEND_H

#include "global.h"
#include "screentank.h"
#include "screenbullet.h"
#include "screenlgm.h"
#include "types.h"
#include "../../gui/lang.h"
#include "viewport_types.h"  /* screen, screenMines, screenGunsight */
#include "client_enums.h"    /* sndEffects, playerNumbers */
#include "alliance_enums.h"


/*********************************************************
*NAME:          frontEndUpdateTankStatusBars
*AUTHOR:        John Morrison
*CREATION DATE: 28/12/98
*LAST MODIFIED: 28/12/98
*PURPOSE:
* Function is called when the tanks status bars need to 
* be updated
*
*ARGUMENTS:
*  shells  - Number of shells
*  mines   - Number of mines
*  armour  - Amount of armour
*  trees   - Amount of trees
*********************************************************/
void frontEndUpdateTankStatusBars(struct ClientSim *cs, BYTE shells, BYTE mines, BYTE armour, BYTE trees);

/*********************************************************
*NAME:          frontEndUpdateBaseStatusBars
*AUTHOR:        John Morrison
*CREATION DATE: 11/1/99
*LAST MODIFIED: 11/1/99
*PURPOSE:
* Function is called when the tanks status bars need to 
* be updated
*
*ARGUMENTS:
*  shells  - Number of shells
*  mines   - Number of mines
*  armour  - Amount of armour
*********************************************************/
void frontEndUpdateBaseStatusBars(struct ClientSim *cs, BYTE shells, BYTE mines, BYTE armour);

/*********************************************************
*NAME:          frontEndSoundEffects
*AUTHOR:        John Morrison
*CREATION DATE: 28/12/98
*LAST MODIFIED: 28/12/98
*PURPOSE:
*  Plays a sound effect if it is turned on
*
*ARGUMENTS:
*  value - The sound effect to play
*********************************************************/
void frontEndPlaySound(struct ClientSim *cs, sndEffects value);

/*********************************************************
*NAME:          frontEndDrawMainScreen
*AUTHOR:        John Morrison
*CREATION DATE: 31/10/98
*LAST MODIFIED: 18/11/99
*PURPOSE:
*  Updates the Window View
*
*ARGUMENTS:
*  value      - Pointer to the sceen structure
*  mineView   - Pointer to the screen mine structure
*  tks        - Pointer to the screen tank structure
*  gs         - pointer to the screen gunsight structure
*  lgms       - Pointer to the screen builder structure
*  srtDelay   - Start delay. If this is greater then 0
*               Then the delay screen should be drawn
*  isPillView - TRUE if we are in pillbox view
*  tank       - Pointer to the player's tank structure
*  edgeX      - X Offset for smooth scrolling
*  edgeY      - Y Offset for smooth scrolling
*********************************************************/
void frontEndDrawMainScreen(struct ClientSim *cs, screen *value, screenMines *mineView, screenTanks *tks, screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms, int32_t srtDelay, bool isPillView, tank *tank, int edgeX, int edgeY);

/*********************************************************
*NAME:          frontEndStatusPillbox
*AUTHOR:        John Morrison
*CREATION DATE:  3/1/99
*LAST MODIFIED:  3/1/99
*PURPOSE:
*  Sets the pillbox status for a particular pillbox
*
*ARGUMENTS:
*  pillNum - The base number to draw (1-16)
*  pa      - The allience of the pillbox
*********************************************************/
void frontEndStatusPillbox(struct ClientSim *cs, BYTE pillNum, pillAlliance pb);

/*********************************************************
*NAME:          frontEndStatusTank
*AUTHOR:        John Morrison
*CREATION DATE: 26/1/99
*LAST MODIFIED: 26/1/99
*PURPOSE:
*  Sets the tank status for a particular tank
*
*ARGUMENTS:
*  tankNum - The tank number to draw (1-16)
*  ts      - The allience of the tank
*********************************************************/
void frontEndStatusTank(struct ClientSim *cs, BYTE tankNum, tankAlliance ts);

/*********************************************************
*NAME:          frontEndStatusBase
*AUTHOR:        John Morrison
*CREATION DATE: 10/1/99
*LAST MODIFIED: 10/1/99
*PURPOSE:
*  Sets the base status for a particular base
*
*ARGUMENTS:
*  pillNum - The base number to draw (1-16)
*  bs      - The allience of the pillbox
*********************************************************/
void frontEndStatusBase(struct ClientSim *cs, BYTE baseNum, baseAlliance bs);

/*********************************************************
*NAME:          frontEndMessages
*AUTHOR:        John Morrison
*CREATION DATE:  3/1/99
*LAST MODIFIED:  3/1/99
*PURPOSE:
*  The messages must be drawn on the screen
*
*ARGUMENTS:
*  top    - The top line to write
*  bottom - The bottom line to write
*********************************************************/
void frontEndMessages(struct ClientSim *cs, char *top, char *bottom);

/*********************************************************
*NAME:          frontEndKillsDeaths
*AUTHOR:        John Morrison
*CREATION DATE:  8/1/99
*LAST MODIFIED:  8/1/99
*PURPOSE:
*  The tank kills/deaths must be updated
*
*ARGUMENTS:
*  kills  - The number of kills the tank has.
*  deaths - The number of times the tank has died
*********************************************************/
void frontEndKillsDeaths(struct ClientSim *cs, int kills, int deaths);

/*********************************************************
*NAME:          frontEndManStatus
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 18/1/99
*PURPOSE:
*  The tank kills/deaths must be updated
*
*ARGUMENTS:
*  isDead - Is the man dead
*  angle  - The angle the man is facing
*********************************************************/
void frontEndManStatus(struct ClientSim *cs, bool isDead, TURNTYPE angle);

/*********************************************************
*NAME:          frontEndManClear
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 18/1/99
*PURPOSE:
*  Clears the man status (ie man is in tank)
*
*ARGUMENTS:
*
*********************************************************/
void frontEndManClear(struct ClientSim *cs);

/*********************************************************
*NAME:          frontEndGameOver
*AUTHOR:        John Morrison
*CREATION DATE: 29/1/99
*LAST MODIFIED: 29/1/99
*PURPOSE:
*  Time limit is up. The game is over
*
*ARGUMENTS:
*
*********************************************************/
void frontEndGameOver(struct ClientSim *cs);

/*********************************************************
*NAME:          frontEndClearPlayer
*AUTHOR:        John Morrison
*CREATION DATE: 4/2/99
*LAST MODIFIED: 4/2/99
*PURPOSE:
* Clears a player name from the menu and disables it.
* 
*ARGUMENTS:
*  value - The player number to clear
*********************************************************/
void frontEndClearPlayer(struct ClientSim *cs, playerNumbers value);

/*********************************************************
*NAME:          frontEndSetPlayer
*AUTHOR:        John Morrison
*CREATION DATE: 4/2/99
*LAST MODIFIED: 4/2/99
*PURPOSE:
* Sets a player name in the menu and enables it.
*
*ARGUMENTS:
*  value       - The player number to set
*  str         - String identifier of the name
*  countryCode - 2-letter country code (e.g. "US"), or empty
*  ping        - Player ping in ms, 0 = unknown
*  clientType  - ClientType enum (CLIENT_TYPE_*, see player_flags.h)
*  clientFlags - PLAYER_FLAG_* bits (see player_flags.h)
*********************************************************/
void frontEndSetPlayer(struct ClientSim *cs, playerNumbers value, char *str, const char *countryCode, uint16_t ping, uint8_t clientType, uint8_t clientFlags);

/*********************************************************
*NAME:          frontEndDrawDownload
*AUTHOR:        John Morrison
*CREATION DATE: 27/3/99
*LAST MODIFIED: 13/12/99
*PURPOSE:
* A screen redraw request has been made but we are still
* downloading network data. Draw progress line instead.
* 
*ARGUMENTS:
*  justBlack - TRUE if we want just a black screen
*********************************************************/
void frontEndDrawDownload(struct ClientSim *cs, bool justBlack);

/*********************************************************
*NAME:          frontEndSetPlayerCheckState
*AUTHOR:        John Morrison
*CREATION DATE: 27/3/99
*LAST MODIFIED: 27/3/99
*PURPOSE:
* Checks/unchecks a player
* 
*ARGUMENTS:
* value     - The player number
* isChecked - Is the item checked
*********************************************************/
void frontEndSetPlayerCheckState(struct ClientSim *cs, playerNumbers value, bool isChecked);

/*********************************************************
*NAME:          frontEndSetActiveClientSim
*PURPOSE:
* Register which ClientSim drives the visible player-list UI.
* Subsequent frontEnd* calls whose cs argument doesn't match the
* registered active cs are suppressed as no-ops, preventing bot
* / bg_game / gym ClientSims from polluting process-global UI
* state. NULL means "no active UI cs registered yet" — calls
* fall through (preserves pre-registration bootstrapping).
*
*ARGUMENTS:
*  cs - The ClientSim that owns the visible UI (usually humanSim),
*       or NULL to clear the registration.
*********************************************************/
void frontEndSetActiveClientSim(struct ClientSim *cs);

/*********************************************************
*NAME:          frontEndEnableRequestAllyMenu
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED: 1/11/99
*PURPOSE:
* Request to enable/disable the request alliance menu
* item
* 
*ARGUMENTS:
*  enabled - TRUE for enabled/FALSE for diabled
*********************************************************/
void frontEndEnableRequestAllyMenu(bool enabled);

/*********************************************************
*NAME:          frontEndEnableRequestMenu
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED: 1/11/99
*PURPOSE:
* Request to enable/disable the leave alliance menu item
* 
*ARGUMENTS:
*  enabled - TRUE for enabled/FALSE for diabled
*********************************************************/
void frontEndEnableLeaveAllyMenu(bool enabled);

/*********************************************************
*NAME:          frontEndShowGunsight
*AUTHOR:        John Morrison
*CREATION DATE: 4/1/00
*LAST MODIFIED: 4/1/00
*PURPOSE:
* Set the front end whether the gunsight is visible or not
* (Called by auto show/hide gunsight being triggered)
* 
*ARGUMENTS:
*  isShown - Is the gunsight shown or not
*********************************************************/
void frontEndShowGunsight(struct ClientSim *cs, bool isShown);

/*********************************************************
*NAME:          frontEndRedrawAll
*AUTHOR:        Minhiriath
*CREATION DATE: 16/3/2009
*LAST MODIFIED: 16/3/2009
*PURPOSE:
*  called windowRedrawAll()
* 
*ARGUMENTS:
*  none
*********************************************************/
void frontEndRedrawAll(struct ClientSim *cs);

/*********************************************************
*NAME:          frontEndTutorial
*AUTHOR:        John Morrison
*CREATION DATE: 27/5/00
*LAST MODIFIED: 27/5/00
*PURPOSE:
* Set the front end that we have reached a new position
* 
*ARGUMENTS:
*  pos - Y Position on the map
*********************************************************/
bool frontEndTutorial(BYTE pos);

/* Reset the tutorial step sequencer. Call at the start of each new
 * tutorial run so a previously-completed run doesn't leave the
 * sequence past its end. */
void frontEndTutorialReset(void);

#endif /* _FRONT_END_H */
