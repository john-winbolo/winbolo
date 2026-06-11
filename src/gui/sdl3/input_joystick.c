/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
*Name:          Joystick Input
*Filename:      input_joystick.c
*Purpose:
*  Shared joystick-to-movement math used by both touch
*  and gamepad input modules.
*
*  Contains angle conversion, relative (cardinal zone)
*  steering, and absolute (point-to-face) steering with
*  adaptive turn rate tracking and smoothing.
*********************************************************/

#include <math.h>
#include <SDL3/SDL.h>
#include "input_joystick.h"
#include "../../common/wb_log.h"

/* Frame counter for rate-limiting turns at small deflections */
static Uint32 s_joyFrameCounter = 0;

/* Debug tick counter for absolute steering logging */
static Uint32 s_absDebugTick = 0;

/* Adaptive turn rate tracking — measure how fast the tank actually
   turns per tick so we can rate-limit appropriately on fast terrain
   without starving slow terrain of turn commands. */
static BYTE  s_prevTankAngle = 0;
static float s_observedTurnRate = 2.0f;  /* bootstrap estimate */

/* Smoothed target angle — low-pass filter on joystick input to
   remove finger wobble. Stored as float in 0-256 circular space. */
static float s_smoothTargetAngle = -1.0f;  /* -1 = uninitialized */

/* Absolute steering flag */
static bool s_absoluteSteering = true;

/* Convert joystick atan2 angle (degrees, 0=right, 90=down) to bolo
   angle (0-255, 0=north, 64=east, 128=south, 192=west). */
BYTE joystickAngleToBolo(float atan2Deg) {
  /* atan2: -90=up(north), 0=right(east), 90=down(south), ±180=left(west)
     bolo:  0=north, 64=east, 128=south, 192=west
     mapping: bolo = (atan2Deg + 90) / 360 * 256 */
  float bolo = (atan2Deg + 90.0f) / 360.0f * 256.0f;
  if (bolo < 0.0f) bolo += 256.0f;
  if (bolo >= 256.0f) bolo -= 256.0f;
  return (BYTE)bolo;
}

static float smoothAngle(float current, float target) {
  /* Interpolate in circular space to handle 0/256 wraparound */
  float diff = target - current;
  if (diff > 128.0f) diff -= 256.0f;
  else if (diff < -128.0f) diff += 256.0f;

  float absDiff = diff < 0.0f ? -diff : diff;

  /* Adaptive: large changes (intentional) track fast,
     small changes (finger jitter) get heavily smoothed.
     At 80+ units difference: factor=0.3 (snap quickly)
     At 10 units difference:  factor=0.03 (heavy filtering) */
  float factor;
  if (absDiff > 80.0f) {
    factor = 0.3f;
  } else if (absDiff > 30.0f) {
    factor = 0.03f + 0.27f * ((absDiff - 30.0f) / 50.0f);
  } else {
    factor = 0.03f;
  }

  float result = current + factor * diff;
  if (result < 0.0f) result += 256.0f;
  else if (result >= 256.0f) result -= 256.0f;
  return result;
}

tankButton joystickGetMovementRelative(float dx, float dy, float dist, float deadzone, float maxReach) {
  float angle = atan2f(dy, dx) * 180.0f / 3.14159265f;

  /* Cardinal directions get 60° zones, diagonals get 30°.
     This makes driving straight much more forgiving. */
  tankButton dir;
  if (angle >= -30.0f && angle < 30.0f)        dir = TRIGHT;
  else if (angle >= 30.0f  && angle < 60.0f)   dir = TRIGHTDECEL;
  else if (angle >= 60.0f  && angle < 120.0f)  dir = TDECEL;
  else if (angle >= 120.0f && angle < 150.0f)  dir = TLEFTDECEL;
  else if (angle >= 150.0f || angle < -150.0f)  dir = TLEFT;
  else if (angle >= -150.0f && angle < -120.0f) dir = TLEFTACCEL;
  else if (angle >= -120.0f && angle < -60.0f)  dir = TACCEL;
  else if (angle >= -60.0f  && angle < -30.0f)  dir = TRIGHTACCEL;
  else return TNONE;

  /* Proportional turning: only rate-limit directions that involve
     turning (left/right and diagonals).  Forward and backward are
     always reported immediately so driving straight feels responsive. */
  if (dir != TACCEL && dir != TDECEL) {
    float reach = (dist - deadzone) / (maxReach - deadzone);
    if (reach > 1.0f) reach = 1.0f;

    /* Map to 1-out-of-N: at minimum deflection report ~1 in 8 frames,
       at full deflection report every frame. */
    s_joyFrameCounter++;
    Uint32 period = (Uint32)(1.0f + 7.0f * (1.0f - reach));  /* 1..8 */
    if ((s_joyFrameCounter % period) != 0) {
      return TNONE;
    }
  }

  return dir;
}

tankButton joystickGetMovementAbsolute(float dx, float dy, float dist, float deadzone, float maxReach, BYTE tankAngle) {
  float atan2Deg = atan2f(dy, dx) * 180.0f / 3.14159265f;

  BYTE rawTarget = joystickAngleToBolo(atan2Deg);

  /* Smooth the target angle to filter out finger jitter */
  if (s_smoothTargetAngle < 0.0f) {
    s_smoothTargetAngle = (float)rawTarget;
  } else {
    s_smoothTargetAngle = smoothAngle(s_smoothTargetAngle, (float)rawTarget);
  }
  BYTE targetAngle = (BYTE)(s_smoothTargetAngle + 0.5f) % 256;
  BYTE currentAngle = tankAngle;

  /* Update observed turn rate (exponential moving average).
     Measure how much the tank actually turned since last tick. */
  int angleDelta = (int)currentAngle - (int)s_prevTankAngle;
  if (angleDelta > 128) angleDelta -= 256;
  else if (angleDelta < -128) angleDelta += 256;
  float absTurnedThisTick = (float)(angleDelta < 0 ? -angleDelta : angleDelta);
  if (absTurnedThisTick > 0.0f) {
    /* Blend: 70% old + 30% new for smooth adaptation */
    s_observedTurnRate = 0.7f * s_observedTurnRate + 0.3f * absTurnedThisTick;
  }
  s_prevTankAngle = currentAngle;

  /* Compute signed difference in 0-255 circular space.
     TRIGHT increases angle (clockwise), TLEFT decreases.
     diff > 0 means target is clockwise from current → turn right. */
  int diff = (int)targetAngle - (int)currentAngle;
  if (diff > 128) diff -= 256;
  else if (diff < -128) diff += 256;

  int absDiff = diff < 0 ? -diff : diff;
  bool turnRight = (diff > 0);

  /* Joystick deflection controls acceleration:
     small deflection = aim/rotate only, large = drive.
     Threshold at 20% of usable range — just past deadzone to aim. */
  float reach = (dist - deadzone) / (maxReach - deadzone);
  if (reach > 1.0f) reach = 1.0f;
  bool wantDrive = (reach > 0.2f);

  /* Adaptive deadzone: widen based on observed turn rate so fast
     terrain settles cleanly. Minimum 10, scales up with turn speed. */
  int dz = (int)(s_observedTurnRate * 3.0f);
  if (dz < 10) dz = 10;
  if (dz > 24) dz = 24;

  /* Adaptive rate-limiting: on fast terrain, skip turn ticks when
     close to target to prevent overshooting. Period increases as
     absDiff shrinks relative to turn rate. */
  s_joyFrameCounter++;
  bool skipTurn = false;
  if (absDiff < dz * 4 && absDiff >= dz) {
    /* How many ticks of turning to reach target? */
    float ticksToTarget = (float)absDiff / s_observedTurnRate;
    /* If we'd arrive in < 3 ticks, start skipping to ease in */
    if (ticksToTarget < 3.0f) {
      Uint32 period = (Uint32)(4.0f - ticksToTarget);  /* 2..4 */
      if (period < 2) period = 2;
      if ((s_joyFrameCounter % period) != 0) {
        skipTurn = true;
      }
    }
  }

  tankButton result;
  if (absDiff < dz) {
    /* Nearly aligned */
    result = wantDrive ? TACCEL : TNONE;
  } else if (absDiff < 64) {
    /* Moderate difference */
    if (skipTurn) {
      result = wantDrive ? TACCEL : TNONE;
    } else if (wantDrive) {
      result = turnRight ? TRIGHTACCEL : TLEFTACCEL;
    } else {
      result = turnRight ? TRIGHT : TLEFT;
    }
  } else {
    /* Large difference — pure turn regardless of deflection,
       don't drive the wrong way */
    result = turnRight ? TRIGHT : TLEFT;
  }

  /* Debug: print every 5 ticks */
  s_absDebugTick++;
  if ((s_absDebugTick % 5) == 0) {
    const char *cmdName;
    switch (result) {
      case TACCEL: cmdName = "ACCEL"; break;
      case TDECEL: cmdName = "DECEL"; break;
      case TLEFT: cmdName = "LEFT"; break;
      case TRIGHT: cmdName = "RIGHT"; break;
      case TLEFTACCEL: cmdName = "LEFT+ACCEL"; break;
      case TRIGHTACCEL: cmdName = "RIGHT+ACCEL"; break;
      case TLEFTDECEL: cmdName = "LEFT+DECEL"; break;
      case TRIGHTDECEL: cmdName = "RIGHT+DECEL"; break;
      default: cmdName = "NONE"; break;
    }
    WB_LOG_TRACE(WB_LOG_CAT_GUI, "[AbsSteer] tank=%d target=%d raw=%d diff=%d absDiff=%d cmd=%s turnRate=%.1f",
            (int)currentAngle, (int)targetAngle, (int)rawTarget, diff, absDiff, cmdName,
            s_observedTurnRate);
  }

  return result;
}

void joystickSetAbsoluteSteering(bool enabled) {
  s_absoluteSteering = enabled;
}

bool joystickGetAbsoluteSteering(void) {
  return s_absoluteSteering;
}

void joystickResetState(void) {
  s_smoothTargetAngle = -1.0f;
}
