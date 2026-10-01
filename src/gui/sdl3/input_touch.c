/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          Touch Input
*Filename:      input_touch.c
*Purpose:
*  Processes SDL3 finger events into virtual joystick
*  and hit-tested action buttons for tablet mode.
*
*  Layout:
*    Left 40%  — floating joystick
*    Right side — registered buttons via hit-testing
*********************************************************/

#include <math.h>
#include "input_touch.h"
#include "input_joystick.h"
#include "input_gamepad.h"

/* Joystick deadzone in pixels */
#define JOYSTICK_DEADZONE 20.0f

/* Zone boundary for joystick (left 40%) */
#define JOYSTICK_ZONE_RIGHT 0.40f

/* Tap detection thresholds */
#define TAP_DISTANCE_THRESHOLD 10.0f
#define TAP_TIME_THRESHOLD_MS  300

/* Maximum simultaneous fingers to track for tap detection */
#define MAX_TAP_FINGERS 4

/* --- Button registration --- */

typedef enum {
  BTN_SHAPE_CIRCLE,
  BTN_SHAPE_RECT
} ButtonShape;

typedef struct {
  bool registered;
  ButtonShape shape;
  /* Circle */
  float cx, cy, radius;
  /* Rect */
  float rx, ry, rw, rh;
  /* State */
  SDL_FingerID fingerID;
  bool held;
  bool tapped;       /* edge-triggered, consumed on read */
  bool tapConsumed;  /* prevents re-fire while held */
} TouchButton;

static TouchButton s_buttons[TOUCH_BTN_COUNT];

/* --- Joystick state --- */
static SDL_FingerID s_joyFingerID  = 0;
static bool         s_joyActive    = false;
static float        s_joyAnchorX   = 0.0f;
static float        s_joyAnchorY   = 0.0f;
static float        s_joyThumbX    = 0.0f;
static float        s_joyThumbY    = 0.0f;
static Uint64       s_joyReleaseTime = 0;

/* Build select tap (-1 = none) */
static int          s_buildSelectTap = -1;

/* Viewport bounds for tap-to-build */
static int          s_vpX = 0, s_vpY = 0, s_vpW = 0, s_vpH = 0;
static int          s_vpZoom = 1;

/* Tap tracking for viewport taps */
typedef struct {
  SDL_FingerID fingerID;
  float        downX, downY;
  Uint64       downTime;
  bool         active;
} TapTracker;
static TapTracker   s_tapTrackers[MAX_TAP_FINGERS];

/* Gunsight change: +1 increase, -1 decrease, 0 none */
static int          s_gunsightChange = 0;

/* Proportional turning: max joystick reach in pixels */
#define JOYSTICK_MAX_REACH 120.0f

static bool         s_viewportTapReady = false;
static BYTE         s_viewportTapTileX = 0;
static BYTE         s_viewportTapTileY = 0;

static BYTE         s_tankAngle = 0;

/* --- Scroll joystick state --- */
static SDL_FingerID s_scrollFingerID  = 0;
static bool         s_scrollActive    = false;
static float        s_scrollAnchorX   = 0.0f;
static float        s_scrollAnchorY   = 0.0f;
static float        s_scrollThumbX    = 0.0f;
static float        s_scrollThumbY    = 0.0f;
static Uint64       s_scrollReleaseTime = 0;

/* Scroll joystick zone (registered rect) */
static float        s_scrollZoneX = 0.0f;
static float        s_scrollZoneY = 0.0f;
static float        s_scrollZoneW = 0.0f;
static float        s_scrollZoneH = 0.0f;
static bool         s_scrollZoneSet = false;

/* General tap position for UI elements (e.g. build bar) */
static bool         s_generalTapReady = false;
static float        s_generalTapX = 0.0f;
static float        s_generalTapY = 0.0f;

/* --- Viewport drag-to-scroll state --- */
static SDL_FingerID s_vpDragFingerID = 0;
static bool         s_vpDragTracking = false; /* finger is down on viewport */
static bool         s_vpDragActive   = false; /* moved past tap threshold */
static float        s_vpDragStartX   = 0.0f;
static float        s_vpDragStartY   = 0.0f;
static float        s_vpDragCurX     = 0.0f;
static float        s_vpDragCurY     = 0.0f;
static float        s_vpDragPrevX    = 0.0f;
static float        s_vpDragPrevY    = 0.0f;
static bool         s_vpDragMoved    = false; /* finger moved this frame */

/* --- Setup / Cleanup --- */

void inputTouchSetup(void) {
  s_joyActive = false;
  s_scrollActive = false;
  s_scrollZoneSet = false;
  s_vpDragTracking = false;
  s_vpDragActive = false;
  s_buildSelectTap = -1;
  s_viewportTapReady = false;
  for (int i = 0; i < TOUCH_BTN_COUNT; i++) {
    s_buttons[i].registered = false;
    s_buttons[i].held = false;
    s_buttons[i].tapped = false;
    s_buttons[i].tapConsumed = false;
  }
  for (int i = 0; i < MAX_TAP_FINGERS; i++) {
    s_tapTrackers[i].active = false;
  }
}

void inputTouchCleanup(void) {
  inputTouchSetup();
}

/* --- Button registration --- */

void inputTouchClearButtons(void) {
  for (int i = 0; i < TOUCH_BTN_COUNT; i++) {
    s_buttons[i].registered = false;
  }
}

void inputTouchRegisterButton(TouchButtonID id, float centerX, float centerY, float radius) {
  if (id < 0 || id >= TOUCH_BTN_COUNT) return;
  s_buttons[id].registered = true;
  s_buttons[id].shape = BTN_SHAPE_CIRCLE;
  s_buttons[id].cx = centerX;
  s_buttons[id].cy = centerY;
  s_buttons[id].radius = radius;
}

void inputTouchRegisterRectButton(TouchButtonID id, float x, float y, float w, float h) {
  if (id < 0 || id >= TOUCH_BTN_COUNT) return;
  s_buttons[id].registered = true;
  s_buttons[id].shape = BTN_SHAPE_RECT;
  s_buttons[id].rx = x;
  s_buttons[id].ry = y;
  s_buttons[id].rw = w;
  s_buttons[id].rh = h;
}

static bool hitTestButton(TouchButton *btn, float px, float py) {
  if (!btn->registered) return false;
  if (btn->shape == BTN_SHAPE_CIRCLE) {
    float dx = px - btn->cx;
    float dy = py - btn->cy;
    return (dx * dx + dy * dy) <= (btn->radius * btn->radius);
  } else {
    return px >= btn->rx && px < btn->rx + btn->rw &&
           py >= btn->ry && py < btn->ry + btn->rh;
  }
}

/* --- Event processing --- */

void inputTouchProcessEvent(SDL_Event *ev, int windowW, int windowH) {
  if (windowW <= 0 || windowH <= 0) return;

  float fx, fy;
  SDL_FingerID fid;
  bool isDown, isUp, isMotion;

  switch (ev->type) {
    case SDL_EVENT_FINGER_DOWN:
      fx = ev->tfinger.x * (float)windowW;
      fy = ev->tfinger.y * (float)windowH;
      fid = ev->tfinger.fingerID;
      isDown = true; isUp = false; isMotion = false;
      break;
    case SDL_EVENT_FINGER_UP:
      fx = ev->tfinger.x * (float)windowW;
      fy = ev->tfinger.y * (float)windowH;
      fid = ev->tfinger.fingerID;
      isDown = false; isUp = true; isMotion = false;
      break;
    case SDL_EVENT_FINGER_MOTION:
      fx = ev->tfinger.x * (float)windowW;
      fy = ev->tfinger.y * (float)windowH;
      fid = ev->tfinger.fingerID;
      isDown = false; isUp = false; isMotion = true;
      break;
    default:
      return;
  }

  /* --- Tap tracking (all fingers, for viewport tap detection) --- */
  if (isDown) {
    for (int i = 0; i < MAX_TAP_FINGERS; i++) {
      if (!s_tapTrackers[i].active) {
        s_tapTrackers[i].fingerID = fid;
        s_tapTrackers[i].downX = fx;
        s_tapTrackers[i].downY = fy;
        s_tapTrackers[i].downTime = SDL_GetTicks();
        s_tapTrackers[i].active = true;
        break;
      }
    }
  }
  if (isUp) {
    for (int i = 0; i < MAX_TAP_FINGERS; i++) {
      if (s_tapTrackers[i].active && s_tapTrackers[i].fingerID == fid) {
        float ddx = fx - s_tapTrackers[i].downX;
        float ddy = fy - s_tapTrackers[i].downY;
        float dist = sqrtf(ddx * ddx + ddy * ddy);
        Uint64 elapsed = SDL_GetTicks() - s_tapTrackers[i].downTime;

        /* If this finger was a viewport drag, don't fire a tap */
        bool wasDrag = (s_vpDragTracking && fid == s_vpDragFingerID && s_vpDragActive);
        if (!wasDrag && dist < TAP_DISTANCE_THRESHOLD && elapsed < TAP_TIME_THRESHOLD_MS) {
          float tx = s_tapTrackers[i].downX;
          float ty = s_tapTrackers[i].downY;
          /* Store as general tap for UI elements (e.g. build bar) */
          s_generalTapReady = true;
          s_generalTapX = tx;
          s_generalTapY = ty;
          /* Check if tap is inside viewport bounds */
          if (tx >= (float)s_vpX && tx < (float)(s_vpX + s_vpW) &&
              ty >= (float)s_vpY && ty < (float)(s_vpY + s_vpH) &&
              s_vpZoom > 0) {
            int tileX = (int)((tx - (float)s_vpX) / (16.0f * (float)s_vpZoom)) + 1;
            int tileY = (int)((ty - (float)s_vpY) / (16.0f * (float)s_vpZoom)) + 1;
            if (tileX >= 1 && tileX <= 15 && tileY >= 1 && tileY <= 15) {
              s_viewportTapReady = true;
              s_viewportTapTileX = (BYTE)tileX;
              s_viewportTapTileY = (BYTE)tileY;
            }
          }
        }
        s_tapTrackers[i].active = false;
        break;
      }
    }
  }

  float normX = fx / (float)windowW;

  /* --- Viewport drag-to-scroll --- */
  bool insideViewport = (fx >= (float)s_vpX && fx < (float)(s_vpX + s_vpW) &&
                         fy >= (float)s_vpY && fy < (float)(s_vpY + s_vpH) &&
                         s_vpW > 0 && s_vpH > 0);
  if (isDown && !s_vpDragTracking && insideViewport) {
    s_vpDragFingerID = fid;
    s_vpDragTracking = true;
    s_vpDragActive   = false;
    s_vpDragMoved    = false;
    s_vpDragStartX   = fx;
    s_vpDragStartY   = fy;
    s_vpDragCurX     = fx;
    s_vpDragCurY     = fy;
    s_vpDragPrevX    = fx;
    s_vpDragPrevY    = fy;
    /* Don't return — let tap tracker also see this finger */
  }
  if (s_vpDragTracking && fid == s_vpDragFingerID) {
    if (isMotion) {
      s_vpDragPrevX = s_vpDragCurX;
      s_vpDragPrevY = s_vpDragCurY;
      s_vpDragCurX = fx;
      s_vpDragCurY = fy;
      s_vpDragMoved = true;
      if (!s_vpDragActive) {
        float ddx = fx - s_vpDragStartX;
        float ddy = fy - s_vpDragStartY;
        if (sqrtf(ddx * ddx + ddy * ddy) >= TAP_DISTANCE_THRESHOLD) {
          s_vpDragActive = true;
        }
      }
    } else if (isUp) {
      s_vpDragTracking = false;
      s_vpDragActive   = false;
      s_vpDragMoved    = false;
    }
    if (s_vpDragActive) return; /* Claimed by drag — skip other handlers */
  }

  /* --- Joystick zone (left 40%, but not over the game viewport) --- */
  if (isDown && !s_joyActive && normX < JOYSTICK_ZONE_RIGHT && !insideViewport) {
    s_joyFingerID = fid;
    s_joyActive   = true;
    s_joyAnchorX  = fx;
    s_joyAnchorY  = fy;
    s_joyThumbX   = fx;
    s_joyThumbY   = fy;
    return;
  }
  if (s_joyActive && fid == s_joyFingerID) {
    if (isMotion) {
      s_joyThumbX = fx;
      s_joyThumbY = fy;
    } else if (isUp) {
      s_joyActive = false;
      s_joyThumbX = s_joyAnchorX;
      s_joyThumbY = s_joyAnchorY;
      s_joyReleaseTime = SDL_GetTicks();
    }
    return;
  }

  /* --- Scroll joystick zone (right side) --- */
  if (s_scrollZoneSet) {
    bool inScrollZone = (fx >= s_scrollZoneX && fx < s_scrollZoneX + s_scrollZoneW &&
                         fy >= s_scrollZoneY && fy < s_scrollZoneY + s_scrollZoneH);
    if (isDown && !s_scrollActive && inScrollZone) {
      s_scrollFingerID = fid;
      s_scrollActive   = true;
      s_scrollAnchorX  = fx;
      s_scrollAnchorY  = fy;
      s_scrollThumbX   = fx;
      s_scrollThumbY   = fy;
      return;
    }
    if (s_scrollActive && fid == s_scrollFingerID) {
      if (isMotion) {
        s_scrollThumbX = fx;
        s_scrollThumbY = fy;
      } else if (isUp) {
        s_scrollActive = false;
        s_scrollThumbX = s_scrollAnchorX;
        s_scrollThumbY = s_scrollAnchorY;
        s_scrollReleaseTime = SDL_GetTicks();
      }
      return;
    }
  }

  /* --- Button hit-testing --- */
  if (isDown) {
    for (int i = 0; i < TOUCH_BTN_COUNT; i++) {
      if (hitTestButton(&s_buttons[i], fx, fy) && !s_buttons[i].held) {
        s_buttons[i].fingerID = fid;
        s_buttons[i].held = true;
        s_buttons[i].tapped = true;
        s_buttons[i].tapConsumed = false;
        /* Track gunsight changes for the game tick */
        if (i == TOUCH_BTN_GS_INCREASE) s_gunsightChange = 1;
        else if (i == TOUCH_BTN_GS_DECREASE) s_gunsightChange = -1;
        return;
      }
    }
  }
  if (isUp) {
    for (int i = 0; i < TOUCH_BTN_COUNT; i++) {
      if (s_buttons[i].held && s_buttons[i].fingerID == fid) {
        s_buttons[i].held = false;
        return;
      }
    }
  }
}

/* --- Button queries --- */

bool inputTouchIsButtonHeld(TouchButtonID id) {
  if (id < 0 || id >= TOUCH_BTN_COUNT) return false;
  return s_buttons[id].held;
}

bool inputTouchIsButtonTapped(TouchButtonID id) {
  if (id < 0 || id >= TOUCH_BTN_COUNT) return false;
  if (s_buttons[id].tapped && !s_buttons[id].tapConsumed) {
    s_buttons[id].tapConsumed = true;
    return true;
  }
  return false;
}

/* --- Joystick --- */

tankButton inputTouchGetMovement(void) {
  if (!s_joyActive) {
    joystickResetState();  /* reset so next touch starts fresh */
    return TNONE;
  }

  float dx = s_joyThumbX - s_joyAnchorX;
  float dy = s_joyThumbY - s_joyAnchorY;
  float dist = sqrtf(dx * dx + dy * dy);
  if (dist < JOYSTICK_DEADZONE) return TNONE;

  if (joystickGetAbsoluteSteering()) {
    return joystickGetMovementAbsolute(dx, dy, dist, JOYSTICK_DEADZONE, JOYSTICK_MAX_REACH, s_tankAngle);
  } else {
    return joystickGetMovementRelative(dx, dy, dist, JOYSTICK_DEADZONE, JOYSTICK_MAX_REACH);
  }
}

void inputTouchSetTankAngle(BYTE angle) {
  s_tankAngle = angle;
}

void inputTouchSetAbsoluteSteering(bool enabled) {
  joystickSetAbsoluteSteering(enabled);
}

bool inputTouchGetAbsoluteSteering(void) {
  return joystickGetAbsoluteSteering();
}

void inputTouchGetJoystickState(float *anchorX, float *anchorY,
                                float *thumbX, float *thumbY, bool *active,
                                Uint64 *releaseTime) {
  *anchorX = s_joyAnchorX;
  *anchorY = s_joyAnchorY;
  *thumbX  = s_joyThumbX;
  *thumbY  = s_joyThumbY;
  *active  = s_joyActive;
  *releaseTime = s_joyReleaseTime;
}

/* --- Haptic --- */

void inputTouchTriggerHaptic(float strength, Uint32 durationMs) {
  inputGamepadRumble(strength, durationMs);
}

/* --- Legacy API --- */

int inputTouchGetBuildSelect(void) {
  int val = s_buildSelectTap;
  s_buildSelectTap = -1;
  return val;
}

void inputTouchSetViewportBounds(int vpX, int vpY, int vpW, int vpH, int zoom) {
  s_vpX = vpX;
  s_vpY = vpY;
  s_vpW = vpW;
  s_vpH = vpH;
  s_vpZoom = zoom;
}

bool inputTouchGetViewportTap(BYTE *tileX, BYTE *tileY) {
  if (s_viewportTapReady) {
    *tileX = s_viewportTapTileX;
    *tileY = s_viewportTapTileY;
    s_viewportTapReady = false;
    return true;
  }
  return false;
}

int inputTouchGetGunsightChange(void) {
  int val = s_gunsightChange;
  s_gunsightChange = 0;
  return val;
}

bool inputTouchConsumeTapInRect(float x, float y, float w, float h) {
  if (!s_generalTapReady) return false;
  if (s_generalTapX >= x && s_generalTapX < x + w &&
      s_generalTapY >= y && s_generalTapY < y + h) {
    s_generalTapReady = false;
    return true;
  }
  return false;
}

/* --- Scroll joystick --- */

void inputTouchSetScrollJoystickZone(float x, float y, float w, float h) {
  s_scrollZoneX = x;
  s_scrollZoneY = y;
  s_scrollZoneW = w;
  s_scrollZoneH = h;
  s_scrollZoneSet = true;
}

bool inputTouchGetScrollDirection(int *scrollX, int *scrollY) {
  *scrollX = 0;
  *scrollY = 0;
  if (!s_scrollActive) return false;

  float dx = s_scrollThumbX - s_scrollAnchorX;
  float dy = s_scrollThumbY - s_scrollAnchorY;
  float dist = sqrtf(dx * dx + dy * dy);
  if (dist < JOYSTICK_DEADZONE) return false;

  float angle = atan2f(dy, dx) * 180.0f / 3.14159265f;

  /* Map angle to 8 directions for scroll */
  if (angle >= -22.5f && angle < 22.5f)        { *scrollX = 1; }
  else if (angle >= 22.5f  && angle < 67.5f)   { *scrollX = 1; *scrollY = 1; }
  else if (angle >= 67.5f  && angle < 112.5f)  { *scrollY = 1; }
  else if (angle >= 112.5f && angle < 157.5f)  { *scrollX = -1; *scrollY = 1; }
  else if (angle >= 157.5f || angle < -157.5f)  { *scrollX = -1; }
  else if (angle >= -157.5f && angle < -112.5f) { *scrollX = -1; *scrollY = -1; }
  else if (angle >= -112.5f && angle < -67.5f)  { *scrollY = -1; }
  else if (angle >= -67.5f  && angle < -22.5f)  { *scrollX = 1; *scrollY = -1; }

  return true;
}

void inputTouchGetScrollJoystickState(float *anchorX, float *anchorY,
                                      float *thumbX, float *thumbY, bool *active,
                                      Uint64 *releaseTime) {
  *anchorX = s_scrollAnchorX;
  *anchorY = s_scrollAnchorY;
  *thumbX  = s_scrollThumbX;
  *thumbY  = s_scrollThumbY;
  *active  = s_scrollActive;
  *releaseTime = s_scrollReleaseTime;
}

/* --- Viewport drag-to-scroll query --- */

bool inputTouchGetViewportDragScroll(int *scrollX, int *scrollY) {
  *scrollX = 0;
  *scrollY = 0;
  if (!s_vpDragActive || !s_vpDragMoved) return false;

  /* Consume the motion flag so we stop scrolling when the finger is still */
  s_vpDragMoved = false;

  float dx = s_vpDragCurX - s_vpDragPrevX;
  float dy = s_vpDragCurY - s_vpDragPrevY;
  float dist = sqrtf(dx * dx + dy * dy);
  if (dist < 1.0f) return false; /* Sub-pixel motion — ignore */

  float angle = atan2f(dy, dx) * 180.0f / 3.14159265f;

  /* Map angle to 8 directions — invert so dragging right scrolls left
     (natural/content-follows-finger scrolling) */
  if (angle >= -22.5f && angle < 22.5f)        { *scrollX = -1; }
  else if (angle >= 22.5f  && angle < 67.5f)   { *scrollX = -1; *scrollY = -1; }
  else if (angle >= 67.5f  && angle < 112.5f)  { *scrollY = -1; }
  else if (angle >= 112.5f && angle < 157.5f)  { *scrollX = 1; *scrollY = -1; }
  else if (angle >= 157.5f || angle < -157.5f)  { *scrollX = 1; }
  else if (angle >= -157.5f && angle < -112.5f) { *scrollX = 1; *scrollY = 1; }
  else if (angle >= -112.5f && angle < -67.5f)  { *scrollY = 1; }
  else if (angle >= -67.5f  && angle < -22.5f)  { *scrollX = -1; *scrollY = 1; }

  return true;
}

bool inputTouchGetViewportDragDelta(float *deltaX, float *deltaY) {
  *deltaX = 0.0f;
  *deltaY = 0.0f;
  if (!s_vpDragActive || !s_vpDragMoved) return false;

  s_vpDragMoved = false;

  *deltaX = s_vpDragCurX - s_vpDragPrevX;
  *deltaY = s_vpDragCurY - s_vpDragPrevY;
  return true;
}

/* Legacy wrappers */
bool inputTouchIsFirePressed(void) {
  return inputTouchIsButtonHeld(TOUCH_BTN_FIRE);
}

bool inputTouchIsMinePressed(void) {
  return inputTouchIsButtonTapped(TOUCH_BTN_MINE);
}

bool inputTouchIsMineHeld(void) {
  return inputTouchIsButtonHeld(TOUCH_BTN_MINE);
}
