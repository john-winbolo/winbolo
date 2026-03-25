/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
*Name:          Touch Input
*Filename:      input_touch.c
*Purpose:
*  Processes SDL3 finger events into virtual joystick,
*  shoot, mine, and build-select inputs for tablet mode.
*
*  Layout (screen regions):
*    Left 40%  — floating joystick
*    Right side, lower — shoot button zone
*    Right side, upper — mine button zone
*
*  The joystick anchor appears at the finger-down location.
*  Dragging maps to 8 directions + diagonals.
*********************************************************/

#include <math.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include "input_touch.h"

/* Joystick deadzone in pixels — drags shorter than this are TNONE */
#define JOYSTICK_DEADZONE 20.0f

/* Zone boundaries (fractions of window size) */
#define JOYSTICK_ZONE_RIGHT 0.40f   /* left 40% */
#define SHOOT_ZONE_LEFT     0.70f   /* right 30%, bottom half */
#define SHOOT_ZONE_TOP      0.50f
#define MINE_ZONE_LEFT      0.70f   /* right 30%, top half */
#define MINE_ZONE_BOTTOM    0.50f

/* Tap detection thresholds */
#define TAP_DISTANCE_THRESHOLD 10.0f  /* pixels */
#define TAP_TIME_THRESHOLD_MS  300    /* milliseconds */

/* Maximum simultaneous fingers to track for tap detection */
#define MAX_TAP_FINGERS 4

/* Joystick state */
static SDL_FingerID s_joyFingerID  = 0;
static bool         s_joyActive    = false;
static float        s_joyAnchorX   = 0.0f;
static float        s_joyAnchorY   = 0.0f;
static float        s_joyThumbX    = 0.0f;
static float        s_joyThumbY    = 0.0f;
static Uint64       s_joyReleaseTime = 0; /* SDL_GetTicks() when joystick released */

/* Shoot button state */
static SDL_FingerID s_shootFingerID = 0;
static bool         s_shootActive   = false;

/* Mine button state (edge-triggered) */
static SDL_FingerID s_mineFingerID  = 0;
static bool         s_mineActive    = false;
static bool         s_mineFired     = false;  /* consumed flag */

/* Build select tap (-1 = none) */
static int          s_buildSelectTap = -1;

/* Viewport bounds for tap-to-build */
static int          s_vpX = 0, s_vpY = 0, s_vpW = 0, s_vpH = 0;
static int          s_vpZoom = 1;

/* Tap tracking — records finger-down position and time for all fingers */
typedef struct {
  SDL_FingerID fingerID;
  float        downX, downY;
  Uint64       downTime;
  bool         active;
} TapTracker;
static TapTracker   s_tapTrackers[MAX_TAP_FINGERS];

/* Viewport tap result (consumed on read) */
static bool         s_viewportTapReady = false;
static BYTE         s_viewportTapTileX = 0;
static BYTE         s_viewportTapTileY = 0;

void inputTouchSetup(void) {
  s_joyActive    = false;
  s_shootActive  = false;
  s_mineActive   = false;
  s_mineFired    = false;
  s_buildSelectTap = -1;
  s_viewportTapReady = false;
  for (int i = 0; i < MAX_TAP_FINGERS; i++) {
    s_tapTrackers[i].active = false;
  }
}

void inputTouchCleanup(void) {
  inputTouchSetup();
}

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
  float normY = fy / (float)windowH;

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
      s_joyReleaseTime = SDL_GetTicks();
    }
    return;
  }

  /* --- Shoot zone (right 30%, bottom half) --- */
  if (isDown && !s_shootActive && normX >= SHOOT_ZONE_LEFT && normY >= SHOOT_ZONE_TOP) {
    s_shootFingerID = fid;
    s_shootActive   = true;
    return;
  }
  if (s_shootActive && fid == s_shootFingerID) {
    if (isUp) {
      s_shootActive = false;
    }
    return;
  }

  /* --- Mine zone (right 30%, top half) --- */
  if (isDown && !s_mineActive && normX >= MINE_ZONE_LEFT && normY < MINE_ZONE_BOTTOM) {
    s_mineFingerID = fid;
    s_mineActive   = true;
    s_mineFired    = false;
    return;
  }
  if (s_mineActive && fid == s_mineFingerID) {
    if (isUp) {
      s_mineActive = false;
    }
    return;
  }
}

tankButton inputTouchGetMovement(void) {
  if (!s_joyActive) return TNONE;

  float dx = s_joyThumbX - s_joyAnchorX;
  float dy = s_joyThumbY - s_joyAnchorY;
  float dist = sqrtf(dx * dx + dy * dy);
  if (dist < JOYSTICK_DEADZONE) return TNONE;

  /* Angle in degrees: 0=right, 90=down, etc. */
  float angle = atan2f(dy, dx) * 180.0f / 3.14159265f;

  /* Map to 8 directions.
   * Up = negative Y in screen coords.
   * Bolo: TACCEL=forward(up), TDECEL=backward(down),
   *        TLEFT=left, TRIGHT=right */
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

bool inputTouchIsFirePressed(void) {
  return s_shootActive;
}

bool inputTouchIsMinePressed(void) {
  if (s_mineActive && !s_mineFired) {
    s_mineFired = true;
    return true;
  }
  return false;
}

bool inputTouchIsMineHeld(void) {
  return s_mineActive;
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

void inputTouchTriggerHaptic(float strength, Uint32 durationMs) {
#if defined(__ANDROID__) || (defined(__APPLE__) && (TARGET_OS_IOS || TARGET_OS_TV))
  /* Use SDL_RumbleGamepad if a gamepad is connected, otherwise fall back
     to platform vibration.  SDL3 on iOS/Android can rumble via
     SDL_RumbleGamepad on virtual/connected controllers. */
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
#ifdef __ANDROID__
  /* Android: SDL_Vibrate is not in SDL3, but we can use SDL_AndroidSendMessage
     or JNI. For now, gamepad rumble is the primary path. */
#endif
#else
  (void)strength;
  (void)durationMs;
#endif
}

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
