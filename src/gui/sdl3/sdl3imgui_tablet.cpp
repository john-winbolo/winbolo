/*
 * Copyright (c) 1998-2008 John Morrison.
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
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "nanosvg.h"
#include "nanosvgrast.h"

extern "C" {
#include "../../bolo/global.h"
#include "../../bolo/screen.h"
#include "../../bolo/screentank.h"
#include "../../bolo/client_sim.h"
#include "../gamefront.h"
#include "../tiles.h"
#include "../ui_mode.h"
#include "input_touch.h"
#include "sdl3draw.h"
}

#include "sdl3imgui.h"
#include "sdl3imgui_tablet.h"

extern "C" {
  void screenSetCursorPosCS(struct ClientSim *csPtr, BYTE posX, BYTE posY);
  void screenManMoveCS(struct ClientSim *csPtr, buildSelect buildS);
  void screenPillViewCS(struct ClientSim *csPtr, int horz, int vert);
  void screenTankViewCS(struct ClientSim *csPtr);
}

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

static SDL_Texture *loadSvgIcon(const char *path, int size) {
  NSVGimage *image = nsvgParseFromFile(path, "px", 96.0f);
  if (!image) return nullptr;
  if (image->width < 1.0f || image->height < 1.0f) { nsvgDelete(image); return nullptr; }
  float scale = (float)size / image->height;
  if (image->width * scale > (float)size) scale = (float)size / image->width;
  int w = size, h = size;
  unsigned char *pixels = (unsigned char *)SDL_malloc((size_t)(w * h * 4));
  if (!pixels) { nsvgDelete(image); return nullptr; }
  memset(pixels, 0, (size_t)(w * h * 4));
  float offX = ((float)w - image->width * scale) * 0.5f;
  float offY = ((float)h - image->height * scale) * 0.5f;
  NSVGrasterizer *rast = nsvgCreateRasterizer();
  nsvgRasterize(rast, image, offX, offY, scale, pixels, w, h, w * 4);
  nsvgDeleteRasterizer(rast);
  nsvgDelete(image);
  SDL_Surface *surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
  if (!surface) { SDL_free(pixels); return nullptr; }
  SDL_Texture *tex = SDL_CreateTextureFromSurface(sdl3DrawGetRenderer(), surface);
  SDL_DestroySurface(surface);
  SDL_free(pixels);
  return tex;
}

static void ensureIconsLoaded(int size) {
  if (s_iconsLoaded) return;
  s_iconsLoaded = true;
  s_iconPlayers  = loadSvgIcon("data/ui/players.svg", size);
  s_iconMessages = loadSvgIcon("data/ui/messages.svg", size);
  s_iconSettings = loadSvgIcon("data/ui/settings.svg", size);
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

  /* Pill view / Tank view — two buttons above gunsight row */
  cfg->pillViewRadius = cfg->gsIncRadius;
  cfg->tankViewRadius = cfg->gsIncRadius;
  float viewRowY = gsRowY - cfg->gsIncRadius - pad - cfg->pillViewRadius;
  cfg->pillViewCenterX = gutterCenterX - cfg->pillViewRadius - gsGap * 0.5f;
  cfg->pillViewCenterY = viewRowY;
  cfg->tankViewCenterX = gutterCenterX + cfg->tankViewRadius + gsGap * 0.5f;
  cfg->tankViewCenterY = viewRowY;

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

  /* --- Build bar — vertical column, right side of gutter --- */
  cfg->buildIconSize = gutterW * 0.18f;
  if (cfg->buildIconSize < 24.0f * pixelScale) cfg->buildIconSize = 24.0f * pixelScale;
  if (cfg->buildIconSize > 48.0f * pixelScale) cfg->buildIconSize = 48.0f * pixelScale;
  cfg->buildSpacing = pad;
  float buildBtnSize = cfg->buildIconSize + 12.0f * pixelScale;
  float buildTotalH = buildBtnSize * 5 + cfg->buildSpacing * 4;
  cfg->buildBarX = rightEdge - buildBtnSize - pad;
  /* Vertically center between top buttons and view toggle */
  float buildRegionTop = cfg->topBtnY + cfg->topBtnSize + pad * 2;
  float buildRegionBot = cfg->pillViewCenterY - cfg->pillViewRadius - pad * 2;
  cfg->buildBarY = buildRegionTop + (buildRegionBot - buildRegionTop - buildTotalH) * 0.5f;
  if (cfg->buildBarY < buildRegionTop) cfg->buildBarY = buildRegionTop;

  /* --- Resource bars — left of build bar, filling remaining gutter space --- */
  cfg->barsW = cfg->buildBarX - gutterLeft - pad * 2;
  if (cfg->barsW < 30.0f * pixelScale) cfg->barsW = 30.0f * pixelScale;
  if (cfg->barsW > 100.0f * pixelScale) cfg->barsW = 100.0f * pixelScale;
  float barsAvailH = buildRegionBot - buildRegionTop;
  cfg->barsH = (barsAvailH - pad) * 0.5f;
  if (cfg->barsH < 60.0f * pixelScale) cfg->barsH = 60.0f * pixelScale;
  if (cfg->barsH > 140.0f * pixelScale) cfg->barsH = 140.0f * pixelScale;
  cfg->tankBarsX = gutterLeft + pad;
  cfg->tankBarsY = buildRegionTop;
  cfg->baseBarsX = cfg->tankBarsX;
  cfg->baseBarsY = cfg->tankBarsY + cfg->barsH + pad;

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
  ImU32 fireFill = fireActive ? IM_COL32(255, 80, 80, 200) : IM_COL32(200, 50, 50, 140);
  dl->AddCircleFilled(ImVec2(s_cfg.fireCenterX, s_cfg.fireCenterY), s_cfg.fireRadius,
                       scaleAlpha(fireFill, fireAlpha), 32);
  dl->AddCircle(ImVec2(s_cfg.fireCenterX, s_cfg.fireCenterY), s_cfg.fireRadius,
                scaleAlpha(IM_COL32(255, 100, 100, 200), fireAlpha), 32, 2.0f);
  const char *fireLabel = "FIRE";
  ImVec2 fireSize = ImGui::CalcTextSize(fireLabel);
  dl->AddText(ImVec2(s_cfg.fireCenterX - fireSize.x * 0.5f, s_cfg.fireCenterY - fireSize.y * 0.5f),
              scaleAlpha(IM_COL32(255, 255, 255, 220), fireAlpha), fireLabel);

  /* Mine button */
  ImU32 mineFill = mineActive ? IM_COL32(80, 80, 255, 200) : IM_COL32(50, 50, 200, 140);
  dl->AddCircleFilled(ImVec2(s_cfg.mineCenterX, s_cfg.mineCenterY), s_cfg.mineRadius,
                       scaleAlpha(mineFill, mineAlpha), 32);
  dl->AddCircle(ImVec2(s_cfg.mineCenterX, s_cfg.mineCenterY), s_cfg.mineRadius,
                scaleAlpha(IM_COL32(100, 100, 255, 200), mineAlpha), 32, 2.0f);
  const char *mineLabel = "MINE";
  ImVec2 mineSize = ImGui::CalcTextSize(mineLabel);
  dl->AddText(ImVec2(s_cfg.mineCenterX - mineSize.x * 0.5f, s_cfg.mineCenterY - mineSize.y * 0.5f),
              scaleAlpha(IM_COL32(255, 255, 255, 220), mineAlpha), mineLabel);
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
  ImU32 decFill = decActive ? IM_COL32(150, 150, 50, 200) : IM_COL32(100, 100, 30, 140);
  dl->AddCircleFilled(ImVec2(s_cfg.gsDecCenterX, s_cfg.gsDecCenterY), s_cfg.gsDecRadius,
                       scaleAlpha(decFill, decAlpha), 32);
  dl->AddCircle(ImVec2(s_cfg.gsDecCenterX, s_cfg.gsDecCenterY), s_cfg.gsDecRadius,
                scaleAlpha(IM_COL32(180, 180, 60, 200), decAlpha), 32, 2.0f);
  const char *decLabel = "-";
  ImVec2 decSize = ImGui::CalcTextSize(decLabel);
  dl->AddText(ImVec2(s_cfg.gsDecCenterX - decSize.x * 0.5f, s_cfg.gsDecCenterY - decSize.y * 0.5f),
              scaleAlpha(IM_COL32(255, 255, 255, 220), decAlpha), decLabel);

  /* Increase button */
  ImU32 incFill = incActive ? IM_COL32(150, 150, 50, 200) : IM_COL32(100, 100, 30, 140);
  dl->AddCircleFilled(ImVec2(s_cfg.gsIncCenterX, s_cfg.gsIncCenterY), s_cfg.gsIncRadius,
                       scaleAlpha(incFill, incAlpha), 32);
  dl->AddCircle(ImVec2(s_cfg.gsIncCenterX, s_cfg.gsIncCenterY), s_cfg.gsIncRadius,
                scaleAlpha(IM_COL32(180, 180, 60, 200), incAlpha), 32, 2.0f);
  const char *incLabel = "+";
  ImVec2 incSize = ImGui::CalcTextSize(incLabel);
  dl->AddText(ImVec2(s_cfg.gsIncCenterX - incSize.x * 0.5f, s_cfg.gsIncCenterY - incSize.y * 0.5f),
              scaleAlpha(IM_COL32(255, 255, 255, 220), incAlpha), incLabel);
}

/* -------------------------------------------------------
 * View toggle button (pill view / tank view)
 * ------------------------------------------------------- */

static void renderViewButtons(ClientSim *cs) {
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  bool inPillView = (bool)cs->inPillView;

  /* Pill view button */
  {
    bool active = inputTouchIsButtonHeld(TOUCH_BTN_PILL_VIEW);
    float alpha = (active || inPillView) ? s_cfg.activeOpacity : s_cfg.idleOpacity;
    ImU32 fill = (active || inPillView) ? IM_COL32(180, 180, 50, 200) : IM_COL32(100, 100, 50, 140);
    float cx = s_cfg.pillViewCenterX, cy = s_cfg.pillViewCenterY, r = s_cfg.pillViewRadius;
    dl->AddCircleFilled(ImVec2(cx, cy), r, scaleAlpha(fill, alpha), 32);
    dl->AddCircle(ImVec2(cx, cy), r, scaleAlpha(IM_COL32(200, 200, 100, 220), alpha), 32, 2.0f);
    const char *label = "PILL";
    ImVec2 sz = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(cx - sz.x * 0.5f, cy - sz.y * 0.5f),
                scaleAlpha(IM_COL32(255, 255, 255, 220), alpha), label);
  }

  /* Tank view button */
  {
    bool active = inputTouchIsButtonHeld(TOUCH_BTN_TANK_VIEW);
    float alpha = (active || !inPillView) ? s_cfg.activeOpacity : s_cfg.idleOpacity;
    ImU32 fill = (active || !inPillView) ? IM_COL32(50, 150, 50, 200) : IM_COL32(30, 100, 30, 140);
    float cx = s_cfg.tankViewCenterX, cy = s_cfg.tankViewCenterY, r = s_cfg.tankViewRadius;
    dl->AddCircleFilled(ImVec2(cx, cy), r, scaleAlpha(fill, alpha), 32);
    dl->AddCircle(ImVec2(cx, cy), r, scaleAlpha(IM_COL32(100, 220, 100, 220), alpha), 32, 2.0f);
    const char *label = "TANK";
    ImVec2 sz = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(cx - sz.x * 0.5f, cy - sz.y * 0.5f),
                scaleAlpha(IM_COL32(255, 255, 255, 220), alpha), label);
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
  float totalH = btnSize * 5 + spacing * 4;
  float barWidth = btnSize + 12.0f;
  float startX = s_cfg.buildBarX;
  float startY = s_cfg.buildBarY;

  ImGui::SetNextWindowPos(ImVec2(startX, startY));
  ImGui::SetNextWindowSize(ImVec2(barWidth, totalH + 12));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, spacing));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.5f));

  if (ImGui::Begin("##BuildBar", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_NoScrollWithMouse)) {

    buildSelect cur = getBuildCurrentSelectCS(cs);
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

      if (ImGui::ImageButton(ids[i], (ImTextureID)tilesTex,
                              ImVec2(iconSize, iconSize), uv0, uv1)) {
        setBuildCurrentSelectCS(cs, values[i]);
      }

      /* Touch tap fallback — ImGui buttons may not register finger events on iOS */
      ImVec2 rMin = ImGui::GetItemRectMin();
      ImVec2 rMax = ImGui::GetItemRectMax();
      if (inputTouchConsumeTapInRect(rMin.x, rMin.y, rMax.x - rMin.x, rMax.y - rMin.y)) {
        setBuildCurrentSelectCS(cs, values[i]);
      }

      ImGui::PopStyleColor(2);
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
  float btnSize = s_cfg.topBtnSize;
  float btnY = s_cfg.topBtnY;
  float alpha = s_cfg.topBtnOpacity;
  ImGuiWindowFlags btnFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar;
  ImVec2 btnDim(btnSize, btnSize);
  ImVec2 winSize(btnSize + 8, btnSize + 8);

  ensureIconsLoaded((int)(btnSize * 2));

  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

  /* Messages button */
  ImGui::SetNextWindowPos(ImVec2(s_cfg.playersBtnX, btnY));
  ImGui::SetNextWindowSize(winSize);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.4f));
  if (ImGui::Begin("##MsgBtn", nullptr, btnFlags)) {
    if (s_iconMessages) {
      if (ImGui::ImageButton("##msgIcon", (ImTextureID)s_iconMessages, btnDim)) {
        sdl3ImguiShowSendMsg(true);
      }
    } else {
      if (ImGui::Button("Msg", btnDim)) { sdl3ImguiShowSendMsg(true); }
    }
    ImVec2 rMin = ImGui::GetItemRectMin();
    ImVec2 rMax = ImGui::GetItemRectMax();
    if (inputTouchConsumeTapInRect(rMin.x, rMin.y, rMax.x - rMin.x, rMax.y - rMin.y)) {
      sdl3ImguiShowSendMsg(true);
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();

  /* Players button */
  ImGui::SetNextWindowPos(ImVec2(s_cfg.msgBtnX, btnY));
  ImGui::SetNextWindowSize(winSize);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.4f));
  if (ImGui::Begin("##PlayersBtn", nullptr, btnFlags)) {
    if (s_iconPlayers) {
      if (ImGui::ImageButton("##playersIcon", (ImTextureID)s_iconPlayers, btnDim)) {
        sdl3ImguiShowPlayersPanel(true);
      }
    } else {
      if (ImGui::Button("Ply", btnDim)) { sdl3ImguiShowPlayersPanel(true); }
    }
    ImVec2 rMin = ImGui::GetItemRectMin();
    ImVec2 rMax = ImGui::GetItemRectMax();
    if (inputTouchConsumeTapInRect(rMin.x, rMin.y, rMax.x - rMin.x, rMax.y - rMin.y)) {
      sdl3ImguiShowPlayersPanel(true);
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();

  /* Settings button */
  ImGui::SetNextWindowPos(ImVec2(s_cfg.cogBtnX, btnY));
  ImGui::SetNextWindowSize(winSize);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.4f));
  if (ImGui::Begin("##CogBtn", nullptr, btnFlags)) {
    if (s_iconSettings) {
      if (ImGui::ImageButton("##settingsIcon", (ImTextureID)s_iconSettings, btnDim)) {
        sdl3ImguiShowSettings();
      }
    } else {
      if (ImGui::Button("Set", btnDim)) { sdl3ImguiShowSettings(); }
    }
    ImVec2 rMin = ImGui::GetItemRectMin();
    ImVec2 rMax = ImGui::GetItemRectMax();
    if (inputTouchConsumeTapInRect(rMin.x, rMin.y, rMax.x - rMin.x, rMax.y - rMin.y)) {
      sdl3ImguiShowSettings();
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();

  ImGui::PopStyleVar(3); /* Alpha, FramePadding, WindowPadding */

  /* Status drawer toggle — only when grids not in gutter */
  if (!s_cfg.showStatusGrids) {
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::SetNextWindowPos(ImVec2(s_cfg.cogBtnX, btnY + btnSize + 8));
    ImGui::SetNextWindowSize(winSize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.4f));
    if (ImGui::Begin("##StatusBtn", nullptr, btnFlags)) {
      if (ImGui::Button("i", btnDim)) {
        s_statusDrawerOpen = !s_statusDrawerOpen;
      }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
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
    float w = s_cfg.barsW;
    float h = s_cfg.barsH;

    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.5f));

    if (ImGui::Begin("##TankBars", nullptr, winFlags)) {
      ImDrawList *dl = ImGui::GetWindowDrawList();
      ImVec2 winPos = ImGui::GetWindowPos();

      /* 4 vertical bars: S M A T */
      float barW = 8.0f;
      float maxBarH = h - 16.0f;
      float barTop = winPos.y + 4.0f;
      float barGap = (w - 8.0f - barW * 4) / 3.0f;
      ImU32 barColor = IM_COL32(0, 200, 0, 200);
      ImU32 bgColor = IM_COL32(40, 40, 40, 180);

      BYTE vals[4] = { shells, mines, armour, trees };

      for (int i = 0; i < 4; i++) {
        float bx = winPos.x + 4.0f + i * (barW + barGap);
        dl->AddRectFilled(ImVec2(bx, barTop), ImVec2(bx + barW, barTop + maxBarH), bgColor);
        float fillH = (vals[i] / 40.0f) * maxBarH;
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
    float w = s_cfg.barsW;
    float h = s_cfg.barsH - 16.0f;  /* slightly shorter, only 3 bars */

    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, hasBase ? 0.5f : 0.25f));

    if (ImGui::Begin("##BaseBars", nullptr, winFlags)) {
      ImDrawList *dl = ImGui::GetWindowDrawList();
      ImVec2 winPos = ImGui::GetWindowPos();

      float barW = 8.0f;
      float maxBarH = h - 16.0f;
      float barTop = winPos.y + 4.0f;
      float barGap = (w - 8.0f - barW * 3) / 2.0f;
      ImU32 barColor = hasBase ? IM_COL32(0, 200, 0, 200) : IM_COL32(60, 60, 60, 120);
      ImU32 bgColor = IM_COL32(40, 40, 40, 180);

      BYTE vals[3] = { baseShells, baseMines, baseArmour };

      for (int i = 0; i < 3; i++) {
        float bx = winPos.x + 4.0f + i * (barW + barGap);
        dl->AddRectFilled(ImVec2(bx, barTop), ImVec2(bx + barW, barTop + maxBarH), bgColor);
        float fillH = hasBase ? (vals[i] / 40.0f) * maxBarH : 0.0f;
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

  if (ImGui::Begin("Status", &s_statusDrawerOpen,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse)) {

    /* Kills / Deaths */
    {
      int kills, deaths;
      screenGetKillsDeathsCS(cs, &kills, &deaths);
      ImGui::Text("Kills: %d  Deaths: %d", kills, deaths);
    }
    ImGui::Separator();

    /* Tank resource bars */
    if (ImGui::CollapsingHeader("Tank Resources", ImGuiTreeNodeFlags_DefaultOpen)) {
      BYTE shells, mines, armour, trees;
      screenGetTankStatsCS(cs, &shells, &mines, &armour, &trees);
      ImGui::ProgressBar((float)shells / 40.0f, ImVec2(-1, 0), "Shells");
      ImGui::ProgressBar((float)mines  / 40.0f, ImVec2(-1, 0), "Mines");
      ImGui::ProgressBar((float)armour / 40.0f, ImVec2(-1, 0), "Armour");
      ImGui::ProgressBar((float)trees  / 40.0f, ImVec2(-1, 0), "Trees");
    }

    /* Pillbox status */
    if (ImGui::CollapsingHeader("Pillboxes")) {
      BYTE total = pillsGetNumPills(&cs->sim.pb);
      for (BYTE i = 1; i <= total; i++) {
        pillAlliance pa = screenPillAllianceCS(cs, i);
        ImVec4 col;
        switch (pa) {
          case pillAllie:   col = ImVec4(0.0f, 0.8f, 0.0f, 1.0f); break;
          case pillGood:    col = ImVec4(0.0f, 0.6f, 0.8f, 1.0f); break;
          case pillNeutral: col = ImVec4(0.6f, 0.6f, 0.6f, 1.0f); break;
          case pillEvil:    col = ImVec4(0.8f, 0.0f, 0.0f, 1.0f); break;
          default:          col = ImVec4(0.3f, 0.3f, 0.3f, 1.0f); break;
        }
        ImGui::TextColored(col, "Pill %d", i);
        if (i % 4 != 0 && i < total) ImGui::SameLine(0, 20);
      }
    }

    /* Base status */
    if (ImGui::CollapsingHeader("Bases")) {
      BYTE total = basesGetNumBases(&cs->sim.bs);
      for (BYTE i = 1; i <= total; i++) {
        baseAlliance ba = screenBaseAllianceCS(cs, i);
        ImVec4 col;
        switch (ba) {
          case baseOwnGood: col = ImVec4(0.0f, 0.8f, 0.0f, 1.0f); break;
          case baseAllieGood: col = ImVec4(0.0f, 0.6f, 0.8f, 1.0f); break;
          case baseNeutral: col = ImVec4(0.6f, 0.6f, 0.6f, 1.0f); break;
          case baseEvil:    col = ImVec4(0.8f, 0.0f, 0.0f, 1.0f); break;
          default:          col = ImVec4(0.3f, 0.3f, 0.3f, 1.0f); break;
        }
        ImGui::TextColored(col, "Base %d", i);
        if (i % 4 != 0 && i < total) ImGui::SameLine(0, 20);
      }
    }

    /* Tanks status */
    if (ImGui::CollapsingHeader("Tanks")) {
      for (BYTE i = 1; i <= MAX_TANKS; i++) {
        tankAlliance ta = screenTankAllianceCS(cs, i);
        ImVec4 col;
        switch (ta) {
          case tankSelf:    col = ImVec4(0.0f, 0.8f, 0.0f, 1.0f); break;
          case tankAllie:   col = ImVec4(0.0f, 0.6f, 0.8f, 1.0f); break;
          case tankEvil:    col = ImVec4(0.8f, 0.0f, 0.0f, 1.0f); break;
          default:          col = ImVec4(0.3f, 0.3f, 0.3f, 1.0f); break;
        }
        if (ta != tankNone) {
          ImGui::TextColored(col, "Tank %d", i);
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
    screenSetCursorPosCS(cs, tileX, tileY);
    screenManMoveCS(cs, getBuildCurrentSelectCS(cs));
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
  inputTouchRegisterButton(TOUCH_BTN_PILL_VIEW, s_cfg.pillViewCenterX, s_cfg.pillViewCenterY, s_cfg.pillViewRadius);
  inputTouchRegisterButton(TOUCH_BTN_TANK_VIEW, s_cfg.tankViewCenterX, s_cfg.tankViewCenterY, s_cfg.tankViewRadius);
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

  /* Register button positions for touch hit-testing */
  registerTouchButtons();

  /* Update viewport bounds for tap detection */
  inputTouchSetViewportBounds(vpX, vpY, vpW, vpH, vpZoom);

  /* Process tap-to-build */
  handleTapToBuild(cs);

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

  /* Handle view button taps */
  if (inputTouchIsButtonTapped(TOUCH_BTN_PILL_VIEW)) {
    if (!cs->inPillView) {
      screenPillViewCS(cs, 0, 0);
    }
  }
  if (inputTouchIsButtonTapped(TOUCH_BTN_TANK_VIEW)) {
    if (cs->inPillView) {
      screenTankViewCS(cs);
    }
  }

  renderJoystickOverlay();
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
