/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * touch_input.c - Virtual thumbstick and action buttons for mobile
 *
 * Shared between Android and iOS. Provides a virtual thumbstick on
 * the left side of the screen for tank movement, and action buttons
 * on the right side for shooting, mining, and gunsight adjustment.
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
static float      sStickCenterX  = 0;
static float      sStickCenterY  = 0;
static float      sStickCurrentX = 0;
static float      sStickCurrentY = 0;
static tankButton sStickDirection = TNONE;

/* -------------------------------------------------------
 * Action buttons: diamond layout in bottom-right area
 *
 *        [GS+]
 *  [Mine]     [Shoot]
 *        [GS-]
 * ------------------------------------------------------- */

#define BTN_SHOOT   0
#define BTN_MINE    1
#define BTN_GS_INC  2
#define BTN_GS_DEC  3
#define BTN_PLAYERS 4
#define BTN_COUNT   5

#define GUNSIGHT_REPEAT_TICKS 6

static float sBtnRadius = 0;
static float sBtnCenterX[BTN_COUNT];
static float sBtnCenterY[BTN_COUNT];

static bool        sBtnActive[BTN_COUNT];
static SDL_FingerID sBtnFingerID[BTN_COUNT];

static bool sFirePressed = false;
static bool sLayMineFlag = false;
static int  sGunsightChange = 0;

static int sGsIncHeldTicks = 0;
static int sGsDecHeldTicks = 0;

/* Fade-out timing for controls */
static Uint64 sLastBtnTouchTime = 0;   /* SDL_GetTicks() of last button touch */
static Uint64 sLastStickTouchTime = 0; /* SDL_GetTicks() of last stick touch */
#define CONTROL_VISIBLE_MS   2000  /* stay visible this long after last touch */
#define CONTROL_FADE_MS      1000  /* fade out over this duration */

/* ------------------------------------------------------- */
static void fingerToPixels(float fx, float fy, float *px, float *py) {
  *px = fx * (float)sScreenW;
  *py = fy * (float)sScreenH;
}

static bool isInStickZone(float px, float py) {
  return (px < (float)sScreenW * STICK_ZONE_X_FRAC) &&
         (py > (float)sScreenH * (1.0f - STICK_ZONE_Y_FRAC));
}

static tankButton directionFromOffset(float dx, float dy) {
  float dist = sqrtf(dx * dx + dy * dy);
  if (dist < sStickDeadZone) {
    return TNONE;
  }

  float angle = atan2f(dx, -dy) * (180.0f / (float)M_PI);
  if (angle < 0) angle += 360.0f;

  if (angle >= 337.5f || angle < 22.5f)   return TACCEL;
  if (angle >= 22.5f  && angle < 67.5f)   return TRIGHTACCEL;
  if (angle >= 67.5f  && angle < 112.5f)  return TRIGHT;
  if (angle >= 112.5f && angle < 157.5f)  return TRIGHTDECEL;
  if (angle >= 157.5f && angle < 202.5f)  return TDECEL;
  if (angle >= 202.5f && angle < 247.5f)  return TLEFTDECEL;
  if (angle >= 247.5f && angle < 292.5f)  return TLEFT;
  if (angle >= 292.5f && angle < 337.5f)  return TLEFTACCEL;

  return TNONE;
}

static void renderFilledCircle(SDL_Renderer *renderer, float cx, float cy,
                               float radius, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
  SDL_Vertex verts[CIRCLE_SEGMENTS + 2];
  int indices[CIRCLE_SEGMENTS * 3];

  SDL_FColor color = { r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f };

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

static int findButtonByFinger(SDL_FingerID fid) {
  for (int i = 0; i < BTN_COUNT; i++) {
    if (sBtnActive[i] && sBtnFingerID[i] == fid) {
      return i;
    }
  }
  return -1;
}

static void releaseButton(int btn) {
  sBtnActive[btn] = false;
  if (btn == BTN_SHOOT) {
    sFirePressed = false;
  }
}

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

  sStickOuterRadius = (float)screenHeight * 0.08f;
  sStickInnerRadius = (float)screenHeight * 0.03f;
  sStickDeadZone    = (float)screenHeight * 0.015f;

  sBtnRadius = (float)screenHeight * 0.05f;

  float spacing = (float)screenHeight * 0.12f;

  float diamondCX = (float)screenWidth  * 0.85f - (float)safeRight;
  float diamondCY = (float)screenHeight * 0.70f;

  sBtnCenterX[BTN_SHOOT]  = diamondCX + spacing;
  sBtnCenterY[BTN_SHOOT]  = diamondCY;
  sBtnCenterX[BTN_MINE]   = diamondCX - spacing;
  sBtnCenterY[BTN_MINE]   = diamondCY;
  sBtnCenterX[BTN_GS_INC] = diamondCX;
  sBtnCenterY[BTN_GS_INC] = diamondCY - spacing;
  sBtnCenterX[BTN_GS_DEC] = diamondCX;
  sBtnCenterY[BTN_GS_DEC] = diamondCY + spacing;

  sBtnCenterX[BTN_PLAYERS] = (float)screenWidth - sBtnRadius - 10.0f - (float)safeRight;
  sBtnCenterY[BTN_PLAYERS] = sBtnRadius + 10.0f + (float)safeTop;

  sStickActive = false;
  sStickDirection = TNONE;
  sFirePressed = false;
  sLayMineFlag = false;
  sGunsightChange = 0;
  sGsIncHeldTicks = 0;
  sGsDecHeldTicks = 0;
  sLastBtnTouchTime = 0;
  sLastStickTouchTime = 0;
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

      {
        int btn = hitTestButtons(px, py);
        if (btn >= 0 && !sBtnActive[btn]) {
          pressButton(btn, fid);
          sLastBtnTouchTime = SDL_GetTicks();
          return true;
        }
      }

      if (!sStickActive && isInStickZone(px, py)) {
        sLastStickTouchTime = SDL_GetTicks();
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
        sLastStickTouchTime = SDL_GetTicks();

        float dx = sStickCurrentX - sStickCenterX;
        float dy = sStickCurrentY - sStickCenterY;
        sStickDirection = directionFromOffset(dx, dy);
        return true;
      }
      break;

    case SDL_EVENT_FINGER_UP:
      fid = event->tfinger.fingerID;

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

/* ------------------------------------------------------- */

/* Compute fade multiplier (0.0 = invisible, 1.0 = full) based on elapsed time */
static float computeFade(Uint64 lastTouchTime) {
  if (lastTouchTime == 0) return 0.0f;
  Uint64 now = SDL_GetTicks();
  Uint64 elapsed = now - lastTouchTime;
  if (elapsed < CONTROL_VISIBLE_MS) return 1.0f;
  Uint64 fadeElapsed = elapsed - CONTROL_VISIBLE_MS;
  if (fadeElapsed >= CONTROL_FADE_MS) return 0.0f;
  return 1.0f - (float)fadeElapsed / (float)CONTROL_FADE_MS;
}

static void renderButton(SDL_Renderer *renderer, int btn, const char *label,
                          Uint8 r, Uint8 g, Uint8 b, float fade) {
  /* Base opacity: very transparent when idle, slightly more when active */
  float baseAlpha = sBtnActive[btn] ? 80.0f : 25.0f;
  Uint8 alpha = (Uint8)(baseAlpha * fade);
  if (alpha == 0) return;

  renderFilledCircle(renderer, sBtnCenterX[btn], sBtnCenterY[btn],
                     sBtnRadius, r, g, b, alpha);
  Uint8 outlineAlpha = (Uint8)((baseAlpha + 30.0f) * fade);
  if (outlineAlpha > 255) outlineAlpha = 255;
  renderCircleOutline(renderer, sBtnCenterX[btn], sBtnCenterY[btn],
                      sBtnRadius, r, g, b, outlineAlpha);

  {
    Uint8 textAlpha = (Uint8)((sBtnActive[btn] ? 200.0f : 100.0f) * fade);
    int len = 0;
    while (label[len]) len++;
    float textW = (float)(len * 8);
    float textX = sBtnCenterX[btn] - textW * 0.5f;
    float textY = sBtnCenterY[btn] - 4.0f;
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, textAlpha);
    SDL_RenderDebugText(renderer, textX, textY, label);
  }
}

void touchInputRender(SDL_Renderer *renderer) {
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

  /* Buttons: visible while any button is active, then fade after release */
  bool anyBtnActive = false;
  for (int i = 0; i < BTN_COUNT; i++) {
    if (sBtnActive[i]) { anyBtnActive = true; break; }
  }
  if (anyBtnActive) sLastBtnTouchTime = SDL_GetTicks();

  float btnFade = computeFade(sLastBtnTouchTime);

  if (btnFade > 0.0f) {
    renderButton(renderer, BTN_SHOOT,  "FIRE", 255,  80,  80, btnFade);
    renderButton(renderer, BTN_MINE,   "MINE", 255, 200,  60, btnFade);
    renderButton(renderer, BTN_GS_INC, "GS+",  100, 180, 255, btnFade);
    renderButton(renderer, BTN_GS_DEC, "GS-",  100, 180, 255, btnFade);

    {
      Uint8 pr = 100, pg = 200, pb = 100;
      if (playersPanelIsOpen()) { pr = 60; pg = 220; pb = 60; }
      renderButton(renderer, BTN_PLAYERS, "PLRS", pr, pg, pb, btnFade);
    }
  }

  /* Joystick: visible while active, then fade after release */
  if (sStickActive) sLastStickTouchTime = SDL_GetTicks();
  float stickFade = sStickActive ? 1.0f : computeFade(sLastStickTouchTime);

  if (stickFade > 0.0f) {
    Uint8 outerAlpha = (Uint8)(60.0f * stickFade);
    Uint8 fillAlpha  = (Uint8)(15.0f * stickFade);
    Uint8 knobAlpha  = (Uint8)(90.0f * stickFade);

    renderCircleOutline(renderer, sStickCenterX, sStickCenterY,
                        sStickOuterRadius, 255, 255, 255, outerAlpha);

    renderFilledCircle(renderer, sStickCenterX, sStickCenterY,
                       sStickOuterRadius * 0.9f, 255, 255, 255, fillAlpha);

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
                       sStickInnerRadius, 255, 255, 255, knobAlpha);
  }
}
