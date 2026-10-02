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
*Name:          LogViewer
*Filename:      logviewer.h
*Author:        John Morrison
*Purpose:
*  Central state struct for the log viewer. Holds all state
*  previously spread across module-level globals, enabling
*  the logviewer to be embedded in the main app (like the
*  map editor pattern).
*********************************************************/

#ifndef _LOGVIEWER_H
#define _LOGVIEWER_H

#include "lv_global.h"
#include "backend.h"
#include "snapshot.h"
#include "lv_players.h"
#include "lv_shells.h"
#include "lv_bolo_map.h"
#include "lv_bases.h"
#include "lv_starts.h"
/* LvScripts, LvRuleChange and their accessors. Included here rather than
 * next to LvRules because it brings in <stdbool.h>, and every bool below this
 * line has been _Bool since sim_rules_names.h was included at this spot. */
#include "lv_scripts.h"
/* LvPresentation and its accessors: the scenario panels, scores,
 * announcement and markers at the playhead. */
#include "lv_presentation.h"

/* Forward declarations for SDL types */
struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
typedef uint32_t SDL_TimerID;

/* Log reader states (mirrors enum in screen.c) */
typedef enum {
  lv_lr_start,
  lv_lr_longwait,
  lv_lr_shortwait
} lvLrStates;

/* Per-player HUD state for game-view armour bar tracking. Logs do not
 * carry tank armour, so the bar is binary (alive=full / dead=destroyed)
 * and the timestamps drive any death/respawn animations. */
typedef struct {
  bool     alive;
  uint32_t deathTimeMs;
  uint32_t respawnTimeMs;
} GameViewPlayerHud;

/* Per-tank shells/mines/armour/trees as the recording states them: from the
 * four bytes on the end of each snapshot's player block, and from the
 * log_TankSetStock records that carry every change in between. A recording
 * written before those existed carries neither, and the slot stays at zero. */
typedef struct {
  BYTE shells;
  BYTE mines;
  BYTE armour;
  BYTE trees;
} TankInventory;

/* Per-tank modifier percentages as the recording states them, from the
 * log_TankSetModifiers records. 0 means classic, so a slot the recording says
 * nothing about reads as an unmodified tank. Stored but not drawn. */
typedef struct {
  BYTE speed;
  BYTE accel;
  BYTE turn;
  BYTE reload;
  BYTE dealt;
  BYTE taken;
} TankMods;

/*********************************************************
 * LogViewerState — all viewer state in one struct.
 *
 * Grouped by the file that originally owned the globals.
 *********************************************************/
/* The gameplay numbers the viewer draws against. The viewer has no sim —
   it rebuilds its world from the .wbv stream — so it keeps its own holder
   rather than reading one. Seeded with the classic values at create, then
   filled with each rule's value at the playhead (lv_screenRuleValueAt) on
   every load, every log_RuleSet the playhead passes and every seek. */
typedef struct {
  BYTE tankFullShells;
  BYTE tankFullMines;
  BYTE tankFullArmour;
  BYTE tankFullTrees;
  BYTE baseFullShells;
  BYTE baseFullMines;
  BYTE baseFullArmour;
  /* pill_max_armour: the armour a pillbox draws as intact against. 1 to 255,
     never 0, since the pill picture divides by it. */
  int  pillMaxArmour;
} LvRules;

typedef struct LogViewerState {
  /* The rules this recording is drawn against. */
  LvRules      rules;
  /* The scripts the recording says the round ran; empty for a plain round. */
  LvScripts    scripts;
  /* Every log_RuleSet the recording holds, in file order: collected by a walk
     over the whole file when it loads, and appended as the playhead meets
     them on a live feed, which has no file to walk. */
  LvRuleChange ruleChanges[LV_RULE_CHANGES_MAX];
  int          ruleChangeCount;
  bool         ruleChangesTruncated;   /* the file held more than the array */
  /* The scenario panels, scores, announcement and markers at the playhead:
     filled as playback passes each record, and rebuilt from the load walk's
     index wherever the playhead jumps. */
  LvPresentation pres;

  /* --- FROM screen.c globals --- */
  screen       view;
  screenMines  mineView;
  map          mp;
  bases        bs;
  pillboxes    pb;
  starts       ss;
  shells       shs;
  char         mapName[64];
  BYTE         gt;
  BYTE         allowHiddenMines;
  BYTE         ai;
  BYTE         maxPlayers;
  bool         usePassword;
  BYTE         wbnKey[32];
  char         logFileName[FILENAME_MAX];
  snapshot     snap;
  BYTE         xOffset;
  BYTE         yOffset;
  int          subPxX;     /* sub-tile pan offset within (xOffset,yOffset) tile,
                            * in zoom-1 native pixels. Range: [0, TILE_SIZE_X). */
  int          subPxY;
  BYTE         screenSizeX;
  BYTE         screenSizeY;
  bool         isPlaying;
  bool         logLoaded;
  uint32_t     timeRunning;
  uint32_t     totalTimeMs;     /* Total log duration in ms, computed once on load */
  bool         centredTank;
  bool         fastForwarding;
  int32_t      gmeStartDelay;
  int32_t      gmeLength;
  int32_t      gmeCreateTime;
  char         serverIP[256];
  unsigned short serverPort;
  BYTE         versionMajor;
  BYTE         versionMinor;
  BYTE         versionRevision;
  /* Live-spectator game phase (SPEC_PHASE_* from spectator_drain.h), decoded
   * from each seed's snapshot. Drives the window-title tag and the on-screen
   * phase badge; SPEC_PHASE_UNKNOWN for the standalone replay viewer. */
  BYTE         gamePhase;
  BYTE         loadedLogVersion;
  BYTE         selectedItem;
  BYTE         selectedItemType;
  int          state;       /* lvLrStates enum value */
  unsigned short waitLen;

  /* --- FROM main.c globals --- */
  BYTE         tc[17];
  bool         playIsPlaying;
  bool         isLoaded;
  bool         isSoundsPlaying;
  int          soundVolume;     /* 0-100; passed to lv_soundSetVolume() */
  bool         useTeamColours;
  bool         wantScreenUpdate;
  bool         doubleSpeed;
  BYTE         speed;
  int          timerSleep;
  SDL_TimerID  timerGameID;
  SDL_TimerID  timerFrameID;

  /* --- Game-view (trailer-capture) skin state --- */
  bool         gameView;             /* gates the rendering branch */
  BYTE         cameraSlot;           /* 0..MAX_TANKS-1, the spectated player */
  bool         savedUseTeamColours;  /* restored when leaving game view mode */
  uint16_t     kills[MAX_TANKS];     /* per-player kill tally */
  uint16_t     deaths[MAX_TANKS];    /* per-player death tally */
  GameViewPlayerHud gameViewHud[MAX_TANKS];
  TankInventory tankInv[MAX_TANKS];  /* per-tank stocks, as the recording states them */
  TankMods     tankMods[MAX_TANKS];  /* per-tank modifiers, as the recording states them */

  /* --- Embedded reel (post-game recap) state --- */
  /* Colour tanks by their alliance to the local player -- green allies, red
   * enemies -- instead of by the viewer's per-team palette. The reel is
   * watched by one of the players, so that is the reading it wants; it also
   * brings up no preferences, so tc[] is all zeros there and every team would
   * otherwise index the sheet's uncoloured row. Only the reel sets it, so the
   * standalone viewer, the spectator and the game view keep the team palette. */
  bool         allyColours;

  /* --- FROM draw.c (SDL handles) --- */
  struct SDL_Window   *window;
  struct SDL_Renderer *renderer;
  bool         ownsWindow;     /* TRUE in standalone, FALSE when embedded */
  bool         fromMainMenu;   /* TRUE when launched from main app menu */
  bool         quit;           /* Set TRUE to exit the event loop */

  /* --- FROM players.c globals (will be moved here in a later step) --- */
  /* players plrs; */
  /* BYTE myPlayerNum; */

  /* --- FROM blocks.c globals (will be moved here in a later step) --- */
  /* log buffer state: logData, logSize, logCapacity, logPosition, logEOF, blockKey, logFile */

  /* --- FROM clientmutex.c (will be moved here in a later step) --- */
  /* SDL_Mutex *mutex; */

} LogViewerState;

/* Entry point for embedded logviewer */
void logViewerRun(struct SDL_Window *window, struct SDL_Renderer *renderer,
                  const char *logPath, bool fromMainMenu);

/* Ask for the whole application to end, not just this viewer, and break the
 * loop that is running.  Wired to Quit; "Return to main menu" pushes the
 * plain quit event instead, which only ends the viewer. */
void logViewerRequestAppQuit(void);

/* Did the run that just returned end because the player quit?  Only the
 * embedded caller asks; it hands the answer to windowSetQuitting(). */
bool logViewerAppQuitRequested(void);

/* Entry point for loading from an in-memory zip buffer (mobile).
 * Takes ownership of zipData. */
void logViewerRunFromMemory(struct SDL_Window *window, struct SDL_Renderer *renderer,
                            uint8_t *zipData, size_t zipLen, bool fromMainMenu);

/* Embedded reel: drives the decoder and the block-grid render-to-texture for a
 * host that already owns an ImGui frame, so a round can be drawn as a texture
 * inside that frame. The viewer's ImGui context, panels, sound and preferences
 * are never brought up. Scalars and void * only — hosts that cannot include
 * this header (the client's GUI, whose types collide with backend.h) hand-
 * declare the same signatures.
 *
 * lvEmbedBegin borrows window/renderer, takes ownership of zipData (freeing it
 * on every refusal), and refuses while any viewer, spectator or earlier embed
 * still holds the decoder singleton. viewW/viewH are the host's image rect in
 * pixels; the tile grid is sized to it and re-fitted by lvEmbedSetViewportSize.
 * lvEmbedFrameTexture updates the render target when the decode timers asked
 * for it and reports the texture plus the visible slice within it (src rect in
 * texture pixels), clearing and presenting nothing. Wheel coordinates are
 * image-local; pan deltas are host screen pixels measured from lvEmbedPanBegin,
 * and a non-zero delta turns off following the tank lvEmbedFocusPlayerByName
 * picked. lvEmbedGetZoomLevel reports the scale the slice is drawn at, so a host can
 * size its image at the slice times the zoom instead of stretching it to fill.
 * The tank names are not in the texture: lvEmbedTankLabelCount and
 * lvEmbedTankLabel give them for the host to draw over its image, and
 * lvEmbedSetTankLabelsInTexture(true) puts them back in for a host that reads
 * the texture's pixels.
 * lvEmbedGetProgress, lvEmbedSeekRatio, lvEmbedSeekToClip and lvEmbedSeekToTime
 * all speak in the presented window rather than the whole log: progress is
 * elapsed and total milliseconds within it (zeros while no embed is running), a
 * ratio addresses it as 0..1, and both seek times are measured from its start.
 * lvEmbedSeekToClip also centres the view on the cell the clip happened at;
 * lvEmbedSeekToTime leaves the view alone, for a caller naming a moment rather
 * than a place. Times past the end of the window clamp to it.
 * lvEmbedStepTicks walks the log forward by whole ticks with no timer driving
 * it, for a host stepping the round a fixed amount at a time; the next
 * lvEmbedFrameTexture repaints to the moment it stepped to.
 * lvEmbedSetSelfName names the player watching, so the round is drawn from
 * their side — green allies, red enemies — rather than from log slot 0's; an
 * empty name leaves the reel drawing as it does with nobody named.
 * lvEmbedEnd is safe to call twice or while inactive. */
bool lvEmbedBegin(struct SDL_Window *window, struct SDL_Renderer *renderer,
                  uint8_t *zipData, size_t zipLen, int viewW, int viewH);
void lvEmbedEnd(void);
bool lvEmbedIsActive(void);
void lvEmbedSetViewportSize(int viewW, int viewH);
bool lvEmbedFrameTexture(void **outTexture, int *outTexW, int *outTexH,
                         int *outSrcX, int *outSrcY, int *outSrcW, int *outSrcH);
float lvEmbedGetZoomLevel(void);
int  lvEmbedTankLabelCount(float pxPerSourcePx);
bool lvEmbedTankLabel(int index, void **outTexture, int *outX, int *outY,
                      int *outW, int *outH);
void lvEmbedSetTankLabelsInTexture(bool inTexture);
void lvEmbedPlay(void);
void lvEmbedPause(void);
bool lvEmbedIsPlaying(void);
void lvEmbedWheel(int localX, int localY, float wheelY);
void lvEmbedPanBegin(void);
void lvEmbedPanDelta(float dxScreenPx, float dyScreenPx);
void lvEmbedGetProgress(uint32_t *outCurMs, uint32_t *outTotalMs);
void lvEmbedSeekRatio(float ratio);
void lvEmbedSeekToClip(uint32_t roundRelMs, int mapX, int mapY);
void lvEmbedSeekToTime(uint32_t roundRelMs);
void lvEmbedStepTicks(int ticks);
void lvEmbedSetSelfName(const char *name);

/* Modal host for the live delayed spectator feed. Borrows the caller's
 * window/renderer and drives the decoder from records drained off the bolo-world
 * ClientSim (passed opaquely as cs) through the spectator_drain.h seam. The
 * caller owns the ClientSim's lifetime; spectatorRun does not disconnect it.
 *
 * serverHost/serverPort are the connected server's typed address, used only to
 * title the borrowed window (logviewer.c cannot reach client_sim.h to read them
 * off the ClientSim); the caller restores the app title on return.
 *
 * Returns true when it exits because the server put the spectator back into
 * live-lobby mode (the delayed game drained to the lobby) — the caller re-enters
 * the live lobby; false when the user left or the feed never loaded. */
bool spectatorRun(struct SDL_Window *window, struct SDL_Renderer *renderer,
                  void *cs, const char *serverHost, uint16_t serverPort);

/* Host-callable decoder lifecycle: create allocates the state, sets its
 * field defaults, and registers it as the active state; destroy closes any
 * loaded log, frees the state, and clears the active state. */
LogViewerState *lv_decoderCreate(bool fromMainMenu);
void lv_decoderDestroy(LogViewerState *lv);

/* State accessors used by screen.c and other modules */
void lv_screenSetState(LogViewerState *lv);
LogViewerState *lv_screenGetState(void);

/* Accessor functions for sounddist.c (replaces extern globals) */
BYTE lv_screenGetXOffset(void);
BYTE lv_screenGetYOffset(void);
bool lv_screenGetFastForwarding(void);

#endif /* _LOGVIEWER_H */
