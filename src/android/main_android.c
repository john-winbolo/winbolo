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
#include "server_sim.h"  /* serverSimBotPoolInit / Destroy */
#include "client_render.h"
#include "frontend.h"
#include "client_net.h"
#include "gui_message.h"
#include "../gui/brainsHandler.h"
#include "../gui/clientmutex.h"
#include "../server/threads.h"  /* threadsCreate / threadsDestroy */
#include "../server/mdns_advertise.h"  /* mdnsAdvertiseSetMulticastHold */
#include "../gui/draw.h"
#include "../gui/gamefront.h"
#include "../gui/input.h"
#include "../gui/lang.h"
#include "../gui/sound.h"
#include "../gui/voice.h"
#include "../gui/ui_mode.h"
#include "../gui/winbolo.h"
#include "../gui/sdl3/sdl3draw.h"
#include "../gui/sdl3/sdl3imgui.h"
#include "../gui/sdl3/input_touch.h"
#include "../gui/sdl3/safe_insets.h"
#include "client_frontend_render.h"
#include "client_frontend_common.h"
#include "client_frontend_tick.h"
#include "client_frontend_tutorial.h"
#include "../gui/sdl3/luabrainshandler.h"
#include "../gui/sdl3/dialog_backend.h"
#include "../gui/sdl3/dialogs/imgui_messagebox.h"
#include "../winbolonet/http_jni.h"
#include "../winbolonet/http.h"  /* httpSetDeadline for a shutdown in the background */
#include "android_device.h"
#include "android_drops.h"
#include "android_netpath.h"
#include "android_share.h"
#include "touch_input.h"

extern ClientSim *humanSim;

/* -------------------------------------------------------
 * Globals declared by winbolo.h / used by android_frontend.c
 * ------------------------------------------------------- */

bool isTutorial = FALSE;

/* The Frame Rate choice (a FrameLimit). Android paces its own screen, so
   it is kept only for the shared code that reads it. */
int frameRate = FRAME_LIMIT_DEFAULT;

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

/* The game loop's own pause while the app is in the background, set by
   windowSuspendBackground and cleared by windowResumeForeground. */
static bool paused = FALSE;

/* A game this device hosts for other players keeps running in the
   background: the server ticks on its own timer thread (gamefront.c's
   hostedServerTimerCb, through serverInstanceTick under the threads mutex,
   the one path into the sim), and the main loop keeps ticking the host's
   own client, without drawing, so the server does not drop the host after
   20 s of silence (CLIENT_TIMEOUT_TICKS). For that the main thread must not
   stop in SDL's event pump when the activity pauses, which is SDL's default
   on Android: SDL_HINT_ANDROID_BLOCK_ON_PAUSE is set to 0 before SDL_Init.
   SDL destroys the window's surface while the activity is paused, so a
   frame drawn then has nowhere to go: the game loop below draws nothing
   while paused. The shared dialog loops, which keep drawing and know
   nothing of the background, are covered by SDL itself: it marks every
   window minimized on the way to the background and restored on the way
   back (SDL_OnApplicationWillEnterBackground), and on Android its renderer
   sends nothing to the GPU and presents nothing for a minimized window
   (SDL_render.c, DONT_DRAW_WHILE_HIDDEN). The pump waits up to 100 ms for
   a lifecycle event on every poll while paused, so a background loop runs
   at about ten frames a second. Android gives no time limit and no warning.
   With no foreground service, Android 15 and later put the app's traffic
   behind the background firewall chain about five seconds after the
   activity stops, and some time later freeze the whole process, server
   thread included; both lift on the return. So the loop keeps the host's
   place for a short stay (a notification, a quick look elsewhere); through
   a longer one the other players lose the game to their own timeouts.
   Ending the process (onDestroy, TERMINATING) shuts the server
   down through the normal end, with a deadline on its WinBolo.net calls so
   an unreachable WinBolo.net does not hold onDestroy. SDL's onDestroy
   joins this thread for one second and then tears SDL's Android state
   down whether or not the thread has finished, so the deadline has to
   leave the rest of the shutdown room inside that second. */
static bool s_hostBackgroundActive = false;
static bool s_quitInBackground = false;
#define ANDROID_HOST_SHUTDOWN_HTTP_MS 700

/* A game joined over the network is given up after this long in the
   background: the server drops a client silent for 20 s, so a return after
   that would be into a game that can no longer be played; the margin covers
   the radio coming back. Measured on the wall clock, since SDL_GetTicks
   does not advance while the device sleeps and the server's clock does.
   The same check as the iOS driver's. */
#define ANDROID_BACKGROUND_KEEP_GAME_MS 15000
static SDL_Time s_backgroundStartNs = 0;
/* Tick time owed after a long freeze is dropped beyond this: the server
   kept its own time on its thread, and the host's client only has to catch
   up with the present, not replay the gap. */
#define ANDROID_HOST_CATCHUP_MAX_MS 1000.0
/* A line in the log this often while hosting in the background, so a
   logcat read shows the loop running. */
#define ANDROID_HOST_LOG_INTERVAL_MS 5000
static Uint64 s_hostBackgroundLogAt = 0;
static unsigned s_hostBackgroundTicks = 0;

/* Between the background pair and the foreground pair of lifecycle events. */
static bool s_inBackground = false;
/* The menu's and the dialogs' own background pause: nothing of theirs
   knows about the background, so the sound is muted for them here and put
   back as it was found. */
static bool s_menuMuteHeld = false;
static bool s_menuMutedBefore = false;

static void androidMenuBackgroundBegin(void) {
  paused = TRUE;
  s_menuMutedBefore = soundIsMuted();
  s_menuMuteHeld = true;
  soundSetMuted(TRUE);
  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Entering background outside a game");
}

static void androidMenuBackgroundEnd(void) {
  paused = FALSE;
  if (s_menuMuteHeld) {
    s_menuMuteHeld = false;
    soundSetMuted(s_menuMutedBefore);
  }
}

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
static void androidTouchSpace(int *tw, int *th);
#define ANDROID_FRAME_CAP_MS 16

/* SDL never queues the app lifecycle events (SDL_SendAppEvent hands them to
   event watches only), so the game loop's own event handling never sees
   them. This watch acts on them. Android sends the background pair from the
   main thread inside the event pump; the foreground pair arrives the same
   way on the return, and posts s_resumeEventType, which the loop turns into
   the resume on its own stack, where the resume's message box may run.
   While a game is up (s_lifecycleCs) the shared suspend and resume run;
   outside one, where no loop knows about the background, the sound is
   muted here and put back on the return. Ending the process (TERMINATING,
   from onDestroy) ends the loop the way Quit does; the preferences are
   written by gamefront.c's own watch. */
static ClientSim *s_lifecycleCs = NULL;
static Uint32 s_resumeEventType = 0;

static bool SDLCALL androidLifecycleWatch(void *userdata, SDL_Event *ev) {
  (void)userdata;
  switch (ev->type) {
  case SDL_EVENT_WILL_ENTER_BACKGROUND:
  case SDL_EVENT_DID_ENTER_BACKGROUND:
    /* WILL and then DID: the second finds everything done. */
    if (s_inBackground) break;
    s_inBackground = true;
    if (s_lifecycleCs != NULL) {
      windowSuspendBackground(s_lifecycleCs);
    } else {
      androidMenuBackgroundBegin();
    }
    break;
  case SDL_EVENT_WILL_ENTER_FOREGROUND:
  case SDL_EVENT_DID_ENTER_FOREGROUND:
    if (!s_inBackground) break;
    s_inBackground = false;
    if (s_lifecycleCs != NULL) {
      if (s_resumeEventType != 0) {
        SDL_Event resume;
        SDL_zero(resume);
        resume.type = s_resumeEventType;
        SDL_PushEvent(&resume);
      }
    } else {
      androidMenuBackgroundEnd();
      WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Back in the foreground outside a game");
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

/* The system Back key. SDL is told to keep it (SDL_HINT_ANDROID_TRAP_BACK_BUTTON
   in main), so instead of ending the activity it arrives as a key event with
   the AC_BACK scancode. This filter, run on every event before it is queued,
   turns that key's down and up into Escape: the dialogs, the lobby and ImGui
   itself already act on Escape as their cancel, so Back cancels the same
   things with no test of its own anywhere. Two places act on the Escape
   this makes: the welcome screen, which has nothing to cancel and hands the
   press back to Android's own back handling (imgui_welcome.cpp), and the
   in-game Escape ladder, which opens Settings when it closed nothing
   (sdl3imgui.cpp). A press while the keyboard is up never gets this far:
   the input method takes it and closes the keyboard. */
static bool SDLCALL androidBackKeyFilter(void *userdata, SDL_Event *ev) {
  (void)userdata;
  if ((ev->type == SDL_EVENT_KEY_DOWN || ev->type == SDL_EVENT_KEY_UP) &&
      ev->key.scancode == SDL_SCANCODE_AC_BACK) {
    ev->key.scancode = SDL_SCANCODE_ESCAPE;
    ev->key.key = SDLK_ESCAPE;
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
   back, and drops the tick time owed for the time away. A game hosted for
   other players is kept running instead (see s_hostBackgroundActive): the
   suspend still mutes, but the server, which only pauses for a solo
   session, carries on, and the loop keeps ticking the host's client. */
void windowSuspendBackground(ClientSim *cs) {
  if (paused) return;
  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Entering background, pausing");
  paused = TRUE;
  if (!SDL_GetCurrentTime(&s_backgroundStartNs)) {
    s_backgroundStartNs = 0;
  }
  clientFrontSuspend(cs);
  if (gameFrontHasLocalServer() && !clientFrontIsSoloSession(cs)) {
    s_hostBackgroundActive = true;
    lastFrameTime = SDL_GetTicks();
    gameTickAccum = 0.0;
    s_hostBackgroundLogAt = lastFrameTime;
    s_hostBackgroundTicks = 0;
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM,
                "[Android] Hosting: keeping the game running in the background");
  }
}

/* Undoes windowSuspendBackground: clears the pause and the background
   hosting, drops the owed tick time and runs the shared resume. Used by
   windowResumeForeground and by the main loop's exit. */
static void androidClearBackgroundPause(ClientSim *cs) {
  paused = FALSE;
  s_hostBackgroundActive = false;
  lastFrameTime = SDL_GetTicks();
  gameTickAccum = 0.0;
  if (clientFrontResumeBegin()) {
    clientFrontResumeEnd(cs);
  }
  /* A pause the menu began before this game loop started is over too. */
  androidMenuBackgroundEnd();
}

void windowResumeForeground(ClientSim *cs) {
  SDL_Time nowNs = 0;
  Sint64 awayMs;

  /* WILL and then DID both post a resume; the second finds nothing paused. */
  if (!paused) return;
  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Resuming from background");
  androidClearBackgroundPause(cs);

  /* Single player keeps its game: its server is in this process and was
     paused with it; a game hosted here for other players kept running. A
     game joined over the network has the server's 20 s timeout against it
     (see ANDROID_BACKGROUND_KEEP_GAME_MS). Without a start time the
     client's own timeout still ends a dead game within ten seconds. */
  if (cs == NULL || !clientSimHasTransport(cs) || gameFrontGetServerSim() != NULL ||
      clientSimGetNetType(cs) != netUdp || s_backgroundStartNs == 0 ||
      !SDL_GetCurrentTime(&nowNs)) {
    s_backgroundStartNs = 0;
    return;
  }
  awayMs = (Sint64)((nowNs - s_backgroundStartNs) / SDL_NS_PER_MS);
  s_backgroundStartNs = 0;
  if (awayMs < ANDROID_BACKGROUND_KEEP_GAME_MS) {
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM,
                "[Android] Back after %lld ms; keeping the network game",
                (long long)awayMs);
    return;
  }
  WB_LOG_INFO(WB_LOG_CAT_PLATFORM,
              "[Android] Back after %lld ms; the server has dropped this client",
              (long long)awayMs);
  /* The same path as the lost-connection check in windowRunGameTick. */
  clientSimConnectionLost(cs);
  imguiMessageBoxEx(DIALOG_BOX_TITLE,
                    langGetText(NETERR_LOSTCONNECTION_RETURN_MENU),
                    IMGUI_MSG_ERROR, IMGUI_MSG_OK);
  finishedLoop = TRUE;
  winboloQuit = FALSE;
}

/* One of the loop's events while paused: the quit, the resume, and a
   finger's release or cancel so a touch held at the moment the app left
   does not read as held on the way back. */
static void androidPausedEvent(ClientSim *cs, SDL_Event *ev) {
  if (ev->type == SDL_EVENT_QUIT || ev->type == SDL_EVENT_TERMINATING) {
    winboloQuit = TRUE;
    s_quitInBackground = true;
  } else if (ev->type == s_resumeEventType) {
    windowResumeForeground(cs);
  } else if (ev->type == SDL_EVENT_FINGER_DOWN || ev->type == SDL_EVENT_FINGER_UP ||
             ev->type == SDL_EVENT_FINGER_MOTION || ev->type == SDL_EVENT_FINGER_CANCELED) {
    int tw = 0, th = 0;
    androidTouchSpace(&tw, &th);
    inputTouchProcessEvent(ev, tw, th);
  }
}

/* One frame of a hosted game in the background: the events that matter, and
   the host's own client ticks so its connection to the server stays alive.
   Nothing is drawn. The shared pause test the foreground loop makes before a
   tick is skipped on purpose: the suspend is one of those pauses, and here
   the game is meant to run through it. */
static void androidHostBackgroundFrame(ClientSim *cs) {
  SDL_Event ev;
  Uint64 now;
  while (SDL_PollEvent(&ev)) {
    androidPausedEvent(cs, &ev);
  }
  if (!paused || winboloQuit) return;
  now = SDL_GetTicks();
  gameTickAccum += (double)(now - lastFrameTime);
  lastFrameTime = now;
  if (gameTickAccum > ANDROID_HOST_CATCHUP_MAX_MS) {
    gameTickAccum = ANDROID_HOST_CATCHUP_MAX_MS;
  }
  while (gameTickAccum >= GAME_TICK_LENGTH && !winboloQuit && !finishedLoop) {
    gameTickAccum -= GAME_TICK_LENGTH;
    windowRunGameTick(cs);
    s_hostBackgroundTicks++;
  }
  if (now - s_hostBackgroundLogAt >= ANDROID_HOST_LOG_INTERVAL_MS) {
    s_hostBackgroundLogAt = now;
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM,
                "[Android] Hosting in the background: %u client ticks run so far",
                s_hostBackgroundTicks);
  }
  SDL_Delay(ANDROID_FRAME_CAP_MS);
}

/* The background without a hosted game: nothing ticks and nothing is
   drawn; the loop waits for the resume or the quit. */
static void androidPausedWait(ClientSim *cs) {
  SDL_Event ev;
  if (SDL_WaitEventTimeout(&ev, 100)) {
    androidPausedEvent(cs, &ev);
  }
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
 * ANDROID_APP_DIR itself is in android_device.h, with the
 * absolute-path helper the shared code opens files by.
 * ------------------------------------------------------- */
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

#if !defined(NDEBUG)
  /* A debug build echoes every category at INFO to logcat. SDL's own
     default leaves the categories this code logs under at ERROR, which hides
     the lines a tester reads a run by. */
  SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
#endif

  /* The main thread keeps running while the activity is paused, for a game
     hosted for other players (see s_hostBackgroundActive). Read once, at
     SDL's video start, so it is set before anything of SDL is up. */
  SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");

  /* The system Back key comes to this code as a key event rather than
     ending the activity (see androidBackKeyFilter). The manifest turns off
     Android 16's predictive back as well, without which the key is never
     dispatched to the app at this target. */
  SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");

  SDL_Init(0);
  /* The bot worker pool, as the desktop and iOS drivers start it. Without
     it every brain on a hosted server thinks one after another on the
     server's tick thread, and the Android thread cap in bot_manager.c
     never applies. */
  if (!serverSimBotPoolInit(0)) {
    WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "[Android] serverSimBotPoolInit failed");
    SDL_Quit();
    return 1;
  }
  androidDropsInstall();

  /* The one argument a cold launch through a VIEW intent carries (see
     WinBoloActivity.getArguments): a winbolo:// link goes to gameFrontStart
     as the command line, as on the desktop; a recording's path is kept for
     the welcome screen, which opens it as a link arriving later would be. */
  for (int i = 1; i < argc; i++) {
    size_t len = strlen(argv[i]);
    if (len > 4 && SDL_strcasecmp(argv[i] + len - 4, ".wbv") == 0) {
      androidDropsKeepRecording(argv[i]);
    } else if (cmdLine[0] == '\0') {
      cmdLine = argv[i];
    }
  }

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

  /* The WinBolo.net transport's Java class, found from this thread, whose
     class loader can see it; the worker threads that make the requests
     cannot find it themselves. */
  if (!httpAndroidInit()) {
    WB_LOG_ERROR(WB_LOG_CAT_PLATFORM, "[Android] WinBolo.net HTTP transport not bound");
  }

  /* The kind of network the phone is on, for the warning before an
     Internet game is hosted on mobile data. Bound from this thread for the
     same reason. Without it the game front asks nothing. */
  if (!androidNetPathInstall()) {
    WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "[Android] network kind not available");
  }

  /* The share sheet a recording or a clip GIF leaves the phone through.
     Bound from this thread for the same reason. */
  if (!androidShareInstall()) {
    WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "[Android] share sheet not available");
  }

  /* Phone or tablet, from the configuration's smallest width, before the
     UI mode is first asked (uiModeDetect reads it). A tablet's classic
     screen has no menu bar and is played by touch, as the iPad's is: the
     touch controls come up when it is touched and go when a key is pressed
     (input_touch.c). */
  if (!androidDeviceInit()) {
    WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "[Android] device size not available; phone assumed");
  }
  if (androidDeviceIsTablet()) {
    inputTouchSetClassicControlsOffered(true);
  }
  /* A hosted game's mDNS advertiser holds the Wi-Fi multicast lock while
     its socket is open (the LAN search holds it for its browse itself). */
  mdnsAdvertiseSetMulticastHold(androidDeviceMulticastHold);

  /* The move to and from the background reaches the game only through an
     event watch (see androidLifecycleWatch). */
  s_resumeEventType = SDL_RegisterEvents(1);
  SDL_AddEventWatch(androidLifecycleWatch, NULL);
  /* The Back key becomes Escape before anything polls it. */
  SDL_SetEventFilter(androidBackKeyFilter, NULL);

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

  /* Voice runs for the life of the process, as on the desktop and iOS: it
     comes up before gameFrontStart, which applies the saved voice settings
     to it and whose pre-game dialogs expect it to exist. It opens the
     playback side only; the microphone waits until a connection carries
     voice, which is also when the permission is asked for (android_voice.c).
     A device that will not open is not fatal - voice stays disabled. */
  voiceInit();

  dialogBackendInit();

  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Starting gameFrontStart...");
  if (gameFrontStart(cmdLine, &keys, FALSE, NULL) == FALSE) {
    WB_LOG_ERROR(WB_LOG_CAT_PLATFORM, "[Android] gameFrontStart FAILED");
    clientMutexDestroy();
    serverSimBotPoolDestroy();
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

      /* The safe area, for display cutouts (notches, punch-holes), as every
         mobile screen reads it (safeInsetsGet), scaled from window
         coordinates to the touch coordinate space so the touch controls
         stay inside it. */
      int safeLeft = 0, safeTop = 0, safeRight = 0, safeBottom = 0;
      {
        const SafeInsets safe = safeInsetsGet(win);
        int winW = 0, winH = 0;
        SDL_GetWindowSize(win, &winW, &winH);
        if (winW > 0 && winH > 0) {
          safeLeft   = (int)(safe.left   * (float)tw / (float)winW);
          safeTop    = (int)(safe.top    * (float)th / (float)winH);
          safeRight  = (int)(safe.right  * (float)tw / (float)winW);
          safeBottom = (int)(safe.bottom * (float)th / (float)winH);
        }
        WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Safe area insets: L=%d T=%d R=%d B=%d",
                    safeLeft, safeTop, safeRight, safeBottom);
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
      voiceCleanup();
      serverSimBotPoolDestroy();
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
  bool returnToLobby = FALSE;
  s_lifecycleCs = cs;
  if (paused) {
    /* The app went to the background before this loop began (a hosted
       lobby's game started while the host was away): the menu's pause
       becomes the game's own, which decides about hosting on. */
    androidMenuBackgroundEnd();
    windowSuspendBackground(cs);
  }
  while (!winboloQuit && !finishedLoop) {
    Uint64 frameCapStart = SDL_GetTicks();
    /* In the background: no drawing, and no ticking unless this device
       hosts the game for other players. */
    if (paused) {
      if (s_hostBackgroundActive) {
        androidHostBackgroundFrame(cs);
      } else {
        androidPausedWait(cs);
      }
      continue;
    }
    /* Process SDL events */
    {
      SDL_Event ev;
      if (!uiModeIsTablet()) {
        /* A tablet's classic screen takes the desktop's event handling
           whole, as the iPad's does: the keys that act on a press, click to
           build, the wheel, and the finger events for the touch controls
           over it. The move to and from the background is
           androidLifecycleWatch's. */
        sdl3ImguiProcessEvents(cs);
      } else while (SDL_PollEvent(&ev)) {
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

    /* Voice encode/decode runs here, on the main thread, beside the game
       tick, once a frame as on the desktop - the codec state has no lock
       of its own. */
    voiceTick(cs);

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

  /* Release the remote talkers with the game they belong to - player
     numbers are handed out afresh next time, so a stale decoder would be
     fed someone else's voice. */
  voiceReset();

  /* A game that ends in the background leaves the pause on, and nothing
     else clears it: the lobby, the shutdown and the menu would run as if
     in the background, muted and with the next game's server paused. The
     watch treats the game as over from here on, so clear it now. */
  if (paused) {
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Game ended while in the background; clearing the pause");
    androidClearBackgroundPause(cs);
    /* Still in the background: what runs next is the lobby or the menu,
       which take the pause the watch gives them outside a game. */
    if (s_inBackground) {
      androidMenuBackgroundBegin();
    }
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
  /* The process is being ended while the game ran in the background: the
     hosted server's shutdown (its WinBolo.net session quit and round-log
     upload) runs inside onDestroy, which waits one second for this thread
     and no longer, so its calls get a deadline inside that second rather
     than their full time limits. */
  if (s_quitInBackground) {
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] Ending in the background; shutting the game down");
    httpSetDeadline(ANDROID_HOST_SHUTDOWN_HTTP_MS);
  }
  gameFrontEnd(&keys, TRUE, winboloQuit);
  if (s_quitInBackground) {
    httpClearDeadline();
    s_quitInBackground = false;
  }
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
  voiceCleanup();
  serverSimBotPoolDestroy();
  SDL_Quit();
  threadsDestroy();
  return 0;
}
