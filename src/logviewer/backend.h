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
#include "lv_save_name.h"   /* LvRosterRow */
#include "lv_ping_view.h"   /* LvPingShown */

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
/* GAME_NUMTOTALTICKS_SEC / GAME_NUMGAMETICKS_SEC come from the engine's
 * global.h. The copies that used to live here carried a stray trailing
 * semicolon, so they were not identical redefinitions. */


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
  gameStrictTournament,
  gameScripted
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

/* The same loads a step at a time, so a host can draw between steps. Begin
 * reads the header and opening snapshot and returns FALSE when the log will
 * not open. Then call lv_screenLoadStep while it returns 1; each call does a
 * few milliseconds of work. The call that finishes the load returns 0. -1 is
 * no load in progress: none was begun, it has finished, or the log was closed
 * part way (lv_screenCloseLog). lv_screenLoadMap and
 * lv_screenLoadMapFromMemory are Begin followed by every step. */
typedef enum {
  LV_LOAD_UNZIP,   /* decompressing log.dat */
  LV_LOAD_SCAN,    /* reading it for total time, names, rules and the rest */
  LV_LOAD_PLAY     /* playing it through once to index every snapshot */
} LvLoadStage;

bool lv_screenLoadMapBegin(char *fileName, int memoryBufferSize);
bool lv_screenLoadMapFromMemoryBegin(uint8_t *zipData, size_t zipLen);
int  lv_screenLoadStep(void);

/* The stage the load is at and how far through that stage it is, 0 to 1. */
void lv_screenLoadProgress(LvLoadStage *stage, float *fraction);

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

/* Most bytes a log_GameSettings payload can hold: the event frames it behind
 * a single length byte. */
#define LV_GAME_SETTINGS_MAX 255

/*********************************************************
*NAME:          lv_screenGetGameSettings
*PURPOSE:
* Copies the lobby settings the loaded log recorded (the
* raw log_GameSettings payload, layout in
* docs/replay-format.md) into out and returns how many
* bytes were written. Returns 0 when the log carries no
* settings event, which is every log written before the
* event existed.
*
*ARGUMENTS:
*  out    - Buffer to copy the payload into
*  maxLen - Bytes out can hold
*********************************************************/
int lv_screenGetGameSettings(BYTE *out, int maxLen);

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

/* How much of the view is under a sidebar whose unused part is clear, at
 * each edge, in zoom-1 native pixels. The map is drawn there, but the middle
 * of the VISIBLE map is the middle of the rest: everything that centres the
 * view (the followed tank, centre-on-click, a jump to an event or an item)
 * takes these off first. All 0 unless the host says otherwise. */
void lv_screenSetViewInset(int left, int top, int right, int bottom);
void lv_screenGetViewInset(int *left, int *top, int *right, int *bottom);

/* Pan to an absolute view position measured in zoom-1 native pixels.
 * Decomposes into (xOffset,yOffset)+(subPxX,subPxY), clamps to the map
 * bounds, and triggers a redraw if the whole-tile component changed. */
void lv_screenPanToTotalPixels(int totalPxX, int totalPxY);

/* kind is an lvEventKind (lv_event_kind.h). mapX/mapY name the map cell the
 * line is about, or are both -1 when it is about no one place. */
void lv_windowAddEvent(int eventType, int kind, int mapX, int mapY, char *msg);
/* Add a clickable highlight line: clicking it in the events panel seeks a few
 * seconds before seekMs and centres the game view on the (mapX,mapY) cell. */
void lv_windowAddHighlight(char *msg, uint32_t seekMs, int mapX, int mapY);
/* Add a load-time round-summary line: no stamp of its own, pinned above the
 * timeline feed so the presentation window never hides it. */
void lv_windowAddSummary(char *msg);
/* Events panel clicks. Both stop playback across the seek and hold the client
 * lock, as the scrubber does. SeekToHighlight always centres the view on the
 * cell; SeekToEvent only when hasCell is non-zero. */
void lv_windowSeekToHighlight(uint32_t ms, int mapX, int mapY);
/* team: the team the event names, for the colour of the ring flashed round
 * its place; -1 for none. */
void lv_windowSeekToEvent(uint32_t ms, int hasCell, int mapX, int mapY, int team);
/* A plain click on a line of the Events panel: the same jump, with the view
 * left where it is. A place (hasCell) is shown by the ring alone. */
void lv_windowSeekToEventKeepView(uint32_t ms, int hasCell, int mapX, int mapY, int team);
/* A double click on the same line or marker. The same jump, and the place
 * goes to the middle of the map that shows whatever held the view: "Tank
 * Centred" is turned off first. With no place (hasCell 0) it is
 * lv_windowSeekToEvent. */
void lv_windowCentreOnEvent(uint32_t ms, int hasCell, int mapX, int mapY, int team);
/* Standalone viewer only: list every event of the file just loaded in the
 * events panel (one decode pass, then back to the start). */
void lv_windowCollectAllEvents(void);
void lv_windowStop(int corruptLog);
void lv_finished();

/* A listener on the decoder, for the text dump (lv_dump.c). record is called
 * for every log record before the decoder applies it, so the world it reads
 * is still the one before the record. bytes holds the first len bytes of the
 * record's payload and frameLen is the payload's whole length; an older file
 * that does not frame its records gives bytes NULL and both lengths -1. event
 * is called for every line that would go to the events panel. Either may be
 * NULL.
 *
 * While a listener is set the decoder calls nothing in the host: no events
 * panel line, no marquee, no end-of-log or corrupt-log callback, and no
 * snapshot is kept for seeking. Set it before the load and clear it (NULL)
 * before the decoder is used for anything else. */
#define LV_OBSERVE_PAYLOAD_MAX 512
typedef struct {
  void (*record)(void *ctx, BYTE code, const BYTE *bytes, int len, int frameLen);
  void (*event)(void *ctx, int kind, int mapX, int mapY, const char *text);
  void *ctx;
} LvDecodeObserver;
void lv_screenSetObserver(const LvDecodeObserver *observer);

bool lv_screenCloseLog();
void lv_screenFastForward();
void lv_screenRewind();
/* Where each of the two would take playback, in the log's own time; FALSE
 * when it would not move (screen.c). */
bool lv_screenFastForwardTargetMs(uint32_t *ms);
bool lv_screenRewindTargetMs(uint32_t *ms);
/* The last move of an animated rewind: to the place lv_screenRewindTargetMs
   gave before the animation moved playback. */
void lv_screenRewindToMs(uint32_t ms);
/* The playback time to the second, for a file name. dest: 16 characters or
 * more. */
void lv_screenGetTime(char *dest);
/* Format an absolute log time as the displayed (window-relative) time to the
 * hundredth of a second, "19:48.22" (lv_time_fmt.h). destSize: 16 or more. */
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
/* Its length in ms; 0 when nothing is loaded. The seek slider goes from the
 * start to the start plus this. */
uint32_t lv_screenWindowLenMs(void);

/* The teams of the players the line now being added is about
 * (lv_messageTagPlayers), each 0..15, or -1 for none. For a host's
 * lv_windowAddEvent to ask while it takes the line. */
void lv_screenEventTeams(int *team, int *otherTeam);
/* The map pings that are on the map at the playback's time now, for the
 * view in use: every team's for the view of all teams, and for one player's
 * view the pings that player was sent. Fills out with at most max of them
 * (LV_PING_SHOWN_MAX at the most), the oldest first, and returns the number.
 * worldX and worldY are 256 to a map square; ageMs is the time since the
 * ping on the playback clock. Called once a frame by each drawer. */
int lv_screenGetPings(LvPingShown *out, int max);
/* The kind of ping the line now being added is (lv_messageTagPing,
 * PING_KIND_*), or -1 for a line that is not a map ping. */
int lv_screenEventPing(void);
/* The first player the line now being added is about (the actor of
 * lv_messageTagPlayers), 0..15, or -1 for none. */
int lv_screenEventPlayer(void);
/* The palette colour a team is drawn in (the tc[] row of the Team Colours
 * dialog); the neutral colour for a number that is not a team. */
unsigned char lv_screenTeamColour(int team);
/* The palette entry neutral pillboxes are drawn in, or -1 for the game's
 * red tile: grey when a team in the game is red too (lv_team_sprite.h). */
int lv_screenNeutralPillEntry(void);

void lv_screenMouseCentreClick(int xPos, int yPos);
void lv_screenMouseClick(int xPos, int yPos);

/* Select a base (itemType 1) or a pillbox (itemType 2) by its number from 0,
 * as an information click on it on the map does. Hold the client mutex. */
void lv_screenSelectItem(BYTE itemType, BYTE itemNum);

/* Copy out what the HUD panel draws (lv_hud.h). Hold the client mutex. */
struct LvHudSnapshot;
void lv_screenHudSnapshot(struct LvHudSnapshot *out);

/* Copy out what the Overview panel shows (lv_overview.h): the pillboxes and
 * bases each team owns at the playback time. Hold the client mutex. */
struct LvOverview;
void lv_screenOverviewSnapshot(struct LvOverview *out);

/* Copy out what the Tank panel shows (lv_tank_status.h): the selected tank's
 * shells, mines, armour and trees with their caps, its builder and the
 * pillboxes it carries, at the playback time. Hold the client mutex. */
struct LvTankStatus;
void lv_screenTankStatusSnapshot(struct LvTankStatus *out);

/* The pillboxes whose numbers the map shows (lv_pill_numbers.h): each one
 * that stands on the map in the squares the view paints, its square in the
 * view and its number. Writes at most max and returns how many. Hold the
 * client mutex. */
struct LvPillSpot;
int lv_screenPillSpots(struct LvPillSpot *out, int max);

/* The pillbox on the map square under a point of the view's picture, xPos,
 * yPos pixels from its top left (the place a click on the map hands
 * lv_screenMouseInformationClick). Returns its number, from 1, with its
 * owner (NEUTRAL for none), its armour, and in team the owner's team (-1 for
 * none); 0 when no pillbox stands there. Hold the client mutex. */
int lv_screenPillAt(int xPos, int yPos, BYTE *owner, BYTE *armour, int *team);

/* The time left, at the playback time, until the builder of a player's tank
 * is in the tank again after his death (lv_builder_status.h). Returns 1 with
 * the time in remainMs when there is one to show:
 *   - exact (approx 0), out of the table the load's play stage fills:
 *     from the builder's death until the tick the new builder is in the
 *     tank, his walk from where he landed included;
 *   - estimated (approx 1) when there is no table (a live feed, a viewer
 *     that does not scan) or the log ends first: only while the new builder
 *     flies, by the game's rule (his distance to the tank over
 *     lgm_helicopter_speed), and only to his landing.
 * Returns 0 for a builder in the tank, and for one out of it who was not
 * killed. Hold the client mutex. */
int lv_screenBuilderReturn(int slot, long *remainMs, int *approx);
/* 1 when the table is filled for the loaded log. */
int lv_screenBuilderTripsKnown(void);
/* How long the builder of a player was away after the death at deathMs (the
 * time of the "lost builder" line): from the death to the tick the new
 * builder was in the tank, out of the same table. It is by the log's time,
 * not the playback's, so it is the same wherever playback is. Returns 1
 * with the time in awayMs. Returns 0, and there is no estimate, when there
 * is no table, when the log ends before he is back, and when the player
 * left the game before he was back. */
int lv_screenBuilderAwayMs(int slot, uint32_t deathMs, uint32_t *awayMs);

/* The selected tank: the one the Tank panel shows and the map marks. Select
 * picks the tank in a slot and changes nothing else (not whose view the map
 * shows); Deselect leaves none; Selected is its slot, or -1 for none or for
 * a player who is not in the game at the playback time. Choose is a Select
 * Team click on a tank's picture: "self" and the selection both go to it.
 * Hold the client mutex. */
void lv_screenSelectTank(int slot);
void lv_screenDeselectTank(void);
int lv_screenSelectedTank(void);
void lv_screenChooseTank(int slot);
/* 1 when the loaded recording holds tank stocks (a snapshot's player block
   or a log_TankSetStock record gave some), 0 for a recording written before
   they were recorded. The stocks of every slot read zero in that case, and
   that is "not known", not "empty". */
int lv_screenHasTankStock(void);

/* The perspective: show the game as one player sees it. Toggle turns it on
 * for the player in a slot, or off when that player has it already; Clear
 * goes back to all teams; Check turns it off when its player is gone and
 * returns TRUE when it did. Hold the client mutex, and mark the map for a
 * redraw (lv_drawDirtyScreen) after a change. */
void lv_screenTogglePerspective(BYTE playerNum);
int  lv_screenPerspectivePlayer(void);
void lv_screenClearPerspective(void);
bool lv_screenCheckPerspective(void);

void lv_screenCentreOnSelectedItem();

/* Move the camera onto the followed tank at native-pixel precision (tile
 * offset plus sub-tile pixel), instead of snapping to whole map squares.
 * No-op when the followed tank has no position right now. */
void lv_screenFollowCentredTank(void);
void lv_screenGetPlayerName(char *name, BYTE playerNum, size_t destSize);

/* Name the log recorded for a slot, from a one-shot scan of its join events at
 * load. Returns FALSE when the log never named that slot. Independent of the
 * playhead, unlike lv_screenGetPlayerName. */
bool lv_screenGetLoggedPlayerName(BYTE slot, char *dest, size_t destSize);
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
/* The same only when the cell is outside the view or near its edge, and not
 * at all with "Tank Centred" on. Returns 1 when the map moved. */
int lv_screenShowCell(int mapX, int mapY, float viewPtsW, float viewPtsH, float zoom);
/* Put a map pixel, or the middle of a cell, in the middle of a part of the
 * view, with "Tank Centred" on or off. The part is given in points from the
 * view's own start. Returns 1 when the view was placed (never in the game
 * view). */
int lv_screenCentrePxIn(float mapPxX, float mapPxY, float startPtX, float startPtY,
                        float lenPtX, float lenPtY, float zoom);
int lv_screenCentreCellIn(int mapX, int mapY, float startPtX, float startPtY,
                          float lenPtX, float lenPtY, float zoom);
/* The first view of a log (lv_first_view.h). Each load asks for one. Due
 * says 1 when the host is to place it: not in the first frame it asks in,
 * and not once the view has been placed by the host or moved by anything
 * else (Cancel, which every pan, zoom and jump to a place calls).
 * lv_screenLandBox gives the box round all that is not deep sea, in squares;
 * 0 when there is nothing (the lobby, or a map of sea alone). */
int  lv_screenFirstViewDue(void);
void lv_screenFirstViewCancel(void);
int  lv_screenLandBox(int *x0, int *y0, int *x1, int *y1);
/* Counts every request to move playback other than by playing. */
uint32_t lv_screenSeekSerial(void);

/* For the name Save Log File offers (lv_save_name.h). The roster is every
 * player the loaded log has had, in the order they came, with the last team
 * each was on and whether it is a bot; the load's play stage fills it. Returns how many rows it wrote. The
 * create time is the header's: the time the server that wrote the log was
 * started, in seconds since 1970, or 0. */
int     lv_screenGetRoster(LvRosterRow *rows, int max);
/* The LvBotChip (lv_bot_chip.h) the panels draw in front of the name of the
   player in a slot; 0 for a person. The caller holds the client mutex. */
int     lv_screenBotChip(BYTE slot);
int32_t lv_screenGetCreateTime(void);

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
/* The hold of the animation of Rewind and Fast Forward (lv_windowSetTickHold).
 * While it is on, lv_screenSpecFrameUpdate plays nothing: the animation moves
 * playback itself, one step a frame. The head time still follows the feed. */
void lv_screenSpecSetHold(bool on);

/* Returns the slot the camera is currently following (cameraSlot), 0 when no
 * log is loaded. */
BYTE lv_screenGetCameraSlot(void);

#endif /* _BACKEND_H */
