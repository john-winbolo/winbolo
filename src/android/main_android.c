/*
 * main_android.c - Android entry point for WinBolo game client
 *
 * Replaces gui/sdl3/winbolo.c for the Android build.
 * Uses a blocking while loop with SDL_GetTicks() for frame timing
 * and frame-based tick accumulation instead of SDL_AddTimer.
 *
 * Modeled on src/wasm/main_wasm.c.
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../bolo/screen.h"
#include "../bolo/client_sim.h"
#include "../bolo/frontend.h"
#include "../bolo/transport.h"
#include "../bolo/transport_udp.h"
#include "../bolo/gui_message.h"
#include "../gui/brainsHandler.h"
#include "../gui/clientmutex.h"
#include "../gui/draw.h"
#include "../gui/gamefront.h"
#include "../gui/input.h"
#include "../gui/lang.h"
#include "../gui/sound.h"
#include "../gui/winbolo.h"
#include "../gui/sdl3/sdl3draw.h"
#include "../gui/sdl3/sdl3imgui.h"
#include "../gui/sdl3/luabrainshandler.h"
#include "../bolo/bot_manager.h"
#include "../gui/sdl3/dialog_backend.h"
#include "touch_input.h"
#include "players_panel.h"

extern ClientSim *humanSim;

/* -------------------------------------------------------
 * Globals declared by winbolo.h / used by android_frontend.c
 * ------------------------------------------------------- */

bool isTutorial = FALSE;

int frameRate = FRAME_RATE_30;

bool showGunsight = FALSE;
bool soundEffects = TRUE;
bool backgroundSound = TRUE;
bool useSoundKeepalive = FALSE;
bool allowNewPlayers = TRUE;

bool showNewswireMessages = TRUE;
bool showAssistantMessages = TRUE;
bool showAIMessages = FALSE;
bool showNetworkStatusMessages = TRUE;
bool showNetworkDebugMessages = FALSE;

bool autoScrollingEnabled = FALSE;
bool smoothScrollingEnabled = FALSE;  /* Touch platform: arrow-key smooth scroll inactive */
BYTE zoomFactor = ZOOM_FACTOR_DOUBLE;

bool showPillLabels = FALSE;
bool showBaseLabels = FALSE;
bool labelSelf = TRUE;
labelLen labelMsg = lblShort;
labelLen labelTank = lblShort;

bool hideMainView = FALSE;
bool isInMenu = FALSE;

keyItems keys;

/* Timing */
DWORD dwSysFrameTotal = 0;
DWORD dwSysFrame = 0;
DWORD dwSysGameTotal = 0;
DWORD dwSysGame = 0;
DWORD dwSysBrainTotal = 0;
DWORD dwSysBrain = 0;

bool drawBusy = FALSE;
static bool doingTutorial = FALSE;
static bool paused = FALSE;
bool winboloQuit = FALSE;
bool finishedLoop = FALSE;
bool showAllianceReq = TRUE;

DWORD oldTick = 0;
DWORD ttick = 0;
time_t ticks = 0;

/* Frame-based tick accumulator */
static double gameTickAccum = 0.0;
static Uint64 lastFrameTime = 0;

/* -------------------------------------------------------
 * Extern declarations for functions in android_frontend.c
 * ------------------------------------------------------- */
extern void sdl3MessageHandler(const char *message, const char *title);

/* -------------------------------------------------------
 * Helper: sync snapshot from transport
 * ------------------------------------------------------- */
static void androidSyncSnapshot(ClientSim *cs, Transport *transport, BYTE myPlayerNum) {
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

/* -------------------------------------------------------
 * windowRunGameTick — game logic using transport
 * ------------------------------------------------------- */
static void windowRunGameTick(ClientSim *cs) {
  static bool inBrain = FALSE;
  static bool justKeys = FALSE;
  static BYTE t2 = 0;
  static uint32_t simTickCounter = 0;
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
  if (transport == NULL || doingTutorial) {
    return;
  }

  {
    BYTE myPlayerNum = gameFrontGetPlayerNum();
    if (justKeys == TRUE) {
      /* Keys tick */
      if (brainRunning == FALSE) {
        tb = touchInputGetKeys();
      }
      InputPacket pkt;
      screenBuildInputPacketCS(cs, &pkt, tb, FALSE, FALSE, brainRunning, FALSE, myPlayerNum, simTickCounter);
      clientMutexWaitFor();
      clientSimKeysTick(cs, &pkt);
      clientMutexRelease();
      transport->recordInput(transport->ctx, &pkt);
      transport->tick(transport->ctx);
      clientMutexWaitFor();
      androidSyncSnapshot(cs, transport, myPlayerNum);
      clientMutexRelease();
      simTickCounter++;
      justKeys = FALSE;
    } else {
      /* Game tick */
      t2++;
      if (brainRunning == FALSE) {
        tb = touchInputGetKeys();
        isShoot = touchInputIsFireKeyPressed();
        isMine = touchInputShouldLayMine();
      }
      InputPacket pkt;
      screenBuildInputPacketCS(cs, &pkt, tb, isShoot, isMine, brainRunning, TRUE, myPlayerNum, simTickCounter);
      if (brainRunning == FALSE) {
        int gsChange = touchInputGetGunsightChange();
        if (gsChange > 0) pkt.flags |= (1 << INPUT_FLAG_GUNSIGHT_SHIFT);
        else if (gsChange < 0) pkt.flags |= (2 << INPUT_FLAG_GUNSIGHT_SHIFT);
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
      androidSyncSnapshot(cs, transport, myPlayerNum);
      clientSimDisplayTick(cs, brainRunning);
      clientMutexRelease();
      simTickCounter++;
      ticks++;
      justKeys = TRUE;
      used = TRUE;
    }
  }

  /* AI */
  if (used == TRUE && inBrain == FALSE && brainRunning == TRUE) {
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
  }

}

/* -------------------------------------------------------
 * main — Android entry point (called by SDL3's SDLActivity)
 * ------------------------------------------------------- */
int main(int argc, char *argv[]) {
  const char *cmdLine = "";
  DWORD tick;

  srand((unsigned int)(time(NULL) ^ getpid()));

  /* Check for winbolo:// URL passed via intent (see WinBoloActivity.getArguments) */
  for (int i = 1; i < argc; i++) {
    if (cmdLine[0] == '\0') {
      cmdLine = argv[i];
    }
  }

  SDL_Init(0);
  initWinboloTimer();

  if (clientMutexCreate() == FALSE) {
    SDL_Log("[Android] Failed to create client mutex");
    return 1;
  }

  /* Auto-detect appropriate zoom level based on screen height */
  {
    SDL_DisplayID disp = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(disp);
    if (dm) {
      int h = dm->h;
      SDL_Log("[Android] Display resolution: %dx%d", dm->w, h);
      if (h >= 1024) {
        zoomFactor = ZOOM_FACTOR_QUAD;
      } else if (h >= 512) {
        zoomFactor = ZOOM_FACTOR_DOUBLE;
      } else {
        zoomFactor = ZOOM_FACTOR_NORMAL;
      }
      SDL_Log("[Android] Auto-selected zoom factor: %d", zoomFactor);
    }
  }

  /* Use ImGui dialog backend with our Android-specific welcome/gamesetup */
  dialogBackendInit();

  SDL_Log("[Android] Starting gameFrontStart...");
  if (gameFrontStart(cmdLine, &keys, FALSE, NULL) == FALSE) {
    SDL_Log("[Android] gameFrontStart FAILED");
    endWinboloTimer();
    clientMutexDestroy();
    SDL_Quit();
    return 1;
  }
  SDL_Log("[Android] gameFrontStart OK");

  /* Set up ImGui and touch input */
  {
    SDL_Window *win = sdl3DrawGetWindow();
    SDL_Renderer *ren = sdl3DrawGetRenderer();
    if (win && ren) {
      sdl3ImguiSetup(win, ren);
    }
    if (win) {
      SDL_ShowWindow(win);
      /* Use logical presentation size for touch coordinate space so that
         touch positions map directly to render coordinates. Falls back to
         window size if no logical presentation is set. */
      int tw = 0, th = 0;
      if (ren) {
        SDL_RendererLogicalPresentation mode;
        SDL_GetRenderLogicalPresentation(ren, &tw, &th, &mode);
      }
      if (tw <= 0 || th <= 0) {
        SDL_GetWindowSize(win, &tw, &th);
      }

      /* Query safe area to handle display cutouts (notches, punch-holes).
         Compute insets relative to the full window so touch controls
         stay inside the safe region. */
      int safeLeft = 0, safeTop = 0, safeRight = 0, safeBottom = 0;
      {
        SDL_Rect safeRect;
        int winW = 0, winH = 0;
        SDL_GetWindowSize(win, &winW, &winH);
        if (SDL_GetWindowSafeArea(win, &safeRect)) {
          safeLeft   = safeRect.x;
          safeTop    = safeRect.y;
          safeRight  = winW - (safeRect.x + safeRect.w);
          safeBottom = winH - (safeRect.y + safeRect.h);
          /* Scale insets from window coords to logical coords */
          if (winW > 0 && winH > 0) {
            safeLeft   = safeLeft   * tw / winW;
            safeTop    = safeTop    * th / winH;
            safeRight  = safeRight  * tw / winW;
            safeBottom = safeBottom * th / winH;
          }
          SDL_Log("[Android] Safe area insets: L=%d T=%d R=%d B=%d",
                  safeLeft, safeTop, safeRight, safeBottom);
        }
      }

      touchInputSetup(tw, th, safeLeft, safeTop, safeRight, safeBottom);
      SDL_Log("[Android] Touch coordinate space: %dx%d", tw, th);
    }
  }

  isInMenu = FALSE;
  finishedLoop = FALSE;
  ClientSim *cs = humanSim;

  /* Handle lobby if the server uses lobby mode.
   * The lobby runs a blocking modal loop (its own ImGui context + event loop)
   * so we must show it before entering the main game loop. Without this,
   * the game loop would tick an uninitialized tank and crash.
   * The lobby creates its own ImGui context, so we must tear down the
   * existing one first to avoid an assertion failure in ImGui_ImplSDL3_Init. */
  if (cs && cs->inLobby &&
      (cs->netStat == netLobby || cs->netStat == netLobbyCountdown)) {
    SDL_Log("[Android] Entering lobby");
    sdl3ImguiCleanup();  /* Tear down existing ImGui — lobby creates its own */
    const DialogBackend *db = dialogBackendGet();
    int lobbyResult = db->lobbyShow(cs);
    if (lobbyResult == 0) {
      /* Player chose to leave or server shut down */
      SDL_Log("[Android] Left lobby, cleaning up");
      gameFrontEnd(&keys, FALSE, TRUE);
      endWinboloTimer();
      clientMutexDestroy();
      sdl3DrawCleanup();
      soundCleanup();
      SDL_Quit();
      return 0;
    }
    /* lobbyResult == 1: game started — load the deferred map */
    if (!gameFrontLoadDeferredMap(cs)) {
      SDL_Log("[Android] Failed to load deferred map");
      gameFrontEnd(&keys, FALSE, TRUE);
      endWinboloTimer();
      clientMutexDestroy();
      sdl3DrawCleanup();
      soundCleanup();
      SDL_Quit();
      return 0;
    }
    cs->netStat = netRunning;
    SDL_Log("[Android] Lobby complete, game starting");
    /* Re-initialize ImGui for the main game loop */
    {
      SDL_Window *win = sdl3DrawGetWindow();
      SDL_Renderer *ren = sdl3DrawGetRenderer();
      if (win && ren) {
        sdl3ImguiSetup(win, ren);
        }
    }
  }

  windowApplyMenuChecks(cs);

  if (soundEffects == TRUE) {
    soundKeepalive(useSoundKeepalive);
  }

  guiMessageSetHandler(sdl3MessageHandler);

  oldTick = winboloTimer();

  /* Flush any stale render state left over from the dialog phase.
     The dialog loop destroys textures and restores logical presentation
     after its last SDL_RenderPresent, which can leave the Metal/GL command
     queue in an inconsistent state.  A clean clear+present here ensures
     the renderer starts fresh before the game loop's first
     SDL_SetRenderTarget call. */
  {
    SDL_Renderer *ren = sdl3DrawGetRenderer();
    if (ren) {
      SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
      SDL_RenderClear(ren);
      SDL_RenderPresent(ren);
    }
  }

  lastFrameTime = SDL_GetTicks();

  SDL_Log("[Android] Starting main loop");

  /* Main game loop */
#define ANDROID_FRAME_CAP_MS 16
  while (!winboloQuit && !finishedLoop) {
    Uint64 frameCapStart = SDL_GetTicks();
    /* Process SDL events — handle touch before forwarding to ImGui */
    {
      SDL_Event ev;
      /* When paused, block on events to avoid burning CPU */
      if (paused) {
        if (SDL_WaitEvent(&ev)) {
          touchInputProcessEvent(&ev);
          if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_TERMINATING) {
            winboloQuit = TRUE;
          } else if (ev.type == SDL_EVENT_WILL_ENTER_FOREGROUND) {
            SDL_Log("[Android] Resuming from background");
            paused = FALSE;
            lastFrameTime = SDL_GetTicks();
            gameTickAccum = 0.0;
            soundSetMuted(FALSE);
          }
        }
        continue; /* Skip tick processing and rendering while paused */
      }
      while (SDL_PollEvent(&ev)) {
        /* Convert coordinates to logical presentation space and forward to
           ImGui so dialogs receive touch input (matches iOS fix). */
        {
          SDL_Renderer *ren = sdl3DrawGetRenderer();
          if (ren) {
            SDL_Event rawEv = ev;
            SDL_ConvertEventToRenderCoordinates(ren, &ev);
            sdl3ImguiForwardEvent(&ev);
            /* Don't pass touch to game when ImGui is handling it (dialog open) */
            if (!sdl3ImguiWantCaptureMouse()) {
              touchInputProcessEvent(&rawEv);
            }
          } else {
            touchInputProcessEvent(&ev);
          }
        }
        if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_TERMINATING) {
          winboloQuit = TRUE;
        } else if (ev.type == SDL_EVENT_DID_ENTER_BACKGROUND) {
          SDL_Log("[Android] Entering background, pausing");
          paused = TRUE;
          soundSetMuted(TRUE);
        } else if (ev.type == SDL_EVENT_WILL_ENTER_FOREGROUND) {
          SDL_Log("[Android] Resuming from background");
          paused = FALSE;
          lastFrameTime = SDL_GetTicks();
          gameTickAccum = 0.0;
          soundSetMuted(FALSE);
        }
      }
    }

    /* Game tick accumulation (replaces SDL_AddTimer) */
    if (gameFrontGetTransport() != NULL) {
      Uint64 now = SDL_GetTicks();
      double elapsed = (double)(now - lastFrameTime);
      lastFrameTime = now;
      gameTickAccum += elapsed;

      while (gameTickAccum >= GAME_TICK_LENGTH) {
        gameTickAccum -= GAME_TICK_LENGTH;
        windowRunGameTick(cs);
      }
    }

    /* Render */
    tick = winboloTimer();
    clientMutexWaitFor();
    if (finishedLoop == FALSE) {
      screenUpdateCS(cs, redraw);
    }
    clientMutexRelease();
    dwSysFrame += (winboloTimer() - tick);

    /* Touch overlay + ImGui + present */
    {
      SDL_Renderer *ren = sdl3DrawGetRenderer();
      if (ren) {
        touchInputRender(ren);
      }
    }
    sdl3ImguiPumpAndRender(cs);
    {
      SDL_Renderer *ren = sdl3DrawGetRenderer();
      if (ren) SDL_RenderPresent(ren);
    }

    /* Cap to 60 FPS */
    {
      Uint64 frameElapsed = SDL_GetTicks() - frameCapStart;
      if (frameElapsed < ANDROID_FRAME_CAP_MS) {
        SDL_Delay((Uint32)(ANDROID_FRAME_CAP_MS - frameElapsed));
      }
    }
  }

  SDL_Log("[Android] Main loop ended, cleaning up");

  /* Cleanup */
  gameFrontEnd(&keys, TRUE, TRUE);
  endWinboloTimer();
  clientMutexDestroy();
  sdl3ImguiCleanup();
  sdl3DrawCleanup();
  soundCleanup();
  SDL_Quit();
  return 0;
}
