/*
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

/* Per-tank inventory inferred from the event stream for legacy logs that
 * do not carry tank shells/mines/armour/trees. Reset at spawn/respawn
 * from gameTypeGetItems; mutated on log_SoundShoot/MineLay/HitTank/
 * MineExplode by tile correlation with the shooter/victim's last
 * log_PlayerLocation; refilled on log_BaseSetStock deltas when a
 * friendly tank is standing on the base tile. Approximate — sound
 * events lack player IDs, so adjacent tanks may steal each other's
 * deltas. New (post-snapshot-tank-stats) logs should override these
 * from authoritative snapshots. */
typedef struct {
  BYTE shells;
  BYTE mines;
  BYTE armour;
  BYTE trees;
} TankInventory;

/*********************************************************
 * LogViewerState — all viewer state in one struct.
 *
 * Grouped by the file that originally owned the globals.
 *********************************************************/
typedef struct LogViewerState {

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
  TankInventory tankInv[MAX_TANKS];  /* inferred per-tank inventory (legacy logs) */
  /* Per-base previous-stock snapshot for log_BaseSetStock delta
   * computation. prevBaseStockValid is set FALSE until the first
   * log_BaseSetStock for a base, so the initial value isn't credited
   * to whichever tank happens to be standing on that tile. Indexed by
   * base number minus 1 (1..MAX_BASES → 0..MAX_BASES-1). */
  BYTE         prevBaseShells[MAX_BASES];
  BYTE         prevBaseMines[MAX_BASES];
  BYTE         prevBaseArmour[MAX_BASES];
  bool         prevBaseStockValid[MAX_BASES];

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
 * image-local; pan deltas are host screen pixels measured from lvEmbedPanBegin.
 * lvEmbedGetZoomLevel reports the scale the slice is drawn at, so a host can
 * size its image at the slice times the zoom instead of stretching it to fill.
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
 * lvEmbedEnd is safe to call twice or while inactive. */
bool lvEmbedBegin(struct SDL_Window *window, struct SDL_Renderer *renderer,
                  uint8_t *zipData, size_t zipLen, int viewW, int viewH);
void lvEmbedEnd(void);
bool lvEmbedIsActive(void);
void lvEmbedSetViewportSize(int viewW, int viewH);
bool lvEmbedFrameTexture(void **outTexture, int *outTexW, int *outTexH,
                         int *outSrcX, int *outSrcY, int *outSrcW, int *outSrcH);
float lvEmbedGetZoomLevel(void);
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
