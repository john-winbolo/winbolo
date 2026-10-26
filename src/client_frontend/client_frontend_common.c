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
#include "client_frontend_tutorial.h"
#include "build_cursor.h"
#include "sdl3imgui.h"      /* sdl3ImguiClearPlayer */
#include "clientmutex.h"
#include "lang.h"           /* STR_TUTORIAL_RESPAWN1 */
#include "server_sim.h"
#include "tutorial.h"       /* tutorialSteps[] */
#include "dialogs/imgui_tutorial_overlay.h"
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
 * Tutorial step sequencer
 *
 * The step index and the presented-frame count are file statics so
 * frontEndTutorialReset and frontEndTutorialNotePresentedFrame can reach
 * them. The frame count holds back the intro: the tutorial starts on tick 1,
 * before a game frame has been presented, and the message should open over
 * the map. The client mutex is taken around every sim change because the
 * desktop's hosted server ticks on another thread; on the web it is a no-op.
 * ------------------------------------------------------- */
#define TUTORIAL_INTRO_MIN_FRAMES 3
static int  tutorialStepIdx = 0;
static int  tutorialFramesPresented = 0;
static bool tutorialRespawn1Shown = false;
static bool tutorialStepShowing = false;

/* Defined once per client binary: the driver owns isTutorial, gamefront owns
 * humanSim. The final step clears the tutorial flag on the human's sim so the
 * player can drive on freely afterwards. */
extern bool isTutorial;
extern ClientSim *humanSim;

void frontEndTutorialReset(void) {
  tutorialStepIdx = 0;
  tutorialFramesPresented = 0;
  tutorialRespawn1Shown = false;
  tutorialStepShowing = false;
}

void frontEndTutorialNotePresentedFrame(void) {
  if (tutorialFramesPresented < TUTORIAL_INTRO_MIN_FRAMES) {
    tutorialFramesPresented++;
  }
}

bool clientFrontTutorialStepShowing(void) {
  return tutorialStepShowing;
}

/* The server sets the flag in tankDeath, on its own thread on the desktop, so
   it is taken and cleared under the mutex. The message goes to the overlay,
   whose open edge freezes a solo game (windowTutorialPause). */
void clientFrontTutorialPollRespawn(void) {
  ServerSim *srv;
  bool fire;
  if (isTutorial != TRUE || tutorialRespawn1Shown) return;
  if (tutorialOverlayIsOpen()) return;   /* don't stack on a step message */
  srv = gameFrontGetServerSim();
  if (!srv) return;
  clientMutexWaitFor();
  fire = serverSimTakeTutorialRespawn1(srv);
  clientMutexRelease();
  if (!fire) return;
  tutorialRespawn1Shown = true;
  {
    uint16_t ids[1];
    ids[0] = STR_TUTORIAL_RESPAWN1;
    tutorialOverlayShow(ids, 1, NULL);
  }
}

/* Runs from the render path when the player dismisses the last message of the
   current step, with the client mutex free and the solo freeze already
   lifted. Advances the sequencer and, on the final step, leaves tutorial
   mode: the client flag and both sims' isTutorial are cleared so the server
   stops halting the tank at the old trigger rows, and completion is saved so
   the welcome menu stops offering the tutorial (Settings turns it back on). */
static void tutorialStepComplete(void) {
  bool finalStep = (tutorialStepIdx == tutorialStepCount - 1);

  if (finalStep) {
    isTutorial = FALSE;
    clientMutexWaitFor();
    if (humanSim) clientSimSetTutorial(humanSim, false);
    {
      ServerSim *srv = gameFrontGetServerSim();
      if (srv) serverSimSetTutorial(srv, false);
    }
    clientMutexRelease();
    gameFrontSetShowTutorialButton(false);
  }

  tutorialStepShowing = false;
  tutorialStepIdx++;

  /* Once the player is past the boat-building step (the tutorialSteps row
     with pos == 66), respawn at start 1 (the far bank) instead of start 0
     (out at sea). Found by scanning for the row, not a fixed index, so it
     survives step-table edits. */
  {
    int boatStep = -1;
    int s;
    for (s = 0; s < tutorialStepCount; s++) {
      if (tutorialSteps[s].pos == 66) { boatStep = s; break; }
    }
    clientMutexWaitFor();
    {
      ServerSim *srv = gameFrontGetServerSim();
      if (srv && boatStep >= 0) {
        serverSimSetTutorialStartIdx(srv, (tutorialStepIdx > boatStep) ? 1 : 0);
      }
    }
    clientMutexRelease();
  }
}

bool frontEndTutorial(BYTE pos) {
  int i;
  int count;

  if (isTutorial != TRUE) {
    tutorialStepIdx = 0;    /* Reset for the next tutorial run. */
    return FALSE;
  }
  if (tutorialStepIdx >= tutorialStepCount) return FALSE;

  /* A message is already up (or pending this frame): hold the tank but don't
     re-trigger. The overlay engages the solo freeze a frame after it opens,
     after which the game tick stops running entirely; this covers that gap. */
  if (tutorialOverlayIsOpen()) return TRUE;

  {
    BYTE stepPos = tutorialSteps[tutorialStepIdx].pos;
    if (stepPos != TUTORIAL_POS_ANY && stepPos != pos) return FALSE;
  }

  /* Intro step only: wait until the game has presented a few frames, so the
     overlay opens over the map rather than an empty backbuffer. */
  if (tutorialSteps[tutorialStepIdx].pos == TUTORIAL_POS_ANY &&
      tutorialFramesPresented < TUTORIAL_INTRO_MIN_FRAMES) {
    return FALSE;
  }

  tutorialStepShowing = true;
  count = 0;
  for (i = 0; i < TUTORIAL_MAX_MSGS; i++) {
    if (tutorialSteps[tutorialStepIdx].msgs[i] == 0) break;
    count++;
  }
  /* Only queues the messages, so it is safe under the client mutex the game
     tick holds. Showing them, the solo freeze and tutorialStepComplete all
     run from the render path. */
  tutorialOverlayShow(tutorialSteps[tutorialStepIdx].msgs, count,
                      tutorialStepComplete);
  return TRUE;
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

/* See clientFrontHoldGameOver in the header. */
static bool s_holdGameOver = false;
static ClientSim *s_heldGameOverCs = NULL;

void clientFrontHoldGameOver(bool hold) {
  s_holdGameOver = hold;
}

bool clientFrontGameOverHeld(void) {
  return s_heldGameOverCs != NULL;
}

void clientFrontRunHeldGameOver(void) {
  ClientSim *cs = s_heldGameOverCs;
  if (cs == NULL) return;
  s_heldGameOverCs = NULL;
  frontEndGameOver(cs);
}

void frontEndGameOver(ClientSim *cs) {
  if (!clientFrontIsActiveUiSim(cs)) return;
  if (!windowGameOverEndsGame(cs)) return;
  if (s_holdGameOver) {
    /* Inside an OS modal loop: only note it. The driver shows it after. */
    s_heldGameOverCs = cs;
    return;
  }
  windowGameOverStopTicks();
  imguiMessageBoxEx(DIALOG_BOX_TITLE, langGetText(STR_WBTIMELIMIT_END),
                    IMGUI_MSG_INFO, IMGUI_MSG_OK);
  windowGameOverLeave();
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
  return clientFrontTutorialStepShowing() ||
         (cs != NULL && clientSimIsSinglePlayer(cs));
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

