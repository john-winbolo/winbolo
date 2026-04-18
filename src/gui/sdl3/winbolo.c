/*
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
*Filename:      winbolo.c (SDL3)
*Author:        John Morrison
*Purpose:
*  SDL3 main event loop, timer callbacks, and all
*  functions declared in winbolo.h.  Replaces the
*  Win32 winbolo.c (WndProc + WM_COMMAND dispatch).
*
*  Menu commands from sdl3imgui.cpp call functions here
*  directly instead of posting Win32 messages.
*********************************************************/

/* MSVC: include crtdbg before SDL to avoid _malloca redefinition warning */
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

/* SDL3 must come before bolo headers (#pragma pack assertions). */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <string.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define chdir _chdir
#else
#include <unistd.h>
#endif

#include "../../bolo/screen.h"
#include "../../bolo/client_sim.h"
#include "../../bolo/frontend.h"
#include "../../bolo/players.h"
#include "../../steam/steam_wrapper.h"
#include "../../bolo/transport.h"
#include "../../bolo/transport_udp.h"
#include "../../server/server_sim.h"
#include "../../server/threads.h"
#include "../../bolo/bot_manager.h"
#include "../../bolo/gui_message.h"
#include "../../bolo/bolo_map.h"
#include "../brainsHandler.h"
#include "../clientmutex.h"
#include "../draw.h"
#include "../gamefront.h"
#include "input.h"
#include "../lang.h"
#include "../sound.h"
#include "../winbolo.h"
#include "sdl3draw.h"
#include "sdl3imgui.h"
#include "luabrainshandler.h"
#include "debug_overlay.h"

#include "../aresource.h"
#include "dialog_backend.h"
#include "../../common/sentry_integration.h"

/* Forward declarations */
void sdl3MessageHandler(const char *message, const char *title);

/* -------------------------------------------------------
 * Globals declared by winbolo.h (extern in sdl3imgui.cpp)
 * ------------------------------------------------------- */

void windowStartTutorial(void);
bool isTutorial = FALSE;

/* The frame rate */
int frameRate = FRAME_RATE_30;
/* Time between frame updates based on frame rate */
static int frameRateTime = (int)(MILLISECONDS / FRAME_RATE_30) - 1;

/* Whether the Gunsight is shown or not */
bool showGunsight = FALSE;

/* Whether the sound effects are turned on or not */
bool soundEffects = TRUE;
/* Do we play background sound */
bool backgroundSound = TRUE;

/* Is Sound Keepalive enabled */
bool useSoundKeepalive = TRUE;

/* Allow new players */
bool allowNewPlayers = TRUE;

/* 5 Message Shown Status */
bool showNewswireMessages = TRUE;
bool showAssistantMessages = TRUE;
bool showAIMessages = FALSE;
bool showNetworkStatusMessages = TRUE;
bool showNetworkDebugMessages = FALSE;

/* Automatic Scrolling */
bool autoScrollingEnabled = FALSE;

/* The Window scaling */
BYTE zoomFactor = ZOOM_FACTOR_NORMAL;

/* Whether Pillbox & base labels should be shown */
bool showPillLabels = FALSE;
bool showBaseLabels = FALSE;

/* Labels of stuff */
bool labelSelf = TRUE;
labelLen labelMsg = lblShort;
labelLen labelTank = lblShort;

/* SDL timers */
static SDL_TimerID timerGameID = 0;
static SDL_TimerID timerFrameID = 0;

/* Stuff for the system info dialog box */
static DWORD dwSysFrameTotal = 0;
static DWORD dwSysFrame = 0;
static DWORD dwSysGameTotal = 0;
static DWORD dwSysGame = 0;
static DWORD dwSysBrainTotal = 0;
static DWORD dwSysBrain = 0;

/* Key Settings */
keyItems keys;

/* Fix for breaking draw code with scrolling messages */
static bool drawBusy = FALSE;

/* Hide the main view or not */
bool hideMainView = FALSE;

/* Are we in a menu or not */
bool isInMenu = FALSE;

/* Tutorial flag */
static bool doingTutorial = FALSE;

/* Time to quit */
static bool winboloQuit = FALSE;
static bool finishedLoop = FALSE;

/* Tick counters */
static DWORD oldTick = 0;
static DWORD ttick = 0;
static DWORD oldFrameTick = 0;
static uint32_t simTickCounter = 0;
static bool justKeysFlag = FALSE;
static time_t ticks = 0;

/* Show alliance request flag */
static bool showAllianceReq = TRUE;

/* Flags set by timer threads to request work on the main thread */
static SDL_AtomicInt needsRedraw = { 0 };
static SDL_AtomicInt needsGameTick = { 0 };

/* Forward declarations */
static Uint32 SDLCALL windowGameTimer(void *userdata, SDL_TimerID timerID, Uint32 interval);
static Uint32 SDLCALL windowFrameRateTimer(void *userdata, SDL_TimerID timerID, Uint32 interval);
static void windowRunGameTick(ClientSim *cs);
int winboloCC(void);

/* -------------------------------------------------------
 * Steam friend "Join Game" callback
 *
 * Fired during steam_run_callbacks() when a friend clicks
 * Join on our rich presence.  Parses the connect string
 * ("+connect host:port") and hands it to gamefront's URL
 * handler so the next gameFrontStart() picks it up.
 * ------------------------------------------------------- */
static void steamJoinRequested(const char *connect_str) {
  /* Expected format: "+connect host:port" */
  const char *prefix = "+connect ";
  if (strncmp(connect_str, prefix, strlen(prefix)) != 0) return;

  steam_set_achievement("ACH_STEAM_JOIN");
  steam_store_stats();

  char buf[FILENAME_MAX];
  snprintf(buf, sizeof(buf), "winbolo://%s", connect_str + strlen(prefix));
  gameFrontHandleUrlOpen(buf);
}

/* -------------------------------------------------------
 * main — SDL3 entry point
 * ------------------------------------------------------- */
int main(int argc, char *argv[]) {
  const char *cmdLine = "";
  ClientSim *cs = NULL;

  for (int i = 1; i < argc; i++) {
    if (cmdLine[0] == '\0') {
      cmdLine = argv[i];
    }
  }

  sentryInit("WinBolo", argc, argv);

  dialogBackendInit();

#if !defined(_WIN32) && !defined(__APPLE__)
  /* Suppress "gtk_disable_setlocale() must be called before gtk_init()"
   * warning that SDL3's internal GTK init triggers. */
  {
    void *libgtk = SDL_LoadObject("libgtk-3.so.0");
    if (libgtk) {
      void (*disable_setlocale)(void) = (void (*)(void))SDL_LoadFunction(libgtk, "gtk_disable_setlocale");
      if (disable_setlocale) disable_setlocale();
      SDL_UnloadObject(libgtk);
    }
  }
#endif

  SDL_Init(0);

  steam_init();
  steam_set_join_callback(steamJoinRequested);

  /* Set working directory to the executable's location so that relative
     paths like "data/svg/..." resolve correctly.  On macOS this is
     essential when launched as an app bundle (cwd defaults to /).
     iOS already does this in winbolo_ios.m. */
  {
    const char *basePath = SDL_GetBasePath();
    if (basePath) {
      chdir(basePath);
    }
  }

  initWinboloTimer();

  if (clientMutexCreate() == FALSE) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE,
                             "Failed to create client mutex", NULL);
    return 1;
  }

  if (gameFrontStart(cmdLine, &keys, FALSE, &cs) == FALSE) {
    endWinboloTimer();
    clientMutexDestroy();
    SDL_Quit();
    return 1;
  }

  winboloQuit = FALSE;
  while (winboloQuit == FALSE) {
    /* Show lobby dialog if the server uses lobby mode */
    if (cs && cs->inLobby &&
        (cs->netStat == netLobby || cs->netStat == netLobbyCountdown)) {
      const DialogBackend *db = dialogBackendGet();
      int lobbyResult = db->lobbyShow(cs);
      if (lobbyResult == 0) {
        /* Player chose to leave — fall through to normal cleanup.
         * gamePlayed=FALSE because no tank exists during lobby. */
        winboloQuit = FALSE;  /* Signal that we want to return to menu */
        gameFrontEnd(&keys, FALSE, FALSE);
        if (gameFrontStart(cmdLine, &keys, TRUE, &cs) == FALSE) {
          winboloQuit = TRUE;
        }
        continue;
      }
      /* lobbyResult == 1: game started — load the map that was
       * downloaded in the background during the lobby. */
      if (!gameFrontLoadDeferredMap(cs)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE,
                                 "Failed to load map from server",
                                 sdl3DrawGetWindow());
        winboloQuit = FALSE;
        gameFrontEnd(&keys, TRUE, FALSE);
        if (gameFrontStart(cmdLine, &keys, TRUE, &cs) == FALSE) {
          winboloQuit = TRUE;
        }
        continue;
      }
      cs->netStat = netRunning;
      simTickCounter = 0;
      justKeysFlag = FALSE;
      /* Set Steam rich presence now that the game is running */
      gameFrontUpdateSteamPresence(cs);
    }

    isInMenu = FALSE;
    finishedLoop = FALSE;
    windowReCreate();
    windowApplyMenuChecks(cs);

    /* Set up sound keepalive */
    if (soundEffects == TRUE) {
      soundKeepalive(useSoundKeepalive);
    }
    SDL_Delay(500);
    oldTick = winboloTimer();
    oldFrameTick = oldTick;
    timerGameID = SDL_AddTimer(GAME_TICK_LENGTH, windowGameTimer, NULL);
    timerFrameID = SDL_AddTimer((Uint32)frameRateTime, windowFrameRateTimer, NULL);
    winboloQuit = TRUE;
    finishedLoop = FALSE;

    /* Enter the SDL3 main event loop */
    bool returnToLobby = FALSE;
    {
      bool done = FALSE;
      SDL_Window *sdlWin = sdl3DrawGetWindow();

      threadsCreate(FALSE);

      /* Flush any stale SDL_QUIT events that may have been queued during
         dialog teardown. Without this, the main loop would exit immediately
         on its first sdl3ImguiProcessEvents() call. */
      SDL_FlushEvent(SDL_EVENT_QUIT);

      if (sdlWin) {
        SDL_SetWindowResizable(sdlWin, false);
        SDL_SetWindowSize(sdlWin, sdl3DrawGetZoomFactor() * SDL3_SCREEN_W, sdl3DrawGetZoomFactor() * SDL3_SCREEN_H);
        SDL_ShowWindow(sdlWin);
        SDL_RaiseWindow(sdlWin);
      }
      guiMessageSetHandler(sdl3MessageHandler);

      /* Initialise ImGui here — after pre-game dialogs are done,
         so modal dialog event pumping can't corrupt ImGui frame state. */
      {
        SDL_Window *win = sdl3DrawGetWindow();
        SDL_Renderer *ren = sdl3DrawGetRenderer();
        if (win && ren) {
          sdl3ImguiSetup(win, ren);
        }
      }
      /* Clear any leftover ImGui nav focus so keyboard input
         reaches the game immediately (not captured by ImGui). */
      sdl3ImguiClearNavFocus();

      while (done == FALSE) {
        sdl3ImguiProcessEvents(cs);
        steam_run_callbacks();

        /* Run game tick on main thread when timer signals */
        if (SDL_GetAtomicInt(&needsGameTick)) {
          SDL_SetAtomicInt(&needsGameTick, 0);
          windowRunGameTick(cs);
        }

        /* Detect game-over returning to lobby */
        if (cs && cs->inLobby &&
            (cs->netStat == netLobby || cs->netStat == netLobbyCountdown)) {
          returnToLobby = TRUE;
          done = TRUE;
        }

        /* Redraw every frame — vsync throttles the present rate.
           Previously only redrawn on timer signal, but with double-
           buffering that left stale content in the alternate backbuffer,
           causing a visible "jump-back" flicker. */
        {
          DWORD tick = winboloTimer();
          clientMutexWaitFor();
          if (finishedLoop == FALSE) {
            screenUpdateCS(cs, redraw);
          }
          clientMutexRelease();
          dwSysFrame += (winboloTimer() - tick);
        }
        /* Consume the timer signal so it doesn't accumulate */
        SDL_SetAtomicInt(&needsRedraw, 0);

        /* Render the ImGui overlay and present the frame */
        sdl3ImguiPumpAndRender(cs);
        {
          SDL_Renderer *ren = sdl3DrawGetRenderer();
          /* Capture main window pixels for zoom window BEFORE present
           * (back buffer is undefined after SDL_RenderPresent). */
          SDL_Surface *zoomSnap = debugZoomCaptureIfOpen(ren);
          if (ren) SDL_RenderPresent(ren);
          debugZoomRenderFrame(zoomSnap);
          if (zoomSnap) SDL_DestroySurface(zoomSnap);
        }

        /* Cap to configured frame rate */
        {
          static Uint64 frameStart = 0;
          Uint64 now = SDL_GetTicks();
          if (frameStart > 0) {
            Uint64 elapsed = now - frameStart;
            if (elapsed < (Uint64)frameRateTime) {
              SDL_Delay((Uint32)((Uint64)frameRateTime - elapsed));
            }
          }
          frameStart = SDL_GetTicks();
        }

        if (finishedLoop) {
          done = TRUE;
        }
      }

      if (sdlWin) {
        SDL_SetWindowResizable(sdlWin, true);
        SDL_HideWindow(sdlWin);
      }
      screenLeaveGame();
    }

    finishedLoop = TRUE;

    /* Kill Timers */
    SDL_RemoveTimer(timerGameID);
    SDL_RemoveTimer(timerFrameID);
    timerGameID = 0;
    timerFrameID = 0;

    /* If returning to lobby after game-over, skip full teardown
     * and loop back to show the lobby dialog again. */
    if (returnToLobby) {
      gameFrontSaveTankPrefs(cs);
      sdl3ImguiCleanup();
      winboloQuit = FALSE;
      continue;
    }

    sdl3ImguiCleanup();
    gameFrontEnd(&keys, TRUE, winboloQuit);
    doingTutorial = FALSE;

    if (winboloQuit == FALSE) {
      if (gameFrontStart(cmdLine, &keys, TRUE, &cs) == FALSE) {
        winboloQuit = TRUE;
      }
    }
  }

  endWinboloTimer();
  clientMutexDestroy();
  /* Explicit cleanup before SDL_Quit so leak checks see freed memory */
  sdl3ImguiCleanup();
  debugZoomCleanup();
  sdl3DrawCleanup();
  steam_shutdown();
  SDL_Quit();
  sentryClose();
  return 0;
}

/* -------------------------------------------------------
 * SDL message handler for guiMessageSetHandler
 * ------------------------------------------------------- */
void sdl3MessageHandler(const char *message, const char *title) {
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION,
                           title ? title : "WinBolo",
                           message ? message : "",
                           sdl3DrawGetWindow());
}

/* -------------------------------------------------------
 * windowGameTimer — SDL3 timer callback (runs on timer thread)
 * Just signals the main thread; all game logic runs there.
 * ------------------------------------------------------- */
static Uint32 SDLCALL windowGameTimer(void *userdata, SDL_TimerID timerID, Uint32 interval) {
  (void)userdata;
  (void)timerID;

  if (gameFrontGetTransport() != NULL) {
    SDL_SetAtomicInt(&needsGameTick, 1);
  }
  return interval;
}

/* -------------------------------------------------------
 * windowRunGameTick — game logic, called from main thread only
 * ------------------------------------------------------- */
static void windowRunGameTick(ClientSim *cs) {
  static bool inBrain = FALSE;
  static BYTE t2 = 0;
  tankButton tb;
  bool isShoot;
  bool isMine = FALSE;
  bool used = FALSE;
  bool brainRunning;
  Transport *transport;

  brainRunning = brainHandlerIsBrainRunning();
  isShoot = FALSE;
  tb = 0;

  transport = gameFrontGetTransport();

  /* Check if the UDP server has disconnected or timed out.
   * Only check for UDP transports (serverSim == NULL means not local). */
  if (transport != NULL && gameFrontGetServerSim() == NULL &&
      transportUdpClientGetJoinState(transport) == UDP_CLIENT_SERVER_SHUTDOWN) {
    screenConnectionLostCS(cs);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE,
                             "You have lost your connection to the server.\n"
                             "Returning to menu.",
                             sdl3DrawGetWindow());
    finishedLoop = TRUE;
    winboloQuit = FALSE;
    return;
  }

  ttick = winboloTimer();
  /* Update the game objects if required */
  if ((ttick - oldTick) > GAME_TICK_LENGTH) {
    while ((ttick - oldTick) > GAME_TICK_LENGTH) {
      if (doingTutorial == FALSE) {
        BYTE myPlayerNum = gameFrontGetPlayerNum();
        if (cs->netStat == netLobby || cs->netStat == netLobbyCountdown) {
          /* Lobby/countdown: just tick the transport to receive packets */
          transport->tick(transport->ctx);
          justKeysFlag = !justKeysFlag; /* Alternate to maintain tick cadence */
        } else if (justKeysFlag == TRUE) {
          /* Keys tick */
          if (brainRunning == FALSE) {
            tb = inputGetKeys(cs, &keys, isInMenu);
          } else {
            inputScroll(cs, &keys, isInMenu);
          }
          InputPacket pkt;
          screenBuildInputPacketCS(cs, &pkt, tb, FALSE, FALSE, brainRunning, FALSE, myPlayerNum, simTickCounter);
          if (!brainRunning) {
            uint8_t gsAdj = inputConsumeGunsightAdj();
            if (gsAdj) pkt.flags |= ((gsAdj & 0x3) << INPUT_FLAG_GUNSIGHT_SHIFT);
          }
          clientMutexWaitFor();
          clientSimKeysTick(cs, &pkt);
          clientMutexRelease();
          transport->recordInput(transport->ctx, &pkt);
          transport->tick(transport->ctx);
          clientMutexWaitFor();
          {
            SnapshotHeader snapHdr;
            TankSnapshot snapTanks[MAX_TANKS];
            ShellSnapshot snapShells[MAX_SNAPSHOT_SHELLS];
            TkExplosionSnapshot snapTkExplosions[MAX_SNAPSHOT_TK_EXPLOSIONS];
            BaseSnapshot snapBases[MAX_SNAPSHOT_BASES];
            PillSnapshot snapPills[MAX_SNAPSHOT_PILLS];
            GameEvent snapEvents[MAX_SNAPSHOT_EVENTS];
            if (transport->getSnapshot(transport->ctx, myPlayerNum,
                                       &snapHdr, snapTanks, MAX_TANKS,
                                       snapShells, MAX_SNAPSHOT_SHELLS,
                                       snapTkExplosions, MAX_SNAPSHOT_TK_EXPLOSIONS,
                                       snapBases, MAX_SNAPSHOT_BASES,
                                       snapPills, MAX_SNAPSHOT_PILLS,
                                       snapEvents, MAX_SNAPSHOT_EVENTS)) {
              clientSimSyncFromSnapshot(cs, &snapHdr, snapTanks, snapHdr.tankCount,
                                     snapShells, snapHdr.shellCount,
                                     snapTkExplosions, snapHdr.tkExplosionCount,
                                     snapBases, snapHdr.baseCount,
                                     snapPills, snapHdr.pillCount,
                                     snapEvents, snapHdr.reliableEventCount,
                                     myPlayerNum);
            }
          }
          clientMutexRelease();
          simTickCounter++;
          justKeysFlag = FALSE;
        } else {
          /* Game tick */
          t2++;
          isShoot = FALSE;
          isMine = FALSE;
          if (brainRunning == FALSE) {
            tb = inputGetKeys(cs, &keys, isInMenu);
            isShoot = inputIsFireKeyPressed(&keys, isInMenu);
            isMine = inputIsMineKeyPressed(&keys, isInMenu);
          } else {
            isMine = FALSE;
            inputScroll(cs, &keys, isInMenu);
          }
          InputPacket pkt;
          screenBuildInputPacketCS(cs, &pkt, tb, isShoot, isMine, brainRunning, TRUE, myPlayerNum, simTickCounter);
          if (!brainRunning) {
            uint8_t gsAdj = inputConsumeGunsightAdj();
            if (gsAdj) pkt.flags |= ((gsAdj & 0x3) << INPUT_FLAG_GUNSIGHT_SHIFT);
          }
          clientMutexWaitFor();
          clientSimGameTick(cs, &pkt, brainRunning);
          clientMutexRelease();
          transport->sendInput(transport->ctx, &pkt);
          /* Tick bot brains before the sim tick (local game only) */
          {
            ServerSim *serverSim = gameFrontGetServerSim();
            if (serverSim != NULL && botManagerGetNumBots() > 0) {
              botManagerTick(serverSim, screenGetAiTypeCS(cs));
            }
          }
          transport->tick(transport->ctx);
          clientMutexWaitFor();
          {
            SnapshotHeader snapHdr;
            TankSnapshot snapTanks[MAX_TANKS];
            ShellSnapshot snapShells[MAX_SNAPSHOT_SHELLS];
            TkExplosionSnapshot snapTkExplosions[MAX_SNAPSHOT_TK_EXPLOSIONS];
            BaseSnapshot snapBases[MAX_SNAPSHOT_BASES];
            PillSnapshot snapPills[MAX_SNAPSHOT_PILLS];
            GameEvent snapEvents[MAX_SNAPSHOT_EVENTS];
            if (transport->getSnapshot(transport->ctx, myPlayerNum,
                                       &snapHdr, snapTanks, MAX_TANKS,
                                       snapShells, MAX_SNAPSHOT_SHELLS,
                                       snapTkExplosions, MAX_SNAPSHOT_TK_EXPLOSIONS,
                                       snapBases, MAX_SNAPSHOT_BASES,
                                       snapPills, MAX_SNAPSHOT_PILLS,
                                       snapEvents, MAX_SNAPSHOT_EVENTS)) {
              clientSimSyncFromSnapshot(cs, &snapHdr, snapTanks, snapHdr.tankCount,
                                     snapShells, snapHdr.shellCount,
                                     snapTkExplosions, snapHdr.tkExplosionCount,
                                     snapBases, snapHdr.baseCount,
                                     snapPills, snapHdr.pillCount,
                                     snapEvents, snapHdr.reliableEventCount,
                                     myPlayerNum);
            }
          }
          clientSimDisplayTick(cs, brainRunning);
          clientMutexRelease();
          simTickCounter++;
          ticks++;
          justKeysFlag = TRUE;
          used = TRUE;
        }
        oldTick += GAME_TICK_LENGTH;
        if (oldTick > ttick) {
          oldTick = ttick;
        }
      }
    }
  }
  dwSysGame += (winboloTimer() - ttick);

  /* AI */
  if (used == TRUE && inBrain == FALSE && brainRunning == TRUE && cs->netStat != netFailed) {
    clientMutexWaitFor();
    inBrain = TRUE;
    clientMutexRelease();
    ttick = winboloTimer();
    brainHandlerRun();
    dwSysBrain += winboloTimer() - ttick;
    clientMutexWaitFor();
    inBrain = FALSE;
    clientMutexRelease();
  }

  if (t2 >= 19) {
    netSecond();
    dwSysFrameTotal = dwSysFrame;
    dwSysGameTotal = dwSysGame;
    dwSysBrainTotal = dwSysBrain;
    dwSysBrain = 0;
    dwSysFrame = 0;
    dwSysGame = 0;
    t2 = 0;
    winboloCC();
  }

}

/* -------------------------------------------------------
 * windowFrameRateTimer — SDL3 timer callback (runs on timer thread)
 * ------------------------------------------------------- */
static Uint32 SDLCALL windowFrameRateTimer(void *userdata, SDL_TimerID timerID, Uint32 interval) {
  (void)userdata;
  (void)timerID;

  /* Don't run frame logic until a game has actually started */
  if (gameFrontGetTransport() == NULL) {
    return (Uint32)frameRateTime;
  }

  /* Signal the main thread to redraw — all SDL rendering must
     happen on the main thread (SDL_Renderer is not thread-safe). */
  if (hideMainView == FALSE) {
    SDL_SetAtomicInt(&needsRedraw, 1);
  }
  return (Uint32)frameRateTime;
}

/* -------------------------------------------------------
 * windowWnd — legacy compat (returns NULL on SDL3)
 * ------------------------------------------------------- */
void *windowWnd(void) {
  return NULL;
}

/* -------------------------------------------------------
 * windowSetQuitting — signal the main loop to exit
 * ------------------------------------------------------- */
void windowSetQuitting(void) {
  winboloQuit = TRUE;
  finishedLoop = TRUE;
}

/* -------------------------------------------------------
 * windowReCreate — prepare for game run
 * ------------------------------------------------------- */
void windowReCreate(void) {
  /* SDL3 draw is already set up by gameFrontStart.
   * Reset the window title back to the default after dialogs. */
  SDL_Window *win = sdl3DrawGetWindow();
  if (win) {
    SDL_SetWindowTitle(win, WIND_TITLE);
  }
}

/* -------------------------------------------------------
 * windowApplyMenuChecks — apply prefs to backend
 * ------------------------------------------------------- */
void windowApplyMenuChecks(ClientSim *cs) {
  screenSetGunsightCS(cs, showGunsight);
  screenSetAutoScroll(cs, autoScrollingEnabled);
  screenSetLabelOwnTank(cs, labelSelf);
  screenSetMesageLabelLen(cs, labelMsg);
  screenSetTankLabelLen(cs, labelTank);
  screenShowMessages(cs, MSG_NEWSWIRE, showNewswireMessages);
  screenShowMessages(cs, MSG_ASSISTANT, showAssistantMessages);
  screenShowMessages(cs, MSG_AI, showAIMessages);
  screenShowMessages(cs, MSG_NETSTATUS, showNetworkStatusMessages);
  screenShowMessages(cs, MSG_NETWORK, showNetworkDebugMessages);
}

/* -------------------------------------------------------
 * Timing info for System Info panel
 * ------------------------------------------------------- */
int windowGetDrawTime(void) {
  return (int)dwSysFrameTotal;
}

int windowGetNetTime(void) {
  return netGetNetTime();
}

int windowGetAiTime(void) {
  return (int)dwSysBrainTotal;
}

int windowGetSimTime(void) {
  return (int)dwSysGameTotal;
}

/* -------------------------------------------------------
 * Key get/set (used by Key Setup modal in sdl3imgui.cpp)
 * ------------------------------------------------------- */
void windowGetKeys(keyItems *value) {
  *value = keys;
}

void windowSetKeys(keyItems *value) {
  keys = *value;
}

/* -------------------------------------------------------
 * Zoom factor
 * ------------------------------------------------------- */
void windowSetZoomFactor(BYTE amount) {
  zoomFactor = amount;
}

BYTE windowGetZoomFactor(void) {
  return zoomFactor;
}

void windowZoomChange(BYTE amount) {
  if (amount == zoomFactor) {
    return;
  }
  drawBusy = TRUE;
  clientMutexWaitFor();
  sdl3DrawCleanup();
  sdl3DrawSetup(amount);
  {
    SDL_Window   *win = sdl3DrawGetWindow();
    SDL_Renderer *ren = sdl3DrawGetRenderer();
    if (win) {
      SDL_ShowWindow(win);
    }
    if (win && ren) {
      sdl3ImguiSetup(win, ren);
    }
  }
  clientMutexRelease();
  drawBusy = FALSE;

  windowSetZoomFactor(amount);
}

/* -------------------------------------------------------
 * Frame rate
 * ------------------------------------------------------- */
void windowSetFrameRate(int newFrameRate, bool setTimer) {
  switch (newFrameRate) {
  case FRAME_RATE_10: frameRateTime = (int)(MILLISECONDS / FRAME_RATE_10); break;
  case FRAME_RATE_12: frameRateTime = (int)(MILLISECONDS / FRAME_RATE_12); break;
  case FRAME_RATE_15: frameRateTime = (int)(MILLISECONDS / FRAME_RATE_15); break;
  case FRAME_RATE_20: frameRateTime = (int)(MILLISECONDS / FRAME_RATE_20) - 1; break;
  case FRAME_RATE_30: frameRateTime = (int)(MILLISECONDS / FRAME_RATE_30) - 1; break;
  case FRAME_RATE_50: frameRateTime = (int)(MILLISECONDS / FRAME_RATE_50) - 4; break;
  case FRAME_RATE_60: frameRateTime = (int)(MILLISECONDS / FRAME_RATE_60) - 4; break;
  }
  frameRate = newFrameRate;
  if (setTimer == TRUE && timerFrameID != 0) {
    SDL_RemoveTimer(timerFrameID);
    timerFrameID = SDL_AddTimer((Uint32)frameRateTime, windowFrameRateTimer, NULL);
  }
}

/* -------------------------------------------------------
 * Menu command handlers — called directly by sdl3imgui.cpp
 * instead of posting WM_COMMAND. No HWND/HMENU needed.
 * ------------------------------------------------------- */

void windowShowGunsight_toggle(ClientSim *cs) {
  showGunsight = !showGunsight;
  screenSetGunsightCS(cs, showGunsight);
}

void windowAutomaticScrolling_toggle(ClientSim *cs) {
  autoScrollingEnabled = !autoScrollingEnabled;
  if (cs) screenSetAutoScroll(cs, autoScrollingEnabled);
}

void windowShowPillLabels_toggle(ClientSim *cs) {
  BYTE count, total;

  showPillLabels = !showPillLabels;
  sdl3DrawSetPillsStatusClear();
  total = pillsGetNumPills(&cs->sim.pb);
  for (count = 1; count <= total; count++) {
    BYTE pillStat = screenPillAllianceCS(cs, count);
    sdl3DrawStatusPillbox(count, pillStat, showPillLabels);
  }
  sdl3DrawCopyPillsStatus(0, 0);
}

void windowShowBaseLabels_toggle(ClientSim *cs) {
  BYTE count, total;

  showBaseLabels = !showBaseLabels;
  sdl3DrawSetBasesStatusClear();
  total = basesGetNumBases(&cs->sim.bs);
  for (count = 1; count <= total; count++) {
    BYTE baseStat = screenBaseAllianceCS(cs, count);
    sdl3DrawStatusBase(count, baseStat, showBaseLabels);
  }
  sdl3DrawCopyBasesStatus(0, 0);
}

void windowSoundEffects_toggle(void) {
  soundEffects = !soundEffects;
}

void windowBackgroundSoundChange_toggle(void) {
  backgroundSound = !backgroundSound;
  if (soundEffects == TRUE) {
    soundKeepalive(backgroundSound ? useSoundKeepalive : FALSE);
  }
}

void windowSoundKeepalive(void) {
  useSoundKeepalive = !useSoundKeepalive;
  if (soundEffects == TRUE && soundIsPlayable() == TRUE) {
    soundKeepalive(useSoundKeepalive);
  }
}

void windowMenuAllowNewPlayers_toggle(ClientSim *cs) {
  allowNewPlayers = !allowNewPlayers;
  clientSimSetAllowNewPlayers(cs, allowNewPlayers);
}

void windowMenuNewswire_toggle(ClientSim *cs) {
  showNewswireMessages = !showNewswireMessages;
  if (cs) screenShowMessages(cs, MSG_NEWSWIRE, showNewswireMessages);
}

void windowMenuAssistant_toggle(ClientSim *cs) {
  showAssistantMessages = !showAssistantMessages;
  if (cs) screenShowMessages(cs, MSG_ASSISTANT, showAssistantMessages);
}

void windowMenuAI_toggle(ClientSim *cs) {
  showAIMessages = !showAIMessages;
  if (cs) screenShowMessages(cs, MSG_AI, showAIMessages);
}

void windowMenuNetwork_toggle(ClientSim *cs) {
  showNetworkStatusMessages = !showNetworkStatusMessages;
  if (cs) screenShowMessages(cs, MSG_NETSTATUS, showNetworkStatusMessages);
}

void windowMenuNetworkDebug_toggle(ClientSim *cs) {
  showNetworkDebugMessages = !showNetworkDebugMessages;
  if (cs) screenShowMessages(cs, MSG_NETWORK, showNetworkDebugMessages);
}

void windowHideMainView_toggle(void) {
  hideMainView = !hideMainView;
}

void windowLabelOwnTank_toggle(ClientSim *cs) {
  labelSelf = !labelSelf;
  if (cs) screenSetLabelOwnTank(cs, labelSelf);
}

void windowSetMessageLabelLen(ClientSim *cs, labelLen newLen) {
  labelMsg = newLen;
  if (cs) screenSetMesageLabelLen(cs, labelMsg);
}

void windowSetTankLabelLen(ClientSim *cs, labelLen newLen) {
  labelTank = newLen;
  if (cs) screenSetTankLabelLen(cs, labelTank);
}

void windowNewGame(void) {
  winboloQuit = FALSE;
  finishedLoop = TRUE;
}

void windowQuit(void) {
  winboloQuit = TRUE;
}

/* -------------------------------------------------------
 * Window show/hide stubs (info panels are ImGui windows)
 * ------------------------------------------------------- */
void windowShowGameInfo(windowShowRequest req) {
  sdl3ImguiShowGameInfo(req == wsrOpen);
}

void windowShowSysInfo(windowShowRequest req) {
  sdl3ImguiShowSysInfo(req == wsrOpen);
}

void windowShowNetInfo(windowShowRequest req) {
  sdl3ImguiShowNetInfo(req == wsrOpen);
}

void windowShowSetPlayerName(windowShowRequest req) {
  /* Handled directly by sdl3imgui.cpp s_showChangeName */
  (void)req;
}

void windowShowSendMessages(windowShowRequest req) {
  sdl3ImguiShowSendMsg(req == wsrOpen);
}

void windowShowSetKeys(windowShowRequest req) {
  if (req == wsrOpen) {
    sdl3ImguiShowKeySetup();
  }
}

void windowShowAboutBox(void) {
  /* Handled directly by sdl3imgui.cpp s_showAbout */
}

bool windowShowAllianceRequest(void) {
  return showAllianceReq;
}

void windowDisableSound(void) {
  soundEffects = FALSE;
  useSoundKeepalive = FALSE;
}

bool windowGetBackgroundSound(void) {
  return backgroundSound;
}

void windowRedrawAll(ClientSim *cs) {
  clientMutexWaitFor();
  sdl3DrawRedrawAll(cs, getBuildCurrentSelectCS(cs), NULL, showPillLabels, showBaseLabels);
  clientMutexRelease();
}

/* Save-map dialog callback state */
typedef struct {
  char path[FILENAME_MAX];
  int done;   /* 0 = waiting, 1 = got result */
  int ok;     /* 1 = user picked a file */
} SaveMapState;

static void SDLCALL saveMapCallback(void *userdata,
                                     const char * const *filelist,
                                     int filter) {
  SaveMapState *st = (SaveMapState *)userdata;
  (void)filter;
  if (filelist && filelist[0]) {
    strncpy(st->path, filelist[0], FILENAME_MAX - 1);
    st->path[FILENAME_MAX - 1] = '\0';
    st->ok = 1;
  }
  st->done = 1;
}

void windowSaveMap(ClientSim *cs) {
  char defaultName[FILENAME_MAX];
  SaveMapState state;
  SDL_DialogFileFilter filters[] = {
    { "Map Files", "map" },
  };

  memset(&state, 0, sizeof(state));
  screenGetMapNameCS(cs, defaultName);

  SDL_ShowSaveFileDialog(saveMapCallback, &state, sdl3DrawGetWindow(),
                         filters, 1, NULL);
  while (!state.done) {
    SDL_Event e;
    SDL_WaitEventTimeout(&e, 100);
  }
  if (state.ok) {
    if (screenSaveMapCS(cs, state.path) == FALSE) {
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE,
                               langGetText(STR_WBERR_SAVEMAP),
                               sdl3DrawGetWindow());
    }
  }
}

void windowKeyPressed(ClientSim *cs, int keyCode) {
  if (keyCode == keys.kiTankView) {
    screenTankViewCS(cs);
  } else if (keyCode == keys.kiPillView) {
    screenPillViewCS(cs, 0, 0);
  }
  if (keyCode == SDL_SCANCODE_F1) { debugOverlayToggle(); }
  if (keyCode == SDL_SCANCODE_F2) { debugZoomToggle(); }
}

void windowButtonAdd(int keyCode) {
  (void)keyCode;
}

void windowButtonRemove(int keyCode) {
  (void)keyCode;
}

void windowMouseClick(int xWin, int yWin, int xPos, int yPos) {
  (void)xWin;
  (void)yWin;
  (void)xPos;
  (void)yPos;
  /* Building selection is handled by sdl3imgui.cpp building selection panel */
}

void windowStartTutorial(void) {
  doingTutorial = TRUE;
}

/* -------------------------------------------------------
 * Frontend callbacks — called by backend (bolo engine)
 * ------------------------------------------------------- */

void frontEndDrawMainScreen(ClientSim *cs, screen *value, screenMines *mineView, screenTanks *tks,
                            screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms,
                            int32_t srtDelay, bool isPillView, tank *tank, int edgeX, int edgeY) {
  if (hideMainView == FALSE && drawBusy == FALSE) {
    BYTE cursorX, cursorY;
    bool showCursor;

    showCursor = screenGetCursorPosCS(cs, &cursorX, &cursorY);
    sdl3DrawSetNetFailed(cs->netStat == netFailed);
    sdl3DrawMainScreen(cs, value, mineView, tks, gs, sBullet, lgms,
                       NULL, showPillLabels, showBaseLabels,
                       srtDelay, isPillView, edgeX, edgeY,
                       showCursor, cursorX, cursorY, tank);
  }
}

void frontEndUpdateTankStatusBars(BYTE shells, BYTE mines, BYTE armour, BYTE trees) {
  DWORD tick = winboloTimer();
  if (armour > TANK_FULL_ARMOUR) {
    armour = 0;
  }
  sdl3DrawStatusTankBars(0, 0, shells, mines, armour, trees);
  dwSysFrame += (winboloTimer() - tick);
}

void frontEndPlaySound(sndEffects value) {
  if (soundEffects == TRUE) {
    soundPlayEffect(value);
  }
}

void windowPlaySound(sndEffects value) {
  if (soundEffects == TRUE) {
    soundPlayEffect(value);
  }
}

void frontEndStatusPillbox(BYTE pillNum, pillAlliance pb) {
  DWORD tick = winboloTimer();
  sdl3DrawStatusPillbox(pillNum, pb, showPillLabels);
  sdl3DrawCopyPillsStatus(0, 0);
  dwSysFrame += (winboloTimer() - tick);
}

void frontEndStatusTank(BYTE tankNum, tankAlliance ts) {
  DWORD tick = winboloTimer();
  sdl3DrawStatusTank(tankNum, ts);
  sdl3DrawCopyTanksStatus(0, 0);
  dwSysFrame += (winboloTimer() - tick);
}

void frontEndMessages(char *top, char *bottom) {
  if (drawBusy == FALSE) {
    DWORD tick = winboloTimer();
    sdl3DrawMessages(0, 0, top, bottom);
    dwSysFrame += (winboloTimer() - tick);
  }
}

void frontEndKillsDeaths(int kills, int deaths) {
  if (drawBusy == FALSE) {
    DWORD tick = winboloTimer();
    sdl3DrawKillsDeaths(0, 0, kills, deaths);
    dwSysFrame += (winboloTimer() - tick);
  }
}

void frontEndStatusBase(BYTE baseNum, baseAlliance bs) {
  DWORD tick = winboloTimer();
  sdl3DrawStatusBase(baseNum, bs, showBaseLabels);
  sdl3DrawCopyBasesStatus(0, 0);
  dwSysFrame += (winboloTimer() - tick);
}

void frontEndUpdateBaseStatusBars(BYTE shells, BYTE mines, BYTE armour) {
  DWORD tick = winboloTimer();
  sdl3DrawStatusBaseBars(0, 0, shells, mines, armour, FALSE);
  dwSysFrame += (winboloTimer() - tick);
}

void frontEndManStatus(bool isDead, TURNTYPE angle) {
  DWORD tick = winboloTimer();
  clientMutexWaitFor();
  sdl3DrawSetManStatus(0, 0, isDead, angle);
  clientMutexRelease();
  dwSysFrame += (winboloTimer() - tick);
}

void frontEndManClear(void) {
  DWORD tick = winboloTimer();
  clientMutexWaitFor();
  sdl3DrawSetManClear();
  sdl3DrawCopyManStatus(0, 0);
  clientMutexRelease();
  dwSysFrame += (winboloTimer() - tick);
}

void frontEndDrawDownload(ClientSim *cs, bool justBlack) {
  if (hideMainView == FALSE && drawBusy == FALSE) {
    DWORD tick = winboloTimer();
    sdl3DrawDownloadScreen(cs, NULL, justBlack);
    dwSysFrame += (winboloTimer() - tick);
  }
}

void frontEndGameOver(void) {
  SDL_RemoveTimer(timerFrameID);
  SDL_RemoveTimer(timerGameID);
  timerFrameID = 0;
  timerGameID = 0;
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE,
                           langGetText(STR_WBTIMELIMIT_END), sdl3DrawGetWindow());
  winboloQuit = TRUE;
}

void frontEndClearPlayer(playerNumbers value) {
  sdl3ImguiClearPlayer((unsigned char)value);
}

void frontEndSetPlayer(ClientSim *cs, playerNumbers value, char *str, const char *countryCode, uint16_t ping, bool wbnParticipant, bool steamParticipant) {
  char cc[3];
  if (!screenGetGameRunningCS(cs)) {
    cc[0] = 'X'; cc[1] = 'X'; cc[2] = '\0';
    sdl3ImguiSetPlayer((unsigned char)value, str, cc);
    return;
  }
  cc[0] = countryCode[0];
  cc[1] = countryCode[1];
  cc[2] = '\0';
  SDL_Log("[FLAGS] frontEndSetPlayer: player=%d name='%s' cc='%s' (0x%02X 0x%02X)", (int)value, str, cc, (unsigned char)cc[0], (unsigned char)cc[1]);
  sdl3ImguiSetPlayer((unsigned char)value, str, cc);
  sdl3ImguiUpdatePlayerMeta((unsigned char)value, ping, wbnParticipant, steamParticipant);
}

void frontEndSetPlayerCheckState(playerNumbers value, bool isChecked) {
  sdl3ImguiSetPlayerCheckState((unsigned char)value, isChecked);
}

/* -------------------------------------------------------
 * frontEndShowAllianceRequest — called from network thread
 * ------------------------------------------------------- */
void frontEndShowAllianceRequest(char *playerName, BYTE playerNum) {
  if (showAllianceReq) {
    sdl3ImguiShowAllianceRequest(playerName, playerNum);
  }
}

/* -------------------------------------------------------
 * Tick counter for server cheat prevention
 * ------------------------------------------------------- */
time_t serverMainGetTicks(void) {
  return ticks;
}

/* -------------------------------------------------------
 * windowsGetTicks — monotonic tick counter for network.c
 * ------------------------------------------------------- */
time_t windowsGetTicks(void) {
  return ticks;
}

/* -------------------------------------------------------
 * windowAllowPlayerNameChange — enable/disable name change
 * In ImGui there is no per-menu-item graying; the ImGui
 * layer handles this via state flags. No-op here.
 * ------------------------------------------------------- */
void windowAllowPlayerNameChange(bool allow) {
  (void)allow;
}

/* -------------------------------------------------------
 * frontEndEnableRequestAllyMenu / frontEndEnableLeaveAllyMenu
 * Menu item enable/disable — no-op in ImGui build.
 * ------------------------------------------------------- */
void frontEndEnableRequestAllyMenu(bool enabled) {
  (void)enabled;
}

void frontEndEnableLeaveAllyMenu(bool enabled) {
  (void)enabled;
}

/* -------------------------------------------------------
 * frontEndRedrawAll — called by backend to force redraw
 * ------------------------------------------------------- */
void frontEndRedrawAll(ClientSim *cs) {
  if (!screenGetGameRunningCS(cs)) return;
  windowRedrawAll(cs);
}

/* -------------------------------------------------------
 * frontEndShowGunsight — auto show/hide gunsight callback
 * ------------------------------------------------------- */
void frontEndShowGunsight(ClientSim *cs, bool isShown) {
  showGunsight = !isShown;
  screenSetGunsightCS(cs, showGunsight);
}

/* -------------------------------------------------------
 * frontEndTutorial — tutorial position-triggered messages
 * Uses SDL_ShowSimpleMessageBox instead of Win32 MessageBoxA.
 * ------------------------------------------------------- */
bool frontEndTutorial(BYTE pos) {
  static BYTE upTo = 0;
  bool returnValue = FALSE;

  if (isTutorial == TRUE) {
    switch (upTo) {
    case 0:
      if (pos == 208) {
        doingTutorial = TRUE;
        clientMutexRelease();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL01), sdl3DrawGetWindow());
        returnValue = TRUE;
        upTo++;
        clientMutexWaitFor();
        doingTutorial = FALSE;
        oldTick = winboloTimer();
        ttick = oldTick;
      }
      break;
    case 1:
      if (pos == 197) {
        doingTutorial = TRUE;
        clientMutexRelease();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL02), sdl3DrawGetWindow());
        returnValue = TRUE;
        upTo++;
        clientMutexWaitFor();
        doingTutorial = FALSE;
        oldTick = winboloTimer();
        ttick = oldTick;
      }
      break;
    case 2:
      if (pos == 192) {
        doingTutorial = TRUE;
        clientMutexRelease();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL03), sdl3DrawGetWindow());
        returnValue = TRUE;
        upTo++;
        clientMutexWaitFor();
        doingTutorial = FALSE;
        oldTick = winboloTimer();
        ttick = oldTick;
      }
      break;
    case 3:
      if (pos == 186) {
        doingTutorial = TRUE;
        clientMutexRelease();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL04), sdl3DrawGetWindow());
        returnValue = TRUE;
        upTo++;
        clientMutexWaitFor();
        doingTutorial = FALSE;
        oldTick = winboloTimer();
        ttick = oldTick;
      }
      break;
    case 4:
      if (pos == 181) {
        doingTutorial = TRUE;
        clientMutexRelease();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL05), sdl3DrawGetWindow());
        returnValue = TRUE;
        upTo++;
        clientMutexWaitFor();
        doingTutorial = FALSE;
        oldTick = winboloTimer();
        ttick = oldTick;
      }
      break;
    case 5:
      if (pos == 175) {
        doingTutorial = TRUE;
        clientMutexRelease();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL06), sdl3DrawGetWindow());
        returnValue = TRUE;
        upTo++;
        clientMutexWaitFor();
        doingTutorial = FALSE;
        oldTick = winboloTimer();
        ttick = oldTick;
      }
      break;
    case 6:
      if (pos == 166) {
        doingTutorial = TRUE;
        clientMutexRelease();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL07), sdl3DrawGetWindow());
        returnValue = TRUE;
        upTo++;
        clientMutexWaitFor();
        doingTutorial = FALSE;
        oldTick = winboloTimer();
        ttick = oldTick;
      }
      break;
    case 7:
      if (pos == 159) {
        doingTutorial = TRUE;
        clientMutexRelease();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL08), sdl3DrawGetWindow());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL09), sdl3DrawGetWindow());
        returnValue = TRUE;
        upTo++;
        upTo++;
        clientMutexWaitFor();
        doingTutorial = FALSE;
        oldTick = winboloTimer();
        ttick = oldTick;
      }
      break;
    case 8:
      if (pos == 1) {
        returnValue = TRUE;
        upTo++;
      }
      break;
    case 9:
      if (pos == 142) {
        doingTutorial = TRUE;
        clientMutexRelease();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL10), sdl3DrawGetWindow());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL11), sdl3DrawGetWindow());
        returnValue = TRUE;
        upTo++;
        upTo++;
        clientMutexWaitFor();
        doingTutorial = FALSE;
        oldTick = winboloTimer();
        ttick = oldTick;
      }
      break;
    case 10:
      if (pos == 1) {
        returnValue = TRUE;
        upTo++;
      }
      break;
    case 11:
      if (pos == 122) {
        doingTutorial = TRUE;
        clientMutexRelease();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL12), sdl3DrawGetWindow());
        returnValue = TRUE;
        upTo++;
        clientMutexWaitFor();
        doingTutorial = FALSE;
        oldTick = winboloTimer();
        ttick = oldTick;
      }
      break;
    case 12:
      if (pos == 120) {
        doingTutorial = TRUE;
        clientMutexRelease();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL13), sdl3DrawGetWindow());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL14), sdl3DrawGetWindow());
        returnValue = TRUE;
        upTo++;
        upTo++;
        clientMutexWaitFor();
        doingTutorial = FALSE;
        oldTick = winboloTimer();
        ttick = oldTick;
      }
      break;
    case 13:
      if (pos == 1) {
        returnValue = TRUE;
        upTo++;
      }
      break;
    case 14:
      if (pos == 110) {
        doingTutorial = TRUE;
        clientMutexRelease();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL15), sdl3DrawGetWindow());
        returnValue = TRUE;
        upTo++;
        clientMutexWaitFor();
        doingTutorial = FALSE;
        oldTick = winboloTimer();
        ttick = oldTick;
      }
      break;
    case 15:
      if (pos == 103) {
        doingTutorial = TRUE;
        clientMutexRelease();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL16), sdl3DrawGetWindow());
        returnValue = TRUE;
        upTo++;
        clientMutexWaitFor();
        doingTutorial = FALSE;
        oldTick = winboloTimer();
        ttick = oldTick;
      }
      break;
    case 16:
      if (pos == 98) {
        doingTutorial = TRUE;
        clientMutexRelease();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL17), sdl3DrawGetWindow());
        returnValue = TRUE;
        upTo++;
        clientMutexWaitFor();
        doingTutorial = FALSE;
        oldTick = winboloTimer();
        ttick = oldTick;
      }
      break;
    case 17:
      if (pos == 84) {
        doingTutorial = TRUE;
        clientMutexRelease();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL18), sdl3DrawGetWindow());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL19), sdl3DrawGetWindow());
        returnValue = TRUE;
        upTo++;
        upTo++;
        clientMutexWaitFor();
        doingTutorial = FALSE;
        oldTick = winboloTimer();
        ttick = oldTick;
      }
      break;
    case 18:
      if (pos == 1) {
        returnValue = TRUE;
        upTo++;
      }
      break;
    case 19:
      if (pos == 66) {
        doingTutorial = TRUE;
        clientMutexRelease();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL20), sdl3DrawGetWindow());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL21), sdl3DrawGetWindow());
        returnValue = TRUE;
        upTo++;
        upTo++;
        clientMutexWaitFor();
        doingTutorial = FALSE;
        oldTick = winboloTimer();
        ttick = oldTick;
      }
      break;
    case 20:
      if (pos == 1) {
        returnValue = TRUE;
        upTo++;
      }
      break;
    case 21:
      if (pos == 47) {
        doingTutorial = TRUE;
        clientMutexRelease();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL22), sdl3DrawGetWindow());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL23), sdl3DrawGetWindow());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL24), sdl3DrawGetWindow());
        returnValue = TRUE;
        upTo++;
        upTo++;
        upTo++;
        clientMutexWaitFor();
        doingTutorial = FALSE;
        oldTick = winboloTimer();
        ttick = oldTick;
      }
      break;
    case 22:
      if (pos == 1) {
        returnValue = TRUE;
        upTo++;
      }
      break;
    case 23:
      if (pos == 1) {
        returnValue = TRUE;
        upTo++;
      }
      break;
    case 24:
      if (pos == 21) {
        doingTutorial = TRUE;
        clientMutexRelease();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, DIALOG_BOX_TITLE, langGetText(STR_TUTORIAL25), sdl3DrawGetWindow());
        returnValue = TRUE;
        upTo++;
        SDL_RemoveTimer(timerGameID);
        clientMutexWaitFor();
        doingTutorial = FALSE;
        oldTick = winboloTimer();
        ttick = oldTick;
      }
      break;
    default:
      break;
    }
  }

  return returnValue;
}

/* -------------------------------------------------------
 * winboloCC — coin check (Win32-only anti-cheat).
 * No-op on SDL3 — EnumWindows is Windows-specific.
 * ------------------------------------------------------- */
int winboloCC(void) {
  return 0;
}

/* -------------------------------------------------------
 * dialogAllianceCreate / dialogAllianceDestroy / dialogAllianceSetName
 * Alliance dialog is now an ImGui panel — these just
 * delegate to the sdl3imgui alliance request API.
 * ------------------------------------------------------- */
void *dialogAllianceCreate(void) {
  return NULL;
}

void dialogAllianceDestroy(void *dlg) {
  (void)dlg;
}

void dialogAllianceSetName(char *playerName, BYTE playerNum) {
  if (playerName != NULL) {
    sdl3ImguiShowAllianceRequest(playerName, playerNum);
  }
}

/* -------------------------------------------------------
 * winUtilWBSubDirExist — cross-platform directory check
 * Checks if a subdirectory exists. On success, modifies
 * subDirName to the full path (prepends exe directory).
 * ------------------------------------------------------- */
#include <sys/stat.h>

bool winUtilWBSubDirExist(char *subDirName) {
#ifdef _WIN32
  /* On Windows, get the directory of the running executable */
  char exePath[FILENAME_MAX];
  char fullPath[FILENAME_MAX];
  DWORD len = GetModuleFileNameA(NULL, exePath, FILENAME_MAX);
  unsigned int i;
  struct _stat st;

  if (len == 0) return FALSE;

  /* Strip the executable name to get the directory */
  i = (unsigned int)len;
  while (i > 0 && exePath[i] != '\\' && exePath[i] != '/') {
    i--;
  }
  exePath[i + 1] = '\0';

  snprintf(fullPath, FILENAME_MAX, "%s%s", exePath, subDirName);

  if (_stat(fullPath, &st) == 0 && (st.st_mode & _S_IFDIR)) {
    strcpy(subDirName, fullPath);
    return TRUE;
  }
  return FALSE;
#else
  struct stat st;
  if (stat(subDirName, &st) == 0 && S_ISDIR(st.st_mode)) {
    return TRUE;
  }
  return FALSE;
#endif
}
