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
#include <process.h>
#define chdir _chdir
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "../../common/wb_log.h"
#include "../../winbolonet/winbolonet_core.h"
#include "bolo_rand.h"
#include "platform_net.h"
#include "client_render.h"
#include "client_sim.h"
#include "frontend.h"
#include "tutorial.h"
#include "../../steam/steam_wrapper.h"
#include "../../steam/steam_input_actions.h"
#include "client_net.h"
#include "server_sim.h"
#include "../../server/threads.h"
#include "gui_message.h"
#include "../brainsHandler.h"
#include "../clientmutex.h"
#include "../draw.h"
#include "../gamefront.h"
#include "input.h"
#include "build_cursor.h"
#include "../lang.h"
#include "../sound.h"
#include "../winbolo.h"
#include "sdl3draw.h"
#include "sdl3imgui.h"
#include "../tiles.h"
#include "luabrainshandler.h"
#include "bg_game.h"
#include "cursor.h"

#include "dialog_backend.h"
#include "dialogs/imgui_messagebox.h"
#include "tutorial_text.h"
#include "../../common/sentry_integration.h"

/* humanSim is owned by gamefront.c; declared up here so the timer
 * callback and main game-tick path can pass it to clientSim* wrappers. */
extern ClientSim *humanSim;

/* Forward declarations */
void sdl3MessageHandler(const char *message, const char *title);
static SDL_Rect getDefaultDisplayBounds(void);

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

/* Master volume (0-100); applied to the audio stream gain */
int soundVolume = 50;

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

/* Smooth (pixel-level) scrolling for arrow keys.
   When TRUE, arrow keys scroll the map by sub-tile pixel
   amounts each tick.  When FALSE, the legacy 1-tile-at-a-time
   behavior (gated by INPUT_SCROLL_WAIT_TIME) is used. */
bool smoothScrollingEnabled = TRUE;

/* The Window scaling */
BYTE zoomFactor = ZOOM_FACTOR_NORMAL;

/* When TRUE, the letterbox/pillarbox bars shown in fullscreen (when the
   monitor aspect differs from the game) are filled gray instead of black. */
bool letterboxBarsGray = FALSE;

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

/* Set on SDL_EVENT_WILL_ENTER_BACKGROUND, cleared by
 * windowResumeForeground.  See windowSuspendBackground for details. */
static bool s_suspended = FALSE;

/* Set while the Steam in-game overlay is open during a single-player
 * game.  Like s_suspended it freezes the local sim, but it is driven by
 * the Steam overlay callback (not SDL background events) and only ever
 * engages in single-player — multiplayer keeps running with the overlay
 * up.  See windowSteamOverlayActivated. */
static bool s_overlayPaused = FALSE;

/* Set while the "Controller Disconnected" dialog is up during a solo game.
 * Same freeze as s_overlayPaused, driven by the controller-lost handler.
 * Multiplayer keeps running (the dialog still shows).  See
 * windowControllerLostPause. */
static bool s_controllerLostPaused = FALSE;

/* Set while the controller pause overlay (Start button / Escape) is up during
 * a solo game.  Same freeze as s_overlayPaused, driven by the pause-menu
 * open/close edge.  Multiplayer keeps running (the menu still shows) so a
 * networked player isn't booted for going idle.  See windowDeckPause. */
static bool s_deckPaused = FALSE;

/* Mute state captured when each pause path engaged, restored verbatim when it
 * releases.  A pause must hand audio back to whatever it found — not force it
 * unmuted — so it doesn't clobber a user mute or another still-active pause's
 * mute.  One slot per path because the paths can nest. */
static bool s_suspendPrevMuted        = FALSE;
static bool s_overlayPrevMuted        = FALSE;
static bool s_controllerLostPrevMuted = FALSE;
static bool s_deckPrevMuted           = FALSE;

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

/* The ClientSim that drives the visible player-list UI. Registered by
 * gamefront when humanSim is created; cleared on teardown. frontEnd*
 * calls whose cs argument doesn't match this pointer are suppressed,
 * so bot / bg_game / gym ClientSims can't pollute process-global
 * UI state (s_playerName, s_playerEnabled, ...).
 * NULL = no registration yet; calls fall through (bootstrapping). */
static ClientSim *s_activeUiCs = NULL;

/* Forward declarations */
static Uint32 SDLCALL windowGameTimer(void *userdata, SDL_TimerID timerID, Uint32 interval);
static Uint32 SDLCALL windowFrameRateTimer(void *userdata, SDL_TimerID timerID, Uint32 interval);
void frontEndTutorialNotePresentedFrame(void);
static void windowRunGameTick(ClientSim *cs);
static void windowUpdateServerPause(ClientSim *cs);
static void windowSteamOverlayActivated(ClientSim *cs, bool active);
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

  bolo_srand((uint64_t)time(NULL) ^ (uint64_t)getpid());

  /* Hold a process-wide Winsock reference for the app's lifetime. The
   * server-ping path (discoveryPingServer) does a WSAStartup/WSACleanup
   * pair per call, and runs concurrently from the game-browser's
   * fire-and-forget ping threads and the main thread's pre-flight host
   * ping. Without this baseline the refcount can hit zero when one ping
   * finishes while others have open sockets, and WSACleanup at zero
   * forcibly deallocates every socket in the process and cancels pending
   * blocking calls — corrupting the in-flight pings (observed as a
   * 0xc0000409 stack/heap fault on internet -> new -> back -> new). The
   * baseline keeps the count >= 1 so per-call pairs only ever go 2<->1.
   * Process exit reclaims it; no matching cleanup needed. */
  bolo_net_init();

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

  wb_log_init("WinBolo", "WinBolo", "winbolo.log");
  atexit(wb_log_shutdown);

  if (!serverSimBotPoolInit(0)) {
    fprintf(stderr, "serverSimBotPoolInit failed\n");
    return 1;
  }

  steam_init();
  steam_set_join_callback(steamJoinRequested);
  /* Steam Input (Path A): start in Menu set — game launches into the
     main menu / lobby UI.  In-game switch handled per-frame in
     sdl3ImguiPumpAndRender.  No-op when running without the SDK. */
  steam_input_init();
  steam_input_activate_action_set(SI_SET_MENU);

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


  if (clientMutexCreate() == FALSE) {
    imguiMessageBoxEx(DIALOG_BOX_TITLE, "Failed to create client mutex",
                      IMGUI_MSG_ERROR, IMGUI_MSG_OK);
    return 1;
  }

  /* Threads mutex must exist BEFORE gameFrontStart — the menu's
   * background bot game (bgGameCreate → serverSimCreateBot →
   * serverSimPublishControl → clientMutexWaitFor → threadsWaitForMutex)
   * runs during setup dialogs. Without this, SDL_LockMutex silently
   * no-ops on the NULL handle and the bg game's "lock" is fictional. */
  threadsCreate(FALSE);

  if (gameFrontStart(cmdLine, &keys, FALSE, &cs) == FALSE) {
    clientMutexDestroy();
    SDL_Quit();
    return 1;
  }

  /* The server threads mutex outlives every per-session start/end cycle:
   * SDL's timer thread may still be running hostedServerTimerCb (which waits
   * on this mutex) while a session shuts down, and SDL_Quit only joins the
   * timer thread at the very end of main. Destroying the mutex before then
   * would strand that waiter on freed memory. */
  if (threadsCreate(FALSE) == FALSE) {
    clientMutexDestroy();
    SDL_Quit();
    return 1;
  }

  winboloQuit = FALSE;
  while (winboloQuit == FALSE) {
    /* Show lobby dialog if the server uses lobby mode */
    if (cs && clientSimIsInLobby(cs) &&
        (clientSimGetNetStatus(cs) == netLobby || clientSimGetNetStatus(cs) == netLobbyCountdown)) {
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
      /* lobbyResult == 1: game started. Both MP and SP have already
       * had their world installed (MP via the UDP transport's
       * CTRL_GAME_PHASE LOBBY→RUNNING watcher; SP via the local
       * transport's localTick path), so the main loop just flips the
       * net status and falls into the per-frame game tick. */
      clientSimSetNetStatus(cs, netRunning);
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
    oldTick = SDL_GetTicks();
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

      /* threadsCreate now runs earlier (right after clientMutexCreate)
       * so the menu's background bot game sees a real threads mutex. */

      /* Flush any stale SDL_QUIT events that may have been queued during
         dialog teardown. Without this, the main loop would exit immediately
         on its first sdl3ImguiProcessEvents() call. */
      SDL_FlushEvent(SDL_EVENT_QUIT);

      if (sdlWin) {
        /* All modes resizable - resizing auto-switches to Custom */
        SDL_SetWindowResizable(sdlWin, true);

        /* Get the monitor where the dialog/splash was shown.
           First try the saved dialog position, then fall back to the
           window's current position (it's the same SDL window from
           the dialog phase), then the primary display. */
        SDL_Rect usable;
        usable = getDefaultDisplayBounds();
        {
          SDL_DisplayID dispID = 0;
          SDL_Point dialogPt = { gameFrontDialogX, gameFrontDialogY };
          if (dialogPt.x >= 0 && dialogPt.y >= 0) {
            dispID = SDL_GetDisplayForPoint(&dialogPt);
          }
          if (!dispID) {
            /* Dialog position unknown — use the window's current display */
            dispID = SDL_GetDisplayForWindow(sdlWin);
          }
          if (dispID) {
            SDL_GetDisplayUsableBounds(dispID, &usable);
          }
        }

        /* Determine target size, falling back to smaller cardinal if needed */
        int targetW, targetH;
        if (zoomFactor == ZOOM_FACTOR_CUSTOM) {
          windowGetCustomSize(&targetW, &targetH);
          if (targetW <= 0 || targetH <= 0) {
            targetW = 2 * SDL3_SCREEN_W;
            targetH = 2 * SDL3_SCREEN_H + MENU_BAR_HEIGHT;
          }
        } else {
          targetW = sdl3DrawGetZoomFactor() * SDL3_SCREEN_W;
          targetH = sdl3DrawGetZoomFactor() * SDL3_SCREEN_H + MENU_BAR_HEIGHT;
        }

        /* If target doesn't fit on this monitor, fall back to smaller cardinal sizes */
        if (targetW > usable.w || targetH > usable.h) {
          /* Try 4x, 3x, 2x, 1x until one fits */
          for (int z = 4; z >= 1; z--) {
            int cardW = z * SDL3_SCREEN_W;
            int cardH = z * SDL3_SCREEN_H + MENU_BAR_HEIGHT;
            if (cardW <= usable.w && cardH <= usable.h) {
              targetW = cardW;
              targetH = cardH;
              /* Update zoomFactor to match */
              zoomFactor = (BYTE)z;
              break;
            }
          }
        }

        /* Big Picture / Gamepad UI: leave the window maximised (set at
           creation) — don't size or reposition it from saved desktop prefs. */
        if (!steam_is_big_picture()) {
          SDL_SetWindowSize(sdlWin, targetW, targetH);

          /* Restore saved window position from preferences, but ensure it's on this monitor */
          {
            int savedX, savedY;
            windowGetSavedPosition(&savedX, &savedY);
            if (savedX >= 0 && savedY >= 0) {
              /* Clamp position to keep window on the target monitor */
              if (savedX + targetW > usable.x + usable.w) savedX = usable.x + usable.w - targetW;
              if (savedY + targetH > usable.y + usable.h) savedY = usable.y + usable.h - targetH;
              if (savedX < usable.x) savedX = usable.x;
              if (savedY < usable.y) savedY = usable.y;
              SDL_SetWindowPosition(sdlWin, savedX, savedY);
            } else {
              /* Center on the dialog's monitor */
              int centeredX = usable.x + (usable.w - targetW) / 2;
              int centeredY = usable.y + (usable.h - targetH) / 2;
              SDL_SetWindowPosition(sdlWin, centeredX, centeredY);
            }
          }
        }
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
        /* Steam overlay open/close (updated by steam_run_callbacks above):
           pause a single-player game — freezing both the client and the
           server tick — and rebase the wallclock on close so the catch-up
           loop doesn't fast-forward the overlay duration.  Multiplayer is a
           no-op; a networked session must keep running with the overlay up. */
        {
          static bool s_lastOverlay = false;
          bool nowOverlay = steam_overlay_is_active();
          if (nowOverlay != s_lastOverlay) {
            windowSteamOverlayActivated(cs, nowOverlay);
            s_lastOverlay = nowOverlay;
          }
        }
        steam_input_run_frame();

        /* Keep in-game rich presence fresh — live player count, and the
         * host's external connect address once the tracker resolves it.
         * Throttled internally; gated to the running game so lobby/countdown
         * frames don't stomp the lobby presence. */
        if (cs && clientSimGetNetStatus(cs) == netRunning) {
          gameFrontTickSteamPresenceGame(cs);
        }

        /* Run game tick on main thread when timer signals */
        if (SDL_GetAtomicInt(&needsGameTick)) {
          SDL_SetAtomicInt(&needsGameTick, 0);
          windowRunGameTick(cs);
        }

        /* Detect game-over returning to lobby */
        if (cs && clientSimIsInLobby(cs) &&
            (clientSimGetNetStatus(cs) == netLobby || clientSimGetNetStatus(cs) == netLobbyCountdown)) {
          returnToLobby = TRUE;
          done = TRUE;
        }

        /* Redraw every frame — vsync throttles the present rate.
           Previously only redrawn on timer signal, but with double-
           buffering that left stale content in the alternate backbuffer,
           causing a visible "jump-back" flicker. */
        {
          DWORD tick = SDL_GetTicks();
          clientMutexWaitFor();
          if (finishedLoop == FALSE) {
            clientSimRenderPrepare(cs, tick);
            clientRenderFrame(cs, redraw);
          }
          clientMutexRelease();
          dwSysFrame += (SDL_GetTicks() - tick);
        }
        /* Consume the timer signal so it doesn't accumulate */
        SDL_SetAtomicInt(&needsRedraw, 0);

        /* Render the ImGui overlay and present the frame */
        sdl3ImguiPumpAndRender(cs);
        {
          SDL_Renderer *ren = sdl3DrawGetRenderer();
          if (ren) SDL_RenderPresent(ren);
        }
        frontEndTutorialNotePresentedFrame();

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

  /* Stop the hosted-server tick timer BEFORE we tear down the
   * mutex it grabs — otherwise SDL_Quit waits for the timer thread,
   * the timer fires hostedServerTimerCb one last time, and
   * threadsReleaseMutex hits "mutex not owned by this thread"
   * because clientMutexDestroy already nuked the mutex. */
  gameFrontShutdownServer();

  clientMutexDestroy();
  /* Explicit cleanup before SDL_Quit so leak checks see freed memory */
  sdl3ImguiCleanup();
  /* Tear down the process-lifetime welcome-screen bg before the renderer
   * and the bot pool: bgGameDestroy calls SDL_DestroyTexture on
   * bg->tilesTex (renderer must still be alive — SDL3 docs say destroying
   * a renderer invalidates its child textures, so destroying a texture
   * afterwards is UB), and bg destruction publishes control events through
   * its sim's subscribers (worker pool must still be live for that flush). */
  {
    BgGame *bg = bgGameGetShared();
    if (bg != NULL) {
      bgGameSetShared(NULL);
      bgGameDestroy(bg);
      SDL_free(bg);
    }
  }
  sdl3DrawCleanup();
  /* Tear down Steam Input before the parent Steam API — Shutdown
     calls into ISteamInput which requires the SteamAPI to be alive. */
  steam_input_shutdown();
  steam_shutdown();
  serverSimBotPoolDestroy();
  SDL_Quit();
  threadsDestroy();
  sentryClose();
  return 0;
}

/* -------------------------------------------------------
 * SDL message handler for guiMessageSetHandler
 * ------------------------------------------------------- */
void sdl3MessageHandler(const char *message, const char *title) {
  imguiMessageBoxEx(title ? title : "WinBolo",
                    message ? message : "",
                    IMGUI_MSG_INFO, IMGUI_MSG_OK);
}

/* -------------------------------------------------------
 * windowGameTimer — SDL3 timer callback (runs on timer thread)
 * Just signals the main thread; all game logic runs there.
 * ------------------------------------------------------- */
static Uint32 SDLCALL windowGameTimer(void *userdata, SDL_TimerID timerID, Uint32 interval) {
  (void)userdata;
  (void)timerID;

  if (clientSimHasTransport(humanSim)) {
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

  /* App is backgrounded (Deck home button / sleep), the Steam overlay is
     open, the controller-disconnected dialog is up, or the controller pause
     menu is open in a solo game — skip all tick work.  The matching resume
     (windowResumeForeground / windowSteamOverlayActivated /
     windowControllerLostPause / windowDeckPause) resets the wallclock baseline
     so we don't fast-forward the paused interval. */
  if (s_suspended || s_overlayPaused || s_controllerLostPaused || s_deckPaused)
    return;

  brainRunning = brainHandlerIsBrainRunning();
  isShoot = FALSE;
  tb = 0;

  /* Check if the UDP server has disconnected or timed out.
   * Only check for UDP transports (serverSim == NULL means not local). */
  if (clientSimHasTransport(cs) && gameFrontGetServerSim() == NULL &&
      clientSimGetConnectState(cs) == CLIENT_CONNECT_SERVER_SHUTDOWN) {
    /* A localized reason is set when the client itself gave up (e.g. an
     * unrecoverable map desync); a plain server shutdown leaves it NULL and
     * falls back to the generic lost-connection message. */
    const char *reason = clientSimGetConnectErrorReason(cs);
    clientSimConnectionLost(cs);
    imguiMessageBoxEx(DIALOG_BOX_TITLE,
                      reason ? reason
                             : "You have lost your connection to the server.\n"
                               "Returning to menu.",
                      IMGUI_MSG_ERROR, IMGUI_MSG_OK);
    finishedLoop = TRUE;
    winboloQuit = FALSE;
    return;
  }

  ttick = SDL_GetTicks();
  /* Update the game objects if required */
  if ((ttick - oldTick) > GAME_TICK_LENGTH) {
    while ((ttick - oldTick) > GAME_TICK_LENGTH) {
      if (doingTutorial == FALSE) {
        BYTE myPlayerNum = gameFrontGetPlayerNum();
        if (clientSimGetNetStatus(cs) == netLobby || clientSimGetNetStatus(cs) == netLobbyCountdown) {
          /* Lobby/countdown: just tick the transport to receive packets */
          clientSimNetTick(cs);
          justKeysFlag = !justKeysFlag; /* Alternate to maintain tick cadence */
        } else if (justKeysFlag == TRUE) {
          /* Keys tick */
          if (brainRunning == FALSE) {
            tb = inputGetKeys(cs, &keys, isInMenu);
          } else {
            inputScroll(cs, &keys, isInMenu);
          }
          InputPacket pkt;
          clientBuildInputPacket(cs, &pkt, tb, FALSE, FALSE, brainRunning, FALSE, myPlayerNum, simTickCounter);
          if (!brainRunning) {
            uint8_t gsAdj = inputConsumeGunsightAdj();
            if (gsAdj) pkt.flags |= ((gsAdj & 0x3) << INPUT_FLAG_GUNSIGHT_SHIFT);
          }
          clientMutexWaitFor();
          clientSimKeysTick(cs, &pkt);
          clientMutexRelease();
          clientSimNetRecordInput(cs, &pkt);
          clientSimNetTick(cs);
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
          clientBuildInputPacket(cs, &pkt, tb, isShoot, isMine, brainRunning, TRUE, myPlayerNum, simTickCounter);
          if (!brainRunning) {
            uint8_t gsAdj = inputConsumeGunsightAdj();
            if (gsAdj) pkt.flags |= ((gsAdj & 0x3) << INPUT_FLAG_GUNSIGHT_SHIFT);
          }
          clientMutexWaitFor();
          clientSimGameTick(cs, &pkt, brainRunning);
          clientMutexRelease();
          clientSimNetSendInput(cs, &pkt);
          /* Bot brains tick on the server timer thread (hostedServerTimerCb
           * -> serverInstanceTick -> botManagerTick) for both single-player
           * and listen-server, under threadsMutex. The System Info "AI
           * Tanks" line reads wall-clock bot cost from
           * serverSimGetBotPoolStats().lastBrainPhaseMs (see sdl3imgui.cpp)
           * rather than dwSysBrain, so there is no main-thread accounting
           * to do here. */
          clientSimNetTick(cs);
          clientMutexWaitFor();
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
  dwSysGame += (SDL_GetTicks() - ttick);

  /* AI */
  if (used == TRUE && inBrain == FALSE && brainRunning == TRUE && clientSimGetNetStatus(cs) != netFailed) {
    clientMutexWaitFor();
    inBrain = TRUE;
    clientMutexRelease();
    ttick = SDL_GetTicks();
    brainHandlerRun();
    dwSysBrain += SDL_GetTicks() - ttick;
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
  if (!clientSimHasTransport(humanSim)) {
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
 * Suspend / resume — Steam Deck Verified requirement.
 * Driven by SDL_EVENT_WILL_ENTER_BACKGROUND (sleep / home
 * button / overlay) and SDL_EVENT_DID_ENTER_FOREGROUND.
 * s_suspended declared near other main-loop state above.
 * ------------------------------------------------------- */
/* True for a local solo session — single-player or the tutorial.  Both run
   the in-process server with no remote peers, so a pause can safely freeze
   them.  Multiplayer (a joined client or a listen-server host) is never solo
   and must keep running. */
static bool windowIsSoloSession(ClientSim *cs) {
  return doingTutorial || (cs != NULL && clientSimIsSinglePlayer(cs));
}

/* Recompute the hosted-server pause gate.  The server tick only freezes for
   a solo session: a listen-server host must keep serving remote players, so
   multiplayer never pauses the server even when the host backgrounds the app
   or opens the overlay. */
static void windowUpdateServerPause(ClientSim *cs) {
  gameFrontSetServerPaused(windowIsSoloSession(cs) &&
                           (s_suspended || s_overlayPaused ||
                            s_controllerLostPaused || s_deckPaused));
}

void windowSuspendBackground(ClientSim *cs) {
  /* Pause local sim (windowRunGameTick early-outs on s_suspended) and
     mute audio.  Network state is left as-is; UDP will time out on its
     own.  In single-player we also freeze the server tick so the world
     holds; a listen-server host keeps serving (see windowUpdateServerPause).
     Idempotent — duplicate WILL_ENTER_BACKGROUND events from SDL are safe. */
  if (s_suspended) return;
  s_suspended = TRUE;
  windowUpdateServerPause(cs);
  s_suspendPrevMuted = soundIsMuted();
  soundSetMuted(TRUE);
}

void windowResumeForeground(ClientSim *cs) {
  if (!s_suspended) return;
  s_suspended = FALSE;

  if (cs != NULL && clientSimGetNetType(cs) == netUdp) {
    /* Network game: UDP timeout has almost certainly killed the
       session and the server has moved on.  Disconnect cleanly via
       the same flow as the in-tick connection-lost handler — show the
       standard "you have been disconnected" message and drop back to
       menu via finishedLoop=TRUE.  No reconnect, no state freeze. */
    clientSimConnectionLost(cs);
    imguiMessageBoxEx(DIALOG_BOX_TITLE,
                      "You have lost your connection to the server.\n"
                      "Returning to menu.",
                      IMGUI_MSG_ERROR, IMGUI_MSG_OK);
    finishedLoop = TRUE;
    winboloQuit = FALSE;
  } else {
    /* Single-player / tutorial / main menu: reset the catchup-loop
       wallclock baseline so the while ((ttick - oldTick) > GAME_TICK_LENGTH)
       loop in windowRunGameTick doesn't try to simulate every frame
       of the suspend duration in one go. */
    oldTick = SDL_GetTicks();
    ttick = oldTick;
  }
  windowUpdateServerPause(cs);
  soundSetMuted(s_suspendPrevMuted);
}

/* Steam in-game overlay opened/closed.  Solo sessions only (single-player or
   tutorial): freeze the client and server sim while the overlay is up and
   rebase the catch-up wallclock on close so we don't simulate the overlay
   duration in one burst.  Multiplayer is a no-op — a networked game keeps
   running with the overlay open. */
static void windowSteamOverlayActivated(ClientSim *cs, bool active) {
  if (active) {
    /* Only a solo session pauses; multiplayer keeps running. */
    if (!windowIsSoloSession(cs)) return;
    if (s_overlayPaused) return;          /* idempotent */
    s_overlayPaused = TRUE;
    windowUpdateServerPause(cs);
    s_overlayPrevMuted = soundIsMuted();
    soundSetMuted(TRUE);
  } else {
    /* Always clear on close — even if the mode changed while the overlay
       was up — so a stale pause can't freeze a later game. */
    if (!s_overlayPaused) return;
    s_overlayPaused = FALSE;
    windowUpdateServerPause(cs);
    /* Reset the catch-up baseline, mirroring the single-player branch of
       windowResumeForeground. */
    oldTick = SDL_GetTicks();
    ttick = oldTick;
    soundSetMuted(s_overlayPrevMuted);
  }
}

/* Controller-disconnected dialog opened/closed.  Solo sessions only (single-
   player or tutorial): freeze the client and server sim while the dialog is up
   and rebase the catch-up wallclock on close, mirroring
   windowSteamOverlayActivated.  Multiplayer is a no-op — the dialog still
   shows but the networked game keeps running. */
void windowControllerLostPause(ClientSim *cs, bool active) {
  if (active) {
    if (!windowIsSoloSession(cs)) return;
    if (s_controllerLostPaused) return;          /* idempotent */
    s_controllerLostPaused = TRUE;
    windowUpdateServerPause(cs);
    s_controllerLostPrevMuted = soundIsMuted();
    soundSetMuted(TRUE);
  } else {
    if (!s_controllerLostPaused) return;
    s_controllerLostPaused = FALSE;
    windowUpdateServerPause(cs);
    oldTick = SDL_GetTicks();
    ttick = oldTick;
    soundSetMuted(s_controllerLostPrevMuted);
  }
}

/* Controller pause menu opened/closed (Start button / Escape).  Solo sessions
   only (single-player or tutorial): freeze the client and server sim while the
   menu is up and rebase the catch-up wallclock on close, mirroring
   windowControllerLostPause.  Multiplayer is a no-op — the menu still shows but
   the networked game keeps running, so the player isn't booted for idling. */
void windowDeckPause(ClientSim *cs, bool active) {
  if (active) {
    if (!windowIsSoloSession(cs)) return;
    if (s_deckPaused) return;          /* idempotent */
    s_deckPaused = TRUE;
    windowUpdateServerPause(cs);
    s_deckPrevMuted = soundIsMuted();
    soundSetMuted(TRUE);
  } else {
    if (!s_deckPaused) return;
    s_deckPaused = FALSE;
    windowUpdateServerPause(cs);
    oldTick = SDL_GetTicks();
    ttick = oldTick;
    soundSetMuted(s_deckPrevMuted);
  }
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
  clientSimSetGunsight(cs, showGunsight);
  clientSimSetAutoScroll(cs, autoScrollingEnabled);
  clientSimSetLabelOwnTank(cs, labelSelf);
  clientSimSetLabelMessage(cs, labelMsg);
  clientSimSetLabelTankLabel(cs, labelTank);
  clientSimShowMessages(cs, MSG_NEWSWIRE, showNewswireMessages);
  clientSimShowMessages(cs, MSG_ASSISTANT, showAssistantMessages);
  clientSimShowMessages(cs, MSG_AI, showAIMessages);
  clientSimShowMessages(cs, MSG_NETSTATUS, showNetworkStatusMessages);
  clientSimShowMessages(cs, MSG_NETWORK, showNetworkDebugMessages);
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

/* Saved custom window position and size */
static int s_customWinX = SDL_WINDOWPOS_CENTERED;
static int s_customWinY = SDL_WINDOWPOS_CENTERED;
static int s_customWinW = 0;
static int s_customWinH = 0;

/* Window position (for all zoom modes) */
static int s_windowX = SDL_WINDOWPOS_CENTERED;
static int s_windowY = SDL_WINDOWPOS_CENTERED;

void windowGetSavedPosition(int *x, int *y) {
  *x = s_windowX;
  *y = s_windowY;
}

void windowSetSavedPosition(int x, int y) {
  s_windowX = x;
  s_windowY = y;
}

void windowGetCustomSize(int *w, int *h) {
  *w = s_customWinW;
  *h = s_customWinH;
}

void windowSetCustomSize(int w, int h) {
  s_customWinW = w;
  s_customWinH = h;
}

static SDL_Rect getDefaultDisplayBounds(void) {
    SDL_Rect bounds = {0, 0, 1920, 1080};
    SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    if (primary) SDL_GetDisplayUsableBounds(primary, &bounds);
    return bounds;
}

void windowComputeAspectCorrectSize(int actualW, int actualH, int actualX, int actualY,
                                     int *outW, int *outH, int *outX, int *outY) {
  int contentH = actualH - MENU_BAR_HEIGHT;
  int correctContentH = actualW * SDL3_SCREEN_H / SDL3_SCREEN_W;
  if (correctContentH <= contentH) {
    *outW = actualW;
    *outH = correctContentH + MENU_BAR_HEIGHT;
    *outX = actualX;
    *outY = actualY + (actualH - *outH) / 2;
  } else {
    *outW = contentH * SDL3_SCREEN_W / SDL3_SCREEN_H;
    *outH = actualH;
    *outX = actualX + (actualW - *outW) / 2;
    *outY = actualY;
  }
}

/* Update saved position from current window */
void windowSaveCurrentPosition(void) {
  SDL_Window *win = sdl3DrawGetWindow();
  if (win) {
    SDL_GetWindowPosition(win, &s_windowX, &s_windowY);
  }
}

/* Cardinal content widths.
   We only define width constants, not height, because:
   - Width = exact multiple of SDL3_SCREEN_W (515)
   - Height = (multiple of SDL3_SCREEN_H) + menu bar
   - Menu bar height can vary with Windows DPI scaling and font settings
   - So we detect cardinal zoom by width alone, which is stable */
#define CARDINAL_1X_W (1 * SDL3_SCREEN_W)
#define CARDINAL_2X_W (2 * SDL3_SCREEN_W)
#define CARDINAL_3X_W (3 * SDL3_SCREEN_W)
#define CARDINAL_4X_W (4 * SDL3_SCREEN_W)

/* Returns the cardinal zoom (1-4) if width matches exactly, or 0 for Custom */
static BYTE detectCardinalZoom(int w, int h) {
  (void)h;  /* Height varies with menu bar, only check width */
  if (w == CARDINAL_1X_W) return ZOOM_FACTOR_NORMAL;
  if (w == CARDINAL_2X_W) return ZOOM_FACTOR_DOUBLE;
  if (w == CARDINAL_3X_W) return ZOOM_FACTOR_TRIPLE;
  if (w == CARDINAL_4X_W) return ZOOM_FACTOR_QUAD;
  return ZOOM_FACTOR_CUSTOM;
}

/* Last applied zoom and window dimensions - to skip redundant changes */
static BYTE s_lastZoom = 255;
static int s_lastWinW = 0;
static int s_lastWinH = 0;

void windowZoomChange(BYTE amount, bool fromDragResize) {
  /* Get current window dimensions */
  int curW = 0, curH = 0;
  SDL_Window *curWin = sdl3DrawGetWindow();
  if (curWin) {
    SDL_GetWindowSize(curWin, &curW, &curH);
  }

  /* Skip if dimensions unchanged from last time (no actual resize happened)
     Only applies to resize-triggered changes, not menu selections.
     Don't change zoomFactor - just exit without doing anything. */
  if (fromDragResize && curW == s_lastWinW && curH == s_lastWinH && curW > 0) {
    return;
  }

  if (amount == zoomFactor) {
    return;
  }

  /* Save custom window position and size if currently in custom mode.
     Save CORRECTED size (proper aspect ratio) so maximize doesn't corrupt saved size.
     Find the largest aspect-correct size that FITS WITHIN the actual window.
     Also save centered position for the corrected size.
     BUT: don't save if the current size is actually a cardinal size (bug recovery). */
  if (zoomFactor == ZOOM_FACTOR_CUSTOM) {
    SDL_Window *win = sdl3DrawGetWindow();
    if (win) {
      int savW, savH, savX, savY;
      SDL_GetWindowPosition(win, &savX, &savY);
      SDL_GetWindowSize(win, &savW, &savH);
      /* Don't save cardinal sizes as "custom" */
      bool isCardinal = (savW == 1 * SDL3_SCREEN_W || savW == 2 * SDL3_SCREEN_W ||
                         savW == 3 * SDL3_SCREEN_W || savW == 4 * SDL3_SCREEN_W);
      if (!isCardinal) {
        int corrW, corrH, corrX, corrY;
        windowComputeAspectCorrectSize(savW, savH, savX, savY, &corrW, &corrH, &corrX, &corrY);
        s_customWinW = corrW; s_customWinH = corrH; s_customWinX = corrX; s_customWinY = corrY;
      }
    }
  }

  /* For custom mode, compute ceiling integer zoom from the target window size.
     Use CURRENT window size (for resize-triggered switch) or saved custom size (for menu). */
  BYTE internalZoom;
  int targetW = 0, targetH = 0;
  if (amount == ZOOM_FACTOR_CUSTOM) {
    if (fromDragResize) {
      /* Use current window size (user just dragged to this size) */
      SDL_Window *win = sdl3DrawGetWindow();
      if (win) {
        int curX, curY;
        SDL_GetWindowSize(win, &targetW, &targetH);
        SDL_GetWindowPosition(win, &curX, &curY);
        /* Save this as the new custom size (only if it's not a cardinal size).
           Save CORRECTED size so maximize doesn't corrupt saved size. */
        bool isCardinal = (targetW == 1 * SDL3_SCREEN_W || targetW == 2 * SDL3_SCREEN_W ||
                           targetW == 3 * SDL3_SCREEN_W || targetW == 4 * SDL3_SCREEN_W);
        if (!isCardinal && targetW > 0) {
          int corrW, corrH, corrX, corrY;
          windowComputeAspectCorrectSize(targetW, targetH, curX, curY, &corrW, &corrH, &corrX, &corrY);
          s_customWinW = corrW; s_customWinH = corrH; s_customWinX = corrX; s_customWinY = corrY;
        }
      }
    }
    /* Use saved custom size when coming from menu (or default 2x if none saved) */
    if (targetW <= 0) {
      targetW = s_customWinW > 0 ? s_customWinW : (2 * SDL3_SCREEN_W);
      targetH = s_customWinH > 0 ? s_customWinH : (2 * SDL3_SCREEN_H + MENU_BAR_HEIGHT);
    }
    int contentH = targetH - MENU_BAR_HEIGHT;  /* Subtract menu bar */
    int zoomW = (targetW + SDL3_SCREEN_W - 1) / SDL3_SCREEN_W;
    int zoomH = (contentH + SDL3_SCREEN_H - 1) / SDL3_SCREEN_H;
    internalZoom = (BYTE)((zoomW > zoomH) ? zoomW : zoomH);
    if (internalZoom < 1) internalZoom = 1;
  } else {
    internalZoom = amount;
  }

  /* Skip recreate if internal zoom isn't changing (avoids flash) */
  int currentInternal = sdl3DrawGetZoomFactor();
  if (currentInternal == internalZoom && sdl3DrawGetWindow() != NULL) {
    /* Still need to resize window for cardinal sizes if not already at that size */
    SDL_Window *win = sdl3DrawGetWindow();
    if (amount != ZOOM_FACTOR_CUSTOM && win) {
      int cardinalW = internalZoom * SDL3_SCREEN_W;
      int cardinalH = internalZoom * SDL3_SCREEN_H + MENU_BAR_HEIGHT;
      int nowW, nowH;
      SDL_GetWindowSize(win, &nowW, &nowH);
      if (nowW != cardinalW || nowH != cardinalH) {
        /* Get the display where the window currently is */
        SDL_DisplayID dispID = SDL_GetDisplayForWindow(win);
        SDL_Rect usable;
        usable = getDefaultDisplayBounds();
        if (dispID) SDL_GetDisplayUsableBounds(dispID, &usable);
        /* Center on that display */
        int centeredX = usable.x + (usable.w - cardinalW) / 2;
        int centeredY = usable.y + (usable.h - cardinalH) / 2;
        SDL_SetWindowSize(win, cardinalW, cardinalH);
        SDL_SetWindowPosition(win, centeredX, centeredY);
      }
    }
    /* Record final state and detect if we landed on a cardinal size */
    if (win) {
      SDL_GetWindowSize(win, &s_lastWinW, &s_lastWinH);
      BYTE detectedZoom = detectCardinalZoom(s_lastWinW, s_lastWinH);
      if (detectedZoom != ZOOM_FACTOR_CUSTOM) {
        amount = detectedZoom;
      }
    }
    windowSetZoomFactor(amount);
    s_lastZoom = amount;
    return;
  }

  drawBusy = TRUE;

  /* Capture the display where the window currently is, BEFORE cleanup destroys it */
  SDL_Rect savedDisplayBounds;
  savedDisplayBounds = getDefaultDisplayBounds();
  {
    SDL_Window *oldWin = sdl3DrawGetWindow();
    if (oldWin) {
      SDL_DisplayID dispID = SDL_GetDisplayForWindow(oldWin);
      if (dispID) SDL_GetDisplayUsableBounds(dispID, &savedDisplayBounds);
    }
  }

  clientMutexWaitFor();
  sdl3DrawCleanup();
  sdl3DrawSetup(internalZoom);

  {
    SDL_Window   *win = sdl3DrawGetWindow();
    SDL_Renderer *ren = sdl3DrawGetRenderer();

    if (win) {
      /* All modes are resizable - resizing in fixed mode auto-switches to Custom */
      SDL_SetWindowResizable(win, true);

      if (amount == ZOOM_FACTOR_CUSTOM) {
        if (fromDragResize && curW > 0 && curH > 0) {
          /* Use the size user just dragged to */
          SDL_SetWindowSize(win, curW, curH);
        } else {
          /* Restore saved custom position and size (default: 2x + menu bar) */
          int restoreW = s_customWinW > 0 ? s_customWinW : (2 * SDL3_SCREEN_W);
          int restoreH = s_customWinH > 0 ? s_customWinH : (2 * SDL3_SCREEN_H + MENU_BAR_HEIGHT);
          SDL_SetWindowSize(win, restoreW, restoreH);
          if (s_customWinX != SDL_WINDOWPOS_CENTERED) {
            SDL_SetWindowPosition(win, s_customWinX, s_customWinY);
          }
        }
      } else {
        /* Cardinal mode: center on the display where the old window was */
        int cardW = internalZoom * SDL3_SCREEN_W;
        int cardH = internalZoom * SDL3_SCREEN_H + MENU_BAR_HEIGHT;
        int centeredX = savedDisplayBounds.x + (savedDisplayBounds.w - cardW) / 2;
        int centeredY = savedDisplayBounds.y + (savedDisplayBounds.h - cardH) / 2;
        SDL_SetWindowSize(win, cardW, cardH);
        SDL_SetWindowPosition(win, centeredX, centeredY);
      }
      SDL_ShowWindow(win);
    }
    if (win && ren) {
      sdl3ImguiSetup(win, ren);
    }
  }
  clientMutexRelease();
  drawBusy = FALSE;

  /* Record final state and detect if we landed on a cardinal size */
  SDL_Window *finalWin = sdl3DrawGetWindow();
  if (finalWin) {
    SDL_GetWindowSize(finalWin, &s_lastWinW, &s_lastWinH);
    BYTE detectedZoom = detectCardinalZoom(s_lastWinW, s_lastWinH);
    if (detectedZoom != ZOOM_FACTOR_CUSTOM) {
      amount = detectedZoom;  /* Use detected cardinal */
    }
  }
  windowSetZoomFactor(amount);
  s_lastZoom = amount;
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
  clientSimSetGunsight(cs, showGunsight);
  gameFrontSaveCurrentPrefs();
}

void windowAutomaticScrolling_toggle(ClientSim *cs) {
  autoScrollingEnabled = !autoScrollingEnabled;
  if (cs) clientSimSetAutoScroll(cs, autoScrollingEnabled);
  gameFrontSaveCurrentPrefs();
}

void windowSmoothScrolling_toggle(void) {
  smoothScrollingEnabled = !smoothScrollingEnabled;
  /* Clear any in-progress sub-tile offset so the view snaps cleanly
     to a tile boundary when toggling off. */
  if (!smoothScrollingEnabled) {
    sdl3DrawSetDragOffset(0, 0);
  }
}

void windowShowPillLabels_toggle(ClientSim *cs) {
  BYTE count, total;

  showPillLabels = !showPillLabels;
  /* cs is NULL from the pre-game Settings dialog (no live sim). The pref is
     flipped above; the status-label refresh below needs the sim, so skip it. */
  if (cs == NULL) {
    return;
  }
  sdl3DrawSetPillsStatusClear();
  total = clientSimGetPillCount(cs);
  for (count = 1; count <= total; count++) {
    BYTE pillStat = clientSimGetPillAlliance(cs, count);
    sdl3DrawStatusPillbox(count, pillStat, showPillLabels);
  }
  sdl3DrawCopyPillsStatus(0, 0);
}

void windowShowBaseLabels_toggle(ClientSim *cs) {
  BYTE count, total;

  showBaseLabels = !showBaseLabels;
  /* cs is NULL from the pre-game Settings dialog (no live sim). The pref is
     flipped above; the status-label refresh below needs the sim, so skip it. */
  if (cs == NULL) {
    return;
  }
  sdl3DrawSetBasesStatusClear();
  total = clientSimGetBaseCount(cs);
  for (count = 1; count <= total; count++) {
    BYTE baseStat = clientSimGetBaseAlliance(cs, count);
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

void windowSetSoundVolume(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  soundVolume = pct;
  soundSetVolume(pct);
}

void windowMenuAllowNewPlayers_toggle(ClientSim *cs) {
  allowNewPlayers = !allowNewPlayers;
  clientSimSetAllowNewPlayers(cs, allowNewPlayers);
}

void windowMenuNewswire_toggle(ClientSim *cs) {
  showNewswireMessages = !showNewswireMessages;
  if (cs) clientSimShowMessages(cs, MSG_NEWSWIRE, showNewswireMessages);
}

void windowMenuAssistant_toggle(ClientSim *cs) {
  showAssistantMessages = !showAssistantMessages;
  if (cs) clientSimShowMessages(cs, MSG_ASSISTANT, showAssistantMessages);
}

void windowMenuAI_toggle(ClientSim *cs) {
  showAIMessages = !showAIMessages;
  if (cs) clientSimShowMessages(cs, MSG_AI, showAIMessages);
}

void windowMenuNetwork_toggle(ClientSim *cs) {
  showNetworkStatusMessages = !showNetworkStatusMessages;
  if (cs) clientSimShowMessages(cs, MSG_NETSTATUS, showNetworkStatusMessages);
}

void windowMenuNetworkDebug_toggle(ClientSim *cs) {
  showNetworkDebugMessages = !showNetworkDebugMessages;
  if (cs) clientSimShowMessages(cs, MSG_NETWORK, showNetworkDebugMessages);
}

void windowHideMainView_toggle(void) {
  hideMainView = !hideMainView;
}

void windowLabelOwnTank_toggle(ClientSim *cs) {
  labelSelf = !labelSelf;
  if (cs) clientSimSetLabelOwnTank(cs, labelSelf);
}

void windowSetMessageLabelLen(ClientSim *cs, labelLen newLen) {
  labelMsg = newLen;
  if (cs) clientSimSetLabelMessage(cs, labelMsg);
}

void windowSetTankLabelLen(ClientSim *cs, labelLen newLen) {
  labelTank = newLen;
  if (cs) clientSimSetLabelTankLabel(cs, labelTank);
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
  sdl3DrawRedrawAll(cs, clientSimGetCurrentBuildSelect(cs), NULL, showPillLabels, showBaseLabels);
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
  strcpy(defaultName, clientSimGetMapName(cs));

  SDL_ShowSaveFileDialog(saveMapCallback, &state, sdl3DrawGetWindow(),
                         filters, 1, NULL);
  while (!state.done) {
    SDL_Event e;
    SDL_WaitEventTimeout(&e, 100);
  }
  if (state.ok) {
    if (clientSaveMap(cs, state.path) == FALSE) {
      imguiMessageBoxEx(DIALOG_BOX_TITLE, langGetText(STR_WBERR_SAVEMAP),
                        IMGUI_MSG_ERROR, IMGUI_MSG_OK);
    }
  }
}

void windowKeyPressed(ClientSim *cs, int keyCode) {
  if (keyCode == keys.kiTankView) {
    clientSimTankView(cs);
  }
  /* Pill view (enter + hold-to-cycle) is handled by polling in
   * pillViewInputStep so holding the key auto-repeats through pills;
   * dispatching it here too would double-step on the entering press. */
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
                            int32_t srtDelay, bool isPillView, int edgeX, int edgeY) {
  if (hideMainView == FALSE && drawBusy == FALSE) {
    BYTE cursorX = 0, cursorY = 0;
    bool showCursor;

    /* Track view scroll in pixels and warp the OS mouse by the same delta
     * so the cursor stays glued to its world tile while the map slides.
     * Total view shift = xOffset whole-tile shift + subPos sub-tile shift.
     * The cursor cell then stays the same frame to frame (no flicker) as
     * autoscroll bumps subPos. Skip warps larger than a few tiles — those
     * come from respawn / scrollCenterObject / mode switch and the user
     * wants the cursor to stay where it is, not teleport. */
    {
      int zf      = sdl3DrawGetZoomFactor();
      int tileWpx = TILE_SIZE_X * zf;
      int tileHpx = TILE_SIZE_Y * zf;
      int subX    = clientSimGetSubPosX(cs);
      int subY    = clientSimGetSubPosY(cs);
      int curScrollPxX = (int)clientSimGetXOffset(cs) * tileWpx + subX * tileWpx / 256;
      int curScrollPxY = (int)clientSimGetYOffset(cs) * tileHpx + subY * tileHpx / 256;
      static int  sLastScrollPxX = 0;
      static int  sLastScrollPxY = 0;
      static bool sLastScrollPxValid = FALSE;
      if (sLastScrollPxValid) {
        int dpx = curScrollPxX - sLastScrollPxX;
        int dpy = curScrollPxY - sLastScrollPxY;
        int maxAuto = 4 * tileWpx;
        if (abs(dpx) <= maxAuto && abs(dpy) <= maxAuto) {
          cursorApplyScrollDelta(dpx, dpy);
        }
      }
      sLastScrollPxX = curScrollPxX;
      sLastScrollPxY = curScrollPxY;
      sLastScrollPxValid = TRUE;
    }

    /* Refresh cursor cell every frame: the autoscroll sub-tile offset
     * changes per tick, so the visually-rendered tile under a stationary
     * mouse changes too. cursorPos re-derives the cell from the cached
     * mouse pixel + current subPos and stores it in the viewport's
     * cursorPosX/Y, which clientSimGetCursorPos then reads. */
    {
      BYTE cx = 0, cy = 0;
      if (cursorPos(NULL, &cx, &cy, clientSimGetSubPosX(cs), clientSimGetSubPosY(cs))) {
        clientSimSetCursorPos(cs, cx, cy);
      } else {
        clientSimSetCursorPos(cs, 0, 0);
      }
    }
    showCursor = clientSimGetCursorPos(cs, &cursorX, &cursorY);

    /* When the gamepad-driven free build cursor is active, override
       the mouse cursor's screen position so the existing build-mode
       reticle render does double-duty.  The build cursor stores an
       absolute map tile; convert to the 1-based screen tile by
       subtracting the camera offset.  Off-screen tiles hide the
       reticle (matching how the mouse cursor hides when it leaves
       the play area). */
    /* Keep an active build cursor inside the visible edge as the view
       scrolls with the tank (no-op while cursor mode is off). */
    buildCursorClampToView(cs);
    bool cursorFaint = false;
    BYTE bcX, bcY;
    if (buildCursorGetTile(&bcX, &bcY)) {
      /* Cursor mode ON — draw the reticle solid at the cursor tile. */
      int sx = (int)bcX - (int)clientSimGetXOffset(cs);
      int sy = (int)bcY - (int)clientSimGetYOffset(cs);
      if (sx >= 1 && sx <= MAIN_SCREEN_SIZE_X &&
          sy >= 1 && sy <= MAIN_SCREEN_SIZE_Y) {
        showCursor = true;
        cursorX    = (BYTE)sx;
        cursorY    = (BYTE)sy;
      } else {
        showCursor = false;
      }
    } else if (!showCursor && buildCursorGetTargetTile(&bcX, &bcY)) {
      /* Cursor mode OFF but a target is locked, and the mouse cursor isn't
         showing (gamepad context): draw the locked target faintly so the
         player can still see where Build Now will place. Off-screen = hidden. */
      int sx = (int)bcX - (int)clientSimGetXOffset(cs);
      int sy = (int)bcY - (int)clientSimGetYOffset(cs);
      if (sx >= 1 && sx <= MAIN_SCREEN_SIZE_X &&
          sy >= 1 && sy <= MAIN_SCREEN_SIZE_Y) {
        showCursor  = true;
        cursorX     = (BYTE)sx;
        cursorY     = (BYTE)sy;
        cursorFaint = true;
      }
    }
    sdl3DrawSetCursorFaint(cursorFaint);

    sdl3DrawSetNetFailed(clientSimGetNetStatus(cs) == netFailed);
    sdl3DrawMainScreen(cs, value, mineView, tks, gs, sBullet, lgms,
                       NULL, showPillLabels, showBaseLabels,
                       srtDelay, isPillView, edgeX, edgeY,
                       showCursor, cursorX, cursorY);
  }
}

void frontEndUpdateTankStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour, BYTE trees) {
  DWORD tick = SDL_GetTicks();
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  if (armour > TANK_FULL_ARMOUR) {
    armour = 0;
  }
  sdl3DrawStatusTankBars(0, 0, shells, mines, armour, trees);
  dwSysFrame += (SDL_GetTicks() - tick);
}

void frontEndPlaySound(ClientSim *cs, sndEffects value) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  if (soundEffects == TRUE) {
    soundPlayEffect(value);
  }
}

void windowPlaySound(sndEffects value) {
  if (soundEffects == TRUE) {
    soundPlayEffect(value);
  }
}

void frontEndStatusPillbox(ClientSim *cs, BYTE pillNum, pillAlliance pb) {
  DWORD tick = SDL_GetTicks();
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  sdl3DrawStatusPillbox(pillNum, pb, showPillLabels);
  sdl3DrawCopyPillsStatus(0, 0);
  dwSysFrame += (SDL_GetTicks() - tick);
}

void frontEndStatusTank(ClientSim *cs, BYTE tankNum, tankAlliance ts) {
  DWORD tick = SDL_GetTicks();
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  sdl3DrawStatusTank(tankNum, ts);
  sdl3DrawCopyTanksStatus(0, 0);
  dwSysFrame += (SDL_GetTicks() - tick);
}

void frontEndMessages(ClientSim *cs, char *top, char *bottom) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  if (drawBusy == FALSE) {
    DWORD tick = SDL_GetTicks();
    sdl3DrawMessages(0, 0, top, bottom);
    dwSysFrame += (SDL_GetTicks() - tick);
  }
}

void frontEndKillsDeaths(ClientSim *cs, int kills, int deaths) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  if (drawBusy == FALSE) {
    DWORD tick = SDL_GetTicks();
    sdl3DrawKillsDeaths(0, 0, kills, deaths);
    dwSysFrame += (SDL_GetTicks() - tick);
  }
}

void frontEndStatusBase(ClientSim *cs, BYTE baseNum, baseAlliance bs) {
  DWORD tick = SDL_GetTicks();
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  sdl3DrawStatusBase(baseNum, bs, showBaseLabels);
  sdl3DrawCopyBasesStatus(0, 0);
  dwSysFrame += (SDL_GetTicks() - tick);
}

void frontEndUpdateBaseStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour) {
  DWORD tick = SDL_GetTicks();
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  sdl3DrawStatusBaseBars(0, 0, shells, mines, armour, FALSE);
  dwSysFrame += (SDL_GetTicks() - tick);
}

void frontEndManStatus(ClientSim *cs, bool isDead, TURNTYPE angle) {
  DWORD tick = SDL_GetTicks();
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  clientMutexWaitFor();
  sdl3DrawSetManStatus(0, 0, isDead, angle);
  clientMutexRelease();
  dwSysFrame += (SDL_GetTicks() - tick);
}

void frontEndManClear(ClientSim *cs) {
  DWORD tick = SDL_GetTicks();
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  clientMutexWaitFor();
  sdl3DrawSetManClear();
  sdl3DrawCopyManStatus(0, 0);
  clientMutexRelease();
  dwSysFrame += (SDL_GetTicks() - tick);
}

void frontEndDrawDownload(ClientSim *cs, bool justBlack) {
  if (hideMainView == FALSE && drawBusy == FALSE) {
    DWORD tick = SDL_GetTicks();
    sdl3DrawDownloadScreen(cs, NULL, justBlack);
    dwSysFrame += (SDL_GetTicks() - tick);
  }
}

void frontEndDrawReturningToLobby(ClientSim *cs) {
  if (hideMainView == FALSE && drawBusy == FALSE) {
    DWORD tick = SDL_GetTicks();
    sdl3DrawReturningToLobby(cs);
    dwSysFrame += (SDL_GetTicks() - tick);
  }
}

void frontEndAudioReturningToLobby(bool active) {
  soundSetReturningToLobby(active);
}

void frontEndGameOver(ClientSim *cs) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  SDL_RemoveTimer(timerFrameID);
  SDL_RemoveTimer(timerGameID);
  timerFrameID = 0;
  timerGameID = 0;
  imguiMessageBoxEx(DIALOG_BOX_TITLE, langGetText(STR_WBTIMELIMIT_END),
                    IMGUI_MSG_INFO, IMGUI_MSG_OK);
  winboloQuit = TRUE;
}

void frontEndSetActiveClientSim(struct ClientSim *cs) {
  if (cs != s_activeUiCs) {
    for (BYTE i = 0; i < MAX_TANKS; i++) {
      sdl3ImguiClearPlayer(i);
    }
    /* Drop the previous game's newswire/kills text so it doesn't linger on
       the message surface when the next game starts (it would otherwise stay
       visible until the first message overwrites it). */
    sdl3DrawResetCachedText();
  }
  s_activeUiCs = cs;
}

void frontEndClearPlayer(struct ClientSim *cs, playerNumbers value) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  sdl3ImguiClearPlayer((unsigned char)value);
}

void frontEndSetPlayer(ClientSim *cs, playerNumbers value, char *str, const char *countryCode, uint16_t ping, uint8_t clientType, uint8_t clientFlags) {
  char cc[3];
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  if (!clientSimIsRunning(cs)) {
    cc[0] = 'X'; cc[1] = 'X'; cc[2] = '\0';
    sdl3ImguiSetPlayer((unsigned char)value, str, cc);
    return;
  }
  /* Defensive: callers may pass "" (a 1-byte string literal) when the
   * country code is unknown, so reading [1] unconditionally would walk
   * off the end. Substitute 'X' for missing chars to match the
   * not-running fallback above. */
  cc[0] = countryCode[0] ? countryCode[0] : 'X';
  cc[1] = (countryCode[0] && countryCode[1]) ? countryCode[1] : 'X';
  cc[2] = '\0';
  WB_LOG_DEBUG(WB_LOG_CAT_GUI, "[FLAGS] frontEndSetPlayer: player=%d name='%s' cc='%s' (0x%02X 0x%02X)", (int)value, str, cc, (unsigned char)cc[0], (unsigned char)cc[1]);
  sdl3ImguiSetPlayer((unsigned char)value, str, cc);
  sdl3ImguiUpdatePlayerMeta((unsigned char)value, ping, clientType, clientFlags);
}

void frontEndUpdatePlayerPing(ClientSim *cs, playerNumbers value, uint16_t ping) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  if (!clientSimIsRunning(cs)) return;
  sdl3ImguiUpdatePlayerPing((unsigned char)value, ping);
}

void frontEndUpdatePlayerFlags(ClientSim *cs, playerNumbers value,
                               uint8_t clientType, uint8_t clientFlags) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  sdl3ImguiUpdatePlayerFlags((unsigned char)value, clientType, clientFlags);
}

void frontEndSetPlayerCheckState(struct ClientSim *cs, playerNumbers value, bool isChecked) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  sdl3ImguiSetPlayerCheckState((unsigned char)value, isChecked);
}

void frontEndApplyLocalTankPrefs(struct ClientSim *cs) {
  extern bool useAutoslow;
  extern bool useAutohide;
  if (cs == NULL) return;
  clientSimSetTankAutoSlowdown(cs, useAutoslow);
  clientSimSetTankAutoHideGunsight(cs, useAutohide);
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
  if (!clientSimIsRunning(cs)) return;
  /* In lobby state the in-game renderer hasn't taken over yet — the
   * lobby ImGui is the active view. Skip the game-frame blit so a
   * subscriber-side playersSetPlayer triggered by a CTRL_PLAYER_JOIN
   * mid-lobby (e.g. another remote adding a bot) doesn't stomp the
   * lobby render. */
  if (clientSimIsInLobby(cs)) return;
  windowRedrawAll(cs);
}

/* -------------------------------------------------------
 * frontEndShowGunsight — auto show/hide gunsight callback
 * ------------------------------------------------------- */
void frontEndShowGunsight(ClientSim *cs, bool isShown) {
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  showGunsight = !isShown;
  clientSimSetGunsight(cs, showGunsight);
}

/* -------------------------------------------------------
 * frontEndTutorial — tutorial position-triggered messages
 * Uses imguiMessageBoxEx instead of Win32 MessageBoxA.
 * ------------------------------------------------------- */
/* Step index and render-frame counter are file-static so
 * frontEndTutorialReset() / frontEndTutorialNotePresentedFrame() can
 * touch them. The frame counter gates the intro: the tutorial starts
 * on tick 1, but the first complete frame hasn't been rendered yet, so
 * imguiMessageBoxEx's captureBackbuffer would grab a grey buffer. We
 * require a few presented frames before firing the intro; by then the
 * game screen is on the backbuffer and the modal dims over it. */
#define TUTORIAL_INTRO_MIN_FRAMES 3
static int tutorialStepIdx = 0;
static int tutorialFramesPresented = 0;

/* humanSim lives in gamefront.c; we need it so the tutorial can clear
 * sim->isTutorial on both sims when the final dialog closes, letting
 * the player drive around freely afterwards. */
extern ClientSim *humanSim;

void frontEndTutorialReset(void) {
  tutorialStepIdx = 0;
  tutorialFramesPresented = 0;
}

/* Called from the main loop immediately after SDL_RenderPresent so we
 * can defer the intro until a real game frame is on screen. */
void frontEndTutorialNotePresentedFrame(void) {
  if (tutorialFramesPresented < TUTORIAL_INTRO_MIN_FRAMES) {
    tutorialFramesPresented++;
  }
}

bool frontEndTutorial(BYTE pos) {
  int i;

  if (isTutorial != TRUE) {
    tutorialStepIdx = 0;    /* Reset for the next tutorial run. */
    return FALSE;
  }
  if (tutorialStepIdx >= tutorialStepCount) return FALSE;
  {
    BYTE stepPos = tutorialSteps[tutorialStepIdx].pos;
    if (stepPos != TUTORIAL_POS_ANY && stepPos != pos) return FALSE;
  }

  /* Intro step only: defer until the game has rendered a frame, so
   * the backbuffer the modal captures shows the map, not grey. */
  if (tutorialSteps[tutorialStepIdx].pos == TUTORIAL_POS_ANY &&
      tutorialFramesPresented < TUTORIAL_INTRO_MIN_FRAMES) {
    return FALSE;
  }

  doingTutorial = TRUE;
  /* Freeze the server sim's tankUpdate before we release the mutex so
   * the tank doesn't drift forward while the modal is up. */
  {
    ServerSim *srv = gameFrontGetServerSim();
    if (srv) serverSimSetPaused(srv, TRUE);
  }
  clientMutexRelease();
  for (i = 0; i < TUTORIAL_MAX_MSGS; i++) {
    uint16_t mid = tutorialSteps[tutorialStepIdx].msgs[i];
    if (mid == 0) break;
    {
      TutorialSeg segs[TUTORIAL_SEG_MAX];
      int n = tutorialResolveSegments(mid, segs, TUTORIAL_SEG_MAX);
      imguiMessageBoxRich(DIALOG_BOX_TITLE, segs, n,
                          IMGUI_MSG_INFO, IMGUI_MSG_OK);
    }
  }
  /* Final step: exit tutorial mode so the player can keep driving.
   * We clear the global client flag plus both sims' isTutorial so that
   * frontEndTutorial() no-ops on future ticks and the server stops
   * auto-halting the tank at old trigger rows. The game timer keeps
   * running — the old implementation removed it, locking the player
   * out of movement, which is not the behaviour we want.
   * Also persist that the tutorial is complete so the welcome menu
   * stops offering it (the player can re-enable from Settings). */
  if (tutorialStepIdx == tutorialStepCount - 1) {
    isTutorial = FALSE;
    if (humanSim) clientSimSetTutorial(humanSim, false);
    {
      ServerSim *srv = gameFrontGetServerSim();
      if (srv) serverSimSetTutorial(srv, false);
    }
    gameFrontSetShowTutorialButton(false);
  }
  clientMutexWaitFor();
  {
    ServerSim *srv = gameFrontGetServerSim();
    if (srv) serverSimSetPaused(srv, FALSE);
  }
  doingTutorial = FALSE;
  oldTick = SDL_GetTicks();
  ttick = oldTick;
  tutorialStepIdx++;
  return TRUE;
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
