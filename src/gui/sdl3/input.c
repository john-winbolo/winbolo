/*
 * $Id$
 *
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
*Name:          Input
*Filename:      input.c
*Author:        John Morrison
*Creation Date: 16/12/98
*Last Modified:   1/5/00
*Purpose:
*  Keyboard and mouse routines (SDL3 implementation)
*  Polls SDL_GetKeyboardState() using SDL_Scancode values.
*********************************************************/

#include <SDL3/SDL.h>
#include <math.h>
#include "global.h"
#include "client_sim.h"
#include "client_command.h"  /* VIEW_KIND_* — which view a view key drives */
#include "client_render.h"
#include "../gamefront.h"
#include "../tiles.h"
#include "input.h"
#include "input_touch.h"
#include "input_gamepad.h"
#include "build_cursor.h"
#include "sdl3imgui.h"
#include "sdl3draw.h"
#include "../ui_mode.h"
#include "../clientmutex.h"

extern bool smoothScrollingEnabled;

/* Smooth-scroll speed: game pixels advanced per scroll tick.
   Tile = 16 game pixels, game runs at 20 ticks/sec, so:
     px=4  →  5 tiles/sec
     px=6  →  7.5 tiles/sec  (default)
     px=8  → 10 tiles/sec
     px=12 → 15 tiles/sec
   Adjust to taste. */
static int smoothScrollSpeedPx = 6;

/* Sub-tile pixel accumulators for smooth scrolling (in zoomed pixels,
   matching gDragOffsetX/Y units). */
static int smoothScrollAccumX = 0;
static int smoothScrollAccumY = 0;

static BYTE scrollKeyCount = 0;

/* Item view auto-repeat: minimum wall-clock gap between advances while a key
 * is held. Each advance jumps a whole pill, base or allied tank, so these are
 * deliberately slow compared with map scrolling. Wall-clock based so they're
 * independent of how often inputGetKeys/inputScroll are polled. The view keys
 * cycle a bit faster than directional stepping. */
#define ITEMVIEW_CYCLE_INTERVAL_MS 165
#define ITEMVIEW_STEP_INTERVAL_MS  250
static Uint32 itemViewCycleMs = 0;
static Uint32 itemViewStepMs  = 0;
/* Previous physical state of each view key, for edge detection. Entering a
 * view requires a fresh key-down edge so a key still held after the player
 * hit Tank View can't immediately re-enter and trap them. */
static bool   pillViewKeyWasDown = FALSE;
static bool   baseViewKeyWasDown = FALSE;
static bool   allyViewKeyWasDown = FALSE;

/* TRUE when pill view was entered via the controller view button: that mode
 * cycles pills on each press and snaps back to tank view on the first driving
 * input (forward or turn).  Not set for keyboard-entered pill view. */
static bool s_controllerPillView = false;

/* Gunsight adjustment state — set by inputGetKeys, consumed by
 * screenBuildInputPacket via inputConsumeGunsightAdj().
 * 0 = no change, 1 = increase, 2 = decrease. */
static uint8_t lastGunsightAdj = 0;

/* Mine key state tracking.
 * SDL_GetKeyboardState can report modifier keys (LSHIFT) as
 * permanently held on some platforms.  We track via SDL events
 * instead, but also use SDL_GetKeyboardState as a fallback
 * for the very first press detection. */
static bool mineKeyEventDown = FALSE;  /* set by SDL_EVENT_KEY_DOWN/UP */
static bool mineKeyPhysicalDown = FALSE; /* tracks physical key state for edge detection */
static bool mineKeyEventsActive = FALSE; /* TRUE once we've seen any event for this key */

/*********************************************************
*NAME:          appHasFocus
*PURPOSE:
*  Returns true if keyboard focus is on one of the windows
*  the player drives the game from: the main window or the
*  Map Overview pop-out.  The Send Message pop-out and the
*  info pop-outs are not in that set, so typing in them
*  never steers the tank.
*********************************************************/
static bool appHasFocus(void) {
  return sdl3ImguiGameInputWindowHasFocus();
}

/* Returns non-zero if the key at the given SDL_Scancode is currently held */
static bool keyDown(int sc) {
  const bool *state = SDL_GetKeyboardState(NULL);
  if (!state || sc <= 0 || sc >= SDL_SCANCODE_COUNT) {
    return false;
  }
  return state[sc];
}

#define KEY_DOWN(sc) keyDown(sc)

/* True while the full screen map has the scroll keys. It is the only map on
   screen there, so it is what they scroll (overviewViewHandleInput) — leaving
   them wired here as well would drag the hidden classic view off the tank and
   latch the manual-scroll override, so the player drops back to a view sitting
   somewhere they never scrolled it to. The item views are unaffected: they
   take the keys before either map sees them, and stepping between items is
   still what they do there. The gamepad stick is unaffected too — it drives
   the build cursor as well, and neither of those has moved. */
static bool overviewOwnsScrollKeys(void) {
  return sdl3DrawIsOverviewInWindow();
}

/*********************************************************
*NAME:          inputSetup
*PURPOSE:
*  Sets up input systems.
*  Returns whether the operation was successful or not
*********************************************************/
bool inputSetup(void) {
  scrollKeyCount = 0;
  itemViewCycleMs = 0;
  itemViewStepMs = 0;
  smoothScrollAccumX = 0;
  smoothScrollAccumY = 0;
  buildCursorReset();
  return TRUE;
}

/*********************************************************
*NAME:          itemViewInputStep
*PURPOSE:
*  Handles the three view keys (pillbox, base and allied
*  tank): each enters its own view and, held, cycles through
*  the items of that kind. While in any of them the scroll
*  keys step to the nearest item in the pressed direction.
*  Both auto-repeat while a key is held, with the first
*  action firing immediately and subsequent ones held to
*  ITEMVIEW_STEP_INTERVAL_MS so it doesn't race through the
*  items. A category the server has turned off is inert: its
*  key does nothing. Returns TRUE if in an item view after
*  processing (caller then suppresses map scrolling).
*********************************************************/
static bool itemViewInputStep(ClientSim *cs, keyItems *setKeys) {
  Uint32 now = SDL_GetTicks();
  /* One row per view key: the scancode it is bound to, the view it drives,
   * the visibility category the server can turn off, the call that enters or
   * cycles that view, and where its held state lives. */
  const struct {
    int          scancode;
    uint8_t      kind;
    ViewCategory category;
    void       (*enterView)(ClientSim *, int, int);
    bool        *wasDown;
  } viewKeys[] = {
    { setKeys->kiPillView, VIEW_KIND_PILL, viewCategoryPill,
      clientSimPillView, &pillViewKeyWasDown },
    { setKeys->kiBaseView, VIEW_KIND_BASE, viewCategoryBase,
      clientSimBaseView, &baseViewKeyWasDown },
    { setKeys->kiAllyView, VIEW_KIND_ALLY, viewCategoryAlly,
      clientSimAllyView, &allyViewKeyWasDown },
  };
  bool anyViewKeyDown = FALSE;
  int i;

  /* View keys. While already in that key's own view, holding it auto-cycles
   * through the items on the cadence (a fresh press also steps immediately).
   * When NOT in that view, only a fresh key-down edge enters — a key still
   * held after the player pressed Tank View must not re-enter, otherwise the
   * held key fights the exit and traps them there until the item goes away.
   * A press while in another item view switches kind. Exit is the Tank View
   * key (handled elsewhere). */
  for (i = 0; i < (int)(sizeof(viewKeys) / sizeof(viewKeys[0])); i++) {
    if (clientSimGetViewPolicy(cs, viewKeys[i].category) == viewPolicyOff) {
      *viewKeys[i].wasDown = FALSE;
      continue;
    }
    bool keyIsDown = KEY_DOWN(viewKeys[i].scancode);
    bool keyEdge = keyIsDown && !*viewKeys[i].wasDown;
    *viewKeys[i].wasDown = keyIsDown;
    if (!keyIsDown) {
      continue;
    }
    anyViewKeyDown = TRUE;
    if (clientSimGetViewKind(cs) == viewKeys[i].kind) {
      if (keyEdge || itemViewCycleMs == 0 ||
          (now - itemViewCycleMs) >= ITEMVIEW_CYCLE_INTERVAL_MS) {
        itemViewCycleMs = now;
        viewKeys[i].enterView(cs, 0, 0);
      }
    } else if (keyEdge) {
      itemViewCycleMs = now;
      viewKeys[i].enterView(cs, 0, 0);
    }
  }
  /* One cycle timer for all three: pressing a different key is always an
     edge, so it starts its own cadence. Rearm once no view key is held. */
  if (!anyViewKeyDown) {
    itemViewCycleMs = 0;
  }

  bool inView = clientSimIsInItemView(cs);

  /* Scroll-based item stepping is disabled in build mode: the stick / scroll
   * then drives the build cursor instead, and shouldn't also jump items.
   * (Building is a tank-view activity; this only matters in the edge case of
   * being in an item view with build mode on.) */
  bool buildActive = buildCursorIsActive();

  /* Gamepad right stick steps items too while in an item view (its normal map
     scroll is suppressed here). */
  bool padUp = false, padDown = false, padLeft = false, padRight = false;
  if (inView && !buildActive && inputGamepadIsConnected()) {
    float gdx = 0.0f, gdy = 0.0f;
    if (inputGamepadGetScrollDirection(&gdx, &gdy)) {
      const float th = 0.5f;
      padRight = gdx >  th; padLeft = gdx < -th;
      padDown  = gdy >  th; padUp   = gdy < -th;
    }
  }

  /* Directional item stepping — only in an item view (and not while
   * building), on the slower step cadence. Read from inView, so it follows
   * the view the keys above just left us in. */
  bool stepUp    = inView && !buildActive && (KEY_DOWN(setKeys->kiScrollUp)    || padUp);
  bool stepDown  = inView && !buildActive && (KEY_DOWN(setKeys->kiScrollDown)  || padDown);
  bool stepLeft  = inView && !buildActive && (KEY_DOWN(setKeys->kiScrollLeft)  || padLeft);
  bool stepRight = inView && !buildActive && (KEY_DOWN(setKeys->kiScrollRight) || padRight);
  if (!stepUp && !stepDown && !stepLeft && !stepRight) {
    itemViewStepMs = 0;
  } else if (itemViewStepMs == 0 ||
             (now - itemViewStepMs) >= ITEMVIEW_STEP_INTERVAL_MS) {
    itemViewStepMs = now;
    if (stepUp)    { clientRenderFrame(cs, up); }
    if (stepDown)  { clientRenderFrame(cs, down); }
    if (stepLeft)  { clientRenderFrame(cs, left); }
    if (stepRight) { clientRenderFrame(cs, right); }
  }

  return clientSimIsInItemView(cs);
}

/*********************************************************
*NAME:          smoothScrollAccumulate
*PURPOSE:
*  Pure accumulator: feeds pixel deltas (in zoomed pixels)
*  into the smooth-scroll sub-tile accumulator. Commits
*  whole-tile crossings to the engine via clientRenderFrame
*  and pushes the remainder to sdl3DrawSetDragOffset for
*  sub-tile rendering.
*
*  When called with dx=dy=0 and no key/stick was held,
*  snaps the accumulator to the nearest tile boundary so
*  the view comes to rest cleanly.
*********************************************************/
static void smoothScrollAccumulate(ClientSim *cs, int dx, int dy) {
  int zoom = sdl3DrawGetZoomFactor();
  if (zoom < 1) zoom = 1;
  int tileW = TILE_SIZE_X * zoom;
  int tileH = TILE_SIZE_Y * zoom;

  if (dx == 0 && dy == 0) {
    if (smoothScrollAccumX != 0 || smoothScrollAccumY != 0) {
      if (smoothScrollAccumX >  tileW / 2) clientRenderFrame(cs, right);
      if (smoothScrollAccumX < -tileW / 2) clientRenderFrame(cs, left);
      if (smoothScrollAccumY >  tileH / 2) clientRenderFrame(cs, down);
      if (smoothScrollAccumY < -tileH / 2) clientRenderFrame(cs, up);
      smoothScrollAccumX = 0;
      smoothScrollAccumY = 0;
      sdl3DrawSetDragOffset(0, 0);
    }
    return;
  }

  /* Don't ramp a sub-tile drag into an edge we can't actually cross
   * (manualScrollKeepsTankOnScreen would block the whole-tile commit).
   * Zero the blocked axis so the view rests instead of sliding-and-
   * snapping against the edge. dx/dy here are already in zoomed pixels
   * (smoothScrollTick multiplied by the step), so no extra scaling. */
  if (dx > 0 && !clientRenderCanScroll(cs, right)) { dx = 0; smoothScrollAccumX = 0; }
  if (dx < 0 && !clientRenderCanScroll(cs, left))  { dx = 0; smoothScrollAccumX = 0; }
  if (dy > 0 && !clientRenderCanScroll(cs, down))  { dy = 0; smoothScrollAccumY = 0; }
  if (dy < 0 && !clientRenderCanScroll(cs, up))    { dy = 0; smoothScrollAccumY = 0; }

  smoothScrollAccumX += dx;
  smoothScrollAccumY += dy;

  while (smoothScrollAccumX >= tileW)  { clientRenderFrame(cs, right); smoothScrollAccumX -= tileW; }
  while (smoothScrollAccumX <= -tileW) { clientRenderFrame(cs, left);  smoothScrollAccumX += tileW; }
  while (smoothScrollAccumY >= tileH)  { clientRenderFrame(cs, down);  smoothScrollAccumY -= tileH; }
  while (smoothScrollAccumY <= -tileH) { clientRenderFrame(cs, up);    smoothScrollAccumY += tileH; }

  sdl3DrawSetDragOffset(smoothScrollAccumX, smoothScrollAccumY);
}

/*********************************************************
*NAME:          smoothScrollGetStepPx
*PURPOSE:
*  Returns the per-frame zoomed pixel step for one unit
*  of input (keyboard direction or full-deflection stick).
*********************************************************/
static int smoothScrollGetStepPx(void) {
  int zoom = sdl3DrawGetZoomFactor();
  if (zoom < 1) zoom = 1;
  int stepZoomed = smoothScrollSpeedPx * zoom;
  if (stepZoomed < 1) stepZoomed = 1;
  return stepZoomed;
}

/*********************************************************
*NAME:          smoothScrollTick
*PURPOSE:
*  Sums keyboard and gamepad scroll contributions and feeds
*  a single (dx, dy) into smoothScrollAccumulate per call.
*  Unifying the two prevents the keyboard path's snap-to-tile
*  branch from clearing a sub-tile accumulator that the
*  gamepad path is still building up.
*********************************************************/
static void smoothScrollTick(ClientSim *cs, keyItems *setKeys) {
  int dx = 0, dy = 0;
  int step = smoothScrollGetStepPx();

  /* Keyboard contribution (dx/dy in {-1, 0, +1}).  Only when smooth
     scrolling is enabled — with it off the caller drives the scroll keys
     through the legacy step-scroll path instead, so feeding them here too
     would double-scroll.  The gamepad stick below is analog and always
     uses this smooth path regardless of the preference. */
  if (smoothScrollingEnabled && !overviewOwnsScrollKeys()) {
    if (KEY_DOWN(setKeys->kiScrollLeft))  dx -= 1;
    if (KEY_DOWN(setKeys->kiScrollRight)) dx += 1;
    if (KEY_DOWN(setKeys->kiScrollUp))    dy -= 1;
    if (KEY_DOWN(setKeys->kiScrollDown))  dy += 1;
    dx *= step;
    dy *= step;
  }

  /* Gamepad contribution (right stick, normalised).  When the free
     build cursor is active the right stick steers the cursor instead
     of scrolling — buildCursorTick handles its own camera follow, so
     the scroll path gets no contribution from the stick that frame. */
  g_dbgCursorStickMag = 0.0f;
  g_dbgCursorMoveMag  = 0.0f;
  if (inputGamepadIsConnected()) {
    float fdx = 0.0f, fdy = 0.0f;
    if (inputGamepadGetScrollDirection(&fdx, &fdy)) {
      /* getScrollDirection returns the raw reach-scaled vector; apply the
         relevant sensitivity here — build-cursor sensitivity while the cursor
         is active, map-scroll sensitivity otherwise — so the two are tuned
         independently. */
      bool buildActive = buildCursorIsActive();
      float sens = buildActive ? g_gamepadBuildCursorSensitivity
                               : g_gamepadScrollSensitivity;
      int gx = (int)(fdx * (float)step * sens);
      int gy = (int)(fdy * (float)step * sens);
      if (buildActive) {
        /* DEBUG: right-stick reach and the resulting cursor move speed
           (reach * sensitivity), both clamped to 0..1. */
        float reach = sqrtf(fdx * fdx + fdy * fdy);
        float mv    = reach * g_gamepadBuildCursorSensitivity;
        g_dbgCursorStickMag = reach > 1.0f ? 1.0f : reach;
        g_dbgCursorMoveMag  = mv > 1.0f ? 1.0f : mv;
      }
      /* Min 1px nudge so deadzone-grazing input still moves. */
      if (gx == 0 && fdx >  0.0f) gx =  1;
      if (gx == 0 && fdx <  0.0f) gx = -1;
      if (gy == 0 && fdy >  0.0f) gy =  1;
      if (gy == 0 && fdy <  0.0f) gy = -1;
      if (buildActive) {
        /* Build cursor is a reticle, always analog. */
        buildCursorTick(cs, gx, gy);
      } else if (smoothScrollingEnabled) {
        /* Smooth map scroll. */
        dx += gx;
        dy += gy;
      } else {
        /* Smooth Scrolling off: the right stick steps whole tiles like the
           legacy keyboard scroll (rate-limited), respecting the preference
           rather than always gliding. */
        static BYTE padScrollCount = 0;
        padScrollCount++;
        if (padScrollCount >= INPUT_SCROLL_WAIT_TIME) {
          padScrollCount = 0;
          const float th = 0.4f;
          bool scrolled = FALSE;
          if (fdx >  th) { clientRenderFrame(cs, right); scrolled = TRUE; }
          if (fdx < -th) { clientRenderFrame(cs, left);  scrolled = TRUE; }
          if (fdy >  th) { clientRenderFrame(cs, down);  scrolled = TRUE; }
          if (fdy < -th) { clientRenderFrame(cs, up);    scrolled = TRUE; }
          if (scrolled) clientSimSetAutoScrollOverride(cs, TRUE);
        }
      }
    }
  }

  /* Latch the manual-scroll override on any user scroll input.
     scrollAutoScroll honours the flag and skips its recenter pull
     until the tank's screen position reaches NO_SCROLL_EDGE, at which
     point it clears the flag and resumes tracking. */
  if (dx != 0 || dy != 0) {
    clientSimSetAutoScrollOverride(cs, TRUE);
  }

  smoothScrollAccumulate(cs, dx, dy);
}

/*********************************************************
*NAME:          inputCleanup
*PURPOSE:
*  Destroys and cleans up input systems.
*********************************************************/
void inputCleanup(void) {
}

/*********************************************************
*NAME:          inputActivate
*PURPOSE:
*  Application has just got focus. Re-acquire input.
*  No-op for SDL3.
*********************************************************/
void inputActivate(void) {
}

/*********************************************************
*NAME:          inputResetHeldKeys
*PURPOSE:
*  Drops all held-key and latched edge state. Called on a
*  window focus transition so a key released while the
*  game window was unfocused (no KEY_UP delivered) can't
*  stay "held" in the polled keyboard state and drive the
*  tank when focus returns.
*********************************************************/
void inputResetHeldKeys(void) {
  /* SDL only clears its global key array when focus leaves the app entirely
     (focus -> NULL); a move to one of our own pop-out windows leaves it
     untouched. Force it up here so a stale held key reads as released until
     it is physically pressed again. */
  SDL_ResetKeyboard();

  /* Latched edge / auto-repeat state owned by this module — SDL_ResetKeyboard
     doesn't touch these. */
  mineKeyEventDown = FALSE;
  mineKeyPhysicalDown = FALSE;
  mineKeyEventsActive = FALSE;
  pillViewKeyWasDown = FALSE;
  baseViewKeyWasDown = FALSE;
  allyViewKeyWasDown = FALSE;
  s_controllerPillView = false;
  scrollKeyCount = 0;
  itemViewCycleMs = 0;
  itemViewStepMs = 0;
  smoothScrollAccumX = 0;
  smoothScrollAccumY = 0;
  lastGunsightAdj = 0;
}

/* End build-cursor mode.  When `execute` and the "exit executes the build"
   option are both set, dispatch the build at the cursor tile first (keeping
   the current build selection); then exit.  Cancel passes execute=false. */
static void buildCursorEnd(ClientSim *cs, bool execute, bool momentary) {
  /* "Only on momentary": when that sub-option is set, a normal tap-off exit
     does NOT build — only leaving a press-and-hold (momentary) session does. */
  if (execute && g_buildExitExecutes && buildCursorIsActive() &&
      (!g_buildExitExecutesMomentaryOnly || momentary)) {
    BYTE bx = 0, by = 0;
    if (buildCursorGetTile(&bx, &by)) {
      clientMutexWaitFor();
      clientSimManMoveToMap(cs, bx, by, clientSimGetCurrentBuildSelect(cs));
      clientMutexRelease();
    }
  }
  buildCursorExit();
}

/*********************************************************
*NAME:          inputGetKeys
*PURPOSE:
*  Gets the current Buttons that are being pressed.
*  Returns tank buttons being pressed.
*
*ARGUMENTS:
*  setKeys - Structure that holds the key settings
*  isMenu  - True if we are in a menu
*********************************************************/
tankButton inputGetKeys(ClientSim *cs, keyItems *setKeys, bool isMenu) {
  static BYTE gunsightKeyCount = 0;
  tankButton tb;
  buildSelect curSelect;

  if (isMenu == TRUE || sdl3ImguiWantsKeyboard() || !appHasFocus()) {
    return TNONE;
  }

  tb = TNONE;

  if (KEY_DOWN(setKeys->kiForward) && KEY_DOWN(setKeys->kiRight)) {
    tb = TRIGHTACCEL;
  } else if (KEY_DOWN(setKeys->kiForward) && KEY_DOWN(setKeys->kiLeft)) {
    tb = TLEFTACCEL;
  } else if (KEY_DOWN(setKeys->kiBackward) && KEY_DOWN(setKeys->kiLeft)) {
    tb = TLEFTDECEL;
  } else if (KEY_DOWN(setKeys->kiBackward) && KEY_DOWN(setKeys->kiRight)) {
    tb = TRIGHTDECEL;
  } else if (KEY_DOWN(setKeys->kiForward)) {
    tb = TACCEL;
  } else if (KEY_DOWN(setKeys->kiBackward)) {
    tb = TDECEL;
  } else if (KEY_DOWN(setKeys->kiLeft)) {
    tb = TLEFT;
  } else if (KEY_DOWN(setKeys->kiRight)) {
    tb = TRIGHT;
  }

  /* Movement priority: keyboard -> gamepad -> touch.
     Gamepad outranks touch on devices that have both
     (e.g. Steam Deck in dock with touchscreen monitor + pad). */
  if (tb == TNONE && inputGamepadIsConnected()) {
    tb = inputGamepadGetMovement(clientSimGetTank256Dir(cs));
  }
  if (tb == TNONE && uiModeIsTablet()) {
    inputTouchSetTankAngle(clientSimGetTank256Dir(cs));
    tb = inputTouchGetMovement();
  }

  /* Lock direction (controller, while held): drop the turn component so the
     tank keeps its heading and the stick only drives forward/back along it. */
  if (inputGamepadIsLockHeadingHeld()) {
    switch (tb) {
      case TLEFT:      case TRIGHT:      tb = TNONE;   break;
      case TLEFTACCEL: case TRIGHTACCEL: tb = TACCEL;  break;
      case TLEFTDECEL: case TRIGHTDECEL: tb = TDECEL;  break;
      default: break;
    }
  }

  /* Controller pill-view peek: the first forward/turn driving input snaps
   * back to tank view. Cleared if pill view was left by any other means. */
  if (s_controllerPillView) {
    if (!clientSimIsInPillView(cs)) {
      s_controllerPillView = false;
    } else if (tb == TACCEL || tb == TLEFT || tb == TRIGHT ||
               tb == TLEFTACCEL || tb == TRIGHTACCEL ||
               tb == TLEFTDECEL || tb == TRIGHTDECEL) {
      clientSimTankView(cs);
      s_controllerPillView = false;
    }
  }

  /* Gamepad-only actions: build-type cycle, builder confirm, view toggle. */
  if (inputGamepadIsConnected()) {
    int delta = inputGamepadGetBuildSelectChange();
    if (delta != 0) {
      clientSimCycleBuildSelect(cs, delta);
      /* Sync the status-panel's cached gCurrentBuildSelect — the mouse
         click path does this at sdl3draw.c:589, but the cycle only
         updates the ClientSim field. Without this the left-side indent
         doesn't move when D-pad cycles. */
      sdl3DrawSelectIndentsOn(clientSimGetCurrentBuildSelect(cs), 0, 0);
    }

    /* Build-cursor toggle gesture:
         - quick tap     -> toggle cursor mode (sticky);
         - hold >200ms   -> momentary (opt): cursor on while held, off on
                            release — "quick build, then back to autoscroll";
         - double-tap    -> (opt) build a road directly under the tank, leaving
                            the build selection and cursor position unchanged.
       Turning cursor mode OFF dispatches the build first when the "exiting
       executes the build" option is on; the Cancel binding exits without it. */
    {
      const Uint32 BC_HOLD_MS = 200;
      const Uint32 BC_DTAP_MS = 300;
      static Uint32 s_bcDownTime        = 0;
      static bool   s_bcPressed         = false;
      static bool   s_bcHeldMode        = false;
      static bool   s_bcWasOffAtPress   = false;
      static bool   s_bcModeBeforePress = false;
      static bool   s_bcDoubleTap       = false;
      static Uint32 s_bcLastReleaseTime = 0;
      static bool   s_bcTapPending      = false;

      Uint32 nowMs  = SDL_GetTicks();
      bool   bcHeld = inputGamepadIsBuildCursorToggleHeld();

      /* Cancel binding: leave build mode without building, whatever the
         exit-executes option says.  Reset the gesture state too. */
      if (inputGamepadIsBuildCancelEdge()) {
        if (buildCursorIsActive()) buildCursorExit();
        s_bcPressed = false; s_bcDoubleTap = false;
        s_bcTapPending = false; s_bcHeldMode = false;
      }

      if (inputGamepadIsBuildCursorToggleEdge()) {
        if (g_buildDoubleTapRoad && s_bcTapPending &&
            (nowMs - s_bcLastReleaseTime) <= BC_DTAP_MS) {
          /* Second tap of a double-tap: build a road under the tank.  Pass
             BsRoad explicitly so the player's current build selection is left
             alone, and don't touch the cursor position.  Undo the first tap's
             toggle so build mode ends up where it started (plain exit — not a
             build-on-exit). */
          BYTE tx = 0, ty = 0;
          if (clientSimGetMyTankMapPos(cs, &tx, &ty)) {
            clientMutexWaitFor();
            clientSimManMoveToMap(cs, tx, ty, BsRoad);
            clientMutexRelease();
          }
          if (buildCursorIsActive() != s_bcModeBeforePress) {
            buildCursorToggle(cs);
          }
          s_bcTapPending = false;
          s_bcDoubleTap  = true;
          s_bcPressed    = true;
          s_bcDownTime   = nowMs;
          s_bcHeldMode   = false;
        } else {
          /* Fresh press — toggle immediately (operate as usual). */
          s_bcModeBeforePress = buildCursorIsActive();
          s_bcWasOffAtPress   = !buildCursorIsActive();
          if (buildCursorIsActive()) {
            buildCursorEnd(cs, /*execute=*/true, /*momentary=*/false); /* tap-off: build-on-exit */
          } else {
            buildCursorToggle(cs);                  /* turn on */
          }
          s_bcPressed    = true;
          s_bcDownTime   = nowMs;
          s_bcHeldMode   = false;
          s_bcDoubleTap  = false;
          s_bcTapPending = false;
        }
      }

      /* Arm momentary mode once held past the threshold (only when enabled and
         this press turned the cursor ON). */
      if (g_buildHoldMomentary && s_bcPressed && !s_bcDoubleTap &&
          s_bcWasOffAtPress && !s_bcHeldMode &&
          (nowMs - s_bcDownTime) > BC_HOLD_MS) {
        s_bcHeldMode = true;
      }

      /* Release edge (held went false). */
      if (s_bcPressed && !bcHeld) {
        s_bcPressed = false;
        if (s_bcDoubleTap) {
          s_bcDoubleTap  = false;
          s_bcTapPending = false;
        } else if (s_bcHeldMode) {
          /* Momentary release: end build mode (build-on-exit if enabled). */
          buildCursorEnd(cs, /*execute=*/true, /*momentary=*/true);
          s_bcTapPending = false;
        } else {
          /* Quick tap: keep it sticky; remember for a possible double-tap. */
          s_bcLastReleaseTime = nowMs;
          s_bcTapPending      = true;
        }
      }
    }

    if (inputGamepadIsViewPlayersEdge()) {
      sdl3ImguiTogglePlayersPanel();
    }

    if (inputGamepadIsTankViewEdge()) {
      clientSimTankView(cs);
    }

    if (inputGamepadIsBuilderConfirmEdge()) {
      BYTE bx, by;
      if (buildCursorGetTargetTile(&bx, &by)) {
        /* Dispatch to the build cursor's stored target tile.  This works
           whether cursor mode is ON or OFF: once a target has been set (by
           the stick or the mouse) it persists, so the player can target a
           tile, turn cursor mode off, drive out of range, and still Build Now
           there. The target stays put for repeated builds at the same spot. */
        clientMutexWaitFor();
        clientSimManMoveToMap(cs, bx, by, clientSimGetCurrentBuildSelect(cs));
        clientMutexRelease();
      } else {
        /* No target set yet — fall back to the gunsight tile. */
        BYTE gsX = 0, gsY = 0;
        clientSimGetGunsightTile(cs, &gsX, &gsY);
        clientMutexWaitFor();
        clientSimManMoveToMap(cs, gsX, gsY, clientSimGetCurrentBuildSelect(cs));
        clientMutexRelease();
      }
      /* Auto-close build cursor mode after the build, if the option is set.
         Plain exit (the build already went out above — no build-on-exit). */
      if (g_buildAutoCloseOnExecute && buildCursorIsActive()) {
        buildCursorExit();
      }
    }

    if (inputGamepadIsViewToggleEdge() &&
        clientSimGetViewPolicy(cs, viewCategoryPill) != viewPolicyOff) {
      /* Enter pill view, or advance to the next pill if already in it. Unlike
       * the old toggle, repeated presses cycle pills rather than returning to
       * tank view — driving (forward/turn) does that, handled above. The
       * button is inert while the server has pill view off, matching the
       * pill view key. */
      clientSimPillView(cs, 0, 0);
      s_controllerPillView = true;
    }
  }

  /* Mine laying is now handled via InputPacket — see inputIsMineKeyPressed() */

  {
    buildSelect newSelect = BsTrees; /* init to suppress warning */
    bool wantSwitch = false;
    if (KEY_DOWN(setKeys->kiQuickTree))         { newSelect = BsTrees;    wantSwitch = true; }
    else if (KEY_DOWN(setKeys->kiQuickRoad))    { newSelect = BsRoad;     wantSwitch = true; }
    else if (KEY_DOWN(setKeys->kiQuickWall))    { newSelect = BsBuilding; wantSwitch = true; }
    else if (KEY_DOWN(setKeys->kiQuickPillbox)) { newSelect = BsPillbox;  wantSwitch = true; }
    else if (KEY_DOWN(setKeys->kiQuickMine))    { newSelect = BsMine;     wantSwitch = true; }
    if (wantSwitch) {
      curSelect = clientSimGetCurrentBuildSelect(cs);
      if (curSelect != newSelect) {
        sdl3DrawSelectIndentsOff(curSelect, 0, 0);
        sdl3DrawSelectIndentsOn(newSelect, 0, 0);
        clientSimSetCurrentBuildSelect(cs, newSelect);
      }
    }
  }

  /* An item view consumes the scroll keys (and the view keys) to step between
   * items; map scrolling is suppressed while one is active. */
  if (itemViewInputStep(cs, setKeys)) {
    smoothScrollAccumX = 0;
    smoothScrollAccumY = 0;
    sdl3DrawSetDragOffset(0, 0);
  } else {
    /* smoothScrollTick always runs so the gamepad right stick scrolls
       the map (and steers the build cursor) regardless of the keyboard
       smooth-scroll preference — the stick is analog and inherently
       smooth.  Its keyboard contribution is internally gated on
       smoothScrollingEnabled; when that is off, the scroll keys fall
       through to the legacy step-scroll below. */
    smoothScrollTick(cs, setKeys);
    if (!smoothScrollingEnabled && !overviewOwnsScrollKeys()) {
      scrollKeyCount++;
      if (scrollKeyCount >= INPUT_SCROLL_WAIT_TIME) {
        scrollKeyCount = 0;
        bool scrolled = FALSE;
        if (KEY_DOWN(setKeys->kiScrollUp))    { clientRenderFrame(cs, up);    scrolled = TRUE; }
        if (KEY_DOWN(setKeys->kiScrollDown))  { clientRenderFrame(cs, down);  scrolled = TRUE; }
        if (KEY_DOWN(setKeys->kiScrollLeft))  { clientRenderFrame(cs, left);  scrolled = TRUE; }
        if (KEY_DOWN(setKeys->kiScrollRight)) { clientRenderFrame(cs, right); scrolled = TRUE; }
        if (scrolled) clientSimSetAutoScrollOverride(cs, TRUE);
      }
    }
  }

  gunsightKeyCount++;
  if (gunsightKeyCount >= INPUT_GUNSIGHT_WAIT_TIME) {
    /* Consume the gamepad edge once; merge with keyboard so a sub-rate
       gamepad press isn't dropped by the WAIT_TIME branch. */
    int padDelta = inputGamepadGetGunsightChange();
    if (KEY_DOWN(setKeys->kiGunIncrease) || padDelta > 0) {
      lastGunsightAdj = 1;  /* increase — flows through InputPacket */
      gunsightKeyCount = 0;
    } else if (KEY_DOWN(setKeys->kiGunDecrease) || padDelta < 0) {
      lastGunsightAdj = 2;  /* decrease — flows through InputPacket */
      gunsightKeyCount = 0;
    } else if (gunsightKeyCount > (INPUT_GUNSIGHT_WAIT_TIME + 1)) {
      gunsightKeyCount = INPUT_GUNSIGHT_WAIT_TIME;
    }
  }

  return tb;
}

/*********************************************************
*NAME:          inputScroll
*PURPOSE:
*  Checks and does scrolling of the window
*
*ARGUMENTS:
*  setKeys - Structure that holds the key settings
*  isMenu  - True if we are in a menu
*********************************************************/
void inputScroll(ClientSim *cs, keyItems *setKeys, bool isMenu) {
  if (isMenu == TRUE || sdl3ImguiWantsKeyboard() || !appHasFocus()) {
    return;
  }

  /* An item view consumes the scroll keys (and the view keys) to step between
   * items; map scrolling is suppressed while one is active. */
  if (itemViewInputStep(cs, setKeys)) {
    smoothScrollAccumX = 0;
    smoothScrollAccumY = 0;
    sdl3DrawSetDragOffset(0, 0);
    return;
  }

  /* smoothScrollTick always runs so the gamepad right stick scrolls (and
     steers the build cursor) regardless of the keyboard smooth-scroll
     preference.  Its keyboard contribution is internally gated on
     smoothScrollingEnabled; when off, the scroll keys fall through to the
     legacy step-scroll below. */
  smoothScrollTick(cs, setKeys);
  if (smoothScrollingEnabled || overviewOwnsScrollKeys()) {
    return;
  }

  scrollKeyCount++;
  if (scrollKeyCount >= INPUT_SCROLL_WAIT_TIME) {
    scrollKeyCount = 0;
    bool scrolled = FALSE;
    if (KEY_DOWN(setKeys->kiScrollUp))    { clientRenderFrame(cs, up);    scrolled = TRUE; }
    if (KEY_DOWN(setKeys->kiScrollDown))  { clientRenderFrame(cs, down);  scrolled = TRUE; }
    if (KEY_DOWN(setKeys->kiScrollLeft))  { clientRenderFrame(cs, left);  scrolled = TRUE; }
    if (KEY_DOWN(setKeys->kiScrollRight)) { clientRenderFrame(cs, right); scrolled = TRUE; }
    if (scrolled) clientSimSetAutoScrollOverride(cs, TRUE);
  }
}

/*********************************************************
*NAME:          inputIsFireKeyPressed
*PURPOSE:
*  Returns whether the fire key is pressed
*
*ARGUMENTS:
*  setKeys - Structure that holds the key settings
*  isMenu  - TRUE if we are in a menu
*********************************************************/
bool inputIsFireKeyPressed(keyItems *setKeys, bool isMenu) {
  if (isMenu == TRUE || sdl3ImguiWantsKeyboard() || !appHasFocus()) {
    return uiModeIsTablet() ? inputTouchIsFirePressed() : FALSE;
  }
  return KEY_DOWN(setKeys->kiShoot)
      || (uiModeIsTablet() && inputTouchIsFirePressed())
      || (inputGamepadIsConnected() && inputGamepadIsFireHeld());
}

/*********************************************************
*NAME:          inputIsMineKeyPressed
*PURPOSE:
*  Returns TRUE while the lay-mine key is held down.
*  tankLayMine() on the server is self-limiting via
*  mapIsMine() so continuous TRUE is safe and lets the
*  player hold the key to lay mines while driving.
*
*ARGUMENTS:
*  setKeys - Structure that holds the key settings
*  isMenu  - TRUE if we are in a menu
*********************************************************/
bool inputIsMineKeyPressed(keyItems *setKeys, bool isMenu) {
  if (isMenu == TRUE || sdl3ImguiWantsKeyboard() || !appHasFocus()) {
    return uiModeIsTablet() ? inputTouchIsMinePressed() : FALSE;
  }

  bool touchMine   = uiModeIsTablet() ? inputTouchIsMinePressed() : false;
  bool gamepadMine = inputGamepadIsConnected() ? inputGamepadIsMineHeld() : false;
  return KEY_DOWN(setKeys->kiLayMine) || touchMine || gamepadMine;
}

/*********************************************************
*NAME:          inputButtonInput
*PURPOSE:
*  For future use when an SDL event loop replaces the
*  Win32 message loop. No-op since we poll
*  SDL_GetKeyboardState() directly.
*
*ARGUMENTS:
*  setKeys  - Structure that holds the key bindings
*  scancode - SDL_Scancode of the key
*  newState - true if pressed, false if released
*********************************************************/
void inputButtonInput(keyItems *setKeys, SDL_Scancode scancode, bool newState) {
  if ((int)scancode == setKeys->kiLayMine) {
    /* Edge detection: only set mineKeyEventDown on a fresh press,
     * not on auto-repeat KEY_DOWN events */
    if (newState && !mineKeyPhysicalDown) {
      mineKeyEventDown = TRUE;
    }
    mineKeyPhysicalDown = newState;
    mineKeyEventsActive = TRUE;
  }
}

uint8_t inputConsumeGunsightAdj(void) {
  /* The on-screen gunsight buttons are a second source for the same pending
     adjustment the wheel and the gunsight keys feed through
     inputBumpGunsight, so fold them in here rather than having each caller
     read them separately. inputTouchGetGunsightChange is consuming, so only
     take it when nothing is already pending — otherwise a wheel bump and a
     button tap in the same tick would drop one of the two. */
  if (lastGunsightAdj == 0 && uiModeIsTablet()) {
    int gs = inputTouchGetGunsightChange();
    if (gs > 0) {
      lastGunsightAdj = 1;
    } else if (gs < 0) {
      lastGunsightAdj = 2;
    }
  }
  uint8_t val = lastGunsightAdj;
  lastGunsightAdj = 0;
  return val;
}

void inputBumpGunsight(int direction) {
  if (direction > 0) {
    lastGunsightAdj = 1;
  } else if (direction < 0) {
    lastGunsightAdj = 2;
  }
}
