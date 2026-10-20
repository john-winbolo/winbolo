/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * main_android.c - Android entry point for WinBolo game client
 *
 * Replaces gui/sdl3/winbolo.c for the Android build: the driver that
 * decides when to tick. A blocking while loop with SDL_GetTicks() for frame
 * timing and a wall-clock accumulator in place of SDL_AddTimer. The tick
 * itself, the connect sequence and the guarded frontEnd* bodies are the
 * shared cores in src/client_frontend/, as on the desktop, the web and iOS.
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../common/wb_log.h"
#include "bolo_rand.h"
#include "client_sim.h"
#include "client_render.h"
#include "frontend.h"
#include "client_net.h"
#include "gui_message.h"
#include "../gui/brainsHandler.h"
#include "../gui/clientmutex.h"
#include "../server/threads.h"  /* threadsCreate / threadsDestroy */
#include "../gui/draw.h"
#include "../gui/gamefront.h"
#include "../gui/input.h"
#include "../gui/lang.h"
#include "../gui/sound.h"
#include "../gui/ui_mode.h"
#include "../gui/winbolo.h"
#include "../gui/sdl3/sdl3draw.h"
#include "../gui/sdl3/sdl3imgui.h"
#include "../gui/sdl3/input_touch.h"
#include "client_frontend_render.h"
#include "client_frontend_common.h"
#include "client_frontend_tick.h"
#include "client_frontend_tutorial.h"
#include "../gui/sdl3/luabrainshandler.h"
#include "../gui/sdl3/dialog_backend.h"
#include "../gui/sdl3/dialogs/imgui_messagebox.h"
#include "touch_input.h"

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
int  soundVolume = 50;         /* MENU / Sound Volume — the game sounds */
int  windowMasterVolume = 100; /* MENU / Master Volume — sounds and voice */
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
bool winboloQuit = FALSE;
bool finishedLoop = FALSE;
bool showAllianceReq = TRUE;
bool positionalSound = TRUE;     /* MENU / Positional Sound — sounds carry a side */
bool letterboxBarsGray = FALSE;  /* gray vs black letterbox bars (sdl3draw) */

DWORD oldTick = 0;
DWORD ttick = 0;
time_t ticks = 0;

/* Frame-based tick accumulator */
static double gameTickAccum = 0.0;
static Uint64 lastFrameTime = 0;

/* -------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------- */
extern void sdl3MessageHandler(const char *message, const char *title);
static void windowRunGameTick(ClientSim *cs);
void windowTutorialPause(ClientSim *cs, bool active);
void windowDeckPause(ClientSim *cs, bool active);
void windowControllerLostPause(ClientSim *cs, bool active);
void windowSuspendBackground(ClientSim *cs);
void windowResumeForeground(ClientSim *cs);

/* SDL never queues the app lifecycle events (SDL_SendAppEvent hands them to
   event watches only), so the game loop's own event handling never sees
   them. This watch acts on them while a game is up (s_lifecycleCs). Android
   sends the background pair from the main thread inside the event pump, and
   then holds the main thread in the pump until the app comes back; the
   foreground pair arrives the same way on the return, and posts
   s_resumeEventType, which the loop turns into the resume on its own
   stack, where the resume's message box may run. Ending the process
   (TERMINATING, from onDestroy) ends the loop the way Quit does; the
   preferences are written by gamefront.c's own watch. */
static ClientSim *s_lifecycleCs = NULL;
static Uint32 s_resumeEventType = 0;

static bool SDLCALL androidLifecycleWatch(void *userdata, SDL_Event *ev) {
  (void)userdata;
  switch (ev->type) {
  case SDL_EVENT_WILL_ENTER_BACKGROUND:
  case SDL_EVENT_DID_ENTER_BACKGROUND:
    if (s_lifecycleCs != NULL) windowSuspendBackground(s_lifecycleCs);
    break;
  case SDL_EVENT_WILL_ENTER_FOREGROUND:
  case SDL_EVENT_DID_ENTER_FOREGROUND:
    if (s_lifecycleCs != NULL && s_resumeEventType != 0) {
      SDL_Event resume;
      SDL_zero(resume);
      resume.type = s_resumeEventType;
      SDL_PushEvent(&resume);
    }
    break;
  case SDL_EVENT_TERMINATING:
    windowSetQuitting();
    break;
  default:
    break;
  }
  return true;
}

/* -------------------------------------------------------
 * The solo-game freezes and the background suspend. The pause state, the
 * solo-session test and the hosted-server pause are in
 * client_frontend_common.c, one body with the desktop and iOS.
 * ------------------------------------------------------- */

/* One solo freeze's open or close edge, through the shared body. Closing
   drops the owed tick time, so a held touch or the press that closed the
   overlay does not run on into the game. */
static void androidSoloPause(ClientSim *cs, ClientFrontPause which, bool active) {
  if (clientFrontSoloPause(cs, which, active)) {
    gameTickAccum = 0.0;
    lastFrameTime = SDL_GetTicks();
  }
}

/* Pause hooks driven from sdl3imgui.cpp: the controller-disconnected dialog
   and the pause overlay, and the tutorial message overlay, driven by its
   open/close edge in the render path. Solo games only, as on the desktop. */
void windowControllerLostPause(ClientSim *cs, bool active) {
  androidSoloPause(cs, CLIENT_FRONT_PAUSE_CONTROLLER_LOST, active);
}
void windowDeckPause(ClientSim *cs, bool active) {
  androidSoloPause(cs, CLIENT_FRONT_PAUSE_DECK, active);
}
void windowTutorialPause(ClientSim *cs, bool active) {
  androidSoloPause(cs, CLIENT_FRONT_PAUSE_TUTORIAL, active);
}

/* The move to and from the background. The shared suspend pauses a solo
   game's hosted server and mutes the sound with the state it had kept for
   the way back; the resume recomputes the server pause and puts the sound
   back, and drops the tick time owed for the time away. */
void windowSuspendBackground(ClientSim *cs) {
  if (clientFrontSuspend(cs)) {
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Entering background, pausing");
  }
}

void windowResumeForeground(ClientSim *cs) {
  if (!clientFrontResumeBegin()) return;
  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Resuming from background");
  lastFrameTime = SDL_GetTicks();
  gameTickAccum = 0.0;
  clientFrontResumeEnd(cs);
}

/* -------------------------------------------------------
 * windowRunGameTick — one half-step of the shared client tick, plus the
 * driver-side work around it (disconnect check, AI, stats)
 * ------------------------------------------------------- */
static void windowRunGameTick(ClientSim *cs) {
  static bool inBrain = FALSE;
  static BYTE t2 = 0;
  bool used = FALSE;
  bool brainRunning;

  brainRunning = brainHandlerIsBrainRunning();

  /* Check if the UDP server has disconnected or timed out.
   * Only check for UDP transports (serverSim == NULL means not local). */
  if (clientSimHasTransport(cs) && gameFrontGetServerSim() == NULL &&
      clientSimGetConnectState(cs) == CLIENT_CONNECT_SERVER_SHUTDOWN) {
    clientSimConnectionLost(cs);
    imguiMessageBoxEx(DIALOG_BOX_TITLE,
                      langGetText(NETERR_LOSTCONNECTION_RETURN_MENU),
                      IMGUI_MSG_ERROR, IMGUI_MSG_OK);
    finishedLoop = TRUE;
    winboloQuit = FALSE;
    return;
  }

  /* One keys/game/lobby half-step lives in the shared client-frontend tick
   * core (desktop, web and iOS call the same one); the brain run and the
   * per-second stat rollover below stay here in the driver.  The main loop
   * owns the catch-up, calling this once per owed half-step. */
  ttick = SDL_GetTicks();
  if (clientFrontRunTickStep(cs)) {
    t2++;
    ticks++;
    used = TRUE;
  }
  dwSysGame += (SDL_GetTicks() - ttick);

  /* AI */
  if (used == TRUE && inBrain == FALSE && brainRunning == TRUE &&
      clientSimGetNetStatus(cs) != netFailed) {
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
  }
}

/* -------------------------------------------------------
 * The asset trees as files. The shared code opens maps, scripts and data
 * with fopen on data/... and Brains/... relative paths and lists those
 * directories, neither of which can read an APK asset. So the two trees are
 * copied once into app/ under the internal storage directory, which then
 * becomes the working directory, as the bundle is on iOS. The listing is
 * asset_manifest.txt, written by the gradle copy task since asset
 * directories cannot be listed through SDL; its first line is a stamp over
 * every listed file, kept beside the copy, so a build whose assets changed
 * makes the copy again and one whose assets did not leaves it alone. The
 * rest of the internal storage directory is SDL's preferences path:
 * WinBolo.json, the saved maps and the user's own brains live there.
 * ------------------------------------------------------- */
#define ANDROID_APP_DIR        "app"
#define ANDROID_ASSET_MANIFEST "asset_manifest.txt"
#define ANDROID_ASSET_STAMP    "assets_stamp.txt"

static void androidRemoveTree(const char *path);

static SDL_EnumerationResult SDLCALL androidRemoveEntry(void *userdata, const char *dirname,
                                                        const char *fname) {
  char full[FILENAME_MAX];
  size_t n = strlen(dirname);
  (void)userdata;
  snprintf(full, sizeof(full), "%s%s%s", dirname,
           (n > 0 && dirname[n - 1] == '/') ? "" : "/", fname);
  androidRemoveTree(full);
  return SDL_ENUM_CONTINUE;
}

static void androidRemoveTree(const char *path) {
  SDL_PathInfo info;
  if (!SDL_GetPathInfo(path, &info)) return;
  if (info.type == SDL_PATHTYPE_DIRECTORY) {
    SDL_EnumerateDirectory(path, androidRemoveEntry, NULL);
  }
  SDL_RemovePath(path);
}

/* One listed file from the APK into appDir, with its parent directories. */
static bool androidExtractOne(const char *appDir, const char *rel) {
  char dest[FILENAME_MAX];
  size_t len = 0;
  void *data;
  char *slash;
  snprintf(dest, sizeof(dest), "%s/%s", appDir, rel);
  slash = strrchr(dest, '/');
  if (slash) {
    *slash = '\0';
    SDL_CreateDirectory(dest);
    *slash = '/';
  }
  /* Read as an asset: nothing of this name sits in the internal storage
     directory itself, which SDL looks in first. An APK built on a
     case-insensitive host can carry the brain tree under its old lower-case
     name; the copy still goes to Brains/, where the scanner looks. */
  data = SDL_LoadFile(rel, &len);
  if (!data && strncmp(rel, "Brains/", 7) == 0) {
    char lower[FILENAME_MAX];
    snprintf(lower, sizeof(lower), "brains/%s", rel + 7);
    data = SDL_LoadFile(lower, &len);
  }
  if (!data) {
    WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "[Android] asset '%s' not readable: %s", rel, SDL_GetError());
    return false;
  }
  if (!SDL_SaveFile(dest, data, len)) {
    WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "[Android] could not write '%s': %s", dest, SDL_GetError());
    SDL_free(data);
    return false;
  }
  SDL_free(data);
  return true;
}

/* Reads the first line of a small text file; empty when it is not there. */
static void androidReadStamp(const char *path, char *out, size_t outLen) {
  FILE *fp = fopen(path, "r");
  out[0] = '\0';
  if (!fp) return;
  if (fgets(out, (int)outLen, fp)) {
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
  }
  fclose(fp);
}

static bool androidExtractAssets(void) {
  const char *base = SDL_GetAndroidInternalStoragePath();
  char appDir[FILENAME_MAX];
  char stampPath[FILENAME_MAX];
  char haveStamp[128];
  char wantStamp[128];
  size_t manifestLen = 0;
  char *manifest;
  char *line;
  char *save = NULL;
  int copied = 0, failed = 0;

  if (!base) {
    WB_LOG_ERROR(WB_LOG_CAT_PLATFORM, "[Android] no internal storage path: %s", SDL_GetError());
    return false;
  }
  snprintf(appDir, sizeof(appDir), "%s/%s", base, ANDROID_APP_DIR);
  snprintf(stampPath, sizeof(stampPath), "%s/%s", base, ANDROID_ASSET_STAMP);

  manifest = (char *)SDL_LoadFile(ANDROID_ASSET_MANIFEST, &manifestLen);
  if (!manifest) {
    WB_LOG_ERROR(WB_LOG_CAT_PLATFORM, "[Android] no %s in the APK: %s", ANDROID_ASSET_MANIFEST, SDL_GetError());
    return false;
  }
  line = strtok_r(manifest, "\r\n", &save);
  if (!line || strncmp(line, "stamp ", 6) != 0) {
    WB_LOG_ERROR(WB_LOG_CAT_PLATFORM, "[Android] %s has no stamp line", ANDROID_ASSET_MANIFEST);
    SDL_free(manifest);
    return false;
  }
  snprintf(wantStamp, sizeof(wantStamp), "%s", line);
  androidReadStamp(stampPath, haveStamp, sizeof(haveStamp));
  if (strcmp(haveStamp, wantStamp) == 0) {
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] assets in %s are current", appDir);
    SDL_free(manifest);
    return true;
  }

  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] extracting assets into %s", appDir);
  /* A file this build no longer ships must not linger from the last one. */
  {
    char old[FILENAME_MAX];
    snprintf(old, sizeof(old), "%s/data", appDir);
    androidRemoveTree(old);
    snprintf(old, sizeof(old), "%s/Brains", appDir);
    androidRemoveTree(old);
  }
  SDL_CreateDirectory(appDir);
  while ((line = strtok_r(NULL, "\r\n", &save)) != NULL) {
    if (line[0] == '\0') continue;
    if (androidExtractOne(appDir, line)) copied++; else failed++;
  }
  SDL_free(manifest);
  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] extracted %d assets, %d failed", copied, failed);
  if (failed > 0) {
    return false;  /* no stamp: the next launch tries again */
  }
  {
    char stampLine[160];
    snprintf(stampLine, sizeof(stampLine), "%s\n", wantStamp);
    if (!SDL_SaveFile(stampPath, stampLine, strlen(stampLine))) {
      WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "[Android] could not write %s: %s", stampPath, SDL_GetError());
    }
  }
  return true;
}

/* The logical size touch coordinates are read in: the renderer's logical
   presentation when one is set, else the window. */
static void androidTouchSpace(int *tw, int *th) {
  SDL_Renderer *ren = sdl3DrawGetRenderer();
  *tw = 0;
  *th = 0;
  if (ren) {
    SDL_RendererLogicalPresentation mode;
    SDL_GetRenderLogicalPresentation(ren, tw, th, &mode);
  }
  if (*tw <= 0 || *th <= 0) {
    SDL_GetWindowSize(sdl3DrawGetWindow(), tw, th);
  }
}

/* -------------------------------------------------------
 * main — Android entry point (called by SDL3's SDLActivity)
 * ------------------------------------------------------- */
int main(int argc, char *argv[]) {
  const char *cmdLine = "";
  DWORD tick;

  bolo_srand((uint64_t)time(NULL) ^ (uint64_t)getpid());

  /* Check for winbolo:// URL passed via intent (see WinBoloActivity.getArguments) */
  for (int i = 1; i < argc; i++) {
    if (cmdLine[0] == '\0') {
      cmdLine = argv[i];
    }
  }

  SDL_Init(0);

  /* The data and brain trees as files, and the working directory they are
     relative to (see androidExtractAssets). A failed extraction is logged
     and the launch goes on: the opens that need a file then fail where
     they are made. */
  androidExtractAssets();
  {
    const char *base = SDL_GetAndroidInternalStoragePath();
    char appDir[FILENAME_MAX];
    if (base) {
      snprintf(appDir, sizeof(appDir), "%s/%s", base, ANDROID_APP_DIR);
      if (chdir(appDir) != 0) {
        WB_LOG_ERROR(WB_LOG_CAT_PLATFORM, "[Android] chdir('%s') failed", appDir);
      } else {
        WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Working directory: %s", appDir);
      }
    }
  }

  /* The move to and from the background reaches the game only through an
     event watch (see androidLifecycleWatch). */
  s_resumeEventType = SDL_RegisterEvents(1);
  SDL_AddEventWatch(androidLifecycleWatch, NULL);

  if (clientMutexCreate() == FALSE) {
    WB_LOG_ERROR(WB_LOG_CAT_PLATFORM, "[Android] Failed to create client mutex");
    return 1;
  }

  /* The thread manager's mutex, as the desktop creates it right after the
     client mutex (winbolo.c). Without it the handle is NULL, SDL_LockMutex
     does nothing, and the lock the hosted server timer thread (gamefront.c)
     and the main thread take around the local server is fictional: each
     overwrites the owner, and the timer thread dies on
     serverSimApplyCommand's assertion that it holds the mutex, at the first
     bot command of a single-player game. Found and fixed on iOS first. */
  if (threadsCreate(FALSE) == FALSE) {
    WB_LOG_ERROR(WB_LOG_CAT_PLATFORM, "[Android] Failed to create the threads mutex");
    clientMutexDestroy();
    return 1;
  }

  /* Auto-detect appropriate zoom level based on screen height */
  {
    SDL_DisplayID disp = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(disp);
    if (dm) {
      int h = dm->h;
      WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Display resolution: %dx%d", dm->w, h);
      if (h >= 1024) {
        zoomFactor = ZOOM_FACTOR_QUAD;
      } else if (h >= 512) {
        zoomFactor = ZOOM_FACTOR_DOUBLE;
      } else {
        zoomFactor = ZOOM_FACTOR_NORMAL;
      }
      WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Auto-selected zoom factor: %d", zoomFactor);
    }
  }

  dialogBackendInit();

  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Starting gameFrontStart...");
  if (gameFrontStart(cmdLine, &keys, FALSE, NULL) == FALSE) {
    WB_LOG_ERROR(WB_LOG_CAT_PLATFORM, "[Android] gameFrontStart FAILED");
    clientMutexDestroy();
    SDL_Quit();
    threadsDestroy();
    return 1;
  }
  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] gameFrontStart OK");

android_game_start:
  /* Start the shared keys/game cadence from a known state, so the first
     running step is a game step on tick 0 (even = game is the wire contract
     the server routes by).  Placed on the restart label rather than after
     the lobby, so it covers both the first game and every game reached
     through the goto below. */
  clientFrontTickReset();

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
      androidTouchSpace(&tw, &th);

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
          WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Safe area insets: L=%d T=%d R=%d B=%d",
                  safeLeft, safeTop, safeRight, safeBottom);
        }
      }

      touchInputSetup(tw, th, safeLeft, safeTop, safeRight, safeBottom);
      WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Touch coordinate space: %dx%d", tw, th);
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
  if (clientFrontInLobbyPhase(cs)) {
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Entering lobby");
    sdl3ImguiCleanup();  /* Tear down existing ImGui — lobby creates its own */
    const DialogBackend *db = dialogBackendGet();
    int lobbyResult = db->lobbyShow(cs);
    if (lobbyResult == 0 && !windowIsQuitting()) {
      /* The player left the lobby rather than quitting: back to the menu,
         as the desktop does. No tank exists in the lobby, so the game is
         ended as not played. */
      WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Left lobby, returning to the menu");
      gameFrontEnd(&keys, FALSE, FALSE);
      if (gameFrontStart("", &keys, TRUE, NULL)) {
        goto android_game_start;
      }
      WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "[Android] gameFrontStart failed after leaving the lobby");
    }
    if (lobbyResult == 0) {
      /* Player chose to leave or server shut down */
      WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Left lobby, cleaning up");
      gameFrontEnd(&keys, FALSE, TRUE);
      clientMutexDestroy();
      sdl3DrawCleanup();
      soundCleanup();
      SDL_Quit();
      threadsDestroy();
      return 0;
    }
    /* lobbyResult == 1: game started. The UDP transport's
     * CTRL_GAME_PHASE LOBBY→RUNNING watcher already installed the
     * map onto the ClientSim, so we fall straight through to the
     * per-frame game-tick loop below. */
    clientSimSetNetStatus(cs, netRunning);
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Lobby complete, game starting");
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

  oldTick = SDL_GetTicks();

  /* Flush any stale render state left over from the dialog phase.
     The dialog loop destroys textures and restores logical presentation
     after its last SDL_RenderPresent, which can leave the GL command
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

  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Starting main loop");

  /* Main game loop */
#define ANDROID_FRAME_CAP_MS 16
  bool returnToLobby = FALSE;
  s_lifecycleCs = cs;
  while (!winboloQuit && !finishedLoop) {
    Uint64 frameCapStart = SDL_GetTicks();
    /* Process SDL events */
    {
      SDL_Event ev;
      while (SDL_PollEvent(&ev)) {
        SDL_Event rawEv = ev;
        /* Convert coordinates to logical presentation space for ImGui */
        SDL_Renderer *ren = sdl3DrawGetRenderer();
        if (ren) {
          SDL_ConvertEventToRenderCoordinates(ren, &ev);
        }
        /* Forward to ImGui so dialogs receive mouse/touch input */
        sdl3ImguiForwardEvent(&ev);

        /* Always route finger events to the touch input system in the
           touch layout: the overlay's buttons (build bar, etc.) use
           inputTouchConsumeTapInRect for hit-testing, so the touch system
           must see every event regardless of ImGui's WantCaptureMouse
           state. The shared tick reads the movement, fire, mine and
           gunsight state from there (input.c). */
        if (rawEv.type == SDL_EVENT_FINGER_DOWN || rawEv.type == SDL_EVENT_FINGER_UP ||
            rawEv.type == SDL_EVENT_FINGER_MOTION || rawEv.type == SDL_EVENT_FINGER_CANCELED) {
          int tw = 0, th = 0;
          androidTouchSpace(&tw, &th);
          inputTouchProcessEvent(&rawEv, tw, th);
        }
        if (ev.type == SDL_EVENT_QUIT) {
          winboloQuit = TRUE;
        } else if (ev.type == s_resumeEventType) {
          /* Posted by androidLifecycleWatch on the return to the
             foreground: both WILL and DID post one; the second finds
             nothing suspended. */
          windowResumeForeground(cs);
        }
      }
    }

    /* Game tick accumulation (replaces SDL_AddTimer) */
    if (clientSimHasTransport(cs)) {
      Uint64 now = SDL_GetTicks();
      double elapsed = (double)(now - lastFrameTime);
      lastFrameTime = now;
      gameTickAccum += elapsed;

      while (gameTickAccum >= GAME_TICK_LENGTH) {
        /* A tutorial message is up, or the tick just run queued one:
           stop here and drop the owed time, so the interval the
           message is up is not fast-forwarded afterwards. */
        if (clientFrontAnyPause() || clientFrontTutorialStepShowing()) {
          gameTickAccum = 0.0;
          break;
        }
        gameTickAccum -= GAME_TICK_LENGTH;
        windowRunGameTick(cs);
      }
    }

    /* The round has handed the game back to the lobby (a vote to return,
       or a game over): leave the loop before drawing, and show the lobby
       screen from the top, as the desktop does. */
    if (clientFrontInLobbyPhase(cs)) {
      returnToLobby = TRUE;
      break;
    }

    /* Render */
    tick = SDL_GetTicks();
    clientMutexWaitFor();
    if (finishedLoop == FALSE) {
      clientFrontRenderPrepare(cs, tick);
      clientRenderFrame(cs, redraw);
    }
    clientMutexRelease();
    /* The in-window map overview draws from the snapshot the frame above
       filled, now that the lock is off. */
    sdl3DrawFlushOverviewInWindow();
    dwSysFrame += (SDL_GetTicks() - tick);

    /* The touch layout's overlay is drawn by sdl3ImguiPumpAndRender. */
    sdl3ImguiPumpAndRender(cs);
    {
      SDL_Renderer *ren = sdl3DrawGetRenderer();
      if (ren) SDL_RenderPresent(ren);
    }
    /* Tutorial: count the presented frame (the intro waits for a few)
       and show the respawn message the server may have flagged. Both do
       nothing outside the tutorial. */
    frontEndTutorialNotePresentedFrame();
    clientFrontTutorialPollRespawn();

    /* Cap to 60 FPS */
    {
      Uint64 frameElapsed = SDL_GetTicks() - frameCapStart;
      if (frameElapsed < ANDROID_FRAME_CAP_MS) {
        SDL_Delay((Uint32)(ANDROID_FRAME_CAP_MS - frameElapsed));
      }
    }
  }

  s_lifecycleCs = NULL;

  /* A game that ends in the background leaves the suspend on, and nothing
     else clears it: the lobby, the shutdown and the menu would run muted
     with the next game's server paused. The watch does nothing from here
     on, so clear it now. */
  if (clientFrontResumeBegin()) {
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Game ended while in the background; clearing the suspend");
    clientFrontResumeEnd(cs);
  }

  /* Back to the lobby screen: the game and its ClientSim stay, as on the
     desktop. Not if the player quit in the same frame. */
  if (returnToLobby && !winboloQuit && !windowIsQuitting()) {
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Round over, returning to the lobby");
    gameFrontSaveTankPrefs(cs);
    sdl3ImguiCleanup();
    goto android_game_start;
  }

  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Main loop ended, cleaning up");

  sdl3ImguiCleanup();
  gameFrontEnd(&keys, TRUE, winboloQuit);
  /* A step left showing, or its pause, would stop the next game's ticks. */
  frontEndTutorialReset();
  windowTutorialPause(NULL, false);
  windowDeckPause(NULL, false);
  windowControllerLostPause(NULL, false);

  if (!winboloQuit) {
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Returning to menu (windowNewGame)");
    /* Restart the pre-game dialogs and re-enter the game loop */
    if (gameFrontStart("", &keys, TRUE, NULL)) {
      goto android_game_start;
    }
    WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "[Android] gameFrontStart failed after leave game");
  }

  /* Cleanup */
  clientMutexDestroy();
  sdl3DrawCleanup();
  soundCleanup();
  SDL_Quit();
  threadsDestroy();
  return 0;
}
