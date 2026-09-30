/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
*Name:          SDL3 ImGui Tablet Overlay
*Filename:      sdl3imgui_tablet.cpp
*Purpose:
*  Renders the tablet-mode ImGui overlay:
*  - Floating virtual joystick visualization
*  - Action buttons (fire, mine, gunsight, view toggle)
*  - Vertical build select bar (right side)
*  - Top bar (players, messages, settings icons)
*  - Resource bars (tank S/M/A/T + closest base S/M/A)
*  - Status grids fallback drawer
*  - Messages overlay
*********************************************************/

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

#ifdef _WIN32
#include <WinSock2.h>
#endif

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "nanosvg.h"
#include "nanosvgrast.h"
#include "dialogs/imgui_dialog_utils.h"

extern "C" {
#include "global.h"
#include "screentank.h"
#include "client_sim.h"
#include "client_command.h" /* VIEW_KIND_* — which view a button selects */
#include "client_render.h"  /* clientRenderFrame */
#include "../gamefront.h"
#include "../tiles.h"
#include "../ui_mode.h"
#include "../lang.h"
#include "input_touch.h"
#include "sdl3draw.h"
#include "build_cursor.h"
#include "scenario_panel_slot.h"
}

#include "sdl3imgui.h"
#include "sdl3imgui_tablet.h"

static bool s_statusDrawerOpen = false;

/* Static layout config, recomputed each frame */
static TabletLayoutConfig s_cfg;

/* Message fade state */
static char s_lastMsgTop[512] = {0};
static char s_lastMsgBottom[512] = {0};
static Uint64 s_msgLastChangeTime = 0;
static bool s_msgTimerInitialized = false;

/* Haptic state */
static bool s_prevFirePressed = false;
static bool s_prevMinePressed = false;

/* Damage detection */
static BYTE s_prevArmour = 0;
static bool s_armourInitialized = false;

/* Top bar icon textures (loaded from SVG on first use) */
static SDL_Texture *s_iconPlayers  = nullptr;
static SDL_Texture *s_iconMessages = nullptr;
static SDL_Texture *s_iconSettings = nullptr;
static bool s_iconsLoaded = false;

static void ensureIconsLoaded(int size) {
  if (s_iconsLoaded) return;
  s_iconsLoaded = true;
  SDL_Renderer *r = sdl3DrawGetRenderer();
  s_iconPlayers  = imguiLoadSvgIcon(r, "data/ui/players.svg", size);
  s_iconMessages = imguiLoadSvgIcon(r, "data/ui/messages.svg", size);
  s_iconSettings = imguiLoadSvgIcon(r, "data/ui/settings.svg", size);
}

/* Tile sheet dimensions */
#define TILESHEET_W 496.0f
#define TILESHEET_H 176.0f

/* -------------------------------------------------------
 * Layout configuration
 * ------------------------------------------------------- */

void tabletLayoutConfigure(TabletLayoutConfig *cfg, int screenW, int screenH,
                           int viewportX, int viewportY, int viewportW, int viewportH,
                           int effectiveZoom) {
  cfg->screenW = screenW;
  cfg->screenH = screenH;
  cfg->viewportX = viewportX;
  cfg->viewportY = viewportY;
  cfg->viewportW = viewportW;
  cfg->viewportH = viewportH;
  cfg->effectiveZoom = effectiveZoom;


  /* Safe area insets */
  cfg->safeTop = cfg->safeBottom = cfg->safeLeft = cfg->safeRight = 0.0f;
  {
    SDL_Window *win = sdl3DrawGetWindow();
    if (win) {
      SDL_Rect safeRect;
      int winW = 0, winH = 0;
      SDL_GetWindowSize(win, &winW, &winH);
      if (SDL_GetWindowSafeArea(win, &safeRect) && winW > 0 && winH > 0) {
        cfg->safeLeft   = (float)safeRect.x * (float)screenW / (float)winW;
        cfg->safeTop    = (float)safeRect.y * (float)screenH / (float)winH;
        cfg->safeRight  = (float)(winW - safeRect.x - safeRect.w) * (float)screenW / (float)winW;
        cfg->safeBottom = (float)(winH - safeRect.y - safeRect.h) * (float)screenH / (float)winH;
      }
    }
  }

  /* Gutters */
  cfg->leftGutter = viewportX;
  cfg->rightGutter = screenW - viewportX - viewportW;
  cfg->topGutter = viewportY;
  cfg->bottomGutter = screenH - viewportY - viewportH;

  /* Pixel scale: all hardcoded pixel values are authored for the reference
     space (480px height).  Scale them proportionally to the actual logical
     coordinate space so they stay the same physical size on screen. */
  float pixelScale = (float)screenH / 480.0f;
  if (pixelScale < 0.7f) pixelScale = 0.7f;

  /* Breakpoints — scale threshold too */
  cfg->showStatusGrids = (cfg->leftGutter >= (int)(100 * pixelScale));

  float scaleFactor = pixelScale;

  /* Joystick */
  cfg->joyOuterRadius = 60.0f * scaleFactor;
  cfg->joyInnerRadius = 25.0f * scaleFactor;
  cfg->joyZoneRight = 0.40f;

  /* Opacity */
  cfg->idleOpacity = 0.4f;
  cfg->activeOpacity = 1.0f;
  cfg->topBtnOpacity = 0.6f;
  cfg->joyFadeOutMs = 1000;

  /* Message fade */
  cfg->msgFadeSeconds = 5.0f;
  cfg->msgFadeDuration = 1.0f;

  /* --- Right-side layout: everything fits in the right gutter --- */
  float gutterLeft = (float)(viewportX + viewportW);  /* left edge of right gutter */
  float rightEdge = (float)screenW - cfg->safeRight;  /* right edge (safe) */
  float gutterW = rightEdge - gutterLeft;
  float bottomEdge = (float)screenH - cfg->safeBottom;
  float topEdge = cfg->safeTop;
  float gutterCenterX = gutterLeft + gutterW * 0.5f;
  float pad = gutterW * 0.03f;  /* ~3% of gutter as padding */
  if (pad < 4.0f) pad = 4.0f;

  /* Fire button — biggest, bottom-right, sized relative to gutter */
  cfg->fireRadius = gutterW * 0.18f;
  if (cfg->fireRadius < 25.0f * pixelScale) cfg->fireRadius = 25.0f * pixelScale;
  cfg->fireCenterX = rightEdge - cfg->fireRadius - pad;
  cfg->fireCenterY = bottomEdge - cfg->fireRadius - pad;

  /* Mine button — left of fire */
  cfg->mineRadius = gutterW * 0.13f;
  if (cfg->mineRadius < 18.0f * pixelScale) cfg->mineRadius = 18.0f * pixelScale;
  cfg->mineCenterX = cfg->fireCenterX - cfg->fireRadius - cfg->mineRadius - pad;
  cfg->mineCenterY = cfg->fireCenterY;
  if (cfg->mineCenterX - cfg->mineRadius < gutterLeft + pad) {
    cfg->mineCenterX = gutterLeft + pad + cfg->mineRadius;
  }

  /* Gunsight +/- — row above fire/mine, as circles */
  cfg->gsIncRadius = gutterW * 0.10f;
  if (cfg->gsIncRadius < 16.0f * pixelScale) cfg->gsIncRadius = 16.0f * pixelScale;
  cfg->gsDecRadius = cfg->gsIncRadius;
  float gsRowY = cfg->fireCenterY - cfg->fireRadius - pad - cfg->gsIncRadius;
  float gsGap = pad * 2;
  cfg->gsDecCenterX = gutterCenterX - cfg->gsDecRadius - gsGap * 0.5f;
  cfg->gsDecCenterY = gsRowY;
  cfg->gsIncCenterX = gutterCenterX + cfg->gsIncRadius + gsGap * 0.5f;
  cfg->gsIncCenterY = gsRowY;

  /* Item views / Tank view — vertical stack in gap between viewport edge and
     mine button. Every button is here; tabletViewButtonsApplyPolicies drops
     the ones the server turned off and closes the gaps. */
  cfg->pillViewRadius = cfg->gsIncRadius;
  cfg->baseViewRadius = cfg->gsIncRadius;
  cfg->allyViewRadius = cfg->gsIncRadius;
  cfg->tankViewRadius = cfg->gsIncRadius;
  float viewGapLeft = gutterLeft;
  float viewGapRight = cfg->mineCenterX - cfg->mineRadius;
  float viewBtnX = (viewGapLeft + viewGapRight) * 0.5f;
  cfg->viewBtnGap = pad;
  cfg->pillViewCenterX = viewBtnX;
  cfg->baseViewCenterX = viewBtnX;
  cfg->allyViewCenterX = viewBtnX;
  cfg->tankViewCenterX = viewBtnX;
  cfg->showPillViewBtn = true;
  cfg->showBaseViewBtn = true;
  cfg->showAllyViewBtn = true;
  {
    float step = cfg->pillViewRadius * 2.0f + cfg->viewBtnGap;
    cfg->pillViewCenterY = cfg->fireCenterY;  /* bottom — aligned with fire/mine */
    cfg->baseViewCenterY = cfg->pillViewCenterY - step;
    cfg->allyViewCenterY = cfg->baseViewCenterY - step;
    cfg->tankViewCenterY = cfg->allyViewCenterY - step;
  }

  /* --- Top bar buttons — spread across gutter --- */
  cfg->topBtnSize = gutterW * 0.25f;
  if (cfg->topBtnSize < 30.0f * pixelScale) cfg->topBtnSize = 30.0f * pixelScale;
  if (cfg->topBtnSize > 52.0f * pixelScale) cfg->topBtnSize = 52.0f * pixelScale;
  cfg->topBtnY = topEdge + pad;
  float topBtnGap = (gutterW - pad * 2 - cfg->topBtnSize * 3) / 2.0f;
  if (topBtnGap < pad) topBtnGap = pad;
  cfg->playersBtnX = gutterLeft + pad;
  cfg->msgBtnX = cfg->playersBtnX + cfg->topBtnSize + topBtnGap;
  cfg->cogBtnX = cfg->msgBtnX + cfg->topBtnSize + topBtnGap;

  /* --- Build bar — 2-column grid, right of resource bars --- */
  cfg->buildSpacing = pad;
  float buildBarLeft = cfg->playersBtnX + cfg->topBtnSize + pad;
  float buildAvailW = rightEdge - buildBarLeft - pad;  /* space to right edge */
  float buildBtnPad = 16.0f;
  /* Size icons to fit 2 columns + spacing + window padding within available width */
  float maxIconFromSpace = (buildAvailW - 12.0f - cfg->buildSpacing - buildBtnPad * 2) / 2.0f;
  cfg->buildIconSize = gutterW * 0.18f;
  if (cfg->buildIconSize < 24.0f * pixelScale) cfg->buildIconSize = 24.0f * pixelScale;
  if (cfg->buildIconSize > 48.0f * pixelScale) cfg->buildIconSize = 48.0f * pixelScale;
  if (cfg->buildIconSize > maxIconFromSpace) cfg->buildIconSize = maxIconFromSpace;
  float buildBtnSize = cfg->buildIconSize + buildBtnPad;
  float buildGridW = buildBtnSize * 2 + cfg->buildSpacing;
  float buildGridH = buildBtnSize * 3 + cfg->buildSpacing * 2;
  cfg->buildBarX = buildBarLeft;
  /* Align top with tank stock bars */
  float buildRegionTop = cfg->topBtnY + cfg->topBtnSize + pad * 2;
  cfg->buildBarY = buildRegionTop;

  /* --- Resource bars — left of build bar, compact like the main game --- */
  cfg->barsW = cfg->buildBarX - gutterLeft - pad * 2;
  if (cfg->barsW < 30.0f * pixelScale) cfg->barsW = 30.0f * pixelScale;
  if (cfg->barsW > 100.0f * pixelScale) cfg->barsW = 100.0f * pixelScale;
  cfg->barsH = 280.0f;  /* tall bars, readable on tablet */
  cfg->tankBarsX = cfg->playersBtnX;
  cfg->tankBarsY = buildRegionTop;
  cfg->baseBarsX = cfg->tankBarsX;
  cfg->baseBarsY = cfg->tankBarsY + cfg->barsH;

  /* --- Status grids (left gutter) --- */
  cfg->tanksGridX = cfg->safeLeft + 4.0f * pixelScale;
  cfg->tanksGridY = cfg->safeTop + 4.0f * pixelScale;
  float availW = (float)cfg->leftGutter - cfg->safeLeft - 8.0f * pixelScale;
  cfg->statusGridScale = availW / (90.0f * pixelScale);
  if (cfg->statusGridScale > 2.0f) cfg->statusGridScale = 2.0f;
  if (cfg->statusGridScale < 0.5f) cfg->statusGridScale = 0.5f;
  float gridH = 66.0f * pixelScale * cfg->statusGridScale;
  float gridGap = 4.0f * pixelScale * cfg->statusGridScale;
  cfg->pillsGridX = cfg->tanksGridX;
  cfg->pillsGridY = cfg->tanksGridY + gridH + gridGap;
  cfg->basesGridX = cfg->tanksGridX;
  cfg->basesGridY = cfg->pillsGridY + gridH + gridGap;

  /* --- Messages overlay — bottom center of viewport --- */
  cfg->msgOverlayW = (float)viewportW;
  cfg->msgOverlayX = (float)viewportX;
  cfg->msgOverlayY = (float)(viewportY + viewportH) - 76.0f * pixelScale;

  /* --- Scroll joystick — in the gap between build/resource area and gunsight buttons --- */
  float scrollTop = buildRegionTop + buildGridH + pad * 2;
  float scrollBottom = gsRowY - cfg->gsIncRadius - pad * 2;
  cfg->scrollJoyX = gutterLeft;
  cfg->scrollJoyY = scrollTop;
  cfg->scrollJoyW = gutterW;
  cfg->scrollJoyH = scrollBottom - scrollTop;
  cfg->scrollJoyOuterRadius = cfg->scrollJoyH * 0.40f;
  if (cfg->scrollJoyOuterRadius > gutterW * 0.30f) cfg->scrollJoyOuterRadius = gutterW * 0.30f;
  if (cfg->scrollJoyOuterRadius < 20.0f * pixelScale) cfg->scrollJoyOuterRadius = 20.0f * pixelScale;
  cfg->scrollJoyInnerRadius = cfg->scrollJoyOuterRadius * 0.42f;

  /* --- Scenario panel — top-right of the game view, left of the gutter --- */
  scnPanelSlotRect(screenW, screenH, (float)viewportX, (float)viewportY,
                   (float)viewportW, (float)viewportH, pad,
                   &cfg->scnSlotX, &cfg->scnSlotY, &cfg->scnSlotSide);
}

/* Drop the view buttons whose category the server turned off and stack the
   rest back up from the pill slot, so the column has no holes in it. Runs
   after tabletLayoutConfigure and before anything reads the slots (the
   chrome behind them takes its top edge from the tank button). */
static void tabletViewButtonsApplyPolicies(TabletLayoutConfig *cfg, ClientSim *cs) {
  cfg->showPillViewBtn = clientSimGetViewPolicy(cs, viewCategoryPill) != viewPolicyOff;
  cfg->showBaseViewBtn = clientSimGetViewPolicy(cs, viewCategoryBase) != viewPolicyOff;
  cfg->showAllyViewBtn = clientSimGetViewPolicy(cs, viewCategoryAlly) != viewPolicyOff;

  float step = cfg->pillViewRadius * 2.0f + cfg->viewBtnGap;
  float y = cfg->fireCenterY;   /* bottom of the column */
  if (cfg->showPillViewBtn) { cfg->pillViewCenterY = y; y -= step; }
  if (cfg->showBaseViewBtn) { cfg->baseViewCenterY = y; y -= step; }
  if (cfg->showAllyViewBtn) { cfg->allyViewCenterY = y; y -= step; }
  cfg->tankViewCenterY = y;
}

/* -------------------------------------------------------
 * Beveled chrome background (matches desktop background.bmp style)
 * ------------------------------------------------------- */

/* Draw a recessed (inset) beveled rectangle — dark top/left, light bottom/right,
   black fill inside.  border is the bevel thickness in pixels. */
static void drawInsetRect(ImDrawList *dl, float x, float y, float w, float h,
                          float border, ImU32 colLight, ImU32 colDark, ImU32 colFill) {
  /* Dark edge on top and left */
  dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + border), colDark);              /* top */
  dl->AddRectFilled(ImVec2(x, y + border), ImVec2(x + border, y + h), colDark);     /* left */
  /* Light edge on bottom and right */
  dl->AddRectFilled(ImVec2(x, y + h - border), ImVec2(x + w, y + h), colLight);     /* bottom */
  dl->AddRectFilled(ImVec2(x + w - border, y), ImVec2(x + w, y + h - border), colLight); /* right */
  /* Black fill */
  dl->AddRectFilled(ImVec2(x + border, y + border),
                    ImVec2(x + w - border, y + h - border), colFill);
}

/* Draw a raised (outset) beveled rectangle — light top/left, dark bottom/right,
   medium gray fill inside. */
static void drawRaisedRect(ImDrawList *dl, float x, float y, float w, float h,
                           float border, ImU32 colLight, ImU32 colDark, ImU32 colFill) {
  /* Light edge on top and left */
  dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + border), colLight);             /* top */
  dl->AddRectFilled(ImVec2(x, y + border), ImVec2(x + border, y + h), colLight);    /* left */
  /* Dark edge on bottom and right */
  dl->AddRectFilled(ImVec2(x, y + h - border), ImVec2(x + w, y + h), colDark);      /* bottom */
  dl->AddRectFilled(ImVec2(x + w - border, y), ImVec2(x + w, y + h - border), colDark); /* right */
  /* Fill */
  dl->AddRectFilled(ImVec2(x + border, y + border),
                    ImVec2(x + w - border, y + h - border), colFill);
}

static void renderTabletBackground(void) {
  /* Panel borders for the gutter UI elements.  The chrome gray fill and
     viewport bevel are drawn earlier via SDL in sdl3DrawMainScreen()
     (sdl3draw.c) so they appear behind the game tiles.  These borders
     use ImGui's BackgroundDrawList which renders behind ImGui windows
     but on top of SDL content — correct for the gutter panels which
     are all ImGui-drawn. */
  const TabletLayoutConfig &c = s_cfg;
  float ps = (float)c.screenH / 480.0f;
  if (ps < 0.7f) ps = 0.7f;
  float border = 2.0f * ps;  /* bevel thickness — 2px at reference 480p */

  ImU32 colLight  = IM_COL32(160, 160, 160, 255);  /* highlight edge */
  ImU32 colDark   = IM_COL32(64,  64,  64,  255);  /* shadow edge */
  ImU32 colBlack  = IM_COL32(0,   0,   0,   255);  /* recessed fill */

  ImDrawList *dl = ImGui::GetBackgroundDrawList();

  /* --- Left gutter: status grids (tanks, pills, bases) --- */
  if (c.showStatusGrids) {
    float gridW = 90.0f * ps * c.statusGridScale;
    float gridH = 66.0f * ps * c.statusGridScale;
    float gridPad = 2.0f * ps;

    drawInsetRect(dl,
      c.tanksGridX - gridPad - border, c.tanksGridY - gridPad - border,
      gridW + (gridPad + border) * 2, gridH + (gridPad + border) * 2,
      border, colLight, colDark, colBlack);

    drawInsetRect(dl,
      c.pillsGridX - gridPad - border, c.pillsGridY - gridPad - border,
      gridW + (gridPad + border) * 2, gridH + (gridPad + border) * 2,
      border, colLight, colDark, colBlack);

    drawInsetRect(dl,
      c.basesGridX - gridPad - border, c.basesGridY - gridPad - border,
      gridW + (gridPad + border) * 2, gridH + (gridPad + border) * 2,
      border, colLight, colDark, colBlack);
  }

  /* --- Right gutter: top buttons row --- */
  {
    float rowX = c.playersBtnX - 3.0f * ps;
    float rowY = c.topBtnY - 3.0f * ps;
    float rowW = (c.cogBtnX + c.topBtnSize) - c.playersBtnX + 6.0f * ps;
    float rowH = c.topBtnSize + 6.0f * ps;
    drawInsetRect(dl, rowX - border, rowY - border,
      rowW + border * 2, rowH + border * 2,
      border, colLight, colDark, colBlack);
  }

  /* --- Right gutter: resource bars area --- */
  {
    float pad = 3.0f * ps;
    drawInsetRect(dl,
      c.tankBarsX - pad - border, c.tankBarsY - pad - border,
      c.topBtnSize + (pad + border) * 2, c.barsH + (pad + border) * 2,
      border, colLight, colDark, colBlack);

    float baseH = c.barsH - 16.0f;
    drawInsetRect(dl,
      c.baseBarsX - pad - border, c.baseBarsY - pad - border,
      c.topBtnSize + (pad + border) * 2, baseH + (pad + border) * 2,
      border, colLight, colDark, colBlack);
  }

  /* --- Right gutter: build bar area --- */
  {
    float iconSize = c.buildIconSize;
    float btnPad = 8.0f;
    float btnSize = iconSize + btnPad * 2;
    float spacing = c.buildSpacing;
    int cols = 2, rows = 3;
    float gridW = btnSize * cols + spacing * (cols - 1);
    float gridH = btnSize * rows + spacing * (rows - 1);
    float pad = 3.0f * ps;
    /* Account for ImGui window padding (6px) */
    float winPadX = 6.0f;
    float winPadY = 6.0f;
    drawInsetRect(dl,
      c.buildBarX - pad - border, c.buildBarY - pad - border,
      gridW + winPadX * 2 + (pad + border) * 2,
      gridH + winPadY * 2 + (pad + border) * 2,
      border, colLight, colDark, colBlack);
  }

  /* --- Right gutter: fire/mine button area --- */
  {
    float pad = 3.0f * ps;
    /* Encompass fire, mine, pill view, and tank view buttons */
    float leftEdge = c.pillViewCenterX - c.pillViewRadius;
    float topEdge = c.tankViewCenterY - c.tankViewRadius;
    float rightEdge = c.fireCenterX + c.fireRadius;
    float bottomEdge = c.fireCenterY + c.fireRadius;
    drawInsetRect(dl,
      leftEdge - pad - border, topEdge - pad - border,
      (rightEdge - leftEdge) + (pad + border) * 2,
      (bottomEdge - topEdge) + (pad + border) * 2,
      border, colLight, colDark, colBlack);
  }

  /* --- Right gutter: gunsight +/- buttons --- */
  {
    float pad = 3.0f * ps;
    float leftEdge = c.gsDecCenterX - c.gsDecRadius;
    float topEdge = c.gsDecCenterY - c.gsDecRadius;
    float rightEdge = c.gsIncCenterX + c.gsIncRadius;
    float bottomEdge = c.gsIncCenterY + c.gsIncRadius;
    drawInsetRect(dl,
      leftEdge - pad - border, topEdge - pad - border,
      (rightEdge - leftEdge) + (pad + border) * 2,
      (bottomEdge - topEdge) + (pad + border) * 2,
      border, colLight, colDark, colBlack);
  }

  /* --- Bottom: messages overlay area --- */
  {
    float pad = 3.0f * ps;
    float msgH = 76.0f * ps;  /* matches renderMessagesOverlay height */
    drawInsetRect(dl,
      c.msgOverlayX - pad - border, c.msgOverlayY - pad - border,
      c.msgOverlayW + (pad + border) * 2, msgH + (pad + border) * 2,
      border, colLight, colDark, colBlack);
  }
}

/* -------------------------------------------------------
 * Alpha helper
 * ------------------------------------------------------- */

static ImU32 scaleAlpha(ImU32 col, float alpha) {
  int a = (int)((float)((col >> 24) & 0xFF) * alpha);
  if (a < 0) a = 0;
  if (a > 255) a = 255;
  return (col & 0x00FFFFFF) | ((ImU32)a << 24);
}

/* -------------------------------------------------------
 * Joystick overlay
 * ------------------------------------------------------- */

static void renderJoystickOverlay(void) {
  float ax, ay, tx, ty;
  bool active;
  Uint64 releaseTime;
  inputTouchGetJoystickState(&ax, &ay, &tx, &ty, &active, &releaseTime);

  float alpha = 0.0f;
  if (active) {
    alpha = 1.0f;
  } else if (releaseTime > 0) {
    Uint64 elapsed = SDL_GetTicks() - releaseTime;
    if (elapsed < s_cfg.joyFadeOutMs) {
      alpha = 1.0f - (float)elapsed / (float)s_cfg.joyFadeOutMs;
    }
  }

  if (alpha <= 0.0f) return;

  ImDrawList *dl = ImGui::GetForegroundDrawList();
  dl->AddCircleFilled(ImVec2(ax, ay), s_cfg.joyOuterRadius,
                       scaleAlpha(IM_COL32(200, 200, 200, 80), alpha), 32);
  dl->AddCircle(ImVec2(ax, ay), s_cfg.joyOuterRadius,
                scaleAlpha(IM_COL32(200, 200, 200, 120), alpha), 32, 2.0f);
  dl->AddCircleFilled(ImVec2(tx, ty), s_cfg.joyInnerRadius,
                       scaleAlpha(IM_COL32(200, 200, 200, 160), alpha), 24);
  dl->AddCircle(ImVec2(tx, ty), s_cfg.joyInnerRadius,
                scaleAlpha(IM_COL32(255, 255, 255, 180), alpha), 24, 2.0f);
}

/* -------------------------------------------------------
 * Scroll joystick overlay
 * ------------------------------------------------------- */

static void renderScrollJoystickOverlay(void) {
  float ax, ay, tx, ty;
  bool active;
  Uint64 releaseTime;
  inputTouchGetScrollJoystickState(&ax, &ay, &tx, &ty, &active, &releaseTime);

  float alpha = 0.0f;
  if (active) {
    alpha = 1.0f;
  } else if (releaseTime > 0) {
    Uint64 elapsed = SDL_GetTicks() - releaseTime;
    if (elapsed < s_cfg.joyFadeOutMs) {
      alpha = 1.0f - (float)elapsed / (float)s_cfg.joyFadeOutMs;
    }
  }

  /* Draw zone hint (always visible at low opacity) */
  if (s_cfg.scrollJoyH > 10.0f) {
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    float zoneAlpha = 0.15f;
    float cx = s_cfg.scrollJoyX + s_cfg.scrollJoyW * 0.5f;
    float cy = s_cfg.scrollJoyY + s_cfg.scrollJoyH * 0.5f;

    /* Draw a crosshair hint to indicate scroll directions */
    float hintR = s_cfg.scrollJoyOuterRadius * 0.5f;
    ImU32 hintCol = scaleAlpha(IM_COL32(200, 200, 200, 100), zoneAlpha);
    dl->AddCircle(ImVec2(cx, cy), hintR, hintCol, 24, 1.5f);

    /* Small arrow triangles */
    float arrowDist = hintR * 1.4f;
    float arrowSize = hintR * 0.3f;
    /* Up arrow */
    dl->AddTriangleFilled(
      ImVec2(cx, cy - arrowDist - arrowSize),
      ImVec2(cx - arrowSize * 0.6f, cy - arrowDist + arrowSize * 0.3f),
      ImVec2(cx + arrowSize * 0.6f, cy - arrowDist + arrowSize * 0.3f),
      hintCol);
    /* Down arrow */
    dl->AddTriangleFilled(
      ImVec2(cx, cy + arrowDist + arrowSize),
      ImVec2(cx - arrowSize * 0.6f, cy + arrowDist - arrowSize * 0.3f),
      ImVec2(cx + arrowSize * 0.6f, cy + arrowDist - arrowSize * 0.3f),
      hintCol);
    /* Left arrow */
    dl->AddTriangleFilled(
      ImVec2(cx - arrowDist - arrowSize, cy),
      ImVec2(cx - arrowDist + arrowSize * 0.3f, cy - arrowSize * 0.6f),
      ImVec2(cx - arrowDist + arrowSize * 0.3f, cy + arrowSize * 0.6f),
      hintCol);
    /* Right arrow */
    dl->AddTriangleFilled(
      ImVec2(cx + arrowDist + arrowSize, cy),
      ImVec2(cx + arrowDist - arrowSize * 0.3f, cy - arrowSize * 0.6f),
      ImVec2(cx + arrowDist - arrowSize * 0.3f, cy + arrowSize * 0.6f),
      hintCol);
  }

  if (alpha <= 0.0f) return;

  ImDrawList *dl = ImGui::GetForegroundDrawList();
  dl->AddCircleFilled(ImVec2(ax, ay), s_cfg.scrollJoyOuterRadius,
                       scaleAlpha(IM_COL32(100, 150, 200, 80), alpha), 32);
  dl->AddCircle(ImVec2(ax, ay), s_cfg.scrollJoyOuterRadius,
                scaleAlpha(IM_COL32(100, 150, 200, 120), alpha), 32, 2.0f);
  dl->AddCircleFilled(ImVec2(tx, ty), s_cfg.scrollJoyInnerRadius,
                       scaleAlpha(IM_COL32(100, 150, 200, 160), alpha), 24);
  dl->AddCircle(ImVec2(tx, ty), s_cfg.scrollJoyInnerRadius,
                scaleAlpha(IM_COL32(150, 200, 255, 180), alpha), 24, 2.0f);
}

/* Scroll rate limiter (matches desktop INPUT_SCROLL_WAIT_TIME = 3) */
static BYTE s_scrollKeyCount = 0;

static void processScrollJoystick(ClientSim *cs) {
  int scrollX = 0, scrollY = 0;
  if (!inputTouchGetScrollDirection(&scrollX, &scrollY)) {
    s_scrollKeyCount = 0;
    return;
  }

  s_scrollKeyCount++;
  if (s_scrollKeyCount >= 3) {
    s_scrollKeyCount = 0;
    if (scrollY < 0) clientRenderFrame(cs, up);
    if (scrollY > 0) clientRenderFrame(cs, down);
    if (scrollX < 0) clientRenderFrame(cs, left);
    if (scrollX > 0) clientRenderFrame(cs, right);
  }
}

/* Viewport drag-to-scroll with smooth sub-tile pixel offset.
   Accumulates drag pixels and only commits a full tile scroll
   to the engine when the offset reaches one tile width. */
static float s_vpDragAccumX = 0.0f;
static float s_vpDragAccumY = 0.0f;

static void processViewportDragScroll(ClientSim *cs) {
  float dx = 0.0f, dy = 0.0f;
  if (!inputTouchGetViewportDragDelta(&dx, &dy)) {
    /* Drag ended or finger stopped — reset the pixel offset */
    if (s_vpDragAccumX != 0.0f || s_vpDragAccumY != 0.0f) {
      s_vpDragAccumX = 0.0f;
      s_vpDragAccumY = 0.0f;
      sdl3DrawSetDragOffset(0, 0);
    }
    return;
  }

  /* Accumulate pixel movement (inverted for natural scrolling:
     drag right → content moves right → viewport scrolls left) */
  s_vpDragAccumX -= dx;
  s_vpDragAccumY -= dy;

  /* Get tile size in screen pixels */
  int vpX, vpY, vpW, vpH, vpZoom;
  sdl3DrawGetTabletViewport(&vpX, &vpY, &vpW, &vpH, &vpZoom);
  int tilePx = TILE_SIZE_X * vpZoom;
  if (tilePx < 1) tilePx = 1;

  /* Commit full tile scrolls to the engine */
  while (s_vpDragAccumX >= tilePx)  { clientRenderFrame(cs, right); s_vpDragAccumX -= tilePx; }
  while (s_vpDragAccumX <= -tilePx) { clientRenderFrame(cs, left);  s_vpDragAccumX += tilePx; }
  while (s_vpDragAccumY >= tilePx)  { clientRenderFrame(cs, down);  s_vpDragAccumY -= tilePx; }
  while (s_vpDragAccumY <= -tilePx) { clientRenderFrame(cs, up);    s_vpDragAccumY += tilePx; }

  /* Set the sub-tile pixel offset for smooth rendering */
  sdl3DrawSetDragOffset((int)s_vpDragAccumX, (int)s_vpDragAccumY);
}

/* -------------------------------------------------------
 * Fire / Mine buttons
 * ------------------------------------------------------- */

static void renderFireMineButtons(void) {
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  bool fireActive = inputTouchIsButtonHeld(TOUCH_BTN_FIRE);
  bool mineActive = inputTouchIsButtonHeld(TOUCH_BTN_MINE);

  /* Haptic feedback on press edges */
  if (fireActive && !s_prevFirePressed) {
    inputTouchTriggerHaptic(0.6f, 30);
  }
  s_prevFirePressed = fireActive;

  if (mineActive && !s_prevMinePressed) {
    inputTouchTriggerHaptic(0.8f, 60);
  }
  s_prevMinePressed = mineActive;

  float fireAlpha = fireActive ? s_cfg.activeOpacity : s_cfg.idleOpacity;
  float mineAlpha = mineActive ? s_cfg.activeOpacity : s_cfg.idleOpacity;

  /* Fire button */
  dl->AddCircleFilled(ImVec2(s_cfg.fireCenterX, s_cfg.fireCenterY), s_cfg.fireRadius,
                       scaleAlpha(IM_COL32(0, 0, 0, 200), fireAlpha), 32);
  dl->AddCircle(ImVec2(s_cfg.fireCenterX, s_cfg.fireCenterY), s_cfg.fireRadius,
                scaleAlpha(IM_COL32(255, 255, 255, 220), fireAlpha), 32, 2.0f);
  SDL_Texture *fireTilesTex = sdl3DrawGetTilesTexture();
  if (fireTilesTex) {
    float r = s_cfg.fireRadius;
    float cx = s_cfg.fireCenterX, cy = s_cfg.fireCenterY;
    ImU32 tint = scaleAlpha(IM_COL32(255, 255, 255, 255), fireAlpha);

    /* Tank facing right, shifted left to make room for shells */
    float tankSize = r * 0.825f;
    float tankLeft = cx - r * 0.41f;
    ImVec2 tMin(tankLeft, cy - tankSize * 0.5f);
    ImVec2 tMax(tankLeft + tankSize, cy + tankSize * 0.5f);
    ImVec2 tUv0((float)TANK_SELF_4_X / TILESHEET_W, (float)TANK_SELF_4_Y / TILESHEET_H);
    ImVec2 tUv1((float)(TANK_SELF_4_X + TILE_SIZE_X) / TILESHEET_W,
                (float)(TANK_SELF_4_Y + TILE_SIZE_Y) / TILESHEET_H);
    /* The live sheet, drawn through ImGui, so the bracket both point-samples
       this pixel art and keeps the backend from leaving the map's own
       drawing on the sampler it bound with. */
    imguiPushNearestSamplingOn(dl);
    dl->AddImage((ImTextureID)fireTilesTex, tMin, tMax, tUv0, tUv1, tint);

    /* Two shells to the right of the tank */
    ImVec2 sUv0((float)SHELL_4_X / TILESHEET_W, (float)SHELL_4_Y / TILESHEET_H);
    ImVec2 sUv1((float)(SHELL_4_X + SHELL_4_WIDTH) / TILESHEET_W,
                (float)(SHELL_4_Y + SHELL_4_HEIGHT) / TILESHEET_H);
    float shellW = r * 0.225f;
    float shellH = shellW * ((float)SHELL_4_HEIGHT / SHELL_4_WIDTH);
    float shellX = tankLeft + tankSize + r * 0.04f;
    /* Shell 1 */
    dl->AddImage((ImTextureID)fireTilesTex,
                 ImVec2(shellX, cy - shellH * 0.5f - shellH * 0.4f),
                 ImVec2(shellX + shellW, cy + shellH * 0.5f - shellH * 0.4f),
                 sUv0, sUv1, tint);
    /* Shell 2 */
    dl->AddImage((ImTextureID)fireTilesTex,
                 ImVec2(shellX + shellW * 0.5f, cy - shellH * 0.5f + shellH * 0.4f),
                 ImVec2(shellX + shellW * 1.5f, cy + shellH * 0.5f + shellH * 0.4f),
                 sUv0, sUv1, tint);
    imguiPopNearestSamplingOn(dl);
  }

  /* Mine button */
  dl->AddCircleFilled(ImVec2(s_cfg.mineCenterX, s_cfg.mineCenterY), s_cfg.mineRadius,
                       scaleAlpha(IM_COL32(0, 0, 0, 200), mineAlpha), 32);
  dl->AddCircle(ImVec2(s_cfg.mineCenterX, s_cfg.mineCenterY), s_cfg.mineRadius,
                scaleAlpha(IM_COL32(255, 255, 255, 220), mineAlpha), 32, 2.0f);
  SDL_Texture *tilesTex = sdl3DrawGetTilesTexture();
  if (tilesTex) {
    float iconHalf = s_cfg.mineRadius * 0.65f;
    float cx = s_cfg.mineCenterX + s_cfg.mineRadius * 0.05f, cy = s_cfg.mineCenterY;
    ImVec2 pMin(cx - iconHalf, cy - iconHalf);
    ImVec2 pMax(cx + iconHalf, cy + iconHalf);
    ImVec2 uv0((float)MINE_X / TILESHEET_W, (float)MINE_Y / TILESHEET_H);
    ImVec2 uv1((float)(MINE_X + TILE_SIZE_X) / TILESHEET_W,
                (float)(MINE_Y + TILE_SIZE_Y) / TILESHEET_H);
    ImU32 tint = scaleAlpha(IM_COL32(255, 255, 255, 255), mineAlpha);
    imguiPushNearestSamplingOn(dl);
    dl->AddImage((ImTextureID)tilesTex, pMin, pMax, uv0, uv1, tint);
    imguiPopNearestSamplingOn(dl);
  }
}

/* -------------------------------------------------------
 * Gunsight increase / decrease buttons
 * ------------------------------------------------------- */

static void renderGunsightButtons(void) {
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  bool decActive = inputTouchIsButtonHeld(TOUCH_BTN_GS_DECREASE);
  bool incActive = inputTouchIsButtonHeld(TOUCH_BTN_GS_INCREASE);

  float decAlpha = decActive ? s_cfg.activeOpacity : s_cfg.idleOpacity;
  float incAlpha = incActive ? s_cfg.activeOpacity : s_cfg.idleOpacity;

  /* Decrease button */
  dl->AddCircleFilled(ImVec2(s_cfg.gsDecCenterX, s_cfg.gsDecCenterY), s_cfg.gsDecRadius,
                       scaleAlpha(IM_COL32(0, 0, 0, 200), decAlpha), 32);
  dl->AddCircle(ImVec2(s_cfg.gsDecCenterX, s_cfg.gsDecCenterY), s_cfg.gsDecRadius,
                scaleAlpha(IM_COL32(255, 255, 255, 220), decAlpha), 32, 2.0f);
  const char *decLabel = "-";
  ImVec2 decSize = ImGui::CalcTextSize(decLabel);
  dl->AddText(ImVec2(s_cfg.gsDecCenterX - decSize.x * 0.5f, s_cfg.gsDecCenterY - decSize.y * 0.5f),
              scaleAlpha(IM_COL32(255, 255, 255, 220), decAlpha), decLabel);

  /* Increase button */
  dl->AddCircleFilled(ImVec2(s_cfg.gsIncCenterX, s_cfg.gsIncCenterY), s_cfg.gsIncRadius,
                       scaleAlpha(IM_COL32(0, 0, 0, 200), incAlpha), 32);
  dl->AddCircle(ImVec2(s_cfg.gsIncCenterX, s_cfg.gsIncCenterY), s_cfg.gsIncRadius,
                scaleAlpha(IM_COL32(255, 255, 255, 220), incAlpha), 32, 2.0f);
  const char *incLabel = "+";
  ImVec2 incSize = ImGui::CalcTextSize(incLabel);
  dl->AddText(ImVec2(s_cfg.gsIncCenterX - incSize.x * 0.5f, s_cfg.gsIncCenterY - incSize.y * 0.5f),
              scaleAlpha(IM_COL32(255, 255, 255, 220), incAlpha), incLabel);
}

/* -------------------------------------------------------
 * View buttons (pill / base / allied tank / tank view)
 * ------------------------------------------------------- */

/* The view buttons that are on screen, bottom of the column first. A category
   the server's visibility rules switched off has no row, so it is neither
   drawn nor hit-tested. Fills at most 4 rows; returns how many. */
struct TabletViewButton {
  TouchButtonID id;
  float         cx, cy, r;
  int           tileX, tileY;
};

static int tabletViewButtons(TabletViewButton *out) {
  int n = 0;
  if (s_cfg.showPillViewBtn) {
    out[n++] = { TOUCH_BTN_PILL_VIEW, s_cfg.pillViewCenterX, s_cfg.pillViewCenterY,
                 s_cfg.pillViewRadius, PILL_GOOD15_X, PILL_GOOD15_Y };
  }
  if (s_cfg.showBaseViewBtn) {
    out[n++] = { TOUCH_BTN_BASE_VIEW, s_cfg.baseViewCenterX, s_cfg.baseViewCenterY,
                 s_cfg.baseViewRadius, BASE_GOOD_X, BASE_GOOD_Y };
  }
  if (s_cfg.showAllyViewBtn) {
    out[n++] = { TOUCH_BTN_ALLY_VIEW, s_cfg.allyViewCenterX, s_cfg.allyViewCenterY,
                 s_cfg.allyViewRadius, TANK_GOOD_0_X, TANK_GOOD_0_Y };
  }
  out[n++] = { TOUCH_BTN_TANK_VIEW, s_cfg.tankViewCenterX, s_cfg.tankViewCenterY,
               s_cfg.tankViewRadius, TANK_SELF_0_X, TANK_SELF_0_Y };
  return n;
}

/* Which button the current view lights up. */
static TouchButtonID tabletSelectedViewButton(ClientSim *cs) {
  switch (clientSimGetViewKind(cs)) {
    case VIEW_KIND_PILL: return TOUCH_BTN_PILL_VIEW;
    case VIEW_KIND_BASE: return TOUCH_BTN_BASE_VIEW;
    case VIEW_KIND_ALLY: return TOUCH_BTN_ALLY_VIEW;
    default:             return TOUCH_BTN_TANK_VIEW;
  }
}

static void renderViewButtons(ClientSim *cs) {
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  SDL_Texture *tilesTex = sdl3DrawGetTilesTexture();
  TouchButtonID selectedBtn = tabletSelectedViewButton(cs);
  TabletViewButton btns[4];
  int count = tabletViewButtons(btns);

  for (int i = 0; i < count; i++) {
    const TabletViewButton &b = btns[i];
    bool active = inputTouchIsButtonHeld(b.id);
    bool selected = (b.id == selectedBtn);
    float alpha = (active || selected) ? s_cfg.activeOpacity : s_cfg.idleOpacity;
    dl->AddCircleFilled(ImVec2(b.cx, b.cy), b.r, scaleAlpha(IM_COL32(0, 0, 0, 200), alpha), 32);
    ImU32 outline = selected ? IM_COL32(255, 255, 100, 220) : IM_COL32(255, 255, 255, 220);
    dl->AddCircle(ImVec2(b.cx, b.cy), b.r, scaleAlpha(outline, alpha), 32, 2.0f);
    if (tilesTex) {
      float iconHalf = b.r * 0.65f;
      ImVec2 pMin(b.cx - iconHalf, b.cy - iconHalf);
      ImVec2 pMax(b.cx + iconHalf, b.cy + iconHalf);
      ImVec2 uv0((float)b.tileX / TILESHEET_W, (float)b.tileY / TILESHEET_H);
      ImVec2 uv1((float)(b.tileX + TILE_SIZE_X) / TILESHEET_W,
                  (float)(b.tileY + TILE_SIZE_Y) / TILESHEET_H);
      ImU32 tint = scaleAlpha(IM_COL32(255, 255, 255, 255), alpha);
      imguiPushNearestSamplingOn(dl);
      dl->AddImage((ImTextureID)tilesTex, pMin, pMax, uv0, uv1, tint);
      imguiPopNearestSamplingOn(dl);
    }
  }
}

/* -------------------------------------------------------
 * Build select bar — vertical right column
 * ------------------------------------------------------- */

static const struct { int x, y; } buildIconTiles[] = {
  { FOREST_X,       FOREST_Y       },  /* BsTrees */
  { ROAD_SOLID_X,   ROAD_SOLID_Y   },  /* BsRoad */
  { BUILD_SINGLE_X, BUILD_SINGLE_Y },  /* BsBuilding */
  { PILL_EVIL15_X,  PILL_EVIL15_Y  },  /* BsPillbox */
  { MINE_X,         MINE_Y         },  /* BsMine */
};

static void renderBuildSelectBar(ClientSim *cs) {
  SDL_Texture *tilesTex = sdl3DrawGetTilesTexture();
  if (!tilesTex) return;

  float iconSize = s_cfg.buildIconSize;
  float btnPad = 8.0f;
  float btnSize = iconSize + btnPad * 2;
  float spacing = s_cfg.buildSpacing;
  int cols = 2;
  int rows = 3;  /* 5 items in a 2x3 grid (last cell empty) */
  float gridW = btnSize * cols + spacing * (cols - 1);
  float gridH = btnSize * rows + spacing * (rows - 1);
  float startX = s_cfg.buildBarX;
  float startY = s_cfg.buildBarY;

  ImVec2 framePad = ImGui::GetStyle().FramePadding;
  float winW = gridW + 12 + framePad.x * 4;  /* account for ImGui frame padding on each button */
  float winH = gridH + 12 + framePad.y * 6;  /* frame padding on each row of buttons */
  ImGui::SetNextWindowPos(ImVec2(startX, startY));
  ImGui::SetNextWindowSize(ImVec2(winW, winH));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacing));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.5f));

  if (ImGui::Begin("##BuildBar", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_NoScrollWithMouse)) {

    buildSelect cur = clientSimGetCurrentBuildSelect(cs);
    const char *ids[] = { "##bsTree", "##bsRoad", "##bsWall", "##bsPill", "##bsMine" };
    buildSelect values[] = { BsTrees, BsRoad, BsBuilding, BsPillbox, BsMine };

    for (int i = 0; i < 5; i++) {
      bool selected = (cur == values[i]);

      ImVec2 uv0((float)buildIconTiles[i].x / TILESHEET_W,
                  (float)buildIconTiles[i].y / TILESHEET_H);
      ImVec2 uv1((float)(buildIconTiles[i].x + TILE_SIZE_X) / TILESHEET_W,
                  (float)(buildIconTiles[i].y + TILE_SIZE_Y) / TILESHEET_H);

      if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 0.3f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.65f, 0.35f, 0.9f));
      } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 0.7f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.25f, 0.8f));
      }

      imguiPushNearestSampling();
      if (ImGui::ImageButton(ids[i], (ImTextureID)tilesTex,
                              ImVec2(iconSize, iconSize), uv0, uv1)) {
        clientSimSetCurrentBuildSelect(cs, values[i]);
      }
      imguiPopNearestSampling();
      imguiHandOnHover();

      /* Touch tap fallback — ImGui buttons may not register finger events on iOS */
      ImVec2 rMin = ImGui::GetItemRectMin();
      ImVec2 rMax = ImGui::GetItemRectMax();
      if (inputTouchConsumeTapInRect(rMin.x, rMin.y, rMax.x - rMin.x, rMax.y - rMin.y)) {
        clientSimSetCurrentBuildSelect(cs, values[i]);
      }

      ImGui::PopStyleColor(2);

      /* Place two items per row */
      if (i % cols == 0) ImGui::SameLine();
    }

    /* 6th cell (bottom-right): LGM man-status indicator.
       The mine button (i=4) already called SameLine(), so the cursor
       is positioned for the next item on the same row.
       Offset cursor by FramePadding to center it like the ImageButtons.
       Draw natively via ImGui draw list to avoid scaling artifacts. */
    ImVec2 manCur = ImGui::GetCursorPos();
    manCur.x += framePad.x;
    manCur.y += framePad.y;
    ImGui::SetCursorPos(manCur);
    ImGui::Dummy(ImVec2(iconSize, iconSize));
    {
      bool manDead = false;
      TURNTYPE manAngle = 0;
      bool manReady = sdl3DrawGetManStatusState(&manDead, &manAngle);
      if (manReady) {
        ImVec2 rMin = ImGui::GetItemRectMin();
        ImDrawList *mdl = ImGui::GetWindowDrawList();
        float mcx = rMin.x + iconSize * 0.5f;
        float mcy = rMin.y + iconSize * 0.5f;
        float mr  = iconSize * 0.45f;
        float thick = iconSize * 0.06f;
        if (thick < 1.5f) thick = 1.5f;

        if (manDead) {
          mdl->AddCircleFilled(ImVec2(mcx, mcy), mr, IM_COL32(200, 80, 0, 255), 32);
        } else {
          mdl->AddCircle(ImVec2(mcx, mcy), mr, IM_COL32(255, 255, 255, 255), 32, thick);
          /* Direction arrow from centre */
          TURNTYPE a = manAngle + (TURNTYPE)BRADIANS_SOUTH;
          if (a >= (TURNTYPE)BRADIANS_MAX) a -= (TURNTYPE)BRADIANS_MAX;
          float rad = (float)(a * (2.0 * 3.14159265 / BRADIANS_MAX));
          float ax = mcx + (mr - thick) * sinf(rad);
          float ay = mcy - (mr - thick) * cosf(rad);
          mdl->AddLine(ImVec2(mcx, mcy), ImVec2(ax, ay), IM_COL32(255, 255, 255, 255), thick);
        }
      }
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

/* -------------------------------------------------------
 * Top bar buttons: Players (people icon), Message (chat), Settings (cog)
 * ------------------------------------------------------- */

static void renderTopBarButtons(ClientSim *cs) {
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  float btnSize = s_cfg.topBtnSize;
  float btnY = s_cfg.topBtnY;
  float alpha = s_cfg.topBtnOpacity;
  float radius = btnSize * 0.5f;

  ensureIconsLoaded((int)(btnSize * 2));

  /* Helper: draw one circular button with an icon texture */
  struct {
    float cx, cy;
    SDL_Texture *icon;
    const char *fallback;
  } btns[] = {
    { s_cfg.playersBtnX + radius, btnY + radius, s_iconMessages, langGetText(STR_TABLET_BTN_MSG) },
    { s_cfg.msgBtnX     + radius, btnY + radius, s_iconPlayers,  langGetText(STR_TABLET_BTN_PLY) },
    { s_cfg.cogBtnX     + radius, btnY + radius, s_iconSettings, langGetText(STR_TABLET_BTN_SET) },
  };

  for (int i = 0; i < 3; i++) {
    float cx = btns[i].cx;
    float cy = btns[i].cy;

    /* Black filled circle + white outline */
    dl->AddCircleFilled(ImVec2(cx, cy), radius,
                         scaleAlpha(IM_COL32(0, 0, 0, 200), alpha), 32);
    dl->AddCircle(ImVec2(cx, cy), radius,
                  scaleAlpha(IM_COL32(255, 255, 255, 220), alpha), 32, 2.0f);

    /* Icon (white SVG) or fallback text */
    if (btns[i].icon) {
      float iconHalf = radius * 0.65f;
      ImVec2 pMin(cx - iconHalf, cy - iconHalf);
      ImVec2 pMax(cx + iconHalf, cy + iconHalf);
      ImU32 tint = scaleAlpha(IM_COL32(255, 255, 255, 255), alpha);
      dl->AddImage((ImTextureID)btns[i].icon, pMin, pMax,
                   ImVec2(0, 0), ImVec2(1, 1), tint);
    } else {
      ImVec2 textSize = ImGui::CalcTextSize(btns[i].fallback);
      dl->AddText(ImVec2(cx - textSize.x * 0.5f, cy - textSize.y * 0.5f),
                  scaleAlpha(IM_COL32(255, 255, 255, 220), alpha), btns[i].fallback);
    }
  }

  /* Tap detection — messages */
  if (inputTouchConsumeTapInRect(s_cfg.playersBtnX, btnY, btnSize, btnSize)) {
    sdl3ImguiShowSendMsg(true);
  }
  /* Tap detection — players */
  if (inputTouchConsumeTapInRect(s_cfg.msgBtnX, btnY, btnSize, btnSize)) {
    sdl3ImguiShowPlayersPanel(true);
  }
  /* Tap detection — settings: extend tap target to right screen edge for
     easier touch targeting near the device edge */
  {
    float tapPad = btnSize * 0.25f;
    float tapX = s_cfg.cogBtnX - tapPad;
    float tapY2 = btnY - tapPad;
    float tapW = (float)s_cfg.screenW - tapX;
    float tapH = btnSize + tapPad * 2;
    if (inputTouchConsumeTapInRect(tapX, tapY2, tapW, tapH)) {
      sdl3ImguiShowSettings();
    }
  }

  /* Status drawer toggle — only when grids not in gutter */
  if (!s_cfg.showStatusGrids) {
    float statusCx = s_cfg.cogBtnX + radius;
    float statusCy = btnY + btnSize + 8 + radius;
    dl->AddCircleFilled(ImVec2(statusCx, statusCy), radius,
                         scaleAlpha(IM_COL32(0, 0, 0, 200), alpha), 32);
    dl->AddCircle(ImVec2(statusCx, statusCy), radius,
                  scaleAlpha(IM_COL32(255, 255, 255, 220), alpha), 32, 2.0f);
    const char *label = "i";
    ImVec2 textSize = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(statusCx - textSize.x * 0.5f, statusCy - textSize.y * 0.5f),
                scaleAlpha(IM_COL32(255, 255, 255, 220), alpha), label);
    if (inputTouchConsumeTapInRect(s_cfg.cogBtnX, btnY + btnSize + 8, btnSize, btnSize)) {
      s_statusDrawerOpen = !s_statusDrawerOpen;
    }
  }
}

/* -------------------------------------------------------
 * Resource bars — Tank (S/M/A/T) and Closest Base (S/M/A)
 * ------------------------------------------------------- */

static void renderResourceBars(ClientSim *cs) {
  BYTE shells, mines, armour, trees;
  sdl3DrawGetCachedTankStats(&shells, &mines, &armour, &trees);

  BYTE baseShells, baseMines, baseArmour;
  bool hasBase;
  sdl3DrawGetCachedBaseStats(&baseShells, &baseMines, &baseArmour, &hasBase);

  ImGuiWindowFlags winFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoInputs;

  /* --- Tank resource bars --- */
  {
    float x = s_cfg.tankBarsX;
    float y = s_cfg.tankBarsY;
    float w = s_cfg.topBtnSize;  /* same width as top bar buttons */
    float h = s_cfg.barsH;
    float winPad = 4.0f;
    float barW = 12.0f;
    float barGap = (w - winPad * 2 - barW * 4) / 3.0f;

    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(winPad, winPad));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.5f));

    if (ImGui::Begin("##TankBars", nullptr, winFlags)) {
      ImDrawList *dl = ImGui::GetWindowDrawList();
      ImVec2 winPos = ImGui::GetWindowPos();

      /* 4 vertical bars: S M A T */
      float maxBarH = h - winPad * 2;
      float barTop = winPos.y + winPad;
      ImU32 barColor = IM_COL32(0, 200, 0, 200);
      ImU32 bgColor = IM_COL32(40, 40, 40, 180);

      BYTE vals[4] = { shells, mines, armour, trees };
      /* Each bar fills at its own cap, so a tuned sim draws a full tank
         full rather than off the end of the bar. */
      BYTE fullShells, fullMines, fullArmour, fullTrees;
      clientSimGetTankFullStats(cs, &fullShells, &fullMines, &fullArmour, &fullTrees);
      BYTE fulls[4] = { fullShells, fullMines, fullArmour, fullTrees };

      for (int i = 0; i < 4; i++) {
        float bx = winPos.x + winPad + i * (barW + barGap);
        dl->AddRectFilled(ImVec2(bx, barTop), ImVec2(bx + barW, barTop + maxBarH), bgColor);
        float fillH = fulls[i] ? (vals[i] / (float)fulls[i]) * maxBarH : 0.0f;
        if (fillH > maxBarH) fillH = maxBarH;
        dl->AddRectFilled(ImVec2(bx, barTop + maxBarH - fillH),
                           ImVec2(bx + barW, barTop + maxBarH), barColor);
      }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
  }

  /* --- Base resource bars --- */
  {
    float x = s_cfg.baseBarsX;
    float y = s_cfg.baseBarsY;
    float w = s_cfg.topBtnSize;  /* same width as top bar buttons */
    float h = s_cfg.barsH - 16.0f;  /* slightly shorter, only 3 bars */
    float winPad = 4.0f;
    float barW = 12.0f;
    float barGap = (w - winPad * 2 - barW * 3) / 2.0f;

    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(winPad, winPad));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, hasBase ? 0.5f : 0.25f));

    if (ImGui::Begin("##BaseBars", nullptr, winFlags)) {
      ImDrawList *dl = ImGui::GetWindowDrawList();
      ImVec2 winPos = ImGui::GetWindowPos();

      float maxBarH = h - winPad * 2;
      float barTop = winPos.y + winPad;
      ImU32 barColor = hasBase ? IM_COL32(0, 200, 0, 200) : IM_COL32(60, 60, 60, 120);
      ImU32 bgColor = IM_COL32(40, 40, 40, 180);

      BYTE vals[3] = { baseShells, baseMines, baseArmour };
      /* Each bar fills at its own cap, so a tuned sim draws a full base
         full rather than off the end of the bar. */
      BYTE fullShells, fullMines, fullArmour;
      clientSimGetBaseFullStats(cs, &fullShells, &fullMines, &fullArmour);
      BYTE fulls[3] = { fullShells, fullMines, fullArmour };

      for (int i = 0; i < 3; i++) {
        float bx = winPos.x + winPad + i * (barW + barGap);
        dl->AddRectFilled(ImVec2(bx, barTop), ImVec2(bx + barW, barTop + maxBarH), bgColor);
        float fillH = (hasBase && fulls[i]) ? (vals[i] / (float)fulls[i]) * maxBarH : 0.0f;
        if (fillH > maxBarH) fillH = maxBarH;
        dl->AddRectFilled(ImVec2(bx, barTop + maxBarH - fillH),
                           ImVec2(bx + barW, barTop + maxBarH), barColor);
      }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
  }
}

/* -------------------------------------------------------
 * Status drawer (fallback when gutters too narrow)
 * ------------------------------------------------------- */

static void renderStatusDrawer(ClientSim *cs) {
  if (!s_statusDrawerOpen) return;

  ImGuiIO &io = ImGui::GetIO();
  float panelW = 280.0f;
  float panelH = io.DisplaySize.y;

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - panelW, 0));
  ImGui::SetNextWindowSize(ImVec2(panelW, panelH));

  char statusTitle[128];
  snprintf(statusTitle, sizeof(statusTitle), "%s###tabletstatus", langGetText(STR_TABLET_STATUS_TITLE));
  if (ImGui::Begin(statusTitle, &s_statusDrawerOpen,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse)) {

    /* Kills / Deaths */
    {
      int kills, deaths;
      clientSimGetKillsDeaths(cs, &kills, &deaths);
      MessageArgs args = {};
      args.number = kills;
      args.number2 = deaths;
      ImGui::TextUnformatted(langGetTextFmt(STR_TABLET_KILLS_DEATHS, &args));
    }
    ImGui::Separator();

    /* Tank resource bars */
    if (ImGui::CollapsingHeader(langGetText(STR_TABLET_TANK_RESOURCES), ImGuiTreeNodeFlags_DefaultOpen)) {
      BYTE shells, mines, armour, trees;
      clientSimGetTankStats(cs, &shells, &mines, &armour, &trees);
      BYTE fullShells, fullMines, fullArmour, fullTrees;
      clientSimGetTankFullStats(cs, &fullShells, &fullMines, &fullArmour, &fullTrees);
      ImGui::ProgressBar(fullShells ? (float)shells / fullShells : 0.0f, ImVec2(-1, 0), langGetText(STR_TABLET_SHELLS));
      ImGui::ProgressBar(fullMines  ? (float)mines  / fullMines  : 0.0f, ImVec2(-1, 0), langGetText(STR_TABLET_MINES));
      ImGui::ProgressBar(fullArmour ? (float)armour / fullArmour : 0.0f, ImVec2(-1, 0), langGetText(STR_TABLET_ARMOUR));
      ImGui::ProgressBar(fullTrees  ? (float)trees  / fullTrees  : 0.0f, ImVec2(-1, 0), langGetText(STR_TABLET_TREES));
    }

    /* Pillbox status */
    if (ImGui::CollapsingHeader(langGetText(STR_TABLET_PILLBOXES))) {
      BYTE total = clientSimGetPillCount(cs);
      int shown = 0;
      for (BYTE i = 1; i <= total; i++) {
        pillAlliance pa = clientSimGetPillAlliance(cs, i);
        /* A pillbox a scenario took off the map is not listed, and the rows
           of four are counted over the ones that are. */
        if (pa == pillOffMap) continue;
        if (shown > 0 && shown % 4 != 0) ImGui::SameLine(0, 20);
        shown++;
        ImVec4 col;
        switch (pa) {
          case pillAllie:   col = ImVec4(0.0f, 0.8f, 0.0f, 1.0f); break;
          case pillGood:    col = ImVec4(0.0f, 0.6f, 0.8f, 1.0f); break;
          case pillNeutral: col = ImVec4(0.6f, 0.6f, 0.6f, 1.0f); break;
          case pillEvil:    col = ImVec4(0.8f, 0.0f, 0.0f, 1.0f); break;
          default:          col = ImVec4(0.3f, 0.3f, 0.3f, 1.0f); break;
        }
        MessageArgs args = {};
        args.number = i;
        ImGui::TextColored(col, "%s", langGetTextFmt(STR_TABLET_PILL_FMT, &args));
      }
    }

    /* Base status */
    if (ImGui::CollapsingHeader(langGetText(STR_TABLET_BASES))) {
      BYTE total = clientSimGetBaseCount(cs);
      int shown = 0;
      for (BYTE i = 1; i <= total; i++) {
        baseAlliance ba = clientSimGetBaseAlliance(cs, i);
        /* A base a scenario took off the map is not listed, and the rows of
           four are counted over the ones that are. */
        if (ba == baseOffMap) continue;
        if (shown > 0 && shown % 4 != 0) ImGui::SameLine(0, 20);
        shown++;
        ImVec4 col;
        switch (ba) {
          case baseOwnGood: col = ImVec4(0.0f, 0.8f, 0.0f, 1.0f); break;
          case baseAllieGood: col = ImVec4(0.0f, 0.6f, 0.8f, 1.0f); break;
          case baseNeutral: col = ImVec4(0.6f, 0.6f, 0.6f, 1.0f); break;
          case baseEvil:    col = ImVec4(0.8f, 0.0f, 0.0f, 1.0f); break;
          default:          col = ImVec4(0.3f, 0.3f, 0.3f, 1.0f); break;
        }
        MessageArgs args = {};
        args.number = i;
        ImGui::TextColored(col, "%s", langGetTextFmt(STR_TABLET_BASE_FMT, &args));
      }
    }

    /* Tanks status */
    if (ImGui::CollapsingHeader(langGetText(STR_TABLET_TANKS))) {
      for (BYTE i = 1; i <= MAX_TANKS; i++) {
        tankAlliance ta = clientSimGetTankAlliance(cs, i);
        ImVec4 col;
        switch (ta) {
          case tankSelf:    col = ImVec4(0.0f, 0.8f, 0.0f, 1.0f); break;
          case tankAllie:   col = ImVec4(0.0f, 0.6f, 0.8f, 1.0f); break;
          case tankEvil:    col = ImVec4(0.8f, 0.0f, 0.0f, 1.0f); break;
          default:          col = ImVec4(0.3f, 0.3f, 0.3f, 1.0f); break;
        }
        if (ta != tankNone) {
          MessageArgs args = {};
          args.number = i;
          ImGui::TextColored(col, "%s", langGetTextFmt(STR_TABLET_TANK_FMT, &args));
          if (i % 4 != 0) ImGui::SameLine(0, 20);
        }
      }
    }
  }
  ImGui::End();
}

/* -------------------------------------------------------
 * Messages overlay (bottom center of viewport)
 * ------------------------------------------------------- */

static void renderMessagesOverlay(void) {
  const char *top = NULL, *bottom = NULL;
  sdl3DrawGetCachedMessages(&top, &bottom);

  bool hasContent = (top && top[0] != '\0') || (bottom && bottom[0] != '\0');
  if (!hasContent && !s_msgTimerInitialized) return;

  /* Detect message changes */
  bool changed = false;
  if (top && strcmp(top, s_lastMsgTop) != 0) {
    strncpy(s_lastMsgTop, top, sizeof(s_lastMsgTop) - 1);
    s_lastMsgTop[sizeof(s_lastMsgTop) - 1] = '\0';
    changed = true;
  } else if (!top && s_lastMsgTop[0] != '\0') {
    s_lastMsgTop[0] = '\0';
    changed = true;
  }
  if (bottom && strcmp(bottom, s_lastMsgBottom) != 0) {
    strncpy(s_lastMsgBottom, bottom, sizeof(s_lastMsgBottom) - 1);
    s_lastMsgBottom[sizeof(s_lastMsgBottom) - 1] = '\0';
    changed = true;
  } else if (!bottom && s_lastMsgBottom[0] != '\0') {
    s_lastMsgBottom[0] = '\0';
    changed = true;
  }

  if (changed || !s_msgTimerInitialized) {
    s_msgLastChangeTime = SDL_GetTicks();
    s_msgTimerInitialized = true;
  }

  float msgAlpha = 1.0f;
  if (s_msgTimerInitialized) {
    float elapsed = (float)(SDL_GetTicks() - s_msgLastChangeTime) / 1000.0f;
    if (elapsed > s_cfg.msgFadeSeconds) {
      float fadeProgress = (elapsed - s_cfg.msgFadeSeconds) / s_cfg.msgFadeDuration;
      msgAlpha = 1.0f - fadeProgress;
      if (msgAlpha < 0.0f) msgAlpha = 0.0f;
    }
  }

  if (msgAlpha <= 0.0f || !hasContent) return;

  float x = s_cfg.msgOverlayX;
  float y = s_cfg.msgOverlayY;
  float w = s_cfg.msgOverlayW;

  float fontScale = 1.6f;
  float lineH = ImGui::GetTextLineHeight() * fontScale;
  float h = lineH * 2.0f + 12.0f;

  ImGui::SetNextWindowPos(ImVec2(x, y));
  ImGui::SetNextWindowSize(ImVec2(w, h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.5f * msgAlpha));

  if (ImGui::Begin("##MsgOverlay", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoInputs |
                    ImGuiWindowFlags_NoFocusOnAppearing |
                    ImGuiWindowFlags_NoBringToFrontOnFocus)) {

    float origScale = ImGui::GetFont()->Scale;
    ImGui::GetFont()->Scale *= fontScale;
    ImGui::PushFont(ImGui::GetFont());

    ImU32 textCol = scaleAlpha(IM_COL32(200, 200, 200, 220), msgAlpha);
    float availW = w - 16.0f;
    ImVec2 winPos = ImGui::GetWindowPos();

    if (top && top[0] != '\0') {
      const char *dispTop = top;
      if (ImGui::CalcTextSize(top).x > availW) {
        const char *p = top;
        while (*p) {
          if (ImGui::CalcTextSize(p).x <= availW) break;
          if ((*p & 0x80) == 0) p++;
          else if ((*p & 0xE0) == 0xC0) p += 2;
          else if ((*p & 0xF0) == 0xE0) p += 3;
          else p += 4;
        }
        dispTop = p;
      }
      ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
        ImVec2(winPos.x + 8, winPos.y + 4), textCol, dispTop);
    }

    if (bottom && bottom[0] != '\0') {
      const char *dispBot = bottom;
      if (ImGui::CalcTextSize(bottom).x > availW) {
        const char *p = bottom;
        while (*p) {
          if (ImGui::CalcTextSize(p).x <= availW) break;
          if ((*p & 0x80) == 0) p++;
          else if ((*p & 0xE0) == 0xC0) p += 2;
          else if ((*p & 0xF0) == 0xE0) p += 3;
          else p += 4;
        }
        dispBot = p;
      }
      ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
        ImVec2(winPos.x + 8, winPos.y + 4 + lineH + 2), textCol, dispBot);
    }

    ImGui::PopFont();
    ImGui::GetFont()->Scale = origScale;
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

/* -------------------------------------------------------
 * Tap-to-build handler
 * ------------------------------------------------------- */

static void handleTapToBuild(ClientSim *cs) {
  BYTE tileX, tileY;
  if (inputTouchGetViewportTap(&tileX, &tileY)) {
    /* Discard viewport tap if a dialog/modal is over the game area —
       the tap was meant for the dialog, not for building.
       Use IsDialogOpen rather than WantCaptureMouse because the
       always-visible tablet overlay windows (build bar, resource bars)
       also set WantCaptureMouse and must not block building. */
    if (sdl3ImguiIsDialogOpen()) return;
    clientSimSetCursorPos(cs, tileX, tileY);
    /* Latch the tapped square on the shared build cursor before dispatching,
       so the reticle (which draws from the latch) lands where the tap did and
       stays pinned to that world tile as the view scrolls. */
    BYTE mapX = (BYTE)((int)clientSimGetXOffset(cs) + (int)tileX);
    BYTE mapY = (BYTE)((int)clientSimGetYOffset(cs) + (int)tileY);
    buildCursorSetTile(mapX, mapY);
    clientSimManMoveToMap(cs, mapX, mapY, clientSimGetCurrentBuildSelect(cs));
  }
}

/* -------------------------------------------------------
 * Register buttons for hit-testing
 * ------------------------------------------------------- */

static void registerTouchButtons(void) {
  inputTouchClearButtons();
  inputTouchRegisterButton(TOUCH_BTN_FIRE, s_cfg.fireCenterX, s_cfg.fireCenterY, s_cfg.fireRadius);
  inputTouchRegisterButton(TOUCH_BTN_MINE, s_cfg.mineCenterX, s_cfg.mineCenterY, s_cfg.mineRadius);
  inputTouchRegisterButton(TOUCH_BTN_GS_INCREASE, s_cfg.gsIncCenterX, s_cfg.gsIncCenterY, s_cfg.gsIncRadius);
  inputTouchRegisterButton(TOUCH_BTN_GS_DECREASE, s_cfg.gsDecCenterX, s_cfg.gsDecCenterY, s_cfg.gsDecRadius);
  TabletViewButton viewBtns[4];
  int viewBtnCount = tabletViewButtons(viewBtns);
  for (int i = 0; i < viewBtnCount; i++) {
    inputTouchRegisterButton(viewBtns[i].id, viewBtns[i].cx, viewBtns[i].cy, viewBtns[i].r);
  }
}

/* -------------------------------------------------------
 * Scenario panel — the square a scenario draws in, fixed
 * in the game view's top-right corner. The desktop's
 * movable window is not drawn in tablet mode; this takes
 * its place, with no title, grip or settings.
 * ------------------------------------------------------- */

static void renderScenarioSlot(ClientSim *cs) {
  if (s_cfg.scnSlotSide <= 0.0f || !sdl3ImguiScnPanelShown(cs)) return;

  float side = s_cfg.scnSlotSide;
  ImGui::SetNextWindowPos(ImVec2(s_cfg.scnSlotX, s_cfg.scnSlotY));
  ImGui::SetNextWindowSize(ImVec2(side, side));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

  /* NoInputs so the window is never hovered: a tap on the square falls
     through to the map under it, to build or scroll there as it would with
     no panel. NoBackground because the backing is drawn with the list.
     NoBringToFrontOnFocus keeps it behind every other window. */
  if (ImGui::Begin("##ScenarioSlot", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoScrollWithMouse |
                   ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBackground |
                   ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_NoFocusOnAppearing |
                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoDocking)) {
    ImVec2 pos = ImGui::GetWindowPos();
    sdl3ImguiScnPanelDraw(cs, pos.x, pos.y, side, 1.0f, true);
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void sdl3ImguiTabletNewGame(void) {
  s_lastMsgTop[0] = '\0';
  s_lastMsgBottom[0] = '\0';
  s_msgLastChangeTime = 0;
  s_msgTimerInitialized = false;
  s_prevArmour = 0;
  s_armourInitialized = false;
}

/* -------------------------------------------------------
 * Main overlay entry point
 * ------------------------------------------------------- */

void sdl3ImguiTabletOverlay(ClientSim *cs) {
  if (!uiModeIsTablet()) return;

  /* Get viewport bounds from sdl3draw — now in the same coordinate
     space as ImGui since we override DisplaySize to match. */
  int vpX, vpY, vpW, vpH, vpZoom;
  sdl3DrawGetTabletViewport(&vpX, &vpY, &vpW, &vpH, &vpZoom);

  ImGuiIO &io = ImGui::GetIO();
  int screenW = (int)io.DisplaySize.x;
  int screenH = (int)io.DisplaySize.y;

  /* Reconfigure layout each frame */
  tabletLayoutConfigure(&s_cfg, screenW, screenH, vpX, vpY, vpW, vpH, vpZoom);
  tabletViewButtonsApplyPolicies(&s_cfg, cs);

  /* Draw beveled chrome background behind everything */
  renderTabletBackground();

  /* Register button positions for touch hit-testing */
  registerTouchButtons();

  /* Register scroll joystick zone */
  if (s_cfg.scrollJoyH > 10.0f) {
    inputTouchSetScrollJoystickZone(s_cfg.scrollJoyX, s_cfg.scrollJoyY,
                                    s_cfg.scrollJoyW, s_cfg.scrollJoyH);
  }

  /* Update viewport bounds for tap detection */
  inputTouchSetViewportBounds(vpX, vpY, vpW, vpH, vpZoom);

  /* Process tap-to-build */
  handleTapToBuild(cs);

  /* Process scrolling (joystick + viewport drag) */
  processScrollJoystick(cs);
  processViewportDragScroll(cs);

  /* Damage detection — trigger haptic on armour decrease */
  {
    BYTE shells, mines, armour, trees;
    sdl3DrawGetCachedTankStats(&shells, &mines, &armour, &trees);
    if (s_armourInitialized && armour < s_prevArmour) {
      inputTouchTriggerHaptic(0.3f, 20);
    }
    s_prevArmour = armour;
    s_armourInitialized = true;
  }

  /* Handle view button taps. A tap on the view you are already in is left
     alone, matching what the pill button has always done. */
  if (inputTouchIsButtonTapped(TOUCH_BTN_PILL_VIEW)) {
    if (clientSimGetViewKind(cs) != VIEW_KIND_PILL) {
      clientSimPillView(cs, 0, 0);
    }
  }
  if (inputTouchIsButtonTapped(TOUCH_BTN_BASE_VIEW)) {
    if (clientSimGetViewKind(cs) != VIEW_KIND_BASE) {
      clientSimBaseView(cs, 0, 0);
    }
  }
  if (inputTouchIsButtonTapped(TOUCH_BTN_ALLY_VIEW)) {
    if (clientSimGetViewKind(cs) != VIEW_KIND_ALLY) {
      clientSimAllyView(cs, 0, 0);
    }
  }
  if (inputTouchIsButtonTapped(TOUCH_BTN_TANK_VIEW)) {
    if (clientSimIsInItemView(cs)) {
      clientSimTankView(cs);
    }
  }

  renderScenarioSlot(cs);
  renderJoystickOverlay();
  renderScrollJoystickOverlay();
  renderFireMineButtons();
  renderGunsightButtons();
  renderViewButtons(cs);
  renderBuildSelectBar(cs);
  renderResourceBars(cs);
  renderTopBarButtons(cs);
  renderMessagesOverlay();

  /* Status grids in left gutter, or fall back to drawer */
  if (!s_cfg.showStatusGrids) {
    renderStatusDrawer(cs);
  }
}
