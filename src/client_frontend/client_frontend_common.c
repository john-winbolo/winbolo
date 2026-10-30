/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shared client-frontend callback bodies that carry sim-state logic (guards,
 * pref persistence) rather than platform-specific rendering. Both the desktop
 * (winbolo.c) and web (main_wasm.c) clients link this instead of each keeping
 * its own copy, so the guards below cannot be dropped by one platform. The
 * genuinely platform-specific callbacks (window geometry, save-map dialog,
 * suspend/pause) stay in each driver.
 *
 * These reference frontend globals defined once per client binary
 * (showGunsight/... and keys live in winbolo.c on desktop, main_wasm.c on web)
 * and call windowRedrawAll, which each driver implements for its renderer. */

#include "client_sim.h"
#include "frontend.h"
#include "gamefront.h"      /* gameFrontSaveCurrentPrefs */
#include "input.h"          /* keyItems */
#include "sdl3draw.h"       /* sdl3Draw* status helpers */
#include "sdl3draw_status.h"
#include "winbolo.h"        /* window* decls, windowRedrawAll */
#include "client_frontend_common.h"
#include "build_cursor.h"
#include "sdl3imgui.h"      /* sdl3ImguiClearPlayer */
#include "lang.h"
#include "tutorial_text.h"  /* tutorialInputNow */
#include "clientmutex.h"
#include "server_sim.h"      /* serverSimTakeTutorialDone */
#include "tutorial_maps.h"  /* tutorialMapsIsShippedTutorial, tutorialMapsPopupPauses */
#include "dialogs/imgui_messagebox.h"
#include "../gui/sound.h"   /* soundIsMuted / soundSetMuted — the pause freezes */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>

/* -------------------------------------------------------
 * Active-sim guard
 *
 * The ClientSim that drives the visible player list and the other on-screen
 * UI. gamefront registers the human's sim when it is created and clears it on
 * teardown. frontEnd* calls whose cs is not this sim are dropped, so bot,
 * background-game and gym sims cannot write into process-wide UI state.
 * NULL = no registration yet; calls go ahead (bootstrapping).
 * ------------------------------------------------------- */
static struct ClientSim *s_activeUiCs = NULL;

bool clientFrontIsActiveUiSim(const struct ClientSim *cs) {
  return s_activeUiCs == NULL || cs == s_activeUiCs;
}

void clientFrontSaveMapAutoNamed(struct ClientSim *cs, const char *dir) {
  char base[FILENAME_MAX];
  char ts[32];
  char fullPath[FILENAME_MAX];
  const char *mapName = clientSimGetMapName(cs);
  time_t now = time(NULL);

  /* Filename base from the map name, minus any ".map" suffix; "map" when the
     name is empty. */
  if (mapName && mapName[0]) {
    snprintf(base, sizeof(base), "%s", mapName);
    size_t blen = strlen(base);
    if (blen > 4 && strcmp(base + blen - 4, ".map") == 0) {
      base[blen - 4] = '\0';
    }
  } else {
    snprintf(base, sizeof(base), "map");
  }
  strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", localtime(&now));

  SDL_CreateDirectory(dir);
  snprintf(fullPath, sizeof(fullPath), "%s/%s_%s.map", dir, base, ts);

  if (clientSaveMap(cs, fullPath) == FALSE) {
    imguiMessageBoxEx(DIALOG_BOX_TITLE, langGetText(STR_WBERR_SAVEMAP),
                      IMGUI_MSG_ERROR, IMGUI_MSG_OK);
  } else {
    imguiMessageBoxEx(langGetText(STR_MENU_SAVE_MAP), fullPath,
                      IMGUI_MSG_INFO, IMGUI_MSG_OK);
  }
}

void frontEndSetActiveClientSim(struct ClientSim *cs) {
  if (cs != s_activeUiCs) {
    BYTE i;
    for (i = 0; i < MAX_TANKS; i++) {
      sdl3ImguiClearPlayer(i);
    }
    /* Drop the previous game's newswire/kills text so it doesn't linger on
       the message surface when the next game starts (it would otherwise stay
       visible until the first message overwrites it). */
    sdl3DrawResetCachedText();
    /* Drop the previous game's latched build target for the same reason: it
       is the square a click or tap builds at, and it outlives the ClientSim
       that set it, so without this the first build of the next game is
       dispatched to a tile chosen in the last one. */
    buildCursorReset();
  }
  s_activeUiCs = cs;
}

bool clientFrontInLobbyPhase(ClientSim *cs) {
  if (cs == NULL || !clientSimIsInLobby(cs)) return false;
  return clientSimGetNetStatus(cs) == netLobby ||
         clientSimGetNetStatus(cs) == netLobbyCountdown;
}

extern keyItems keys;
extern bool showGunsight;
extern bool autoScrollingEnabled;
extern bool showPillLabels;
extern bool showBaseLabels;

void frontEndRedrawAll(ClientSim *cs) {
  if (!clientSimIsRunning(cs)) return;
  /* In lobby state the in-game renderer hasn't taken over yet — the lobby
   * ImGui is the active view. Skip the game-frame blit so a subscriber-side
   * playersSetPlayer triggered by a CTRL_PLAYER_JOIN mid-lobby (e.g. another
   * remote adding a bot) doesn't stomp the lobby render. */
  if (clientSimIsInLobby(cs)) return;
  /* windowRedrawAll issues a full-screen redraw (SetRenderTarget, blits,
     status panels). It is only safe on the render thread — but this callback
     is reached from the server-tick thread via playersSetPlayer during
     serverInstanceTick (see clientmutex.c), where it would race the
     main-thread present. The main loop already repaints the whole frame from
     sim state every frame, so an off-thread redraw here is redundant; skip
     it and let the next frame refresh. (On the single-threaded wasm build
     every call site is the render thread, so this never skips there.) */
  if (!sdl3DrawOnRenderThread()) return;
  windowRedrawAll(cs);
}

void windowKeyPressed(ClientSim *cs, int keyCode) {
  if (keyCode == keys.kiTankView) {
    clientSimTankView(cs);
  }
  /* The pill, base and allied tank views (enter + hold-to-cycle) are handled
   * by polling in itemViewInputStep so holding a key auto-repeats through the
   * items; dispatching them here too would double-step on the entering
   * press. */
}

void windowShowGunsight_toggle(ClientSim *cs) {
  showGunsight = !showGunsight;
  if (cs) clientSimSetGunsight(cs, showGunsight);
  gameFrontSaveCurrentPrefs();
}

void windowAutomaticScrolling_toggle(ClientSim *cs) {
  autoScrollingEnabled = !autoScrollingEnabled;
  if (cs) clientSimSetAutoScroll(cs, autoScrollingEnabled);
  gameFrontSaveCurrentPrefs();
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

/* -------------------------------------------------------
 * Message and lobby toggles (the menu bar, Settings)
 *
 * The pref flips and, with a game running, the running game is told: one
 * body for the desktop, web and iOS drivers, which own the flags.
 * ------------------------------------------------------- */
extern bool allowNewPlayers;
extern bool showNewswireMessages;
extern bool showAssistantMessages;
extern bool showAIMessages;
extern bool showNetworkStatusMessages;
extern bool showNetworkDebugMessages;

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

/* -------------------------------------------------------
 * Smooth scrolling
 * ------------------------------------------------------- */

/* Each driver owns the flag; input.c reads it. */
extern bool smoothScrollingEnabled;

void windowSmoothScrolling_toggle(void) {
  smoothScrollingEnabled = !smoothScrollingEnabled;
  /* Clear any in-progress sub-tile offset so the view snaps cleanly
     to a tile boundary when toggling off. */
  if (!smoothScrollingEnabled) {
    sdl3DrawSetDragOffset(0, 0);
  }
}

/* -------------------------------------------------------
 * Time-limit game over
 * ------------------------------------------------------- */

void frontEndGameOver(ClientSim *cs) {
  if (!clientFrontIsActiveUiSim(cs)) return;
  if (!windowGameOverEndsGame(cs)) return;
  windowGameOverStopTicks();
  imguiMessageBoxEx(DIALOG_BOX_TITLE, langGetText(STR_WBTIMELIMIT_END),
                    IMGUI_MSG_INFO, IMGUI_MSG_OK);
  windowGameOverLeave();
}

bool clientFrontPopupPausesGame(ClientSim *cs) {
  bool pauses;
  if (cs == NULL) return false;
  /* The script path is read under the client mutex (gamefront.h). The
     mutex can be taken again by a thread that holds it already. */
  clientMutexWaitFor();
  pauses = tutorialMapsPopupPauses(clientSimIsSinglePlayer(cs),
                                   gameFrontGetServerSim() != NULL,
                                   gameFrontScenarioMapScriptPath());
  clientMutexRelease();
  return pauses;
}

#if !defined(__EMSCRIPTEN__)
/* -------------------------------------------------------
 * Solo-game freezes and the background suspend
 *
 * Not in the web build: its pause hooks are no-ops, and it has no
 * hosted-server pause gate (gameFrontSetServerPaused is gamefront.c's).
 * ------------------------------------------------------- */

static bool s_pauseOn[CLIENT_FRONT_PAUSE_COUNT];
/* Mute state captured when each pause engaged, restored verbatim when it
 * releases. A pause must hand audio back to whatever it found — not force it
 * unmuted — so it doesn't clobber a user mute or another still-active pause's
 * mute. One slot per pause because they can nest. */
static bool s_pausePrevMuted[CLIENT_FRONT_PAUSE_COUNT];

bool clientFrontIsSoloSession(ClientSim *cs) {
  return cs != NULL && clientSimIsSinglePlayer(cs);
}

bool clientFrontAnyPause(void) {
  int i;
  for (i = 0; i < CLIENT_FRONT_PAUSE_COUNT; i++) {
    if (s_pauseOn[i]) return true;
  }
  return false;
}

/* Recompute the hosted-server pause gate.  The server tick only freezes for
   a solo session: a listen-server host must keep serving remote players, so
   multiplayer never pauses the server even when the host backgrounds the app
   or opens the overlay. */
static void clientFrontUpdateServerPause(ClientSim *cs) {
  gameFrontSetServerPaused(clientFrontIsSoloSession(cs) && clientFrontAnyPause());
}

bool clientFrontSoloPause(ClientSim *cs, ClientFrontPause which, bool active) {
  if (which < 0 || which >= CLIENT_FRONT_PAUSE_COUNT) return false;
  if (active) {
    /* Only a solo session pauses; multiplayer keeps running. */
    if (!clientFrontIsSoloSession(cs)) return false;
    /* A scenario popup pauses only the shipped tutorials. */
    if (which == CLIENT_FRONT_PAUSE_TUTORIAL &&
        !clientFrontPopupPausesGame(cs)) {
      return false;
    }
    if (s_pauseOn[which]) return false;          /* idempotent */
    s_pauseOn[which] = true;
    clientFrontUpdateServerPause(cs);
    s_pausePrevMuted[which] = soundIsMuted();
    soundSetMuted(TRUE);
    return false;
  }
  /* Always clear on close — even if the mode changed while the pause was
     up — so a stale pause can't freeze a later game. */
  if (!s_pauseOn[which]) return false;
  s_pauseOn[which] = false;
  clientFrontUpdateServerPause(cs);
  soundSetMuted(s_pausePrevMuted[which]);
  return true;
}

bool clientFrontSuspend(ClientSim *cs) {
  /* Idempotent — duplicate WILL_ENTER_BACKGROUND events from SDL are safe. */
  if (s_pauseOn[CLIENT_FRONT_PAUSE_SUSPEND]) return false;
  s_pauseOn[CLIENT_FRONT_PAUSE_SUSPEND] = true;
  clientFrontUpdateServerPause(cs);
  s_pausePrevMuted[CLIENT_FRONT_PAUSE_SUSPEND] = soundIsMuted();
  soundSetMuted(TRUE);
  return true;
}

bool clientFrontResumeBegin(void) {
  if (!s_pauseOn[CLIENT_FRONT_PAUSE_SUSPEND]) return false;
  s_pauseOn[CLIENT_FRONT_PAUSE_SUSPEND] = false;
  return true;
}

void clientFrontResumeEnd(ClientSim *cs) {
  clientFrontUpdateServerPause(cs);
  soundSetMuted(s_pausePrevMuted[CLIENT_FRONT_PAUSE_SUSPEND]);
}
#endif

/* What the single-player server was last told about its player, so the
   frame poll tells it again only when something changed. */
static ServerSim *s_toldSim   = NULL;
static BYTE       s_toldSeat  = 0;
static BYTE       s_toldInput = SERVER_PLAYER_INPUT_KEYBOARD;
static char       s_toldLang[SERVER_PLAYER_LANG_LEN];

/* The player's language and input now, as the server takes them. */
static void scenarioPlayerNow(char *lang, size_t langCap, BYTE *input) {
  gameFrontGetLanguageCode(lang, (int)langCap);
  switch (tutorialInputNow()) {
    case TUTORIAL_INPUT_TOUCH:      *input = SERVER_PLAYER_INPUT_TOUCH;      break;
    case TUTORIAL_INPUT_CONTROLLER: *input = SERVER_PLAYER_INPUT_CONTROLLER; break;
    default:                        *input = SERVER_PLAYER_INPUT_KEYBOARD;   break;
  }
}

static void tellScenarioPlayer(ServerSim *srv, BYTE seat, const char *lang,
                               BYTE input) {
  serverSimSetPlayerLang(srv, seat, lang);
  serverSimSetPlayerInput(srv, seat, input);
  s_toldSim   = srv;
  s_toldSeat  = seat;
  s_toldInput = input;
  snprintf(s_toldLang, sizeof(s_toldLang), "%s", lang);
}

void clientFrontTellScenarioPlayer(ServerSim *srv, uint8_t seat) {
  char lang[SERVER_PLAYER_LANG_LEN];
  BYTE input;
  if (srv == NULL) return;
  scenarioPlayerNow(lang, sizeof(lang), &input);
  tellScenarioPlayer(srv, seat, lang, input);
}

/* The server marks the seat when the script calls game.tutorial_done. The
   mark is taken under the mutex whatever map is running, so a mark from a
   map outside the tutorials folder is dropped rather than kept for later.
   A hosted game is not single player, so its host's button stays. The same
   pass tells the server the player's language and input when either has
   changed since it was last told. */
void clientFrontPollScenarioPlayer(void) {
  ClientSim *cs = s_activeUiCs;
  ServerSim *srv;
  bool done = false;
  bool shipped = false;
  char lang[SERVER_PLAYER_LANG_LEN];
  BYTE input;

  if (cs == NULL) return;
  srv = gameFrontGetServerSim();
  if (!srv) return;
  scenarioPlayerNow(lang, sizeof(lang), &input);
  clientMutexWaitFor();
  if (clientSimIsSinglePlayer(cs)) {
    BYTE seat = clientSimGetMyPlayerNum(cs);
    if (srv != s_toldSim || seat != s_toldSeat || input != s_toldInput ||
        strcmp(lang, s_toldLang) != 0) {
      tellScenarioPlayer(srv, seat, lang, input);
    }
    done = serverSimTakeTutorialDone(srv, seat);
    if (done) {
      shipped = tutorialMapsIsShippedTutorial(gameFrontScenarioMapScriptPath());
    }
  }
  clientMutexRelease();
  if (done && shipped && gameFrontGetShowTutorialButton()) {
    gameFrontSetShowTutorialButton(false);
  }
}
