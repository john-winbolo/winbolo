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
*  - Shoot and mine buttons
*  - Build select bar
*  - Hamburger menu for all desktop menu items
*  - Status grids (tanks/pills/bases) in left gutter
*  - Stock bars (shells/mines/armour/trees) in right gutter
*  - Status drawer fallback when gutters are too narrow
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

/* Forward declarations — these live in screen.c.
   Including winbolo.h pulls in Win32 headers that conflict with SDL3,
   so we declare them explicitly here, matching sdl3imgui.cpp's approach. */
extern "C" {
  /* Cursor and build actions — used by tap-to-build */
  void screenSetCursorPosCS(struct ClientSim *csPtr, BYTE posX, BYTE posY);
  void screenManMoveCS(struct ClientSim *csPtr, buildSelect buildS);

  /* View toggle — pill view / tank view */
  void screenPillViewCS(struct ClientSim *csPtr, int horz, int vert);
  void screenTankViewCS(struct ClientSim *csPtr);
}

static bool s_statusDrawerOpen = false;

/* Static layout config, recomputed each frame */
static TabletLayoutConfig s_cfg;

/* Message fade state */
static char s_lastMsgTop[512] = {0};
static char s_lastMsgBottom[512] = {0};
static Uint64 s_msgLastChangeTime = 0;  /* SDL_GetTicks() when message content last changed */
static bool s_msgTimerInitialized = false;

/* Haptic state — track previous fire/mine state to detect edges */
static bool s_prevFirePressed = false;
static bool s_prevMinePressed = false;

/* Damage detection — track armour to detect hits */
static BYTE s_prevArmour = 0;
static bool s_armourInitialized = false;

/* Tile sheet dimensions (must match tile.bmp) */
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

  /* Safe area insets — query from SDL */
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

  /* Compute gutters */
  cfg->leftGutter = viewportX;
  cfg->rightGutter = screenW - viewportX - viewportW;
  cfg->topGutter = viewportY;
  cfg->bottomGutter = screenH - viewportY - viewportH;

  /* Breakpoints */
  cfg->showStatusGrids = (cfg->leftGutter >= 100);
  cfg->showStockBars = (cfg->rightGutter >= 60);

  /* Scale factor for small screens */
  float scaleFactor = (screenH < 500) ? 0.7f : 1.0f;

  /* Joystick */
  cfg->joyOuterRadius = 60.0f * scaleFactor;
  cfg->joyInnerRadius = 25.0f * scaleFactor;
  cfg->joyZoneRight = 0.40f;

  /* Action buttons */
  cfg->fireRadius = 35.0f * scaleFactor;
  cfg->fireCenterX = (float)screenW - cfg->safeRight - cfg->fireRadius - 20.0f;
  cfg->fireCenterY = (float)screenH - cfg->safeBottom - cfg->fireRadius * 3.5f;
  cfg->mineRadius = 28.0f * scaleFactor;
  cfg->mineCenterX = cfg->fireCenterX;
  cfg->mineCenterY = cfg->fireCenterY - cfg->fireRadius * 1.5f - cfg->mineRadius * 1.5f;

  /* Build bar */
  cfg->buildIconSize = 40.0f * scaleFactor;
  cfg->buildSpacing = 16.0f * scaleFactor;
  cfg->buildBarY = (float)screenH - cfg->safeBottom - 8.0f - (cfg->buildIconSize + 16.0f);

  /* Opacity */
  cfg->idleOpacity = 0.4f;
  cfg->activeOpacity = 1.0f;
  cfg->topBtnOpacity = 0.6f;
  cfg->joyFadeOutMs = 1000;

  /* Message fade */
  cfg->msgFadeSeconds = 5.0f;
  cfg->msgFadeDuration = 1.0f;

  /* Status grids — positioned in left gutter */
  cfg->tanksGridX = cfg->safeLeft + 4.0f;
  cfg->tanksGridY = cfg->safeTop + 4.0f;

  /* Scale status grids to fit available gutter width */
  float availW = (float)cfg->leftGutter - cfg->safeLeft - 8.0f;
  cfg->statusGridScale = availW / 90.0f;
  if (cfg->statusGridScale > 2.0f) cfg->statusGridScale = 2.0f;
  if (cfg->statusGridScale < 0.5f) cfg->statusGridScale = 0.5f;

  float gridH = 66.0f * cfg->statusGridScale;
  float gridGap = 4.0f * cfg->statusGridScale;
  cfg->pillsGridX = cfg->tanksGridX;
  cfg->pillsGridY = cfg->tanksGridY + gridH + gridGap;
  cfg->basesGridX = cfg->tanksGridX;
  cfg->basesGridY = cfg->pillsGridY + gridH + gridGap;

  /* Stock bars — positioned in right gutter */
  cfg->stockBarsX = (float)(viewportX + viewportW) + 8.0f;
  cfg->stockBarsY = cfg->safeTop + 80.0f;

  /* View toggle button — bottom-right, above the build bar, near FIRE/MINE */
  cfg->viewToggleRadius = 22.0f * scaleFactor;
  cfg->viewToggleCenterX = cfg->fireCenterX - cfg->fireRadius - cfg->viewToggleRadius - 16.0f;
  cfg->viewToggleCenterY = cfg->fireCenterY + cfg->fireRadius;

  /* Messages overlay — above the build bar, same width as viewport */
  cfg->msgOverlayW = (float)viewportW;
  cfg->msgOverlayX = (float)viewportX;
  cfg->msgOverlayY = cfg->buildBarY - 68.0f;

  /* Top bar buttons — right-aligned: [Players] [Message] [Cog] */
  cfg->topBtnSize = 44.0f * scaleFactor;
  cfg->topBtnY = cfg->safeTop + 4.0f;
  float btnStep = cfg->topBtnSize + 8.0f;
  cfg->cogBtnX = (float)screenW - cfg->safeRight - 8.0f - cfg->topBtnSize;
  cfg->msgBtnX = cfg->cogBtnX - btnStep;
  cfg->playersBtnX = cfg->msgBtnX - btnStep;
}

/* -------------------------------------------------------
 * Alpha helper — scale the alpha channel of an ImU32 color
 * ------------------------------------------------------- */

static ImU32 scaleAlpha(ImU32 col, float alpha) {
  int a = (int)((float)((col >> 24) & 0xFF) * alpha);
  if (a < 0) a = 0;
  if (a > 255) a = 255;
  return (col & 0x00FFFFFF) | ((ImU32)a << 24);
}

/* -------------------------------------------------------
 * Joystick overlay — appears on touch, fades out 1s after release
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
 * Shoot / Mine buttons
 * ------------------------------------------------------- */

static void renderShootMineButtons(void) {
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  bool shootActive = inputTouchIsFirePressed();
  bool mineActive = inputTouchIsMineHeld();

  /* Haptic feedback on button press edges */
  if (shootActive && !s_prevFirePressed) {
    inputTouchTriggerHaptic(0.6f, 30);  /* short pulse on FIRE press */
  }
  s_prevFirePressed = shootActive;

  if (mineActive && !s_prevMinePressed) {
    inputTouchTriggerHaptic(0.8f, 60);  /* medium pulse on MINE place */
  }
  s_prevMinePressed = mineActive;

  float shootAlpha = shootActive ? s_cfg.activeOpacity : s_cfg.idleOpacity;
  float mineAlpha = mineActive ? s_cfg.activeOpacity : s_cfg.idleOpacity;

  /* Shoot button */
  float shootX = s_cfg.fireCenterX;
  float shootY = s_cfg.fireCenterY;
  ImU32 shootFill = shootActive ? IM_COL32(255, 80, 80, 200) : IM_COL32(200, 50, 50, 140);
  dl->AddCircleFilled(ImVec2(shootX, shootY), s_cfg.fireRadius,
                       scaleAlpha(shootFill, shootAlpha), 32);
  dl->AddCircle(ImVec2(shootX, shootY), s_cfg.fireRadius,
                scaleAlpha(IM_COL32(255, 100, 100, 200), shootAlpha), 32, 2.0f);

  const char *shootLabel = "FIRE";
  ImVec2 shootSize = ImGui::CalcTextSize(shootLabel);
  dl->AddText(ImVec2(shootX - shootSize.x * 0.5f, shootY - shootSize.y * 0.5f),
              scaleAlpha(IM_COL32(255, 255, 255, 220), shootAlpha), shootLabel);

  /* Mine button */
  float mineX = s_cfg.mineCenterX;
  float mineY = s_cfg.mineCenterY;
  ImU32 mineFill = mineActive ? IM_COL32(80, 80, 255, 200) : IM_COL32(50, 50, 200, 140);
  dl->AddCircleFilled(ImVec2(mineX, mineY), s_cfg.mineRadius,
                       scaleAlpha(mineFill, mineAlpha), 32);
  dl->AddCircle(ImVec2(mineX, mineY), s_cfg.mineRadius,
                scaleAlpha(IM_COL32(100, 100, 255, 200), mineAlpha), 32, 2.0f);

  const char *mineLabel = "MINE";
  ImVec2 mineSize = ImGui::CalcTextSize(mineLabel);
  dl->AddText(ImVec2(mineX - mineSize.x * 0.5f, mineY - mineSize.y * 0.5f),
              scaleAlpha(IM_COL32(255, 255, 255, 220), mineAlpha), mineLabel);
}

/* -------------------------------------------------------
 * Build select bar
 * ------------------------------------------------------- */

/* Representative tile positions for each build type (from tiles.h) */
static const struct { int x, y; } buildIconTiles[] = {
  { FOREST_X,       FOREST_Y       },  /* BsTrees   - forest tile */
  { ROAD_SOLID_X,   ROAD_SOLID_Y   },  /* BsRoad    - solid road tile */
  { BUILD_SINGLE_X, BUILD_SINGLE_Y },  /* BsBuilding - single wall tile */
  { PILL_EVIL15_X,  PILL_EVIL15_Y  },  /* BsPillbox - full-health pillbox */
  { MINE_X,         MINE_Y         },  /* BsMine    - mine tile */
};

static void renderBuildSelectBar(ClientSim *cs) {
  SDL_Texture *tilesTex = sdl3DrawGetTilesTexture();
  if (!tilesTex) return;

  float iconDispSize = s_cfg.buildIconSize;
  float btnPad = 8.0f;
  float btnSize = iconDispSize + btnPad * 2;
  float spacing = s_cfg.buildSpacing;
  float totalW = btnSize * 5 + spacing * 4;
  float barHeight = btnSize + 12.0f;
  float startX = ((float)s_cfg.screenW - totalW) * 0.5f - 8.0f;
  float startY = s_cfg.buildBarY;

  ImGui::SetNextWindowPos(ImVec2(startX, startY));
  ImGui::SetNextWindowSize(ImVec2(totalW + 16, barHeight));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.5f));

  if (ImGui::Begin("##BuildBar", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_NoScrollWithMouse)) {

    buildSelect cur = getBuildCurrentSelectCS(cs);
    const char *ids[] = { "##bsTree", "##bsRoad", "##bsWall", "##bsPill", "##bsMine" };
    buildSelect values[] = { BsTrees, BsRoad, BsBuilding, BsPillbox, BsMine };

    for (int i = 0; i < 5; i++) {
      if (i > 0) ImGui::SameLine();
      bool selected = (cur == values[i]);

      /* Compute UV coordinates for this tile (16x16 region in the tile sheet) */
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
                              ImVec2(iconDispSize, iconDispSize), uv0, uv1)) {
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
 * Top bar buttons: Players, Message, Settings cog, Hamburger
 * ------------------------------------------------------- */

static void renderTopBarButtons(ClientSim *cs) {
  float btnSize = s_cfg.topBtnSize;
  float btnY = s_cfg.topBtnY;
  float alpha = s_cfg.topBtnOpacity;
  ImGuiWindowFlags btnFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar;
  ImVec2 btnDim(btnSize, btnSize);
  ImVec2 winSize(btnSize + 8, btnSize + 8);

  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);

  /* Players button */
  ImGui::SetNextWindowPos(ImVec2(s_cfg.playersBtnX, btnY));
  ImGui::SetNextWindowSize(winSize);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.4f));
  if (ImGui::Begin("##PlayersBtn", nullptr, btnFlags)) {
    if (ImGui::Button("Ply", btnDim)) {
      sdl3ImguiShowPlayersPanel(true);
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();

  /* Message button */
  ImGui::SetNextWindowPos(ImVec2(s_cfg.msgBtnX, btnY));
  ImGui::SetNextWindowSize(winSize);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.4f));
  if (ImGui::Begin("##MsgBtn", nullptr, btnFlags)) {
    if (ImGui::Button("Msg", btnDim)) {
      sdl3ImguiShowSendMsg(true);
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();

  /* Settings cog button */
  ImGui::SetNextWindowPos(ImVec2(s_cfg.cogBtnX, btnY));
  ImGui::SetNextWindowSize(winSize);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.4f));
  if (ImGui::Begin("##CogBtn", nullptr, btnFlags)) {
    if (ImGui::Button("\xe2\x9a\x99", btnDim)) {
      sdl3ImguiShowSettings();
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();

  ImGui::PopStyleVar(); /* Alpha */

  /* Status drawer toggle button — only show when grids are NOT in gutter */
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
 * Render stock bars (shells/mines/armour/trees) in right gutter
 * ------------------------------------------------------- */

static void renderStockBars(ClientSim *cs) {
  BYTE shells, mines, armour, trees;
  screenGetTankStatsCS(cs, &shells, &mines, &armour, &trees);

  int kills, deaths;
  screenGetKillsDeathsCS(cs, &kills, &deaths);

  float x = s_cfg.stockBarsX;
  float y = s_cfg.stockBarsY;
  float panelW = 56.0f;
  float panelH = 220.0f;

  ImGui::SetNextWindowPos(ImVec2(x, y));
  ImGui::SetNextWindowSize(ImVec2(panelW, panelH));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.5f));

  if (ImGui::Begin("##StockBars", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoInputs)) {

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();

    /* Kills/Deaths text */
    char kdText[32];
    snprintf(kdText, sizeof(kdText), "K:%d D:%d", kills, deaths);
    dl->AddText(ImVec2(winPos.x + 4, winPos.y + 4), IM_COL32(200, 200, 200, 220), kdText);

    /* Draw 4 vertical bars */
    float barW = 6.0f;
    float maxBarH = 160.0f;
    float barTop = winPos.y + 24.0f;
    float barGap = (panelW - 8.0f - barW * 4) / 3.0f;
    ImU32 barColor = IM_COL32(0, 200, 0, 200);
    ImU32 bgColor = IM_COL32(40, 40, 40, 180);

    BYTE vals[4] = { shells, mines, armour, trees };
    const char *labels[4] = { "S", "M", "A", "T" };

    for (int i = 0; i < 4; i++) {
      float bx = winPos.x + 4.0f + i * (barW + barGap);
      float by = barTop;

      /* Background */
      dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + barW, by + maxBarH), bgColor);

      /* Filled portion (bottom-up) */
      float fillH = (vals[i] / 40.0f) * maxBarH;
      if (fillH > maxBarH) fillH = maxBarH;
      dl->AddRectFilled(ImVec2(bx, by + maxBarH - fillH),
                         ImVec2(bx + barW, by + maxBarH), barColor);

      /* Label below */
      dl->AddText(ImVec2(bx, by + maxBarH + 2.0f), IM_COL32(200, 200, 200, 200), labels[i]);
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

/* -------------------------------------------------------
 * View toggle button (pill view / tank view)
 * ------------------------------------------------------- */

static const ImU32 VIEW_TOGGLE_COLOR        = IM_COL32(100, 100, 50, 140);
static const ImU32 VIEW_TOGGLE_COLOR_ACTIVE = IM_COL32(180, 180, 50, 200);

static void renderViewToggleButton(ClientSim *cs) {
  ImDrawList *dl = ImGui::GetForegroundDrawList();

  float cx = s_cfg.viewToggleCenterX;
  float cy = s_cfg.viewToggleCenterY;
  float r  = s_cfg.viewToggleRadius;
  bool inPillView = (bool)cs->inPillView;

  dl->AddCircleFilled(ImVec2(cx, cy), r,
                       inPillView ? VIEW_TOGGLE_COLOR_ACTIVE : VIEW_TOGGLE_COLOR, 32);
  dl->AddCircle(ImVec2(cx, cy), r,
                IM_COL32(200, 200, 100, 200), 32, 2.0f);

  const char *label = inPillView ? "PILL" : "VIEW";
  ImVec2 labelSize = ImGui::CalcTextSize(label);
  dl->AddText(ImVec2(cx - labelSize.x * 0.5f, cy - labelSize.y * 0.5f),
              IM_COL32(255, 255, 255, 220), label);

  /* Check for tap on the button via ImGui invisible button */
  ImGui::SetNextWindowPos(ImVec2(cx - r, cy - r));
  ImGui::SetNextWindowSize(ImVec2(r * 2, r * 2));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  if (ImGui::Begin("##ViewToggle", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_NoBackground)) {
    if (ImGui::InvisibleButton("##ViewToggleBtn", ImVec2(r * 2, r * 2))) {
      if (inPillView) {
        screenTankViewCS(cs);
      } else {
        screenPillViewCS(cs, 0, 0);
      }
    }
  }
  ImGui::End();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

/* -------------------------------------------------------
 * Messages overlay (above build bar)
 * ------------------------------------------------------- */

static void renderMessagesOverlay(void) {
  const char *top = NULL, *bottom = NULL;
  sdl3DrawGetCachedMessages(&top, &bottom);

  /* Nothing to show if both lines are empty */
  bool hasContent = (top && top[0] != '\0') || (bottom && bottom[0] != '\0');
  if (!hasContent && !s_msgTimerInitialized) return;

  /* Detect message content changes to reset fade timer */
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

  /* Compute fade alpha: full opacity for msgFadeSeconds, then fade over msgFadeDuration */
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

  /* Scale font up so text is large and readable on tablet */
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
 * Main overlay entry point
 * ------------------------------------------------------- */

void sdl3ImguiTabletOverlay(ClientSim *cs) {
  if (!uiModeIsTablet()) return;

  /* Get viewport bounds from sdl3draw */
  int vpX, vpY, vpW, vpH, vpZoom;
  sdl3DrawGetTabletViewport(&vpX, &vpY, &vpW, &vpH, &vpZoom);

  ImGuiIO &io = ImGui::GetIO();
  int screenW = (int)io.DisplaySize.x;
  int screenH = (int)io.DisplaySize.y;

  /* Reconfigure layout each frame */
  tabletLayoutConfigure(&s_cfg, screenW, screenH, vpX, vpY, vpW, vpH, vpZoom);

  /* Update viewport bounds for tap detection */
  inputTouchSetViewportBounds(vpX, vpY, vpW, vpH, vpZoom);

  /* Process tap-to-build */
  handleTapToBuild(cs);

  /* Damage detection — trigger haptic on armour decrease */
  {
    BYTE shells, mines, armour, trees;
    screenGetTankStatsCS(cs, &shells, &mines, &armour, &trees);
    if (s_armourInitialized && armour < s_prevArmour) {
      inputTouchTriggerHaptic(0.3f, 20);  /* light pulse on taking damage */
    }
    s_prevArmour = armour;
    s_armourInitialized = true;
  }

  renderJoystickOverlay();
  renderShootMineButtons();
  renderBuildSelectBar(cs);
  renderMessagesOverlay();
  renderViewToggleButton(cs);
  renderTopBarButtons(cs);

  /* Status grids are rendered by sdl3DrawMainScreen() using the same SDL
     rendering as desktop (BLENDMODE_NONE), positioned in the left gutter
     via sdl3DrawSetStatusPanelOrigins().  Fall back to the ImGui status
     drawer when the gutter is too narrow. */
  if (!s_cfg.showStatusGrids) {
    renderStatusDrawer(cs);
  }

  /* Stock bars in right gutter, or included in drawer */
  if (s_cfg.showStockBars) {
    renderStockBars(cs);
  }
}
