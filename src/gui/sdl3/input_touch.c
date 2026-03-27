/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
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
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include "input_touch.h"

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

static bool         s_viewportTapReady = false;
static BYTE         s_viewportTapTileX = 0;
static BYTE         s_viewportTapTileY = 0;

/* --- Setup / Cleanup --- */

void inputTouchSetup(void) {
  s_joyActive = false;
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

        if (dist < TAP_DISTANCE_THRESHOLD && elapsed < TAP_TIME_THRESHOLD_MS) {
          /* Check if tap is inside viewport bounds */
          float tx = s_tapTrackers[i].downX;
          float ty = s_tapTrackers[i].downY;
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

  /* --- Joystick zone (left 40%) --- */
  if (isDown && !s_joyActive && normX < JOYSTICK_ZONE_RIGHT) {
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
  if (!s_joyActive) return TNONE;

  float dx = s_joyThumbX - s_joyAnchorX;
  float dy = s_joyThumbY - s_joyAnchorY;
  float dist = sqrtf(dx * dx + dy * dy);
  if (dist < JOYSTICK_DEADZONE) return TNONE;

  float angle = atan2f(dy, dx) * 180.0f / 3.14159265f;

  if (angle >= -22.5f && angle < 22.5f)   return TRIGHT;
  if (angle >= 22.5f  && angle < 67.5f)   return TRIGHTDECEL;
  if (angle >= 67.5f  && angle < 112.5f)  return TDECEL;
  if (angle >= 112.5f && angle < 157.5f)  return TLEFTDECEL;
  if (angle >= 157.5f || angle < -157.5f) return TLEFT;
  if (angle >= -157.5f && angle < -112.5f) return TLEFTACCEL;
  if (angle >= -112.5f && angle < -67.5f)  return TACCEL;
  if (angle >= -67.5f  && angle < -22.5f)  return TRIGHTACCEL;

  return TNONE;
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
#if defined(__ANDROID__) || (defined(__APPLE__) && (TARGET_OS_IOS || TARGET_OS_TV))
  int count = 0;
  SDL_JoystickID *joysticks = SDL_GetGamepads(&count);
  if (joysticks && count > 0) {
    SDL_Gamepad *gp = SDL_OpenGamepad(joysticks[0]);
    if (gp) {
      Uint16 lo = (Uint16)(strength * 65535.0f);
      Uint16 hi = lo;
      SDL_RumbleGamepad(gp, lo, hi, durationMs);
      SDL_CloseGamepad(gp);
    }
    SDL_free(joysticks);
  }
#else
  (void)strength;
  (void)durationMs;
#endif
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
