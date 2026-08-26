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
*Name:          Players
*Filename:      players.c
*Author:        John Morrison
*Creation Date: 18/02/99
*Last Modified: 05/05/01
*Purpose:
*  Looks after players. Alliences between etc.
*********************************************************/

#ifndef _PLAYERS_H
#define _PLAYERS_H
#include "lv_global.h"

#include "lv_allience.h"
#include "lv_screentank.h"
#include "lv_screenlgm.h"

/* Defines */

#define NO_TANK "???\0"

/* Mirror of PLAYER_FLAG_BOT from src/bolo/public/player_flags.h. Logviewer
 * does not include the bolo header (its own `player` / `players` types
 * collide with the bolo ones), so the bit value is duplicated here. */
#define LV_PLAYER_FLAG_BOT 0x20

/* Area tanks must be in to be selected in the 'select nearby' option */
#define PLAYER_MAX_SELECT_LEFT -9
#define PLAYER_MAX_SELECT_RIGHT 9
#define PLAYER_MAX_SELECT_TOP -9
#define PLAYER_MAX_SELECT_BOTTOM 9

/* Brain Stuff */
#define PLAYERS_BRAIN_OBJECT_TYPE_TANK 0
#define PLAYERS_BRAIN_OBJECT_TYPE_LGM 4
#define PLAYERS_BRAIN_OBJECT_TYPE_PARA 5
#define PLAYERS_BRAIN_NEUTRAL 2
#define PLAYERS_BRAIN_HOSTILE 1
#define PLAYERS_BRAIN_FRIENDLY 0

#define NO_TEAM_SET 0xFF

/* The different types of games there are */
typedef struct {
  bool inUse;                       /* Is player slot in use? */
  char playerName[PLAYER_NAME_LEN]; /* Player name */
  char location[512];   /* Location field */
  allience allie;                   /* Alliences this player has */
  BYTE mapX;                        /* Map X and Y co-ordinates */
  BYTE mapY;
  BYTE pixelX;                      /* Pixel X and Y co-ordinates */
  BYTE pixelY;
  BYTE frame;                       /* Animation frame */
  bool onBoat;                      /* Is this player on a boat ? */
  /* LGM Stuff */
  BYTE lgmMapX;                     /* LGM Map X and Y positions */
  BYTE lgmMapY;
  BYTE lgmPixelX;                   /* LGM Pixel X and Y positions */
  BYTE lgmPixelY;
  BYTE lgmFrame;                    /* LGM Frame */
  /* Server emits log_LgmLocation only while the LGM is out, so a
   * per-tick wipe followed by per-event set yields presence. lgmIsDead
   * is sticky from log_LostMan until the next log_LgmLocation. */
  bool lgmIsOut;
  bool lgmIsDead;
  /* Misc */
  bool isChecked;                   /* Is this item checked */
  bool needUpdate;
  BYTE team;
  /* Per-player account flags (v1 logs only). Bit 0 = WBN, bit 1 = Steam,
   * bit 5 = bot. Mirrors PLAYER_FLAG_WBN_VERIFIED / PLAYER_FLAG_WBN_STEAM_LINKED /
   * PLAYER_FLAG_BOT from src/bolo/public/player_flags.h. */
  BYTE accountFlags;
} player;

/* Array of all the players */
typedef struct {
  player item[MAX_TANKS];
} players;



/*********************************************************
*NAME:          lv_playersCreate
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 26/11/99
*PURPOSE:
* Sets up the players structure.
*
*ARGUMENTS:
*
*********************************************************/
void lv_playersCreate();

/*********************************************************
*NAME:          lv_playersDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Destroys the playesr structure
*
*ARGUMENTS:
*
*********************************************************/
void lv_playersDestroy();

/*********************************************************
*NAME:          lv_playersGetSelf
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Returns your own player number
*
*ARGUMENTS:
*
*********************************************************/
BYTE lv_playersGetSelf();

void lv_playersSetSelf(BYTE pn);
  

/*********************************************************
*NAME:          lv_playersSetPlayerName
*AUTHOR:        John Morrison
*CREATION DATE: 18/02/99
*LAST MODIFIED: 05/05/01
*PURPOSE:
* Sets/changes a player name. Returns whether the operation
* succeed or not. (Fails if name is already in use) If it
* sccueeds then it makes the appropriate anouncement
*
*ARGUMENTS:
*  playerNum  - The player number to set
*  playerName - The player name to set
*********************************************************/
bool lv_playersSetPlayerName(BYTE playerNum, char *playerName);

/*********************************************************
*NAME:          lv_playersSetPlayerNameQuiet
*AUTHOR:        John Morrison
*CREATION DATE: 27/06/26
*LAST MODIFIED: 27/06/26
*PURPOSE:
* Marks a slot in use and sets its name without emitting a
* MESSAGE_CHANGENAME newswire line. Touches only inUse +
* playerName. Used by spectator per-keyframe roster apply.
*
*ARGUMENTS:
*  playerNum  - The player number to set
*  name       - The player name to set
*********************************************************/
void lv_playersSetPlayerNameQuiet(BYTE playerNum, const char *name);

/*********************************************************
*NAME:          lv_playersSetPlayer
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 26/11/99
*PURPOSE:
* Sets a player up.
*
*ARGUMENTS:
*  playerNum  - The player number to set
*  playerName - The player name to set
*  location   - Location string of the player.
*  mx         - Map X location
*  my         - Map Y location
*  px         - Pixel X location
*  py         - Pixel Y location
*  frame      - Tank animation frame.
*  onBoat     - is the player on a boat?
*  numAllies  - Number of Allies the player has
*  allies     - BYTE buffer containing each allie
*  announce   - Do we want to announce this fact?
*  override   - Override if the game is fastfowarding
*********************************************************/
void lv_playersSetPlayer(BYTE playerNum, char *playerName, char *location, BYTE mx, BYTE my, BYTE px, BYTE py, BYTE frame, bool onBoat, BYTE numAllies, BYTE *allies, bool announce, bool override, BYTE accountFlags);

void lv_playersUpdateTank(BYTE playerNum, BYTE mx, BYTE my, BYTE px, BYTE py, BYTE frame, bool onBoat);


void lv_playersUpdateLgm(BYTE playerNum, BYTE lgmMX, BYTE lgmMY, BYTE lgmPX, BYTE lgmPY, BYTE lgmFrame);

/*********************************************************
*NAME:          lv_playersGetPlayerName
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Gets a player name.
*
*ARGUMENTS:
*  playerNum  - The player number to set
*  dest       - Destination string
*  destSize   - Size of dest in bytes, including the NUL. Longer names
*               are truncated rather than overrunning the caller.
*********************************************************/
void lv_playersGetPlayerName(BYTE playerNum, char *dest, size_t destSize);

/*********************************************************
*NAME:          lv_playersMakeMessageName
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Makes the message name for a specific player
*
*ARGUMENTS:
*  playerNum  - The player number to set
*  dest       - Destination string
*********************************************************/
void lv_playersMakeMessageName(BYTE playerNum, char *dest);

/*********************************************************
*NAME:          lv_playersMakeScreenName
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Makes the message name for a specific player
*
*ARGUMENTS:
*  playerNum  - The player number to set
*  dest       - Destination string
*********************************************************/
void lv_playersMakeScreenName(BYTE playerNum, char *dest);

/*********************************************************
*NAME:          lv_playersIsAllie
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Returns whether playerA is allied to playerB
*
*ARGUMENTS:
*  playerA - The player number to check
*  playerB  - The player number to check
*********************************************************/
bool lv_playersIsAllie(BYTE playerA, BYTE playerB);

/*********************************************************
*NAME:          lv_playersGetNumAllie
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Returns the number allies a player has. (Atleast 1 as it
* includes themselves)
*
*ARGUMENTS:
*  playerNum - The player number to check
*********************************************************/
BYTE lv_playersGetNumAllie(BYTE playerNum);

/*********************************************************
*NAME:          lv_playersScreenAllience
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Returns a player is screen allience type.
*
*ARGUMENTS:
*  playerNum - Player number to check
*********************************************************/
tankAlliance lv_playersScreenAllience(BYTE playerNum);

/*********************************************************
*NAME:          lv_playersMakeScreenTanks
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED:  8/1/00
*PURPOSE:
* Adds each player to the screen tanks structure
*
*ARGUMENTS:
*  value    - screen tanks data structure
*  leftPos  - Left bound
*  rightPos - Left bound
*  top      - top bound
*  bottom   - Bottom bound
*********************************************************/
void lv_playersMakeScreenTanks(screenTanks *value, BYTE leftPos, BYTE rightPos, BYTE top, BYTE bottom);

/*********************************************************
*NAME:          lv_playersMakeScreenLgm
*AUTHOR:        John Morrison
*CREATION DATE: 19/2/99
*LAST MODIFIED:  7/3/99
*PURPOSE:
* Adds each players lgm to the screen LGM structure
*
*ARGUMENTS:
*  value    - screen LGM data structure
*  leftPos  - Left bound
*  rightPos - Left bound
*  top      - top bound
*  bottom   - Bottom bound
*********************************************************/
void lv_playersMakeScreenLgm(screenLgm *value, BYTE leftPos, BYTE rightPos, BYTE top, BYTE bottom);

/*********************************************************
*NAME:          lv_playersGetNumPlayers
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Returns the number of players in the game
*
*ARGUMENTS:
*
*********************************************************/
BYTE lv_playersGetNumPlayers();

/*********************************************************
*NAME:          lv_playersLeaveGame
*AUTHOR:        John Morrison
*CREATION DATE: 20/3/99
*LAST MODIFIED: 2/11/99
*PURPOSE:
* A player has left the game.
*
*ARGUMENTS:
*  playerNum - The number of the player that has left
*  announce  - Whether to announce this or not
*********************************************************/
void lv_playersLeaveGame(BYTE playerNum, bool announce);

/*********************************************************
*NAME:          lv_playersIsInUse
*AUTHOR:        John Morrison
*CREATION DATE: 31/8/99
*LAST MODIFIED: 31/8/99
*PURPOSE:
* Returns whether a player Number is in use
*
*ARGUMENTS:
*  playerNum - The player num to check
*********************************************************/
bool lv_playersIsInUse(BYTE playerNumber);

/* Returns true when the slot's PLAYER_FLAG_BOT bit is set (brain-driven
 * player, not a human). FALSE for unused slots and for human players. */
bool lv_playersIsBot(BYTE playerNumber);

/*********************************************************
*NAME:          lv_playersGetLgmDetails
*AUTHOR:        John Morrison
*CREATION DATE: 31/8/99
*LAST MODIFIED: 31/8/99
*PURPOSE:
* Gets the LGM details for a player
*
*ARGUMENTS:
*  playerNum - The player num to check
*  mx        - LGM Map X Position
*  my        - LGM Map Y Position
*  px        - LGM Pixel X Position
*  py        - LGM Pixel Y Position
*  frame     - LGM Frame
*********************************************************/
void lv_playersGetLgmDetails(BYTE playerNumber, BYTE *mx, BYTE *my , BYTE *px, BYTE *py, BYTE *frame);

/*********************************************************
*NAME:          lv_playersGetTankDetails
*AUTHOR:        John Morrison
*CREATION DATE: 31/8/99
*LAST MODIFIED: 31/8/99
*PURPOSE:
* Gets the tank details for a player
*
*ARGUMENTS:
*  playerNum - The player num to check
*  mx        - Tank Map X Position
*  my        - Tank Map Y Position
*  px        - Tank Pixel X Position
*  py        - Tank Pixel Y Position
*  frame     - Tank Frame
*  onBoat    - Whether the tank is on a boat
*********************************************************/
void lv_playersGetTankDetails(BYTE playerNumber, BYTE *mx, BYTE *my, BYTE *px, BYTE *py, BYTE *frame, bool *onBoat);


/*********************************************************
*NAME:          lv_playersLeaveAlliance
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED: 1/11/99
*PURPOSE:
* Process a leave alliance request.
*
*ARGUMENTS:
*  playerNum - Player number that is leaving the alliance
*********************************************************/
void lv_playersLeaveAlliance(BYTE playerNum);

/*********************************************************
*NAME:          lv_playersAcceptAlliance
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED:  4/7/00
*PURPOSE:
* A player has been accepted into an alliance
*
*ARGUMENTS:
*  acceptedBy - Who accepted them in
*  newMember  - Who the new member is
*********************************************************/
void lv_playersAcceptAlliance(BYTE acceptedBy, BYTE newMember);

void lv_playersLgmZero();

/* Marks the LGM dead for the given slot. Cleared by the next
 * lv_playersUpdateLgm (i.e. the LGM has respawned and is out again). */
void lv_playersSetLgmDead(BYTE playerNum);

/* Reports the camera-display state of an LGM. Both flags are FALSE for
 * unused slots. isOut means a log_LgmLocation arrived in the current
 * tick batch; isDead means a log_LostMan arrived since the last
 * log_LgmLocation for this slot. */
void lv_playersGetLgmStatus(BYTE playerNum, bool *isOut, bool *isDead);

bool lv_playersChooseView(int x, int y);

BYTE lv_playersGetCentredX();
BYTE lv_playersGetCentredY();

/* Centred tank in native pixels (square * TILE_SIZE + sub-tile pixel), -1 when
 * it has no usable position. The BYTE pair above discards the sub-tile part,
 * which is what limits a follow camera to whole-tile steps. */
int lv_playersGetCentredPixelX(void);
int lv_playersGetCentredPixelY(void);

/* Aim the follow camera at a player by name (the only key shared between the
 * lobby's slots and the log's players). False when nobody matches or the match
 * has no tank on the map at this point in the replay. */
bool lv_playersSetViewByName(const char *name);
BYTE lv_playersGetTeamId(BYTE playerNum);
BYTE lv_playersGetUnusedTeam(BYTE playerNum);
BYTE lv_playersGetTeamForOwner(BYTE owner);
void lv_playersSetTeams(BYTE *pTeams);
void lv_playersCopyPTeams(BYTE *dest);

#endif /* _PLAYERS_H */
