/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * touch_input.c - Virtual thumbstick and action buttons for Android
 *
 * Provides a virtual thumbstick on the left side of the screen for
 * tank movement, and action buttons on the right side for shooting,
 * mining, and gunsight adjustment.
 */

#include "touch_input.h"
#include "players_panel.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Number of segments for drawing circles */
#define CIRCLE_SEGMENTS 32

/* -------------------------------------------------------
 * Screen dimensions and derived sizes
 * ------------------------------------------------------- */
static int sScreenW = 0;
static int sScreenH = 0;

/* Thumbstick sizing (set in touchInputSetup based on screen size) */
static float sStickOuterRadius = 0;
static float sStickInnerRadius = 0;
static float sStickDeadZone    = 0;

/* Safe area insets (pixels in logical coords) */
static int sSafeLeft   = 0;
static int sSafeTop    = 0;
static int sSafeRight  = 0;
static int sSafeBottom = 0;

/* Thumbstick zone: left 40%, bottom 50% */
#define STICK_ZONE_X_FRAC 0.40f
#define STICK_ZONE_Y_FRAC 0.50f

/* -------------------------------------------------------
 * Thumbstick state
 * ------------------------------------------------------- */
static bool       sStickActive   = false;
static SDL_FingerID sStickFingerID = 0;
static float      sStickCenterX  = 0; /* pixel coords where finger first touched */
static float      sStickCenterY  = 0;
static float      sStickCurrentX = 0; /* current finger position */
static float      sStickCurrentY = 0;
static tankButton sStickDirection = TNONE;

/* -------------------------------------------------------
 * Action buttons: diamond layout in bottom-right area
 *
 *        [GS+]
 *  [Mine]     [Shoot]
 *        [GS-]
 * ------------------------------------------------------- */

/* Button indices */
#define BTN_SHOOT   0
#define BTN_MINE    1
#define BTN_GS_INC  2
#define BTN_GS_DEC  3
#define BTN_PLAYERS 4
#define BTN_COUNT   5

/* Gunsight repeat rate (matches desktop INPUT_GUNSIGHT_WAIT_TIME = 6) */
#define GUNSIGHT_REPEAT_TICKS 6

/* Button radius (set in touchInputSetup) */
static float sBtnRadius = 0;

/* Button center positions (set in touchInputSetup) */
static float sBtnCenterX[BTN_COUNT];
static float sBtnCenterY[BTN_COUNT];

/* Per-button finger tracking */
static bool        sBtnActive[BTN_COUNT];
static SDL_FingerID sBtnFingerID[BTN_COUNT];

/* Button output state */
static bool sFirePressed = false;
static bool sLayMineFlag = false;
static int  sGunsightChange = 0;

/* Gunsight repeat counters */
static int sGsIncHeldTicks = 0;
static int sGsDecHeldTicks = 0;

/* -------------------------------------------------------
 * Helper: convert SDL3 normalized finger coords to pixels
 * ------------------------------------------------------- */
static void fingerToPixels(float fx, float fy, float *px, float *py) {
  *px = fx * (float)sScreenW;
  *py = fy * (float)sScreenH;
}

/* -------------------------------------------------------
 * Helper: check if pixel coords are in the thumbstick zone
 * ------------------------------------------------------- */
static bool isInStickZone(float px, float py) {
  return (px < (float)sScreenW * STICK_ZONE_X_FRAC) &&
         (py > (float)sScreenH * (1.0f - STICK_ZONE_Y_FRAC));
}

/* -------------------------------------------------------
 * Helper: compute direction from stick offset
 *
 * Uses 8-way mapping with 45-degree wedges:
 *   Angle 0 = right, measured counter-clockwise
 *   But we use screen coords where Y increases downward,
 *   so atan2 gives: right=0, down=+90, left=+/-180, up=-90
 *
 *   We convert to a 0-360 range with 0=up (north) for
 *   intuitive mapping:
 *     Up    (337.5 - 22.5)  → TACCEL
 *     UR    (22.5  - 67.5)  → TRIGHTACCEL
 *     Right (67.5  - 112.5) → TRIGHT
 *     DR    (112.5 - 157.5) → TRIGHTDECEL
 *     Down  (157.5 - 202.5) → TDECEL
 *     DL    (202.5 - 247.5) → TLEFTDECEL
 *     Left  (247.5 - 292.5) → TLEFT
 *     UL    (292.5 - 337.5) → TLEFTACCEL
 * ------------------------------------------------------- */
static tankButton directionFromOffset(float dx, float dy) {
  float dist = sqrtf(dx * dx + dy * dy);
  if (dist < sStickDeadZone) {
    return TNONE;
  }

  /* atan2 with screen coords: dx=right, dy=down
     Convert to compass bearing: 0=north(up), CW positive */
  float angle = atan2f(dx, -dy) * (180.0f / (float)M_PI);
  if (angle < 0) angle += 360.0f;

  /* 8-way with 45-degree wedges centered on each direction */
  if (angle >= 337.5f || angle < 22.5f)   return TACCEL;       /* Up */
  if (angle >= 22.5f  && angle < 67.5f)   return TRIGHTACCEL;  /* Up-Right */
  if (angle >= 67.5f  && angle < 112.5f)  return TRIGHT;       /* Right */
  if (angle >= 112.5f && angle < 157.5f)  return TRIGHTDECEL;  /* Down-Right */
  if (angle >= 157.5f && angle < 202.5f)  return TDECEL;       /* Down */
  if (angle >= 202.5f && angle < 247.5f)  return TLEFTDECEL;   /* Down-Left */
  if (angle >= 247.5f && angle < 292.5f)  return TLEFT;        /* Left */
  if (angle >= 292.5f && angle < 337.5f)  return TLEFTACCEL;   /* Up-Left */

  return TNONE;
}

/* -------------------------------------------------------
 * Helper: draw a filled circle using triangle fan geometry
 * ------------------------------------------------------- */
static void renderFilledCircle(SDL_Renderer *renderer, float cx, float cy,
                               float radius, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
  /* Triangle fan: center vertex + CIRCLE_SEGMENTS+1 perimeter vertices */
  SDL_Vertex verts[CIRCLE_SEGMENTS + 2];
  int indices[CIRCLE_SEGMENTS * 3];

  SDL_FColor color = { r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f };

  /* Center vertex */
  verts[0].position.x = cx;
  verts[0].position.y = cy;
  verts[0].color = color;
  verts[0].tex_coord.x = 0;
  verts[0].tex_coord.y = 0;

  for (int i = 0; i <= CIRCLE_SEGMENTS; i++) {
    float angle = (float)i / (float)CIRCLE_SEGMENTS * 2.0f * (float)M_PI;
    verts[i + 1].position.x = cx + cosf(angle) * radius;
    verts[i + 1].position.y = cy + sinf(angle) * radius;
    verts[i + 1].color = color;
    verts[i + 1].tex_coord.x = 0;
    verts[i + 1].tex_coord.y = 0;
  }

  for (int i = 0; i < CIRCLE_SEGMENTS; i++) {
    indices[i * 3 + 0] = 0;
    indices[i * 3 + 1] = i + 1;
    indices[i * 3 + 2] = i + 2;
  }

  SDL_RenderGeometry(renderer, NULL, verts, CIRCLE_SEGMENTS + 2,
                     indices, CIRCLE_SEGMENTS * 3);
}

/* -------------------------------------------------------
 * Helper: draw a circle outline using line segments
 * ------------------------------------------------------- */
static void renderCircleOutline(SDL_Renderer *renderer, float cx, float cy,
                                float radius, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
  SDL_SetRenderDrawColor(renderer, r, g, b, a);
  float prevX = cx + radius;
  float prevY = cy;
  for (int i = 1; i <= CIRCLE_SEGMENTS; i++) {
    float angle = (float)i / (float)CIRCLE_SEGMENTS * 2.0f * (float)M_PI;
    float nextX = cx + cosf(angle) * radius;
    float nextY = cy + sinf(angle) * radius;
    SDL_RenderLine(renderer, prevX, prevY, nextX, nextY);
    prevX = nextX;
    prevY = nextY;
  }
}

/* -------------------------------------------------------
 * Helper: check if pixel coords hit a button, return index or -1
 * ------------------------------------------------------- */
static int hitTestButtons(float px, float py) {
  for (int i = 0; i < BTN_COUNT; i++) {
    float dx = px - sBtnCenterX[i];
    float dy = py - sBtnCenterY[i];
    if (dx * dx + dy * dy <= sBtnRadius * sBtnRadius) {
      return i;
    }
  }
  return -1;
}

/* -------------------------------------------------------
 * Helper: find which button (if any) a finger ID owns
 * ------------------------------------------------------- */
static int findButtonByFinger(SDL_FingerID fid) {
  for (int i = 0; i < BTN_COUNT; i++) {
    if (sBtnActive[i] && sBtnFingerID[i] == fid) {
      return i;
    }
  }
  return -1;
}

/* -------------------------------------------------------
 * Helper: release a button
 * ------------------------------------------------------- */
static void releaseButton(int btn) {
  sBtnActive[btn] = false;
  if (btn == BTN_SHOOT) {
    sFirePressed = false;
  }
}

/* -------------------------------------------------------
 * Helper: press a button (finger down)
 * ------------------------------------------------------- */
static void pressButton(int btn, SDL_FingerID fid) {
  sBtnActive[btn] = true;
  sBtnFingerID[btn] = fid;

  switch (btn) {
    case BTN_SHOOT:
      sFirePressed = true;
      break;
    case BTN_MINE:
      sLayMineFlag = true;
      break;
    case BTN_GS_INC:
      sGunsightChange = 1;
      sGsIncHeldTicks = 0;
      break;
    case BTN_GS_DEC:
      sGunsightChange = -1;
      sGsDecHeldTicks = 0;
      break;
    case BTN_PLAYERS:
      playersPanelToggle();
      break;
  }
}

/* -------------------------------------------------------
 * Public API
 * ------------------------------------------------------- */

void touchInputSetup(int screenWidth, int screenHeight,
                     int safeLeft, int safeTop, int safeRight, int safeBottom) {
  sScreenW = screenWidth;
  sScreenH = screenHeight;
  sSafeLeft   = safeLeft;
  sSafeTop    = safeTop;
  sSafeRight  = safeRight;
  sSafeBottom = safeBottom;

  /* Scale control sizes proportionally to screen height */
  sStickOuterRadius = (float)screenHeight * 0.08f;
  sStickInnerRadius = (float)screenHeight * 0.03f;
  sStickDeadZone    = (float)screenHeight * 0.015f;

  /* Button radius: 5% of screen height */
  sBtnRadius = (float)screenHeight * 0.05f;

  /* Button spacing: 12% of screen height (center-to-center in diamond) */
  float spacing = (float)screenHeight * 0.12f;

  /* Diamond layout center at (85% of width, 70% of height),
     pulled inward by safe area insets */
  float diamondCX = (float)screenWidth  * 0.85f - (float)safeRight;
  float diamondCY = (float)screenHeight * 0.70f;

  /*        [GS+]  (top)
   * [Mine]       [Shoot]  (left, right)
   *        [GS-]  (bottom) */
  sBtnCenterX[BTN_SHOOT]  = diamondCX + spacing;
  sBtnCenterY[BTN_SHOOT]  = diamondCY;
  sBtnCenterX[BTN_MINE]   = diamondCX - spacing;
  sBtnCenterY[BTN_MINE]   = diamondCY;
  sBtnCenterX[BTN_GS_INC] = diamondCX;
  sBtnCenterY[BTN_GS_INC] = diamondCY - spacing;
  sBtnCenterX[BTN_GS_DEC] = diamondCX;
  sBtnCenterY[BTN_GS_DEC] = diamondCY + spacing;

  /* Players button: top-right corner, below safe area */
  sBtnCenterX[BTN_PLAYERS] = (float)screenWidth - sBtnRadius - 10.0f - (float)safeRight;
  sBtnCenterY[BTN_PLAYERS] = sBtnRadius + 10.0f + (float)safeTop;

  /* Reset state */
  sStickActive = false;
  sStickDirection = TNONE;
  sFirePressed = false;
  sLayMineFlag = false;
  sGunsightChange = 0;
  sGsIncHeldTicks = 0;
  sGsDecHeldTicks = 0;
  for (int i = 0; i < BTN_COUNT; i++) {
    sBtnActive[i] = false;
  }
}

bool touchInputProcessEvent(const SDL_Event *event) {
  float px, py;
  SDL_FingerID fid;

  switch (event->type) {
    case SDL_EVENT_FINGER_DOWN:
      fingerToPixels(event->tfinger.x, event->tfinger.y, &px, &py);
      fid = event->tfinger.fingerID;

      /* Check buttons first (they have fixed positions) */
      {
        int btn = hitTestButtons(px, py);
        if (btn >= 0 && !sBtnActive[btn]) {
          pressButton(btn, fid);
          return true;
        }
      }

      /* If no active thumbstick and touch is in the stick zone, start tracking */
      if (!sStickActive && isInStickZone(px, py)) {
        sStickActive = true;
        sStickFingerID = fid;
        sStickCenterX = px;
        sStickCenterY = py;
        sStickCurrentX = px;
        sStickCurrentY = py;
        sStickDirection = TNONE;
        return true;
      }
      break;

    case SDL_EVENT_FINGER_MOTION:
      fid = event->tfinger.fingerID;

      /* If this finger owns a button, check if it moved off */
      {
        int btn = findButtonByFinger(fid);
        if (btn >= 0) {
          fingerToPixels(event->tfinger.x, event->tfinger.y, &px, &py);
          float dx = px - sBtnCenterX[btn];
          float dy = py - sBtnCenterY[btn];
          if (dx * dx + dy * dy > sBtnRadius * sBtnRadius) {
            releaseButton(btn);
          }
          return true;
        }
      }

      if (sStickActive && fid == sStickFingerID) {
        fingerToPixels(event->tfinger.x, event->tfinger.y, &px, &py);
        sStickCurrentX = px;
        sStickCurrentY = py;

        float dx = sStickCurrentX - sStickCenterX;
        float dy = sStickCurrentY - sStickCenterY;
        sStickDirection = directionFromOffset(dx, dy);
        return true;
      }
      break;

    case SDL_EVENT_FINGER_UP:
      fid = event->tfinger.fingerID;

      /* Check if this finger owned a button */
      {
        int btn = findButtonByFinger(fid);
        if (btn >= 0) {
          releaseButton(btn);
          return true;
        }
      }

      if (sStickActive && fid == sStickFingerID) {
        sStickActive = false;
        sStickDirection = TNONE;
        return true;
      }
      break;
  }

  return false;
}

tankButton touchInputGetKeys(void) {
  return sStickDirection;
}

bool touchInputIsFireKeyPressed(void) {
  return sFirePressed;
}

bool touchInputShouldLayMine(void) {
  bool val = sLayMineFlag;
  sLayMineFlag = false;
  return val;
}

int touchInputGetGunsightChange(void) {
  int val = sGunsightChange;
  sGunsightChange = 0;

  /* Handle held gunsight buttons with repeat */
  if (sBtnActive[BTN_GS_INC]) {
    sGsIncHeldTicks++;
    if (sGsIncHeldTicks >= GUNSIGHT_REPEAT_TICKS) {
      sGsIncHeldTicks = 0;
      val = 1;
    }
  }
  if (sBtnActive[BTN_GS_DEC]) {
    sGsDecHeldTicks++;
    if (sGsDecHeldTicks >= GUNSIGHT_REPEAT_TICKS) {
      sGsDecHeldTicks = 0;
      val = -1;
    }
  }

  return val;
}

/* -------------------------------------------------------
 * Helper: render a single action button with label
 * ------------------------------------------------------- */
static void renderButton(SDL_Renderer *renderer, int btn, const char *label,
                          Uint8 r, Uint8 g, Uint8 b) {
  Uint8 alpha = sBtnActive[btn] ? 140 : 60;
  renderFilledCircle(renderer, sBtnCenterX[btn], sBtnCenterY[btn],
                     sBtnRadius, r, g, b, alpha);
  renderCircleOutline(renderer, sBtnCenterX[btn], sBtnCenterY[btn],
                      sBtnRadius, r, g, b, (Uint8)(alpha + 60 > 255 ? 255 : alpha + 60));

  /* Draw text label centered on button using SDL_RenderDebugText
     (each character is 8x8 pixels) */
  {
    int len = 0;
    while (label[len]) len++;
    float textW = (float)(len * 8);
    float textX = sBtnCenterX[btn] - textW * 0.5f;
    float textY = sBtnCenterY[btn] - 4.0f;
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, sBtnActive[btn] ? 255 : 180);
    SDL_RenderDebugText(renderer, textX, textY, label);
  }
}

void touchInputRender(SDL_Renderer *renderer) {
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

  /* Action buttons — always visible */
  renderButton(renderer, BTN_SHOOT,  "FIRE", 255,  80,  80); /* red */
  renderButton(renderer, BTN_MINE,   "MINE", 255, 200,  60); /* yellow */
  renderButton(renderer, BTN_GS_INC, "GS+",  100, 180, 255); /* blue */
  renderButton(renderer, BTN_GS_DEC, "GS-",  100, 180, 255); /* blue */

  /* Players button — highlight when panel is open */
  {
    Uint8 pr = 100, pg = 200, pb = 100; /* green tint */
    if (playersPanelIsOpen()) { pr = 60; pg = 220; pb = 60; }
    renderButton(renderer, BTN_PLAYERS, "PLRS", pr, pg, pb);
  }

  /* Only draw thumbstick when active */
  if (sStickActive) {
    /* Outer circle (base) — semi-transparent white outline */
    renderCircleOutline(renderer, sStickCenterX, sStickCenterY,
                        sStickOuterRadius, 255, 255, 255, 100);

    /* Inner filled circle at center — very faint */
    renderFilledCircle(renderer, sStickCenterX, sStickCenterY,
                       sStickOuterRadius * 0.9f, 255, 255, 255, 30);

    /* Knob at current finger position, clamped to outer radius */
    float dx = sStickCurrentX - sStickCenterX;
    float dy = sStickCurrentY - sStickCenterY;
    float dist = sqrtf(dx * dx + dy * dy);
    float knobX = sStickCurrentX;
    float knobY = sStickCurrentY;
    if (dist > sStickOuterRadius) {
      knobX = sStickCenterX + dx / dist * sStickOuterRadius;
      knobY = sStickCenterY + dy / dist * sStickOuterRadius;
    }

    renderFilledCircle(renderer, knobX, knobY,
                       sStickInnerRadius, 255, 255, 255, 160);
  }
}
