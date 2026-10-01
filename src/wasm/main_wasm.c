/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * main_wasm.c - Emscripten/WASM entry point for WinBolo game client
 *
 * Replaces gui/sdl3/winbolo.c for the WASM build.
 * The game runs as a blocking loop in main, paced by the browser's animation
 * frames through ASYNCIFY (wasmFrameWait), with frame-based tick accumulation
 * instead of SDL_AddTimer. The menu and the dialogs it opens run their own
 * blocking loops on the same stack, between games, paced the same way through
 * dialogFrameCapEnd. Functions called directly from JS must never sleep:
 * ASYNCIFY keeps one suspended stack at a time, and main's is always it.
 */

#include <SDL3/SDL.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>

#include <emscripten.h>

#include "bolo_rand.h"
#include "client_render.h"
#include "client_frontend_tick.h"
#include "client_frontend_render.h"
#include "client_sim.h"
#include "frontend.h"
#include "client_net.h"
#include "gui_message.h"
#include "playername_validate.h"
#include "../gui/brainsHandler.h"
#include "../gui/clientmutex.h"
#include "../gui/draw.h"
#include "../gui/gamefront.h"
#include "../common/prefs.h"
#include "../gui/input.h"
#include "../gui/lang.h"
#include "../gui/sound.h"
#include "../gui/ui_mode.h"
#include "../gui/voice.h"
#include "../gui/winbolo.h"
#include "../gui/sdl3/sdl3draw.h"
#include "../gui/sdl3/bg_game.h"
#include "../gui/sdl3/sdl3imgui.h"
#include "../gui/sdl3/input_gamepad.h"
#include "../gui/sdl3/build_cursor.h"
#include "../gui/sdl3/luabrainshandler.h"
#include "../gui/sdl3/dialogs/imgui_gamebrowser.h"
#include "../gui/sdl3/dialogs/imgui_lobby.h"
#include "../gui/sdl3/dialogs/imgui_messagebox.h"
#include "../gui/sdl3/dialogs/imgui_settings.h"
#include "../gui/sdl3/dialogs/imgui_tutorial_overlay.h"
#include "../gui/sdl3/dialogs/imgui_welcome.h"
#include "server_sim.h"
#include "../server/server_lifecycle.h"  /* serverInstanceTick */
#include "tutorial.h"
#include "cJSON.h"
#include "gamefront_wasm.h"

#include <sys/stat.h>

extern ClientSim *humanSim;

/* -------------------------------------------------------
 * Globals declared by winbolo.h
 * ------------------------------------------------------- */

void windowStartTutorial(void);
bool isTutorial = FALSE;

int frameRate = FRAME_RATE_30;
static int frameRateTime = (int)(MILLISECONDS / FRAME_RATE_30) - 1;

bool showGunsight = TRUE;  /* WASM default: gunsight on unless synced prefs override */
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

bool autoScrollingEnabled = TRUE;  /* WASM default: autoscroll on unless synced prefs override */
bool smoothScrollingEnabled = FALSE;  /* WASM: arrow-key smooth scroll inactive */
bool letterboxBarsGray = FALSE;       /* gray vs black letterbox bars (sdl3draw) */
BYTE zoomFactor = ZOOM_FACTOR_CUSTOM;

bool showPillLabels = FALSE;
bool showBaseLabels = FALSE;
bool labelSelf = TRUE;
labelLen labelMsg = lblShort;
labelLen labelTank = lblShort;

bool isInMenu = FALSE;

keyItems keys;

/* Timing */
static DWORD dwSysFrameTotal = 0;
static DWORD dwSysFrame = 0;
static DWORD dwSysGameTotal = 0;
static DWORD dwSysGame = 0;
static DWORD dwSysBrainTotal = 0;
static DWORD dwSysBrain = 0;

static bool drawBusy = FALSE;
static bool doingTutorial = FALSE;
static bool winboloQuit = FALSE;
static bool finishedLoop = FALSE;
static bool showAllianceReq = TRUE;
/* Set by windowLeaveGame; wasmRunGame ends the game on it. */
static bool s_leaveRequested = FALSE;
/* Browser history for games, kept by main's screen loop and not by
 * wasmGameStateReset. s_gameEntryPushed: the game was picked from the menu,
 * which pushed its history entry, so the menu's entry sits behind it.
 * s_leftByHistory: the game ended because Back or Forward moved off it, so
 * the browser is already on the menu's entry. */
static bool s_gameEntryPushed = FALSE;
static bool s_leftByHistory = FALSE;
/* Set when Back or Forward closed the game finder (wasmFinderTakeBack), so
 * the browser is already on the menu's entry. */
static bool s_finderLeftByHistory = FALSE;

/* Terminal connection-failure state. Set on any unrecoverable connection
 * problem (can't reach the relay, version mismatch, used/expired join code,
 * mid-game disconnect, or an unknown transport error). Once set, the main loop
 * shows one error dialog and freezes the game — it stops ticking and sending,
 * so we never re-open the WebSocket (join codes are single-use; there is
 * nothing to reconnect to) and never present a blank screen. */
static bool s_connFailed = FALSE;
static bool s_connErrorShown = FALSE;
static char s_connReason[256] = "";

/* Record a terminal connection failure. reason may be NULL/empty, in which case
 * a generic message is used. Called from gamefront_wasm.c (initial connect) and
 * from the main loop (mid-game disconnect). Idempotent — first reason wins. */
void wasmReportConnectFailure(const char *reason) {
  if (s_connFailed) return;
  if (reason != NULL && reason[0] != '\0') {
    strncpy(s_connReason, reason, sizeof(s_connReason) - 1);
    s_connReason[sizeof(s_connReason) - 1] = '\0';
  } else {
    strncpy(s_connReason, langGetText(STR_WEB_CONNECT_FAILED),
            sizeof(s_connReason) - 1);
    s_connReason[sizeof(s_connReason) - 1] = '\0';
  }
  s_connFailed = TRUE;
}

static DWORD oldTick = 0;
static DWORD ttick = 0;
static time_t ticks = 0;

/* Frame-based tick accumulator (replaces SDL_AddTimer) */
static double gameTickAccum = 0.0;
static double lastFrameTime = 0.0;

/* Flips on each 10 ms client step; the page ticks a practice game's server
 * on every second one (see wasmTickLocalServer). */
static bool s_serverTickDue = FALSE;

/* -------------------------------------------------------
 * SDL message handler
 * ------------------------------------------------------- */
void sdl3MessageHandler(const char *message, const char *title) {
  imguiMessageBoxEx(title ? title : "WinBolo",
                    message ? message : "",
                    IMGUI_MSG_INFO, IMGUI_MSG_OK);
}

/* Mouse-wheel bridge. SDL3's emscripten backend registers its wheel callback on
 * a different target from the pointer-event shim that delivers mouse motion and
 * clicks, and in the browser build it doesn't fire — so scrolling never reached
 * the shared gunsight handler. shell.html adds its own canvas 'wheel' listener
 * and calls this, which injects a native SDL wheel event; sdl3ImguiProcessEvents
 * then handles it exactly like a real wheel (same in-game + ImGui-capture
 * gating). deltaY is the browser value (negative = scroll up); SDL uses
 * positive-up, so flip the sign to a unit step. */
extern void inputBumpGunsight(int direction);       /* gui/sdl3/input.h */
extern uint8_t inputConsumeGunsightAdj(void);       /* gui/sdl3/input.h */
extern bool sdl3ImguiWantCaptureMouse(void);        /* sdl3imgui.cpp */

EMSCRIPTEN_KEEPALIVE
void wbWasmWheel(double deltaY) {
  if (deltaY == 0.0) return;
  /* Only adjust the gunsight in a running game, and not while the wheel is over
   * an ImGui panel/dialog (mirrors the shared desktop handler's gating). Browser
   * deltaY is negative when scrolling up; the gunsight increases on scroll-up. */
  if (!humanSim || clientSimGetNetStatus(humanSim) != netRunning) return;
  if (sdl3ImguiWantCaptureMouse()) return;
  inputBumpGunsight(deltaY < 0.0 ? +1 : -1);
}

extern void inputResetHeldKeys(void);               /* gui/sdl3/input.h */

/* Held-key safety reset for browser blur paths that never reach SDL. The game
 * polls SDL_GetKeyboardState; if the browser stops delivering keyup (tab
 * hidden, window blurred) a movement key reads as held forever and the client
 * keeps sending the turn button. shell.html calls this on visibilitychange
 * (hidden) and on window blur; inputResetHeldKeys clears SDL's key array and
 * the input module's latched edge state. */
EMSCRIPTEN_KEEPALIVE
void wbWasmResetHeldKeys(void) {
  inputResetHeldKeys();
}

/* 1 while the page is hidden (another tab, minimised). Reads document.hidden
 * directly, so it never suspends. */
EM_JS(int, wasmPageHidden, (void), {
  return document.hidden ? 1 : 0;
});

/* -------------------------------------------------------
 * windowRunGameTick — game logic using transport
 * ------------------------------------------------------- */
static void windowRunGameTick(ClientSim *cs) {
  static bool inBrain = FALSE;
  static BYTE t2 = 0;
  static int trackerTime = 11500;
  bool used = FALSE;
  bool brainRunning;

  brainRunning = brainHandlerIsBrainRunning();

  if (!clientSimHasTransport(cs) || doingTutorial) {
    return;
  }

  /* One keys/game/lobby half-step lives in the shared client-frontend tick
   * core (both desktop and web call it); the brain and per-second stat
   * rollover below stay here in the single-threaded driver. */
  if (clientFrontRunTickStep(cs)) {
    t2++;
    trackerTime++;
    ticks++;
    used = TRUE;
  }

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
    dwSysFrameTotal = dwSysFrame;
    dwSysGameTotal = dwSysGame;
    dwSysBrainTotal = dwSysBrain;
    dwSysBrain = 0;
    dwSysFrame = 0;
    dwSysGame = 0;
    t2 = 0;
  }
}

/* Tick the local server of a game whose transport does not tick it itself
 * (practice, connected passive). Called once per 10 ms client step, before
 * the step, and runs serverInstanceTick on every second call: once per
 * SERVER_TICK_LENGTH (20 ms), the rate desktop's host timer ticks its
 * single-player server at. The step that follows then pulls the fresh
 * snapshot. The tutorial's transport ticks the server inside its own tick,
 * and a network game has no local server, so both return here untouched. */
static void wasmTickLocalServer(ClientSim *cs) {
  ServerSim *srv = gameFrontGetServerSim();
  if (srv == NULL || clientSimTransportTicksServer(cs)) {
    return;
  }
  s_serverTickDue = !s_serverTickDue;
  if (s_serverTickDue) {
    serverInstanceTick(srv);
  }
}

/* -------------------------------------------------------
 * main_loop_iteration — one game frame, called by wasmRunGame
 * ------------------------------------------------------- */
void frontEndTutorialNotePresentedFrame(void);
static void tutorialRespawnPoll(void);

/* Defined further down with the other winbolo.h entry points. */
void windowLeaveGame(void);

/* Cloud-prefs bridge (prefs_bridge_wasm.c). */
void wbPrefsSyncNow(void);

static void main_loop_iteration(void) {
  DWORD tick;
  ClientSim *cs = humanSim;

  /* Process events */
  sdl3ImguiProcessEvents(cs);

  /* Detect a mid-game terminal disconnect (server shutdown / dropped /
   * unrecoverable error). The initial-connect failure path sets s_connFailed
   * directly from gameFrontWasmStart, so this only needs to catch failures that
   * arise while running. */
  if (!s_connFailed && cs != NULL && clientSimHasTransport(cs) &&
      gameFrontGetServerSim() == NULL) {  /* UDP only — not local single-player */
    ClientConnectState st = clientSimGetConnectState(cs);
    if (st == CLIENT_CONNECT_ERROR || st == CLIENT_CONNECT_SERVER_SHUTDOWN ||
        st == CLIENT_CONNECT_KICKED) {
      clientSimConnectionLost(cs);
      /* A kick sets no connectErrorReason; show the same message the
       * desktop lobby loop does (STR_DLGLOBBY_KICKED) instead of the
       * generic could-not-connect fallback. */
      if (st == CLIENT_CONNECT_KICKED) {
        wasmReportConnectFailure(langGetText(STR_DLGLOBBY_KICKED));
      } else {
        wasmReportConnectFailure(clientSimGetConnectErrorReason(cs));
      }
    }
  }

  /* On the first frame after a terminal failure, raise the error dialog. The
   * frozen state below renders this last frame without ticking or sending.
   *
   * Dismissing it leaves the game, and main ends it and shows the menu;
   * without this the player is left on the cleared frame with only the menu
   * bar over it. The latch stops this branch re-arming. */
  if (s_connFailed && !s_connErrorShown) {
    imguiMessageBoxEx(DIALOG_BOX_TITLE, s_connReason, IMGUI_MSG_ERROR,
                      IMGUI_MSG_OK);
    s_connErrorShown = TRUE;
    windowLeaveGame();
  }

  /* Game tick accumulation (replaces SDL_AddTimer).
   *
   * Sim ticks owed since the last rendered frame are derived from wall-clock
   * time.  Two kinds of gap need different handling:
   *
   *   - Ordinary jank (a JS GC pause, a couple of dropped frames): a gap of
   *     tens to a couple hundred ms.  Replay it, but spread the catch-up over
   *     a few frames (MAX_CATCHUP) so no single frame hitches.  This keeps a
   *     networked game's transport->tick() calls flowing through a brief
   *     stall instead of skipping packet send/recv.
   *
   *   - Background / suspend: when the tab is hidden the browser throttles or
   *     pauses requestAnimationFrame, so on return `gap` jumps to a second or
   *     more.  Replaying that backlog fast-forwards the game on return, and
   *     it buys nothing: past CLIENT_TIMEOUT_TICKS (~1000 ticks of no
   *     traffic) the server has already dropped us and the client has already
   *     declared SERVER_SHUTDOWN, so the owed ticks are dead either way.  In
   *     single-player there is simply nothing to catch up to.  Drop the debt
   *     and resume from real time.  A hidden tab's frames can also arrive
   *     under STALL_RESET_MS apart; the cap on the leftover after the
   *     catch-up below keeps those from building a backlog.
   *
   * A single-player game (practice or the tutorial, whose server runs in the
   * page) pauses while the page is hidden: it runs no ticks and owes none, so
   * it resumes where it stopped. A network game keeps ticking while hidden to
   * stay in step with its server. */
  if (!s_connFailed && clientSimHasTransport(cs)) {
    const double MAX_ELAPSED_MS  = 200.0;  /* per-frame catch-up bound (ordinary jank) */
    const double STALL_RESET_MS  = 500.0;  /* gap above this = background/suspend → drop */
    const int    MAX_CATCHUP     = 4;      /* at most 4 sim ticks per render frame */
    double now = emscripten_get_now();
    double gap = now - lastFrameTime;
    lastFrameTime = now;
    if (gameFrontGetServerSim() != NULL && wasmPageHidden()) {
      gameTickAccum = 0.0;
    } else if (gap > STALL_RESET_MS) {
      gameTickAccum = 0.0;
    } else {
      gameTickAccum += (gap > MAX_ELAPSED_MS) ? MAX_ELAPSED_MS : gap;
    }

    int ticksThisFrame = 0;
    while (gameTickAccum >= GAME_TICK_LENGTH && ticksThisFrame < MAX_CATCHUP) {
      gameTickAccum -= GAME_TICK_LENGTH;
      wasmTickLocalServer(cs);
      windowRunGameTick(cs);
      ticksThisFrame++;
    }
    /* The leftover drains in future frames, but no more than MAX_ELAPSED_MS
     * of it is kept, so frames that keep arriving too far apart for the
     * catch-up limit (a hidden tab's timer, a sustained slow frame rate) run
     * the game slower instead of banking time. */
    if (gameTickAccum > MAX_ELAPSED_MS) {
      gameTickAccum = MAX_ELAPSED_MS;
    }
  }

  /* Voice encode/decode runs here, beside the game tick and outside the
   * catch-up gate — once per rendered frame, whatever the sim owes. cs is
   * NULL until there is a connection, which the runtime expects. */
  voiceTick(cs);

  /* Render. Frozen still draws, so the error dialog lands over the last frame
   * instead of a blank screen; the lobby is the one case that clears instead
   * of rendering (see below). */
  tick = SDL_GetTicks();
  clientMutexWaitFor();
  if (finishedLoop == FALSE && !s_connFailed) {
    if (cs != NULL && clientSimIsInLobby(cs)) {
      /* In the lobby there is no game to show: clientRenderFrame would take
       * its netLobby branch and paint the whole download-screen chrome, which
       * then ghosts through the 97%-opaque ##LobbyBg the ImGui pass draws on
       * top (and shows outright in its rounded corners). The desktop lobby
       * runs its own blocking loop and clears to this same colour before
       * compositing; do the equivalent here. Nothing is lost by skipping the
       * render: the lobby tick step pumps the transport itself, and its
       * udpClientTick drains the socket. */
      SDL_Renderer *ren = sdl3DrawGetRenderer();
      if (ren) {
        SDL_SetRenderDrawColor(ren, 30, 30, 30, 255);
        SDL_RenderClear(ren);
      }
    } else {
      clientFrontRenderPrepare(cs, tick);
      clientRenderFrame(cs, redraw);
    }
  } else if (s_connFailed) {
    /* Frozen: don't render the (possibly never-connected) game; clear to black
     * so the error dialog draws over a clean background, not garbage. */
    SDL_Renderer *ren = sdl3DrawGetRenderer();
    if (ren) {
      SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
      SDL_RenderClear(ren);
    }
  }
  clientMutexRelease();
  /* The in-window map overview draws from the snapshot the frame above
     filled, now that the lock is off. */
  sdl3DrawFlushOverviewInWindow();
  dwSysFrame += (SDL_GetTicks() - tick);

  /* ImGui overlay + present */
  sdl3ImguiPumpAndRender(cs);
  {
    SDL_Renderer *ren = sdl3DrawGetRenderer();
    if (ren) SDL_RenderPresent(ren);
  }

  /* Tutorial: count presented frames (gates the intro overlay) and poll the
   * server's once-per-run respawn message. No-ops outside tutorial mode. */
  frontEndTutorialNotePresentedFrame();
  tutorialRespawnPoll();

  /* Cloud prefs upload runs from gameFrontPumpDirty, which
   * sdl3ImguiProcessEvents calls at the top of this frame. */
}

/* Wait for the browser's next animation frame. This paces the game loop at
 * the display's rate, one frame per requestAnimationFrame. A hidden
 * tab stops requestAnimationFrame, so a 250 ms timeout also resolves the wait
 * and the loop keeps advancing (the browser throttles it further). While a
 * network game runs in a hidden page, the hidden-page tick worker
 * (wasmSetFastHiddenFrames) also resolves it, every 20 ms. Whichever fires
 * first wins and the others are cancelled or ignored. Suspends main's stack
 * through ASYNCIFY, so only C on that stack may call it. */
EM_ASYNC_JS(void, wasmFrameWait, (void), {
    await new Promise((resolve) => {
        let done = false;
        let timer = 0;
        let frame = 0;
        const finish = () => {
            if (done) return;
            done = true;
            clearTimeout(timer);
            /* A hidden page holds animation-frame callbacks until it is
               shown again; cancel this one so a hidden network game's
               20 ms passes do not queue thousands of them. */
            cancelAnimationFrame(frame);
            resolve();
        };
        timer = setTimeout(finish, 250);
        frame = requestAnimationFrame(finish);
        const hiddenTick = Module.wbHiddenTick;
        if (hiddenTick && hiddenTick.running) {
            hiddenTick.waiter = finish;
        }
    });
});

/* Turn the hidden-page tick on or off for a network game. A network game
 * must keep running in a hidden page: voice sends and plays a frame every
 * 20 ms, and the client must stay in step with its server. A hidden page
 * gets no requestAnimationFrame and the browser slows its timers to a second
 * or more, but a dedicated worker's timers are not slowed, so a small worker
 * made from a blob: URL posts a message every 20 ms and wasmFrameWait
 * resolves on it. The worker runs only while Module.wbFastHiddenFrames is
 * set and the page is hidden; a visibilitychange listener starts and stops
 * it. Its message handler only resolves the pending frame wait and never
 * calls into the module. A single-player game pauses while hidden instead
 * (see main_loop_iteration), so it never sets the flag. Never suspends. */
EM_JS(void, wasmSetFastHiddenFrames, (int on), {
    let tick = Module.wbHiddenTick;
    if (!tick) {
        tick = Module.wbHiddenTick = { worker: null, running: false, waiter: null };
        tick.sync = () => {
            const want = !!Module.wbFastHiddenFrames && document.hidden;
            if (want === tick.running) return;
            if (want && !tick.worker) {
                try {
                    const src = "let id = 0; onmessage = (e) => { clearInterval(id); id = 0;" +
                                " if (e.data) id = setInterval(() => postMessage(0), 20); };";
                    const url = URL.createObjectURL(new Blob([src], { type: "text/javascript" }));
                    tick.worker = new Worker(url);
                    URL.revokeObjectURL(url);
                    tick.worker.onmessage = () => {
                        const waiter = tick.waiter;
                        tick.waiter = null;
                        if (waiter) waiter();
                    };
                } catch (e) {
                    /* No worker: the frame wait keeps its 250 ms timeout. */
                    tick.worker = null;
                    return;
                }
            }
            tick.running = want;
            if (!want) tick.waiter = null;
            tick.worker.postMessage(want ? 1 : 0);
        };
        document.addEventListener("visibilitychange", tick.sync);
    }
    Module.wbFastHiddenFrames = !!on;
    tick.sync();
});

/* Set up the page for a network game's loop (on) or put it back (off).
 * SDL reads SDL_HINT_EMSCRIPTEN_ASYNCIFY in both its present and SDL_Delay.
 * With the hint on, each present sleeps on a page timer, which a hidden page
 * slows to a second or more; every web loop already paces itself with
 * wasmFrameWait, so the game loop turns the present's sleep off. It is off
 * only while the loop runs: the join wait (clientFrontAwaitJoin) needs
 * SDL_Delay to keep yielding to the browser, and the menu, the finder and
 * single player keep SDL's default. */
static void wasmNetworkGameFrames(bool on) {
  SDL_SetHint(SDL_HINT_EMSCRIPTEN_ASYNCIFY, on ? "0" : "1");
  wasmSetFastHiddenFrames(on ? 1 : 0);
}

/* -------------------------------------------------------
 * Browser history for the menu and games
 *
 * shell.html's popstate handler never calls in here; it only records
 * Module.wbNavRequest, which the game loop reads. None of these suspend.
 * ------------------------------------------------------- */

/* Record the screen the page is on ("menu", "finder" or "game") for the
 * popstate handler. */
static void wasmSetScreen(const char *screen) {
  EM_ASM({ Module.wbScreen = UTF8ToString($0); }, screen);
}

/* Make the current history entry the menu, at the menu's address. */
static void wasmHistoryReplaceMenu(void) {
  EM_ASM({
    try { history.replaceState({screen: "menu"}, "", Module.wbMenuUrl()); }
    catch (e) {}
  });
}

/* Push a history entry for a game picked from the menu. Its address is the
 * one that launches the same game directly, so a reload restarts it. */
static void wasmHistoryPushGame(WasmGameMode mode) {
  EM_ASM({
    var query = $0 ? "?tutorial=1" : "?practise=1";
    try { history.pushState({screen: "game"}, "", Module.wbMenuUrl() + query); }
    catch (e) {}
  }, mode == WASM_GAME_TUTORIAL ? 1 : 0);
}

/* Mark the current entry as a game, for a game the page launched straight
 * into. The address is left as it is. */
static void wasmHistoryMarkGame(void) {
  EM_ASM({
    try { history.replaceState({screen: "game"}, "", location.href); }
    catch (e) {}
  });
}

/* Push a history entry for the game finder opened from the menu, at the
 * address that opens the finder directly, so a reload opens it again. */
static void wasmHistoryPushFinder(void) {
  EM_ASM({
    try { history.pushState({screen: "finder"}, "", Module.wbMenuUrl() + "?finder=1"); }
    catch (e) {}
  });
}

/* Mark the current entry as the game finder, for a ?finder=1 launch. The
 * address, ?finder=1 with it, is left as it is. */
static void wasmHistoryMarkFinder(void) {
  EM_ASM({
    try { history.replaceState({screen: "finder"}, "", location.href); }
    catch (e) {}
  });
}

/* Make the finder's entry the entry of the game joined from it, at that
 * game's /join/ address, so a reload rejoins it and Back from the game goes
 * to whatever was behind the finder. */
static void wasmHistoryReplaceJoin(const char *gameKey) {
  EM_ASM({
    var url = "/join/" + encodeURIComponent(UTF8ToString($0));
    try { history.replaceState({screen: "game"}, "", url); }
    catch (e) {}
  }, gameKey);
}

/* Step back to the menu's entry. The popstate this fires arrives once main
 * next waits on a frame, by which time the screen is already the menu, so
 * the handler ignores it. */
static void wasmHistoryBack(void) {
  EM_ASM({ history.back(); });
}

/* Take a Back or Forward request the popstate handler recorded: TRUE, and
 * cleared, when one is waiting. */
static bool wasmTakeNavRequest(void) {
  int pending = EM_ASM_INT({
    if (!Module.wbNavRequest) return 0;
    Module.wbNavRequest = "";
    return 1;
  });
  return pending != 0;
}

/* Called by the game finder once a frame: TRUE, once, when Back or Forward
 * has moved off its history entry, and the finder then closes as Cancel
 * does. Records that the browser is already on the menu's entry, so
 * wasmShowFinder leaves the history alone. */
bool wasmFinderTakeBack(void) {
  if (!wasmTakeNavRequest()) {
    return FALSE;
  }
  s_finderLeftByHistory = TRUE;
  return TRUE;
}

/* The server key of the game the finder's Join picked; empty when the
 * finder closed any other way. */
static char s_finderJoinKey[128] = "";

/* Called by the game finder's Join, which then closes the finder: record
 * the game to join. wasmShowFinder hands it to main, which starts it in this
 * page. */
void wasmFinderJoin(const char *serverKey) {
  SDL_strlcpy(s_finderJoinKey, serverKey ? serverKey : "",
              sizeof(s_finderJoinKey));
}

/* Run the current game until it ends: a game over or quit (finishedLoop) or
 * a leave request (windowLeaveGame). Back or Forward off the game's history
 * entry leaves it the same way, network or single player. */
static void wasmRunGame(void) {
  while (!finishedLoop && !s_leaveRequested) {
    main_loop_iteration();
    if (wasmTakeNavRequest()) {
      s_leftByHistory = TRUE;
      windowLeaveGame();
    }
    wasmFrameWait();
  }
}

/* -------------------------------------------------------
 * main — Emscripten entry point
 * ------------------------------------------------------- */
/* Read a URL query parameter. Returns "" if not found. */
static const char *getUrlParam(const char *name) {
  static char buf[512];
  char js[640];
  snprintf(js, sizeof(js),
    "(function(){ var p = new URLSearchParams(window.location.search).get('%s');"
    " return p ? p : ''; })()", name);
  const char *result = emscripten_run_script_string(js);
  if (result) {
    strncpy(buf, result, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return buf;
  }
  return "";
}

/* Defined with windowSetQuitting, beside the quit flags it resets. */
static void wasmGameStateReset(void);

/* Most bot runners the page asks for, the page's thread included: one per
 * tank, the cap botManagerInit also applies. The link's PTHREAD_POOL_SIZE
 * expression (CMakeLists.txt) sizes the workers by the same rule, one fewer
 * than the runners, so a change here must be made there too. The link also
 * sets PTHREAD_POOL_SIZE_STRICT=2: a thread the started workers cannot take
 * is refused rather than made later, so a mismatch fails the pool create
 * below, which logs and leaves thinks on the page's thread, instead of the
 * first bot tick waiting for a worker the browser cannot start until the
 * page's thread yields. */
#define WASM_MAX_BOT_RUNNERS MAX_TANKS

/* Create the bot worker pool, before any sim is made: each sim sizes its
 * bot runners from the pool when it is created. The runners are one fewer
 * than the logical cores, leaving a core for the browser, at most
 * WASM_MAX_BOT_RUNNERS and at least 1; 1 runs every think on the page's
 * thread. */
static void wasmStartBotPool(void) {
  int cores = SDL_GetNumLogicalCPUCores();
  int runners = cores - 1;

  if (runners > WASM_MAX_BOT_RUNNERS) {
    runners = WASM_MAX_BOT_RUNNERS;
  }
  if (runners < 1) {
    runners = 1;
  }
  if (serverSimBotPoolInit(runners)) {
    /* The page's thread is one of the runners; the pool holds the rest. */
    printf("[WASM] bot pool: %d logical cores, %d runners, %d workers\n",
           cores, runners, runners - 1);
  } else {
    printf("[WASM] bot pool: create failed for %d runners (%d logical cores); "
           "thinks run on the page's thread\n", runners, cores);
  }
}

/* Copy one URL parameter into dst. getUrlParam returns a shared static
 * buffer, so each value is copied out before the next read. */
static void wasmCopyUrlParam(const char *name, char *dst, size_t dstSize) {
  strncpy(dst, getUrlParam(name), dstSize - 1);
  dst[dstSize - 1] = '\0';
}

/* Read how the page was launched. This is the only place the URL is read;
 * everything after page start takes its mode from the WasmLaunch. A game_key
 * (/join/<key>) or proxyURL joins a game and never shows the menu; otherwise
 * tutorial (/tutorial) goes straight into the tutorial and practise
 * (/practise) straight into single player. A page with none of these (/)
 * opens on the menu. finder (/?finder=1) sets *openFinder and showMenu: the
 * game finder opens first, and the menu once it closes. */
static void wasmReadLaunch(WasmLaunch *out, bool *openFinder) {
  char tutorial[8];
  char practise[8];
  char finder[8];

  memset(out, 0, sizeof(*out));
  wasmCopyUrlParam("game_key", out->gameKey, sizeof(out->gameKey));
  wasmCopyUrlParam("proxyURL", out->devProxy, sizeof(out->devProxy));
  wasmCopyUrlParam("password", out->password, sizeof(out->password));
  wasmCopyUrlParam("name", out->name, sizeof(out->name));
  wasmCopyUrlParam("tutorial", tutorial, sizeof(tutorial));
  wasmCopyUrlParam("practise", practise, sizeof(practise));
  wasmCopyUrlParam("finder", finder, sizeof(finder));

  if (out->gameKey[0] != '\0' || out->devProxy[0] != '\0') {
    out->mode = WASM_GAME_JOIN;
  } else if (tutorial[0] != '\0') {
    out->mode = WASM_GAME_TUTORIAL;
  } else {
    out->mode = WASM_GAME_PRACTICE;
  }
  out->showMenu = (out->mode != WASM_GAME_JOIN &&
                   (finder[0] != '\0' ||
                    (out->mode == WASM_GAME_PRACTICE && practise[0] == '\0')));
  *openFinder = (out->showMenu && finder[0] != '\0');
}

/* Start the page's relay latency test without waiting (finder_wasm.c). */
void wasmRelayProbeStart(void);

/* Show the game finder over the menu's background, as the desktop's Internet
 * row does, and return when it closes. Returns TRUE when it closed on a Join,
 * with the game's server key in s_finderJoinKey, and main starts that game;
 * FALSE when it closed by Cancel or by Back or Forward, and the menu follows.
 * Its Sign in to join loads another page.
 *
 * History: opened from the menu, the finder pushes its own entry; on a
 * ?finder=1 launch it marks the launch entry. Closed by a Join, that entry
 * becomes the game's (wasmHistoryReplaceJoin), with the menu's entry behind
 * it when the finder was pushed, and main sets the screen to the game.
 * Closed by Back or Forward, the browser is already on the menu's entry, and
 * a Join made in the same frame is dropped. Closed by Cancel, a pushed entry
 * is stepped back off, and a launch entry becomes the menu at the menu's
 * address. Nothing between here and the next frame wait suspends, so the
 * screen has changed by the time history.back()'s popstate arrives, and the
 * handler ignores it. */
static bool wasmShowFinder(bool launched) {
  bool joined;

  /* A Join from the finder then finds the closest relay already picked. */
  wasmRelayProbeStart();
  s_finderLeftByHistory = FALSE;
  s_finderJoinKey[0] = '\0';
  if (launched) {
    wasmHistoryMarkFinder();
  } else {
    wasmHistoryPushFinder();
  }
  wasmSetScreen("finder");

  imguiGameBrowserShow(langGetText(STR_GAMEFRONT_TRACKERFINDER_TITLE), TRUE);

  /* The finder's last frame waits on the browser after it decides to close,
   * so Back or Forward can still arrive then; take it here. */
  wasmFinderTakeBack();
  joined = (s_finderJoinKey[0] != '\0' && !s_finderLeftByHistory);

  if (joined) {
    wasmHistoryReplaceJoin(s_finderJoinKey);
  } else {
    if (s_finderLeftByHistory) {
      /* The browser is already on the menu's entry. */
    } else if (launched) {
      wasmHistoryReplaceMenu();
    } else {
      wasmHistoryBack();
    }
    wasmSetScreen("menu");
  }
  s_finderLeftByHistory = FALSE;
  return joined;
}

/* Set next up to join the game the finder's Join picked: a network game
 * with the finder's server key and no dev proxy or password. */
static void wasmUseFinderJoin(WasmLaunch *next) {
  next->mode = WASM_GAME_JOIN;
  SDL_strlcpy(next->gameKey, s_finderJoinKey, sizeof(next->gameKey));
  next->devProxy[0] = '\0';
  next->password[0] = '\0';
  next->inPage = TRUE;
}

/* End a game, network or single player, so the menu, and then another game,
 * can follow in the same page. The first three run in the order the
 * desktop's game end runs them (winbolo.c): the voice talkers go with their
 * game, then the game's ImGui context, then the sims, and with the client
 * sim a network game's transport and its socket. The lobby and tutorial
 * overlay state is per-game too and is dropped last. */
static void wasmEndGame(void) {
  voiceReset();
  sdl3ImguiCleanup();
  /* A start that failed has already freed its sims (humanSim is NULL). */
  if (humanSim != NULL) {
    gameFrontEnd(&keys, TRUE, FALSE);
  }
  imguiLobbyFrameReset();
  tutorialOverlayReset();

  /* The in-game menu's toggles change only the live settings, as on the
   * desktop, which writes them when its game ends; do the same here, after
   * gameFrontEnd has read the tank options back. The menu's frames upload
   * the change. */
  gameFrontPutPrefs(&keys);
}

/* Run one game from start to end: reset the per-game state, start the game
 * the launch describes, set up its UI and run the loop until it ends.
 * Returns TRUE for a network game (no local server sim), whose connection
 * the caller closes before it ends the game; FALSE for single player,
 * including a single-player start that failed, which has shown its error and
 * freed what it made. */
static bool wasmPlayGame(const char *cmdLine, const WasmLaunch *launch) {
  wasmGameStateReset();

  /* Free the menu's background game before this one starts. Each of its
   * bots holds tens of MB of brain, and the page's heap never shrinks, so
   * keeping them through the game would leave that much less for it.
   * main makes a new one when the menu shows again. No-op on a page that
   * went straight into a game. */
  wasmBackgroundGameDestroy();

  printf("[WASM] Starting gameFrontWasmStart...\n");
  bool started = (gameFrontWasmStart(cmdLine, &keys, launch) != FALSE);
  if (!started && !s_connFailed) {
    /* A single-player start that failed (the server sim or the local join
     * would not come up). The start freed humanSim without clearing it, so
     * clear it and the player panel's owner here, show the error the
     * desktop shows for the same failure, and go back to the menu. */
    printf("[WASM] gameFrontWasmStart FAILED\n");
    humanSim = NULL;
    frontEndSetActiveClientSim(NULL);
    imguiMessageBoxEx(DIALOG_BOX_TITLE,
                      langGetText(STR_GAMEFRONTERR_STARTSERVER),
                      IMGUI_MSG_ERROR, IMGUI_MSG_OK);
    return FALSE;
  }
  /* From here either we connected, or the connection failed but
   * gameFrontWasmStart kept humanSim alive in its error state — we still set
   * up ImGui and enter the loop so the error dialog can draw (never a blank
   * screen). */
  fprintf(stderr, "[WASM] gameFrontWasmStart %s; humanSim=%p\n",
          started ? "OK" : "CONNECT FAILED", (void*)humanSim);
  fflush(stderr);

  if (started) {
    /* Start the shared tick cadence from a known state (first step is a game
     * step on tick 0), mirroring the desktop run-start. */
    clientFrontTickReset();
  }

  isInMenu = FALSE;
  finishedLoop = FALSE;
  windowApplyMenuChecks(humanSim);

  if (soundEffects == TRUE) {
    soundKeepalive(useSoundKeepalive);
  }

  /* Set up ImGui */
  {
    SDL_Window *win = sdl3DrawGetWindow();
    SDL_Renderer *ren = sdl3DrawGetRenderer();
    if (win && ren) {
      sdl3ImguiSetup(win, ren);
    }
    if (win) {
      SDL_ShowWindow(win);
    }
  }

  guiMessageSetHandler(sdl3MessageHandler);

  /* Set again here, not only in wasmGameStateReset: a join can wait up to
   * 30s inside gameFrontWasmStart, and the first frame should not count
   * that as owed time. */
  oldTick = SDL_GetTicks();
  lastFrameTime = emscripten_get_now();

  fprintf(stderr, "[WASM] Starting main loop; humanSim=%p\n", (void*)humanSim);
  fflush(stderr);
  {
    bool network = (gameFrontGetServerSim() == NULL);
    if (network) {
      wasmNetworkGameFrames(TRUE);
    }
    wasmRunGame();
    if (network) {
      wasmNetworkGameFrames(FALSE);
    }
  }

  return gameFrontGetServerSim() == NULL;
}

/* Choose the name a single-player game plays under: a validated ?name= from
 * the page's launch wins; otherwise "Me". Run before every single-player
 * game, because a join replaces the name with its own network name (the
 * account name or web<rand>) inside gameFrontWasmStart. */
static void wasmChooseSinglePlayerName(const WasmLaunch *launch) {
  char validated[PLAYER_NAME_LEN];
  if (launch->name[0] != '\0' &&
      playerNameValidate(launch->name, validated, PLAYER_NAME_LEN, NULL)) {
    gameFrontSetPlayerName(validated);
    printf("[WASM] single player: name=%s (from URL)\n", validated);
  } else {
    gameFrontSetPlayerName((char *)"Me");
    printf("[WASM] single player: default name=Me\n");
  }
}

int main(int argc, char *argv[]) {
  const char *cmdLine = "";
  WasmLaunch launch;
  bool openFinder = FALSE;

  (void)argc;
  (void)argv;

  bolo_srand((uint64_t)time(NULL) ^ (uint64_t)getpid());

  if (argc > 1) {
    cmdLine = argv[1];
  }

  SDL_Init(0);

  /* Force the desktop UI (with the chrome background) for the browser
   * build. uiModeDetect would otherwise flip to the touch layout —
   * which omits the background bitmap — whenever a touch device is
   * present and the canvas is under 1200px wide, as desktop browsers
   * commonly report. */
  uiModeSet(UI_MODE_DESKTOP);

  if (clientMutexCreate() == FALSE) {
    printf("[WASM] Failed to create client mutex\n");
    return 1;
  }

  /* Initialise the in-memory preferences document. The MEMFS path is a scratch
   * backing only — persistence is the WinBolo.net API (prefs_bridge_wasm.c),
   * not the browser filesystem. */
  prefsInit("/WinBolo.json");

  /* Voice runs for the life of the process. It comes up before
   * gameFrontWasmStart, as on desktop. This platform has no capture or
   * playback device yet (voice_wasm.c), so this only brings the codec up —
   * every device-facing entry point declines. */
  voiceInit();

  /* The only read of the page URL; the game mode comes from here on. */
  wasmReadLaunch(&launch, &openFinder);

  wasmStartBotPool();

  /* Page-lifetime setup: default keys, language, window, sound, brains. */
  if (gameFrontWasmSetup(&keys) == FALSE) {
    printf("[WASM] gameFrontWasmSetup FAILED\n");
    clientMutexDestroy();
    SDL_Quit();
    return 1;
  }

  /* Pull the account's cloud prefs and apply them over the defaults the
   * setup seeded — keys, menu toggles, game options, gamepad sensitivities,
   * build options and voice — before the first game starts. No game start
   * seeds them again. There is no humanSim yet, so wasmApplyJoinPrefs only
   * sets the globals; the game start and windowApplyMenuChecks below push
   * them into the new game. No-op when not signed in. */
  wbPrefsSyncNow();

  /* Screens: the menu, then a game, then the menu again. A launch that names
   * a game, a join included, goes straight into it, and its end shows the
   * menu. A game picked from the menu carries no join key, dev proxy or
   * password; a game joined from the finder carries only its server key.
   *
   * History: the menu's entry is the page's first. A game picked from the
   * menu pushes its own entry, so Back returns to the menu; a game the page
   * launched straight into marks its entry as the game, with no menu behind
   * it, and that entry becomes the menu's when the game ends. A ?finder=1
   * launch marks its entry as the finder's (wasmShowFinder), which becomes
   * the menu's once the finder closes. A Join in the finder turns the
   * finder's entry into the game's: from the menu's finder the menu's entry
   * is behind it, from a ?finder=1 launch nothing is. */
  WasmLaunch next = launch;
  bool menu = launch.showMenu;
  if (openFinder) {
    /* wasmShowFinder marks the entry, keeping its address. */
  } else if (launch.showMenu) {
    wasmHistoryReplaceMenu();
    wasmSetScreen("menu");
  } else {
    wasmHistoryMarkGame();
    s_gameEntryPushed = FALSE;
  }
  for (;;) {
    if (menu) {
      /* The menu and the finder draw the background game; a page that goes
       * straight into a game never makes it. */
      if (bgGameGetShared() == NULL) {
        wasmBackgroundGameCreate();
      }
      /* A ?finder=1 launch opens the finder before the menu's first
       * showing, once. A Join there plays the game, whose entry has no
       * menu behind it; anything else shows the menu. */
      if (openFinder) {
        openFinder = FALSE;
        if (!wasmShowFinder(TRUE)) {
          continue;
        }
        wasmUseFinderJoin(&next);
        s_gameEntryPushed = FALSE;
      } else {
        /* The welcome dialog returns an openingStates value (gamefront.h). */
        int r = imguiWelcomeShow();
        if (r == openSetup || r == openTutorial) {
          next.mode = (r == openSetup) ? WASM_GAME_PRACTICE
                                       : WASM_GAME_TUTORIAL;
          next.gameKey[0] = '\0';
          next.devProxy[0] = '\0';
          next.password[0] = '\0';
          next.inPage = FALSE;
          wasmHistoryPushGame(next.mode);
          s_gameEntryPushed = TRUE;
        } else if (r == openInternet) {
          /* A Join in the finder plays the game, whose entry has the
           * menu's behind it; closed any other way, the menu shows. */
          if (!wasmShowFinder(FALSE)) {
            continue;
          }
          wasmUseFinderJoin(&next);
          s_gameEntryPushed = TRUE;
        } else {
          if (r == openSettings) {
            imguiSettingsShow();
          }
          /* Settings closed, or a row with nothing behind it on the web
           * (Local, Quit): show the menu again. */
          continue;
        }
      }
    }

    /* From here Back or Forward asks the game to end. Set before the start,
     * so a request made while it comes up (a join waits on its join code and
     * the server's answer), or while a failed start shows its error, is
     * still taken below. */
    wasmSetScreen("game");
    if (next.mode != WASM_GAME_JOIN) {
      wasmChooseSinglePlayerName(&next);
    }

    if (wasmPlayGame(cmdLine, &next)) {
      /* A network game has ended. One that still has its connection sends
       * the server a graceful quit (PACKET_QUIT, written to the socket
       * before it closes); the lobby's Leave has already disconnected, so
       * the transport check stops a second disconnect. */
      if (humanSim != NULL && clientSimHasTransport(humanSim)) {
        clientSimDisconnect(humanSim);
      }
    }

    /* End the game and go back to the menu, keeping every setting the
     * player changed. The menu's frames upload a change to them. */
    wasmEndGame();

    /* Put the history on the menu's entry. A request the game loop never
     * took (the start failed or the game ended in the same frame) still
     * means the browser has moved off the game. Nothing between here and
     * the menu's first frame wait suspends, so the screen is already the
     * menu when history.back()'s popstate arrives, and the handler ignores
     * it. */
    if (wasmTakeNavRequest()) {
      s_leftByHistory = TRUE;
    }
    if (s_leftByHistory) {
      /* The browser is already on the menu's entry. */
    } else if (s_gameEntryPushed) {
      wasmHistoryBack();
    } else {
      /* Launched straight into the game: that entry becomes the menu. */
      wasmHistoryReplaceMenu();
    }
    wasmSetScreen("menu");
    s_gameEntryPushed = FALSE;
    s_leftByHistory = FALSE;
    menu = TRUE;
  }
}

/* -------------------------------------------------------
 * winbolo.h function implementations
 * ------------------------------------------------------- */

void windowReCreate(void)  { }
/* Raised only by windowSetQuitting, exactly as on the desktop: winboloQuit
 * cannot answer the question on its own, because the game loop sets it TRUE
 * on the way in as its default answer.  See windowIsQuitting in
 * src/gui/sdl3/winbolo.c. */
static bool quitRequested = FALSE;
void windowSetQuitting(void) { quitRequested = TRUE; winboloQuit = TRUE; finishedLoop = TRUE; }
bool windowIsQuitting(void)   { return quitRequested; }

/* Runs before each game: put back the per-game state the last game may have
 * left behind — the tutorial pause, the connection-failure latch, the loop
 * and quit flags, the tick accumulator and clocks, and the timing totals.
 * The tutorial step sequencer is reset by gameFrontWasmStart
 * (frontEndTutorialReset). */
static void wasmGameStateReset(void) {
  doingTutorial = FALSE;
  s_connFailed = FALSE;
  s_connErrorShown = FALSE;
  s_connReason[0] = '\0';
  finishedLoop = FALSE;
  winboloQuit = FALSE;
  quitRequested = FALSE;
  s_leaveRequested = FALSE;
  isInMenu = FALSE;
  gameTickAccum = 0.0;
  s_serverTickDue = FALSE;
  lastFrameTime = emscripten_get_now();
  oldTick = SDL_GetTicks();
  dwSysFrameTotal = 0;
  dwSysFrame = 0;
  dwSysGameTotal = 0;
  dwSysGame = 0;
  dwSysBrainTotal = 0;
  dwSysBrain = 0;
}

/* Leave the game: ask wasmRunGame to end it after the current frame. main
 * then disconnects a network game that is still connected, ends the game
 * and shows the menu. */
void windowLeaveGame(void) { s_leaveRequested = TRUE; }

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
  clientSimSetAllowNewPlayers(cs, allowNewPlayers);
}

/* -------------------------------------------------------
 * C5: apply WinBolo.net synced prefs from the join metadata frame.
 *
 * Applies a projected prefs subset (KEYS / MENU / GAME OPTIONS / SETTINGS)
 * from JSON. Two callers feed it: the relay's join-prefs 0x01 frame
 * (udpClientParseProxyMeta, emscripten only) and the WinBolo.net cloud-prefs
 * GET (prefs_bridge_wasm.c, on adopt). Values mirror the
 * desktop INI store: "Yes"/"No" bools, stringified ints, SDL3 keycodes for
 * KEYS. Native JSON bool/number is tolerated too. Absent keys keep the
 * current (default) value. This runs after gameFrontWasmSetup seeded defaults,
 * so it overrides exactly what the user has synced; no game start seeds them
 * again.
 * ------------------------------------------------------- */
extern bool useAutoslow;   /* defined in gamefront_wasm.c */
extern bool useAutohide;
void windowSetSoundVolume(int pct);   /* defined below, after this function */
void windowSetMasterVolume(int pct);  /* likewise */
/* Likewise the voice settings, defined below with the rest of them. */
void windowSetVoiceEnabled(bool on);
void windowSetVoiceMode(int mode);
void windowSetVoiceMicGain(float gain);
void windowSetVoiceVolume(float gain);
void windowSetShowTankMicIcons(bool on);
bool windowGetShowTankMicIcons(void);

static bool prefBool(cJSON *o, const char *k, bool dflt) {
  cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
  if (it == NULL) return dflt;
  if (cJSON_IsBool(it))   return cJSON_IsTrue(it);
  if (cJSON_IsNumber(it)) return it->valuedouble != 0;
  if (cJSON_IsString(it) && it->valuestring)
    return (SDL_strcasecmp(it->valuestring, "Yes")  == 0 ||
            SDL_strcasecmp(it->valuestring, "true") == 0 ||
            strcmp(it->valuestring, "1") == 0);
  return dflt;
}

static int prefInt(cJSON *o, const char *k, int dflt) {
  cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
  if (it == NULL) return dflt;
  if (cJSON_IsNumber(it)) return (int)it->valuedouble;
  if (cJSON_IsString(it) && it->valuestring) return atoi(it->valuestring);
  return dflt;
}

static float prefFloat(cJSON *o, const char *k, float dflt) {
  cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
  if (it == NULL) return dflt;
  if (cJSON_IsNumber(it)) return (float)it->valuedouble;
  if (cJSON_IsString(it) && it->valuestring) return (float)atof(it->valuestring);
  return dflt;
}

static const char *prefStr(cJSON *o, const char *k, const char *dflt) {
  cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
  if (it == NULL) return dflt;
  if (cJSON_IsString(it) && it->valuestring) return it->valuestring;
  return dflt;
}

void wasmApplyJoinPrefs(const char *prefsJson, int len) {
  if (prefsJson == NULL || len <= 0) return;
  cJSON *root = cJSON_ParseWithLength(prefsJson, (size_t)len);
  if (root == NULL) { printf("[WASM] join prefs: parse failed\n"); return; }

  cJSON *k = cJSON_GetObjectItemCaseSensitive(root, "KEYS");
  if (cJSON_IsObject(k)) {
    keys.kiForward      = prefInt(k, "Forward",        keys.kiForward);
    keys.kiBackward     = prefInt(k, "Backwards",      keys.kiBackward);
    keys.kiLeft         = prefInt(k, "Left",           keys.kiLeft);
    keys.kiRight        = prefInt(k, "Right",          keys.kiRight);
    keys.kiShoot        = prefInt(k, "Shoot",          keys.kiShoot);
    keys.kiLayMine      = prefInt(k, "Lay Mine",       keys.kiLayMine);
    keys.kiGunIncrease  = prefInt(k, "Increase Range", keys.kiGunIncrease);
    keys.kiGunDecrease  = prefInt(k, "Decrease Range", keys.kiGunDecrease);
    keys.kiTankView     = prefInt(k, "Tank View",      keys.kiTankView);
    keys.kiPillView     = prefInt(k, "Pill View",      keys.kiPillView);
    keys.kiAllyView     = prefInt(k, "Ally View",      keys.kiAllyView);
    keys.kiBaseView     = prefInt(k, "Base View",      keys.kiBaseView);
    keys.kiOverviewZoom = prefInt(k, "Overview Zoom",  keys.kiOverviewZoom);
    keys.kiOverviewFollow  = prefInt(k, "Overview Follow",   keys.kiOverviewFollow);
    keys.kiOverviewZoomIn  = prefInt(k, "Overview Zoom In",  keys.kiOverviewZoomIn);
    keys.kiOverviewZoomOut = prefInt(k, "Overview Zoom Out", keys.kiOverviewZoomOut);
    keys.kiScrollUp     = prefInt(k, "Scroll Up",      keys.kiScrollUp);
    keys.kiScrollDown   = prefInt(k, "Scroll Down",    keys.kiScrollDown);
    keys.kiScrollLeft   = prefInt(k, "Scroll Left",    keys.kiScrollLeft);
    keys.kiScrollRight  = prefInt(k, "Scroll Right",   keys.kiScrollRight);
    keys.kiQuickTree    = prefInt(k, "Quick Tree",     keys.kiQuickTree);
    keys.kiQuickRoad    = prefInt(k, "Quick Road",     keys.kiQuickRoad);
    keys.kiQuickWall    = prefInt(k, "Quick Wall",     keys.kiQuickWall);
    keys.kiQuickPillbox = prefInt(k, "Quick Pillbox",  keys.kiQuickPillbox);
    keys.kiQuickMine    = prefInt(k, "Quick Mine",     keys.kiQuickMine);
    keys.kiPing[0]      = prefInt(k, "Ping 1",         keys.kiPing[0]);
    keys.kiPing[1]      = prefInt(k, "Ping 2",         keys.kiPing[1]);
    keys.kiPing[2]      = prefInt(k, "Ping 3",         keys.kiPing[2]);
    {
      int pi;
      for (pi = 0; pi < PING_BIND_DIRECT_SLOTS; pi++) {
        char name[32];
        snprintf(name, sizeof(name), "Ping Direct %d", pi + 1);
        keys.kiPingDirect[pi] = prefInt(k, name, keys.kiPingDirect[pi]);
      }
    }
  }

  cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "MENU");
  if (cJSON_IsObject(m)) {
    showGunsight              = prefBool(m, "Show Gunsight",                 showGunsight);
    soundEffects              = prefBool(m, "Sound Effects",                 soundEffects);
    showNewswireMessages      = prefBool(m, "Show Newswire Messages",        showNewswireMessages);
    showAssistantMessages     = prefBool(m, "Show Assistant Messages",       showAssistantMessages);
    showAIMessages            = prefBool(m, "Show AI Messages",              showAIMessages);
    showNetworkStatusMessages = prefBool(m, "Show Network Status Messages",  showNetworkStatusMessages);
    showNetworkDebugMessages  = prefBool(m, "Show Network Debug Messages",   showNetworkDebugMessages);
    autoScrollingEnabled      = prefBool(m, "Autoscroll Enabled",            autoScrollingEnabled);
    showPillLabels            = prefBool(m, "Show Pill Labels",              showPillLabels);
    showBaseLabels            = prefBool(m, "Show Base Labels",              showBaseLabels);
    labelSelf                 = prefBool(m, "Label Own Tank",                labelSelf);
    labelMsg  = (labelLen)prefInt(m, "Message Label Size", (int)labelMsg);
    labelTank = (labelLen)prefInt(m, "Tank Label Size",    (int)labelTank);
    int vol = prefInt(m, "Sound Volume", soundVolume);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    windowSetSoundVolume(vol);
    vol = prefInt(m, "Master Volume", windowMasterVolume);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    windowSetMasterVolume(vol);
  }

  cJSON *g = cJSON_GetObjectItemCaseSensitive(root, "GAME OPTIONS");
  if (cJSON_IsObject(g)) {
    useAutoslow = prefBool(g, "Auto Slowdown",           useAutoslow);
    useAutohide = prefBool(g, "Auto Show-Hide Gunsight",  useAutohide);
  }

  /* SETTINGS: gamepad sensitivities (clamped to the same ranges as desktop
   * gamefront.c) and build-cursor options. The globals live in input_gamepad.c
   * and build_cursor.c (both compiled into the wasm build) and are read by the
   * shared input code each tick. */
  cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "SETTINGS");
  if (cJSON_IsObject(s)) {
    float gs = prefFloat(s, "Gamepad Scroll Sens", g_gamepadScrollSensitivity);
    if (gs >= 0.25f && gs <= 4.0f) g_gamepadScrollSensitivity = gs;
    float ts = prefFloat(s, "Gamepad Tank Sens", g_gamepadTankSensitivity);
    if (ts >= 0.10f && ts <= 1.0f) g_gamepadTankSensitivity = ts;
    float bs = prefFloat(s, "Gamepad Build Cursor Sens", g_gamepadBuildCursorSensitivity);
    if (bs >= 0.25f && bs <= 2.0f) g_gamepadBuildCursorSensitivity = bs;

    g_buildExitExecutes = prefBool(s, "Build Exit Executes", g_buildExitExecutes);
    g_buildExitExecutesMomentaryOnly =
        prefBool(s, "Build Exit Executes Momentary Only", g_buildExitExecutesMomentaryOnly);
    g_buildDoubleTapRoad = prefBool(s, "Build Double Tap Road", g_buildDoubleTapRoad);
    g_buildHoldMomentary = prefBool(s, "Build Hold Momentary", g_buildHoldMomentary);
    g_buildAutoCloseOnExecute = prefBool(s, "Build Auto Close On Execute", g_buildAutoCloseOnExecute);
  }

  /* VOICE: written by the desktop, adopted here, so a player's voice
   * settings follow them into the browser. Applied straight onto the
   * running voice module. The mode names and the clamps are the desktop
   * reader's — keep them in step with gamefront.c. "Echo Cancel" is not
   * read: it is desktop-only, the browser gets cancellation from
   * getUserMedia. */
  cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "VOICE");
  if (cJSON_IsObject(v)) {
    windowSetVoiceEnabled(prefBool(v, "Enabled", voiceIsEnabled()));

    const char *mode = prefStr(v, "Mode", NULL);
    if (mode != NULL) {
      if (strcmp(mode, "Off") == 0) {
        windowSetVoiceMode(VOICE_MODE_OFF);
      } else if (strcmp(mode, "Open Mic") == 0) {
        windowSetVoiceMode(VOICE_MODE_OPEN);
      } else {
        windowSetVoiceMode(VOICE_MODE_PTT);
      }
    }

    /* An out-of-range value is rejected back to 1.0 rather than clamped to
     * the edge, so this reader and the desktop's agree on it. */
    float mg = prefFloat(v, "Mic Gain", voiceGetMicGain());
    if (!(mg >= 0.0f && mg <= 4.0f)) mg = 1.0f;
    windowSetVoiceMicGain(mg);
    float vv = prefFloat(v, "Voice Volume", voiceGetOutputVolume());
    if (!(vv >= 0.0f && vv <= 2.0f)) vv = 1.0f;
    windowSetVoiceVolume(vv);

    windowSetShowTankMicIcons(prefBool(v, "Tank Icons", windowGetShowTankMicIcons()));
  }

  cJSON_Delete(root);

  /* Push the toggles into the live client/sim. Keys are read from the global
   * on the next input tick; pill/base label globals on the next status draw. */
  if (humanSim) {
    windowApplyMenuChecks(humanSim);
    clientSimSetTankAutoSlowdown(humanSim, useAutoslow);
    clientSimSetTankAutoHideGunsight(humanSim, useAutohide);
  }
  printf("[WASM] applied join prefs\n");
}

int windowGetDrawTime(void) { return (int)dwSysFrameTotal; }
int windowGetNetTime(void)  { return netGetNetTime(); }
int windowGetAiTime(void)   { return (int)dwSysBrainTotal; }
int windowGetSimTime(void)  { return (int)dwSysGameTotal; }

void windowGetKeys(keyItems *value) { *value = keys; }
void windowSetKeys(keyItems *value) { keys = *value; }

void windowSetZoomFactor(BYTE amount)  { zoomFactor = amount; }
BYTE windowGetZoomFactor(void)         { return zoomFactor; }

/* The page sizes the canvas and the game renders at the integer zoom that
 * covers it (Custom mode, adapted every frame), so there is no zoom to change
 * to. Nothing calls this on the web: the Window Size menu and the Settings
 * option are hidden, and the resize handler leaves a page-sized window alone.
 * Kept as the symbol sdl3imgui.cpp links against. */
void windowZoomChange(BYTE amount, bool fromDragResize) {
  (void)amount;
  (void)fromDragResize;
}

void windowSetFrameRate(int newFrameRate, bool setTimer) {
  (void)setTimer;
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
}

/* Menu toggles — called by sdl3imgui.cpp */
void windowSoundEffects_toggle(void)          { soundEffects = !soundEffects; }
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
  soundSetEffectsVolume(pct);
}

/* Master reaches the mixer and the voice module separately: voice has its own
 * playback path and its own gain, and nothing downstream covers both. */
void windowSetMasterVolume(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  windowMasterVolume = pct;
  soundSetMasterVolume(pct);
  voiceSetMasterVolume((float)pct / 100.0f);
}

/* Full screen from the screens outside a game. Here for the same reason as
 * the setters below: the welcome, lobby and settings dialogs are shared and
 * call this, and winbolo.c's copy is not part of this build. SDL maps it onto
 * the browser's Fullscreen API, which needs a user gesture — every call site
 * is inside a click handler, so that holds. No SDL_SyncWindow: the desktop
 * copy waits because Wayland and X11 apply the change asynchronously under a
 * dialog that is about to read the window position, and neither applies. */
void windowFullScreenChoose(bool on) {
  gameFrontFullScreen = on;
  SDL_Window *win = sdl3DrawGetWindow();
  if (win) {
    SDL_SetWindowFullscreen(win, on);
  }
  gameFrontSaveCurrentPrefs();
}

/* -------------------------------------------------------
 * Voice settings — apply a value to the running voice module
 * and clamp it to the range the UI offers, the same way
 * winbolo.c does for the desktop.  The settings dialog is
 * shared and compiles into this target with WINBOLO_VOICE
 * set, so it calls these here as well; winbolo.c, which holds
 * the desktop copies, is not part of the wasm build.  Keep
 * the clamps in step with that copy.
 * ------------------------------------------------------- */
void windowSetVoiceEnabled(bool on) {
  voiceSetEnabled(on);
}

void windowSetVoiceMode(int mode) {
  if (mode < VOICE_MODE_OFF || mode > VOICE_MODE_OPEN) {
    mode = VOICE_MODE_PTT;
  }
  voiceSetMode((VoiceMode)mode);
}

void windowSetVoiceMicGain(float gain) {
  if (gain < 0.0f) gain = 0.0f;
  if (gain > 4.0f) gain = 4.0f;
  voiceSetMicGain(gain);
}

void windowSetVoiceVolume(float gain) {
  if (gain < 0.0f) gain = 0.0f;
  if (gain > 2.0f) gain = 2.0f;
  voiceSetOutputVolume(gain);
}

/* The mic icons live on the status pane, which is shared, so these are
 * here for the same reason the setters above are: the settings dialog
 * calls them and winbolo.c's copies are not part of this build. */
void windowSetShowTankMicIcons(bool on) {
  sdl3DrawStatusSetShowMicIcons(on);
}

bool windowGetShowTankMicIcons(void) {
  return sdl3DrawStatusGetShowMicIcons();
}

void windowMenuAllowNewPlayers_toggle(ClientSim *cs) {
  allowNewPlayers = !allowNewPlayers;
  clientSimSetAllowNewPlayers(cs, allowNewPlayers);
}
void windowMenuNewswire_toggle(ClientSim *cs)    { showNewswireMessages = !showNewswireMessages; if (cs) clientSimShowMessages(cs, MSG_NEWSWIRE, showNewswireMessages); }
void windowMenuAssistant_toggle(ClientSim *cs)   { showAssistantMessages = !showAssistantMessages; if (cs) clientSimShowMessages(cs, MSG_ASSISTANT, showAssistantMessages); }
void windowMenuAI_toggle(ClientSim *cs)          { showAIMessages = !showAIMessages; if (cs) clientSimShowMessages(cs, MSG_AI, showAIMessages); }
void windowMenuNetwork_toggle(ClientSim *cs)     { showNetworkStatusMessages = !showNetworkStatusMessages; if (cs) clientSimShowMessages(cs, MSG_NETSTATUS, showNetworkStatusMessages); }
void windowMenuNetworkDebug_toggle(ClientSim *cs){ showNetworkDebugMessages = !showNetworkDebugMessages; if (cs) clientSimShowMessages(cs, MSG_NETWORK, showNetworkDebugMessages); }
void windowLabelOwnTank_toggle(ClientSim *cs)    { labelSelf = !labelSelf; if (cs) clientSimSetLabelOwnTank(cs, labelSelf); }
void windowSetMessageLabelLen(ClientSim *cs, labelLen n){ labelMsg = n; if (cs) clientSimSetLabelMessage(cs, labelMsg); }
void windowSetTankLabelLen(ClientSim *cs, labelLen n)   { labelTank = n; if (cs) clientSimSetLabelTankLabel(cs, labelTank); }
void windowNewGame(void)                 { winboloQuit = FALSE; }
void windowQuit(void)                    { winboloQuit = TRUE; }

/* Window show/hide — ImGui panels */
void windowShowGameInfo(windowShowRequest req) { sdl3ImguiShowGameInfo(req == wsrOpen); }
void windowShowSysInfo(windowShowRequest req)  { sdl3ImguiShowSysInfo(req == wsrOpen); }
void windowShowNetInfo(windowShowRequest req)  { sdl3ImguiShowNetInfo(req == wsrOpen); }
void windowShowSetPlayerName(windowShowRequest req) { (void)req; }
void windowShowSendMessages(windowShowRequest req)  { sdl3ImguiShowSendMsg(req == wsrOpen); }
void windowShowSetKeys(windowShowRequest req) {
  if (req == wsrOpen) sdl3ImguiShowKeySetup();
}
void windowShowAboutBox(void)           { }
bool windowShowAllianceRequest(void)    { return showAllianceReq; }
void windowDisableSound(void)           { soundEffects = FALSE; useSoundKeepalive = FALSE; }
bool windowGetBackgroundSound(void)     { return backgroundSound; }
void windowRedrawAll(ClientSim *cs) {
  clientMutexWaitFor();
  sdl3DrawRedrawAll(cs, clientSimGetCurrentBuildSelect(cs), NULL, showPillLabels, showBaseLabels);
  clientMutexRelease();
}
void *windowWnd(void) { return NULL; }

void windowSaveMap(ClientSim *cs) {
  (void)cs;
  imguiMessageBoxEx(DIALOG_BOX_TITLE,
                    "Save Map is not available in the web version.",
                    IMGUI_MSG_INFO, IMGUI_MSG_OK);
}

void windowButtonAdd(int keyCode)    { (void)keyCode; }
void windowButtonRemove(int keyCode) { (void)keyCode; }
void windowMouseClick(int xWin, int yWin, int xPos, int yPos) {
  (void)xWin; (void)yWin; (void)xPos; (void)yPos;
}
void windowStartTutorial(void) { doingTutorial = TRUE; }

/* Desktop-only window helpers — wasm has no native window position/size to
 * persist or aspect-correct, so these are no-ops. */
void windowSmoothScrolling_toggle(void) { smoothScrollingEnabled = !smoothScrollingEnabled; }
void windowLetterboxBarsGray_toggle(void) {
  letterboxBarsGray = !letterboxBarsGray;
  gameFrontSaveCurrentPrefs();
}
void windowComputeAspectCorrectSize(int actualW, int actualH, int actualX, int actualY,
                                    int *saveW, int *saveH, int *saveX, int *saveY) {
  if (saveW) *saveW = actualW;
  if (saveH) *saveH = actualH;
  if (saveX) *saveX = actualX;
  if (saveY) *saveY = actualY;
}
void windowGetSavedPosition(int *x, int *y) { if (x) *x = -1; if (y) *y = -1; }
void windowSetSavedPosition(int x, int y) { (void)x; (void)y; }
void windowGetCustomSize(int *w, int *h) { if (w) *w = 0; if (h) *h = 0; }
void windowSetCustomSize(int w, int h) { (void)w; (void)h; }
void windowSaveCurrentPosition(void) {}
void windowAllowPlayerNameChange(bool allow) { (void)allow; }

/* Suspend/resume and pause hooks.  The desktop build freezes the local
 * (and solo-server) sim and mutes audio while backgrounded, while a
 * controller is lost, or while the Deck/tutorial pause menus are up.  The
 * browser build has no OS-level background/foreground or controller hot-plug
 * lifecycle to drive these, so they are no-ops. */
void windowSuspendBackground(ClientSim *cs) { (void)cs; }
void windowResumeForeground(ClientSim *cs) { (void)cs; }
void windowControllerLostPause(ClientSim *cs, bool active) { (void)cs; (void)active; }
void windowDeckPause(ClientSim *cs, bool active) { (void)cs; (void)active; }
void windowTutorialPause(ClientSim *cs, bool active) { (void)cs; (void)active; }

/* -------------------------------------------------------
 * Frontend callbacks — called by backend (bolo engine)
 * ------------------------------------------------------- */

/* Tracks which ClientSim owns the on-screen player panel, so stale callbacks
 * from a previous game can't write into the live UI (mirrors winbolo.c).
 * NULL between games, when the menu's background game runs: its bots'
 * sims would pass this test, so each callback also drops a bot's sim
 * outright. A bot is never the client on screen, and its callbacks arrive
 * on the bot worker pool's threads. */
static struct ClientSim *s_activeUiCs = NULL;

void frontEndDrawMainScreen(ClientSim *cs, screen *value, screenMines *mineView, screenTanks *tks,
                            screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms,
                            int32_t srtDelay, bool isPillView, int edgeX, int edgeY) {
  if (drawBusy == FALSE) {
    BYTE cursorX, cursorY;
    bool showCursor = clientSimGetCursorPos(cs, &cursorX, &cursorY);
    /* Resolve the reticle through the shared build cursor, exactly as the
       desktop frontend does — the click path (sdl3draw.c, shared with this
       build) dispatches to the latched target, so the reticle must come from
       the same place or the two would disagree here only. */
    bool cursorFaint = false;
    showCursor = buildCursorResolveReticle(cs, showCursor, cursorX, cursorY,
                                           &cursorX, &cursorY, &cursorFaint);
    sdl3DrawSetCursorFaint(cursorFaint);
    sdl3DrawSetNetFailed(clientSimGetNetStatus(cs) == netFailed);
    sdl3DrawMainScreen(cs, value, mineView, tks, gs, sBullet, lgms,
                       NULL, showPillLabels, showBaseLabels,
                       srtDelay, isPillView, edgeX, edgeY,
                       showCursor, cursorX, cursorY);
  }
}

void frontEndUpdateTankStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour, BYTE trees) {
  if (clientSimIsBot(cs)) return;
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  BYTE fullShells, fullMines, fullArmour, fullTrees;
  clientSimGetTankFullStats(cs, &fullShells, &fullMines, &fullArmour, &fullTrees);
  sdl3DrawStatusTankBars(0, 0, shells, mines, armour, trees,
                         fullShells, fullMines, fullArmour, fullTrees);
}

void frontEndPlaySound(ClientSim *cs, sndEffects value) {
  if (clientSimIsBot(cs)) return;
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  if (soundEffects == TRUE) soundPlayEffect(value);
}

void frontEndPlaySoundPan(ClientSim *cs, sndEffects value,
                          uint16_t gainL, uint16_t gainR) {
  (void)gainL; (void)gainR;
  if (clientSimIsBot(cs)) return;
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  frontEndPlaySound(cs, value);
}

void windowPlaySound(sndEffects value) {
  if (soundEffects == TRUE) soundPlayEffect(value);
}

void frontEndStatusPillbox(ClientSim *cs, BYTE pillNum, pillAlliance pb) {
  if (clientSimIsBot(cs)) return;
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  /* The per-frame render pass repaints every pill icon from sim state on the
     render thread; skip the direct draw when called from a bot's worker. */
  if (!sdl3DrawOnRenderThread()) return;
  sdl3DrawStatusPillbox(pillNum, pb, showPillLabels);
  sdl3DrawCopyPillsStatus(0, 0);
}

void frontEndStatusTank(ClientSim *cs, BYTE tankNum, tankAlliance ts) {
  if (clientSimIsBot(cs)) return;
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  /* See frontEndStatusPillbox — repainted every frame from sim state; skip
     the direct draw when off the render thread. */
  if (!sdl3DrawOnRenderThread()) return;
  sdl3DrawStatusTank(tankNum, ts);
  sdl3DrawCopyTanksStatus(0, 0);
}

void frontEndMessages(ClientSim *cs, char *top, char *bottom) {
  if (clientSimIsBot(cs)) return;
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  if (drawBusy == FALSE) sdl3DrawMessages(0, 0, top, bottom);
}

void frontEndKillsDeaths(ClientSim *cs, int kills, int deaths) {
  if (clientSimIsBot(cs)) return;
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  if (drawBusy == FALSE) sdl3DrawKillsDeaths(0, 0, kills, deaths);
}

void frontEndUpdatePlayerPing(ClientSim *cs, playerNumbers value, uint16_t ping) {
  if (clientSimIsBot(cs)) return;
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  if (!clientSimIsRunning(cs)) return;
  sdl3ImguiUpdatePlayerPing((unsigned char)value, ping);
}

void frontEndUpdatePlayerFlags(ClientSim *cs, playerNumbers value, uint8_t clientType, uint8_t clientFlags) {
  if (clientSimIsBot(cs)) return;
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  sdl3ImguiUpdatePlayerFlags((unsigned char)value, clientType, clientFlags);
}

void frontEndStatusBase(ClientSim *cs, BYTE baseNum, baseAlliance bs) {
  if (clientSimIsBot(cs)) return;
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  /* See frontEndStatusPillbox — repainted every frame from sim state; skip
     the direct draw when off the render thread. */
  if (!sdl3DrawOnRenderThread()) return;
  sdl3DrawStatusBase(baseNum, bs, showBaseLabels);
  sdl3DrawCopyBasesStatus(0, 0);
}

void frontEndUpdateBaseStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour) {
  if (clientSimIsBot(cs)) return;
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  BYTE fullShells, fullMines, fullArmour;
  clientSimGetBaseFullStats(cs, &fullShells, &fullMines, &fullArmour);
  sdl3DrawStatusBaseBars(0, 0, shells, mines, armour,
                         fullShells, fullMines, fullArmour, FALSE);
}

void frontEndManStatus(ClientSim *cs, bool isDead, TURNTYPE angle) {
  if (clientSimIsBot(cs)) return;
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  clientMutexWaitFor();
  sdl3DrawSetManStatus(0, 0, isDead, angle);
  clientMutexRelease();
}

void frontEndManClear(ClientSim *cs) {
  if (clientSimIsBot(cs)) return;
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  clientMutexWaitFor();
  sdl3DrawSetManClear();
  sdl3DrawCopyManStatus(0, 0);
  clientMutexRelease();
}

void frontEndDrawDownload(ClientSim *cs, bool justBlack) {
  if (drawBusy == FALSE) {
    sdl3DrawDownloadScreen(cs, NULL, justBlack);
  }
}

void frontEndDrawReturningToLobby(ClientSim *cs) {
  if (drawBusy == FALSE) {
    sdl3DrawReturningToLobby(cs);
  }
}

void frontEndAudioReturningToLobby(bool active) {
  soundSetReturningToLobby(active);
}

void frontEndGameOver(ClientSim *cs) {
  if (clientSimIsBot(cs)) return;
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  /* A practice round ends in the lobby: the server holds game over, then
   * returns to the lobby with the win message, and the player stays in the
   * page's game loop. Only a game with no lobby leaves here. */
  ServerSim *srv = gameFrontGetServerSim();
  if (srv != NULL && cs != NULL && !clientSimTransportTicksServer(cs)) {
    return;
  }
  imguiMessageBoxEx(DIALOG_BOX_TITLE, langGetText(STR_WBTIMELIMIT_END),
                    IMGUI_MSG_INFO, IMGUI_MSG_OK);
  finishedLoop = TRUE;
  /* Dismissing the dialog leaves the game. finishedLoop ends the game loop
   * after this frame; main then ends the game and shows the menu. */
  windowLeaveGame();
}

void frontEndClearPlayer(struct ClientSim *cs, playerNumbers value) {
  if (clientSimIsBot(cs)) return;
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  sdl3ImguiClearPlayer((unsigned char)value);
}

void frontEndSetPlayer(ClientSim *cs, playerNumbers value, char *str, const char *countryCode, uint16_t ping, uint8_t clientType, uint8_t clientFlags) {
  char cc[3];
  if (clientSimIsBot(cs)) return;
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  if (!clientSimIsRunning(cs)) {
    cc[0] = 'X'; cc[1] = 'X'; cc[2] = '\0';
    sdl3ImguiSetPlayer((unsigned char)value, str, cc);
    return;
  }
  /* Country code may arrive as "" when unknown; substitute 'X' so we never
   * read past the end (matches winbolo.c). */
  cc[0] = countryCode[0] ? countryCode[0] : 'X';
  cc[1] = (countryCode[0] && countryCode[1]) ? countryCode[1] : 'X';
  cc[2] = '\0';
  sdl3ImguiSetPlayer((unsigned char)value, str, cc);
  sdl3ImguiUpdatePlayerMeta((unsigned char)value, ping, clientType, clientFlags);
}

void frontEndSetPlayerCheckState(struct ClientSim *cs, playerNumbers value, bool isChecked) {
  if (clientSimIsBot(cs)) return;
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  sdl3ImguiSetPlayerCheckState((unsigned char)value, isChecked);
}

void frontEndApplyLocalTankPrefs(struct ClientSim *cs) {
  if (cs == NULL) return;
  clientSimSetTankAutoSlowdown(cs, useAutoslow);
  clientSimSetTankAutoHideGunsight(cs, useAutohide);
}

void frontEndSetActiveClientSim(struct ClientSim *cs) {
  if (cs != s_activeUiCs) {
    for (BYTE i = 0; i < MAX_TANKS; i++) {
      sdl3ImguiClearPlayer(i);
    }
    /* Drop the previous game's newswire/kills text so it doesn't linger. */
    sdl3DrawResetCachedText();
    /* Drop the previous game's latched build target for the same reason: it
       is the square a click builds at, and it outlives the ClientSim that
       set it, so without this the first click of the next game is dispatched
       to a tile chosen in the last one. sdl3ImguiNewGame does this for a new
       game, so only the game-ended path calls it here. */
    if (cs != NULL) {
      sdl3ImguiNewGame(cs);
    } else {
      buildCursorReset();
    }
  }
  s_activeUiCs = cs;
}
void frontEndEnableRequestAllyMenu(bool enabled) { (void)enabled; }
void frontEndEnableLeaveAllyMenu(bool enabled)   { (void)enabled; }

void frontEndShowGunsight(ClientSim *cs, bool isShown) {
  if (clientSimIsBot(cs)) return;
  if (s_activeUiCs != NULL && cs != s_activeUiCs) return;
  showGunsight = !isShown;
  clientSimSetGunsight(cs, showGunsight);
}

void frontEndShowAllianceRequest(char *playerName, BYTE playerNum) {
  if (showAllianceReq) {
    sdl3ImguiShowAllianceRequest(playerName, playerNum);
  }
}

/* -------------------------------------------------------
 * Tutorial step driver. Ported from gui/sdl3/winbolo.c — the WASM build
 * replaces that event loop with this file. The step DATA lives in
 * bolo/tutorial.c (tutorialSteps[]), the trigger is tank.c calling
 * frontEndTutorial(pos), and the overlay is imgui_tutorial_overlay.cpp drawn
 * from sdl3imgui.cpp. This drives the step sequence and intro gating.
 * Single-threaded under emscripten, so no client-mutex dance is needed.
 * ------------------------------------------------------- */
#define TUTORIAL_INTRO_MIN_FRAMES 3
static int  tutorialStepIdx = 0;
static int  tutorialFramesPresented = 0;
static bool respawn1Shown = false;

void frontEndTutorialReset(void) {
  tutorialStepIdx = 0;
  tutorialFramesPresented = 0;
  respawn1Shown = false;
}

/* Called once per frame after present so the intro defers until a real game
 * frame is on screen (the modal dims over the map, not a grey backbuffer). */
void frontEndTutorialNotePresentedFrame(void) {
  if (tutorialFramesPresented < TUTORIAL_INTRO_MIN_FRAMES) {
    tutorialFramesPresented++;
  }
}

/* Shows the respawn message once per run when the server flags a death. */
static void tutorialRespawnPoll(void) {
  if (isTutorial != TRUE || respawn1Shown) return;
  if (tutorialOverlayIsOpen()) return;  /* don't stack on a step message */
  ServerSim *srv = gameFrontGetServerSim();
  if (!srv) return;
  if (!serverSimTakeTutorialRespawn1(srv)) return;
  respawn1Shown = true;
  {
    uint16_t ids[1];
    ids[0] = STR_TUTORIAL_RESPAWN1;
    tutorialOverlayShow(ids, 1, NULL);
  }
}

/* Runs when the player dismisses the last message of the current step.
 * Advances the sequencer; on the final step, leaves tutorial mode so the
 * player can keep driving. */
static void tutorialStepComplete(void) {
  bool finalStep = (tutorialStepIdx == tutorialStepCount - 1);
  if (finalStep) {
    isTutorial = FALSE;
    if (humanSim) clientSimSetTutorial(humanSim, false);
    {
      ServerSim *srv = gameFrontGetServerSim();
      if (srv) serverSimSetTutorial(srv, false);
    }
    gameFrontSetShowTutorialButton(false);
  }
  doingTutorial = FALSE;
  tutorialStepIdx++;

  /* After the boat-building step (the row with pos == 66), respawn at start 1
   * (the far bank) instead of start 0 (at sea). Scan for the row so it
   * survives step-table edits. */
  {
    int boatStep = -1;
    int s;
    for (s = 0; s < tutorialStepCount; s++) {
      if (tutorialSteps[s].pos == 66) { boatStep = s; break; }
    }
    ServerSim *srv = gameFrontGetServerSim();
    if (srv && boatStep >= 0) {
      serverSimSetTutorialStartIdx(srv, (tutorialStepIdx > boatStep) ? 1 : 0);
    }
  }
}

bool frontEndTutorial(BYTE pos) {
  int i;
  int count;

  if (isTutorial != TRUE) {
    tutorialStepIdx = 0;  /* reset for the next run */
    return FALSE;
  }
  if (tutorialStepIdx >= tutorialStepCount) return FALSE;
  /* A message is up (or pending this frame): hold the tank, don't re-trigger. */
  if (tutorialOverlayIsOpen()) return TRUE;

  {
    BYTE stepPos = tutorialSteps[tutorialStepIdx].pos;
    if (stepPos != TUTORIAL_POS_ANY && stepPos != pos) return FALSE;
  }
  /* Intro step: defer until a frame has rendered so the modal opens over the
   * map rather than a grey backbuffer. */
  if (tutorialSteps[tutorialStepIdx].pos == TUTORIAL_POS_ANY &&
      tutorialFramesPresented < TUTORIAL_INTRO_MIN_FRAMES) {
    return FALSE;
  }

  doingTutorial = TRUE;
  count = 0;
  for (i = 0; i < TUTORIAL_MAX_MSGS; i++) {
    if (tutorialSteps[tutorialStepIdx].msgs[i] == 0) break;
    count++;
  }
  tutorialOverlayShow(tutorialSteps[tutorialStepIdx].msgs, count,
                      tutorialStepComplete);
  return TRUE;
}

/* -------------------------------------------------------
 * Misc required symbols
 * ------------------------------------------------------- */
time_t serverMainGetTicks(void) { return ticks; }
time_t windowsGetTicks(void)    { return ticks; }
int winboloCC(void)             { return 0; }

void *dialogAllianceCreate(void)                  { return NULL; }
void dialogAllianceDestroy(void *dlg)             { (void)dlg; }
void dialogAllianceSetName(char *playerName, BYTE playerNum) {
  if (playerName != NULL) {
    sdl3ImguiShowAllianceRequest(playerName, playerNum);
  }
}

bool winUtilWBSubDirExist(char *subDirName) {
  struct stat st;
  if (stat(subDirName, &st) == 0 && S_ISDIR(st.st_mode)) {
    return TRUE;
  }
  return FALSE;
}
