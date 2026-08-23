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
*Name:          BackEnd
*Filename:      BackEnd.h
*Author:        John Morrison
*Creation Date: 25/11/99
*Last Modified: 21/01/01
*Purpose:
*  Functions called by the front end
*********************************************************/

#ifndef _BACKEND_H
#define _BACKEND_H

#include "lv_global.h"
#include "lv_screenlgm.h"
#include "lv_screenbullet.h"
#include "lv_pillbox.h"
#include "lv_screentank.h"

/* Defines */

/* Defines the screen sizes */
#define MAIN_SCREEN_SIZE_X 15 /*16 is bolo window size  times 16 pixels */
#define MAIN_SCREEN_SIZE_Y 15 /*18 is bolo window size  times 16 pixels */

#define SCREEN_SIZE_X 30 /*16 is bolo window size */
#define SCREEN_SIZE_Y 30 /*18 is bolo window size */


/* Size of the back buffer */
#define MAIN_BACK_BUFFER_SIZE_X (MAIN_SCREEN_SIZE_X + 2)
#define MAIN_BACK_BUFFER_SIZE_Y (MAIN_SCREEN_SIZE_Y + 2)
/* The game timer is 20 milliseconds between events */
#define GAME_TICK_LENGTH 10 /* 20 */
#define GAME_NUMTOTALTICKS_SEC (1000 / GAME_TICK_LENGTH);
#define GAME_NUMGAMETICKS_SEC (1000 / 20);


/* The different types of games there are */


#ifndef _LABELLEN_ENUM
#define _LABELLEN_ENUM

typedef enum {
  lblNone,
  lblShort,
  lblLong
} labelLen;

#endif 

#ifndef _UPDATETYPE_ENUM
#define _UPDATETYPE_ENUM
typedef enum {
  left,
  right,
  up,
  down,
  redraw
} updateType;

#endif

#ifndef _SNDEFFECTS_ENUM
#define _SNDEFFECTS_ENUM
typedef enum {
  shootSelf,
  shootNear,
  shotTreeNear,
  shotTreeFar,
  shotBuildingNear,
  shotBuildingFar,
  hitTankNear,
  hitTankFar,
  hitTankSelf,
  bubbles,
  tankSinkNear,
  tankSinkFar,
  bigExplosionNear,
  bigExplosionFar,
  farmingTreeNear,
  farmingTreeFar,
  manBuildingNear,
  manBuildingFar,
  manDyingNear,
  manDyingFar,
  manLayingMineNear,
  mineExplosionNear,
  mineExplosionFar,
  shootFar
} sndEffects;

#endif

#ifndef _AITYPE_ENUM
#define _AITYPE_ENUM

typedef enum {
  aiNone,
  aiYes,
  aiYesAdvantage
} aiType;

#endif 

#ifndef _GAMETYPE_ENUM
#define _GAMETYPE_ENUM

typedef enum {
  gameOpen = 1,
  gameTournament,
  gameStrictTournament
} gameType;

#endif

#ifndef BASES_H
typedef enum {
  baseDead,
  baseOwnGood,
  baseAllieGood,
  baseNeutral,
  baseEvil
} baseAlliance;
#endif

#ifndef PILLBOX_H
typedef enum {
  pillDead,
  pillAllie,
  pillGood,
  pillNeutral,
  pillEvil,
  pillTankGood,
  pillTankAllie,
  pillTankEvil
} pillAlliance;
#endif



/* Type definitions */
/* The screen object - Details what tiles are on the screen*/
typedef struct screenObj *screen;
struct screenObj {
  BYTE *screenItem;
//  BYTE screenItem[MAIN_BACK_BUFFER_SIZE_X][MAIN_BACK_BUFFER_SIZE_Y];
};
/* Screen Mines - Array of boolean values that report whether
a tile on the screen whold have a mine on it*/
typedef struct screenMineObj *screenMines;
struct screenMineObj {
//  bool mineItem[MAIN_BACK_BUFFER_SIZE_X][MAIN_BACK_BUFFER_SIZE_Y];
    bool *mineItem;
};

/* Defines the gunsight position on the screen */
/* If turned off mapX is set to NO_GUNSIGHT */
typedef struct {
  int mapX;
  BYTE mapY;
  BYTE pixelX;
  BYTE pixelY;
} screenGunsight;


/* Button Pressed - These are the valid items 
   that should be passed to this module*/
#ifndef _UPDATETYPE_ENUM
#define _UPDATETYPE_ENUM
typedef enum {
  left,
  right,
  up,
  down,
  redraw
} updateType;

#endif

#ifndef _BUILDSELECT_ENUM
#define _BUILDSELECT_ENUM

/* The type of building operation currently being selected */
typedef enum {
  BsTrees,
  BsRoad,
  BsBuilding,
  BsPillbox,
  BsMine
} buildSelect;
#endif


#ifndef _PLAYERNUMBERS_ENUM 
#define _PLAYERNUMBERS_ENUM 
/* Player Numbers */
typedef enum {
  player01,
  player02,
  player03,
  player04,
  player05,
  player06,
  player07,
  player08,
  player09,
  player10,
  player11,
  player12,
  player13,
  player14,
  player15,
  player16
} playerNumbers;

#endif
/* Prototypes */

/*********************************************************
*NAME:          lv_screenSetup
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 26/1/99
*PURPOSE:
*  Sets up all the variables - Should be run when the
*  program starts.
*
*ARGUMENTS:
*  game - The game type-Open/tournament/strict tournament
*  hiddenMines - Are hidden mines allowed
*  srtDelay    - Game start delay (50th second increments)
*  gmeLen      - Length of the game (in 50ths)
*                (-1 =unlimited)
*********************************************************/
void lv_screenSetup();

/*********************************************************
*NAME:          lv_screenDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Destroys the structures Should be called on
*  program exit
*
*ARGUMENTS:
*
*********************************************************/
void lv_screenDestroy();

/*********************************************************
*NAME:          lv_screenUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Updates the screen. Takes numerous directions
*
*ARGUMENTS:
*  value - Pointer to the screen structure
*********************************************************/
void lv_screenUpdate(updateType value);

/*********************************************************
*NAME:          lv_screenGetPos
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Gets the value of a square in the structure
*  Return DEEP_SEA if out of range
*
*ARGUMENTS:
*  value  - Pointer to the screen structure
*  xValue - The X co-ordinate
*  yValue - The Y co-ordinate
*********************************************************/
BYTE lv_screenGetPos(screen *value,BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          lv_screenIsMine
*AUTHOR:        John Morrison
*CREATION DATE: 6/11/98
*LAST MODIFIED: 6/11/98
*PURPOSE:
*  Returns if a square on the screen should have a mine
*  drawn on it.
*  If value is out of range returns FALSE
*
*ARGUMENTS:
*  value  - Pointer to the screenMines structure
*  xValue - The X co-ordinate
*  yValue - The Y co-ordinate
*********************************************************/
bool lv_screenIsMine(screenMines *value,BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          lv_screenLoadMap
*AUTHOR:        John Morrison
*CREATION DATE: 29/10/98
*LAST MODIFIED: 12/12/99
*PURPOSE:
*  Loads a map. Returns if it was sucessful reading the
*  map or not.
*
*ARGUMENTS:
* fileName - File name and path to map to open
*  game - The game type-Open/tournament/strict tournament
*  hiddenMines - Are hidden mines allowed
*  srtDelay    - Game start delay (50th second increments)
*  gmeLen      - Length of the game (in 50ths) 
*                (-1 =unlimited)
*  playerName  - Name of the player
*  wantFree    - Should we free the backend after loading
*                Usually TRUE if you only want to check
*                if a map is valid
*********************************************************/
bool lv_screenLoadMap(char *fileName, int memoryBufferSize);

/*********************************************************
*NAME:          lv_screenLoadMapFromMemory
*PURPOSE:
*  Loads a log from an in-memory zip buffer. Takes ownership
*  of zipData (freed when the log is closed).
*  Returns TRUE on success.
*
*ARGUMENTS:
* zipData - Heap-allocated zip file data (ownership transferred)
* zipLen  - Size of zipData in bytes
*********************************************************/
bool lv_screenLoadMapFromMemory(uint8_t *zipData, size_t zipLen);

/* Loads the decoder from a plaintext (v2) byte stream: the caller supplies the
 * header + opening snapshot bytes, then appends more via lv_blocksAppendBytes
 * and steps lv_screenLogTick. Returns TRUE on success. */
bool lv_screenLoadFromStream(const uint8_t *bytes, size_t len);

/* Feeds a live, append-only byte stream into the decoder: appends the
 * caller-supplied newly-arrived bytes, then advances playback over the whole
 * records that are now fully buffered and parks when caught up. Appends must be
 * record-aligned (whole records, as the ring/translator emit) so a tick never
 * reads past the buffer. Never finishes on "caught up" (a live stream carries
 * no LOG_QUIT). Returns the decoder's isPlaying state. */
bool lv_screenStreamPump(const uint8_t *bytes, size_t len);

/* Game-info the synthesized spectator-seed header needs. Kept POD and free of
 * bolo/sim headers so this seam stays includable in logviewer-world TUs; the
 * live values arrive with the spectate-start handshake. mapName may be NULL
 * (treated as empty). */
typedef struct {
  const char *mapName;
  BYTE gameType;
  BYTE allowHiddenMines;
  BYTE ai;
  BYTE usePassword;
  BYTE maxPlayers;
  BYTE versionMajor;
  BYTE versionMinor;
  BYTE versionRevision;
} LvSpecSeedInfo;

/* Loads a raw spectator-ring keyframe seed blob
 * ([u32 bodyLen BE][world body][u32 ctrlLen BE][control snapshot]) into the
 * decoder: synthesizes the v2 header from info, translates the keyframe and
 * feeds header + opening snapshot to lv_screenLoadFromStream. The keyframe's
 * control-snapshot slice is copied into a module-owned buffer for a later HUD
 * consumer (lv_specSeedControl), not written to the decoded stream. The decoder
 * must already be created (lv_decoderCreate) and sized (lv_screenSetSizeX/Y).
 * Returns TRUE on success. Defined in src/logviewer/spec_seed_load.c. */
bool lv_specSeedLoad(const LvSpecSeedInfo *info, const uint8_t *seed,
                     size_t seedLen);

/* Returns the control-snapshot slice stashed by the most recent successful
 * lv_specSeedLoad, or NULL if none was stashed. *outLen (may be NULL) receives
 * its length. The buffer is module-owned and valid until the next
 * lv_specSeedLoad or lv_specSeedControlClear. */
const uint8_t *lv_specSeedControl(size_t *outLen);

/* Releases the stashed control-snapshot slice (idempotent). */
void lv_specSeedControlClear(void);

/* Translates one forward spectator-ring record and pumps it into the decoder so
 * the delayed view advances. isKeyframe selects the translation: a keyframe
 * payload ([u32 bodyLen BE][world body][u32 ctrlLen BE][control snapshot]) ->
 * [LOG_EVENT_SNAPSHOT][body] (the decoder re-syncs) and its control slice
 * refreshes the stash (lv_specSeedControl); an event-tick payload (concatenated
 * [type][u16 BE len][body] events, or empty) -> LOG_EVENT/LOG_EVENT_LONG/
 * LOG_NOEVENTS bytes. The translated bytes are appended via lv_screenStreamPump.
 * The decoder must already be seeded (lv_specSeedLoad). Returns the decoder's
 * isPlaying state, or FALSE on a malformed record. Defined in
 * src/logviewer/spec_seed_load.c. */
bool lv_specRecordPump(bool isKeyframe, const uint8_t *payload, size_t len);

/*********************************************************
*NAME:          lv_screenNumBases
*AUTHOR:        John Morrison
*CREATION DATE: 21/12/98
*LAST MODIFIED: 21/12/98
*PURPOSE:
*  Returns the number of bases
*
*ARGUMENTS:
*
*********************************************************/
BYTE lv_screenNumBases(void);

/*********************************************************
*NAME:          lv_screenNumPills
*AUTHOR:        John Morrison
*CREATION DATE: 21/12/98
*LAST MODIFIED: 21/12/98
*PURPOSE:
*  Returns the number of pillboxes
*
*ARGUMENTS:
*
*********************************************************/
BYTE lv_screenNumPills(void);

/*********************************************************
*NAME:          lv_screenGetMapName
*AUTHOR:        John Morrison
*CREATION DATE: 26/1/99
*LAST MODIFIED: 26/1/99
*PURPOSE:
* The front end eants to know what the map name is.
* Make a copy for it.
*
*ARGUMENTS:
*  value - Place to hold copy of the map name
*********************************************************/
void lv_screenGetMapName(char *value);

/*********************************************************
*NAME:          lv_screenGetNumPlayers
*AUTHOR:        John Morrison
*CREATION DATE: 26/1/99
*LAST MODIFIED: 26/1/99
*PURPOSE:
* Returns the number of players in the game
*
*ARGUMENTS:
*
*********************************************************/
BYTE lv_screenGetNumPlayers();

/*********************************************************
*NAME:          lv_screenGetGameType
*AUTHOR:        John Morrison
*CREATION DATE: 26/1/99
*LAST MODIFIED: 26/1/99
*PURPOSE:
* Returns the game type (open/tournament etc.)
*
*ARGUMENTS:
*
*********************************************************/
gameType lv_screenGetGameType();

/*********************************************************
*NAME:          lv_screenGetAllowHiddenMines
*AUTHOR:        John Morrison
*CREATION DATE: 26/1/99
*LAST MODIFIED: 26/1/99
*PURPOSE:
* Returns whether hidden mines are allowed in the game
* or not
*
*ARGUMENTS:
*
*********************************************************/
bool lv_screenGetAllowHiddenMines();

/*********************************************************
*NAME:          lv_screenGetGameTimeLeft
*AUTHOR:        John Morrison
*CREATION DATE: 27/1/99
*LAST MODIFIED: 27/1/99
*PURPOSE:
* Returns the time remaining the current game
*
*ARGUMENTS:
*
*********************************************************/
int32_t lv_screenGetGameTimeLeft();

/*********************************************************
*NAME:          lv_screenGetGameStartDelay
*AUTHOR:        John Morrison
*CREATION DATE: 27/1/99
*LAST MODIFIED: 27/1/99
*PURPOSE:
* Returns the time remaining to start the current game
*
*ARGUMENTS:
*
*********************************************************/
int32_t lv_screenGetGameStartDelay();

/*********************************************************
*NAME:          lv_screenSaveMap
*AUTHOR:        John Morrison
*CREATION DATE:  5/2/99
*LAST MODIFIED: 31/10/99
*PURPOSE:
* Saves the map. Returns whether the operation was 
* sucessful or not.
*
*ARGUMENTS:
*  fileName - path and filename to save
*  saveOwnerships - Do we save ownerships or not
*********************************************************/
bool lv_screenSaveMap(char *fileName, bool saveOwnerships);



bool lv_screenIsPlaying();

bool lv_screenLogTick();

BYTE lv_screenGetSizeX();

BYTE lv_screenGetSizeY();

void lv_screenSetSizeX(BYTE x);
void lv_screenSetSizeY(BYTE y);

void lv_screenGetOffsets(BYTE *x, BYTE *y);
void lv_screenSetOffset(BYTE x, BYTE y);

/* Sub-tile pan, in zoom-1 native pixels within the (xOffset,yOffset) tile.
 * Used by mouse-drag panning to give smooth scrolling between tiles. */
void lv_screenGetSubOffset(int *x, int *y);
void lv_screenSetSubOffset(int x, int y);

/* Pan to an absolute view position measured in zoom-1 native pixels.
 * Decomposes into (xOffset,yOffset)+(subPxX,subPxY), clamps to the map
 * bounds, and triggers a redraw if the whole-tile component changed. */
void lv_screenPanToTotalPixels(int totalPxX, int totalPxY);

void lv_windowAddEvent(int eventType, char *msg);
/* Add a clickable highlight line: clicking it in the events panel seeks a few
 * seconds before seekMs and centres the game view on the (mapX,mapY) cell. */
void lv_windowAddHighlight(char *msg, uint32_t seekMs, int mapX, int mapY);
/* Add a load-time round-summary line: no stamp of its own, pinned above the
 * timeline feed so the presentation window never hides it. */
void lv_windowAddSummary(char *msg);
void lv_windowStop(int corruptLog);
void lv_finished();

bool lv_screenCloseLog();
void lv_screenFastForward();
void lv_screenRewind();
void lv_screenGetTime(char *dest);
/* Format an absolute log time as the displayed (window-relative) mm:ss. */
void lv_screenFormatTime(uint32_t absMs, char *dest, size_t destSize);
/* Playback position in absolute log ms (the decoder's own clock, not the
 * presented window). */
uint32_t lv_screenGetTimeRunning(void);
void lv_screenTankCentred(int enabled);

/* Presentation window ("Hide Lobby"). While enabled and the loaded log carries a
 * log_LobbyExit marker, displayed times and every seek run against
 * [gameStart, totalTime) rather than the whole file; the decoder, the snapshot
 * index and the highlight clip times stay absolute. Enabling it while the
 * playhead sits in the lobby seeks to game start; disabling it moves nothing.
 * Inert on a log with no lobby and on a live spectator feed. */
void lv_screenSetHideLobby(int enabled);
int  lv_screenGetHideLobby(void);

/* Start of the presented window in absolute log ms; 0 when the window is the
 * whole file. Event lines below it belong to the hidden lobby. */
uint32_t lv_screenWindowStartMs(void);

void lv_screenMouseCentreClick(int xPos, int yPos);
void lv_screenMouseClick(int xPos, int yPos);

void lv_screenCentreOnSelectedItem();
void lv_screenGetPlayerName(char *name, BYTE playerNum);
void lv_screenGetMapName(char *dest);

void lv_screenGetLogProgress(size_t *currentPos, size_t *totalSize, uint32_t *currentTime, uint32_t *totalTime);
void lv_screenSeekToPosition(float ratio);

/* Log playback time (ms) at which the game started (the lobby ended); 0 when the
 * log has no lobby. Attribution/highlight ticks are game-relative, so this is the
 * offset that maps them onto the scrubber's absolute clock. Computed at load. */
uint32_t lv_screenGameStartMs(void);

/* Calibration anchors for mapping attribution ticks to scrubber ms: the
 * playback times of two specific base-ownership gains, each identified as the
 * ordinal-th gain at cell (x,y) by `owner`. Ordinal matching is what pins the
 * exact event: the game-over handover re-assigns every base to the winner, so
 * "the last gain at this cell" can be a later event than the capture the
 * attribution track recorded. v2 logs only; false if unavailable. Lets the
 * highlight times/seeks be fitted per log. */
bool lv_walkFindBaseOwnerTimes(uint8_t xE, uint8_t yE, uint8_t ownerE, int ordE,
                               uint8_t xL, uint8_t yL, uint8_t ownerL, int ordL,
                               uint32_t *outMsE, uint32_t *outMsL);

/* Seek playback to an absolute log time in ms (clamped to the log length). */
void lv_screenSeekToTimeMs(uint32_t ms);

/* Centre the game view on a map cell (mapX,mapY). */
void lv_screenCentreOnCell(int mapX, int mapY);

/* Spectator live-DVR (driven by spectatorRun). While live mode is on,
 * lv_screenStreamPump only appends the arriving bytes and never auto-advances to
 * the head; the host drives the advance via lv_screenSpecFrameUpdate so a parked
 * spectator can watch the past unfold at real time while the head keeps growing.
 * totalTimeMs is repointed at the tracked live head so the existing scrubber/seek
 * math works unchanged; standalone .wbv playback (live mode off) is untouched. */
void lv_screenSpecSetLiveMode(bool on);            /* enter/leave; resets DVR state, follows the head */
bool lv_screenSpecIsLiveMode(void);
void lv_screenSpecNoteHeadTick(uint32_t gameTick); /* latest drained forward-record game tick */
uint32_t lv_screenSpecHeadTick(void);              /* current tracked head tick (0 before the first record) */
void lv_screenSpecFrameUpdate(uint32_t nowMs);     /* once per frame: advance per follow/parked + pause; track head time */
void lv_screenSpecJumpToLive(void);                /* advance to the head and resume following */
void lv_screenSpecResetSegment(void);              /* world reset (new lobby/map): drop the seek index + restart head tracking */

/* Returns the slot the camera is currently following (cameraSlot), 0 when no
 * log is loaded. */
BYTE lv_screenGetCameraSlot(void);

#endif /* _BACKEND_H */
