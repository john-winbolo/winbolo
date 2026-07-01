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
  windowRedrawAll(cs);
}

void windowKeyPressed(ClientSim *cs, int keyCode) {
  if (keyCode == keys.kiTankView) {
    clientSimTankView(cs);
  }
  /* Pill view (enter + hold-to-cycle) is handled by polling in
   * pillViewInputStep so holding the key auto-repeats through pills;
   * dispatching it here too would double-step on the entering press. */
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
