/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          SDL3 ImGui Tablet Overlay
*Filename:      sdl3imgui_tablet.cpp
*Purpose:
*  Renders the tablet-mode ImGui overlay:
*  - Floating virtual joystick visualization
*  - Action buttons (fire, mine, gunsight, view toggle, voice talk)
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
#include "../../common/wb_log.h"
#include "global.h"
#include "screentank.h"
#include "client_sim.h"
#include "client_command.h" /* VIEW_KIND_* — which view a button selects */
#include "client_render.h"  /* clientRenderFrame */
#include "../gamefront.h"
#include "../tiles.h"
#include "../positions.h"  /* STATUS_TANKS_WIDTH/HEIGHT: the status grid frames */
#include "../ui_mode.h"
#include "../lang.h"
#include "input_touch.h"
#include "sdl3draw.h"
#include "build_cursor.h"
#include "scenario_panel_slot.h"
#include "../clientmutex.h"
#if defined(WINBOLO_VOICE)
#include "../voice.h"  /* the talk button: mode, enabled, muted, transmitting */
#endif
}

#include "overview_view.h"
#include "overview_hud_layout.h"

#include "sdl3imgui.h"
#include "sdl3imgui_tablet.h"

static bool s_statusDrawerOpen = false;

/* Static layout config, recomputed each frame */
static TabletLayoutConfig s_cfg;

/* The touch layout's full screen (sdl3DrawIsTabletMapFullScreen), read once a
   frame. The map then fills the screen and there is no gray chrome behind the
   gutters, so the panels drawn there are laid over the map at the desktop
   full screen HUD's transparency settings: the status grids and the stock
   bars at the status panel's, the build bar at the builder tools', the
   newswire at the newswire's. Each panel's pieces and its black backing take
   separate alphas (sdl3DrawHudPanelAlphas). All six are 1 outside full screen,
   where the panels look as they always have. */
struct TabletPanelAlpha {
  float piece;
  float backing;
};
static bool             s_fullScreen = false;
static TabletPanelAlpha s_statusAlpha = { 1.0f, 1.0f };
static TabletPanelAlpha s_buildAlpha  = { 1.0f, 1.0f };
static TabletPanelAlpha s_newsAlpha   = { 1.0f, 1.0f };

static void tabletPanelAlphasUpdate(void) {
  s_fullScreen = sdl3DrawIsTabletMapFullScreen();
  if (!s_fullScreen) {
    s_statusAlpha = s_buildAlpha = s_newsAlpha = { 1.0f, 1.0f };
    return;
  }
  sdl3DrawHudPanelAlphas(sdl3DrawGetStatusPanelTransparency(),
                         &s_statusAlpha.piece, &s_statusAlpha.backing);
  sdl3DrawHudPanelAlphas(sdl3DrawGetBuildPanelTransparency(),
                         &s_buildAlpha.piece, &s_buildAlpha.backing);
  sdl3DrawHudPanelAlphas(sdl3DrawGetNewswireTransparency(),
                         &s_newsAlpha.piece, &s_newsAlpha.backing);
}

/* The list every touch control is drawn on. ImGui's background list, which
   is drawn over the game and under every ImGui window, so a dialog is always
   on top of the controls; the foreground list they used to be on is drawn
   over every window, dialogs included. */
static ImDrawList *controlsDrawList(void) {
  return ImGui::GetBackgroundDrawList();
}

/* Hand the touch input the rects of the open dialogs, so a finger that comes
   down on one is the dialog's and not the control under it: every window up
   last frame that takes input, other than the overlay's own panels, tooltips
   and ImGui's implicit debug window. One frame behind, which a finger landing
   cannot tell. */
static void tabletBlockDialogTouches(const SDL_FRect *extra, int extraCount) {
  /* "##OverviewInWindow" is the full screen map's own host window on the
     classic screen, which the controls drive. */
  static const char *const kOwnWindows[] = {
    "##BuildBar", "##TankBars", "##BaseBars", "##ScenarioSlot", "Debug##Default",
    "##OverviewInWindow",
  };
  ImGuiContext &g = *ImGui::GetCurrentContext();
  SDL_FRect rects[16];
  int n = 0;
  for (int i = 0; i < extraCount && n < (int)(sizeof(rects) / sizeof(rects[0])); i++) {
    rects[n++] = extra[i];
  }
  for (ImGuiWindow *w : g.Windows) {
    if (n >= (int)(sizeof(rects) / sizeof(rects[0]))) break;
    if (!w->WasActive || w->Hidden) continue;
    if (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_NoInputs |
                    ImGuiWindowFlags_Tooltip)) {
      continue;
    }
    if (w->Size.x <= 0.0f || w->Size.y <= 0.0f) continue;
    bool own = false;
    for (const char *name : kOwnWindows) {
      if (strcmp(w->Name, name) == 0) { own = true; break; }
    }
    if (own) continue;
    rects[n].x = w->Pos.x;
    rects[n].y = w->Pos.y;
    rects[n].w = w->Size.x;
    rects[n].h = w->Size.y;
    n++;
  }
  inputTouchSetBlockedRects(rects, n);

  /* Which windows block, each time the set changes, for checking by eye. */
  static int lastCount = -1;
  if (n != lastCount) {
    lastCount = n;
    WB_LOG_DEBUG(WB_LOG_CAT_GUI, "tablet: %d dialog rect(s) take touches", n);
    for (ImGuiWindow *w : g.Windows) {
      if (!w->WasActive || w->Hidden) continue;
      WB_LOG_DEBUG(WB_LOG_CAT_GUI, "tablet:   window '%s' flags=0x%x %.0fx%.0f at %.0f,%.0f",
                   w->Name, (unsigned)w->Flags, w->Size.x, w->Size.y, w->Pos.x, w->Pos.y);
    }
  }
}

static ImU32 scaleAlpha(ImU32 col, float alpha) {
  int a = (int)((float)((col >> 24) & 0xFF) * alpha);
  if (a < 0) a = 0;
  if (a > 255) a = 255;
  return (col & 0x00FFFFFF) | ((ImU32)a << 24);
}

/* Scale the alpha of every vertex added to dl since vtxStart, so a panel
   drawn in colours of its own fades as one piece at its setting. */
static void scaleDrawListAlphaSince(ImDrawList *dl, int vtxStart, float alpha) {
  if (alpha >= 1.0f) return;
  for (int i = vtxStart; i < dl->VtxBuffer.Size; i++) {
    dl->VtxBuffer[i].col = scaleAlpha(dl->VtxBuffer[i].col, alpha);
  }
}

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

#if defined(WINBOLO_VOICE)
/* Talk button icons (white SVG masks, loaded on first draw of the button) */
static SDL_Texture *s_iconMic      = nullptr;
static SDL_Texture *s_iconMicMuted = nullptr;
static bool s_micIconsLoaded = false;
#endif

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

/* The window's safe area insets, in the layout's units (the window's points
   scaled to screenW x screenH). */
static void tabletSafeInsets(TabletLayoutConfig *cfg, int screenW, int screenH) {
  cfg->safeTop = cfg->safeBottom = cfg->safeLeft = cfg->safeRight = 0.0f;
  SDL_Window *win = sdl3DrawGetWindow();
  if (!win) return;
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


  tabletSafeInsets(cfg, screenW, screenH);
  /* The touch input's own stick rule: the left 40% outside the game view. */
  cfg->joyZoneX = cfg->joyZoneY = cfg->joyZoneW = cfg->joyZoneH = 0.0f;
  /* The map button at the game view's top left (overviewButtonRect). */
  cfg->mapBtnX = cfg->mapBtnY = cfg->mapBtnR = 0.0f;

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

  /* Voice talk button — same row as the gunsight pair, in the space between
     the increase button and the right edge, so it sits above the fire button
     and below the scroll joystick zone.  Shrunk when that space is narrower
     than a gunsight button, rather than moved over something else. */
  {
    float spaceLeft = cfg->gsIncCenterX + cfg->gsIncRadius + pad;
    float spaceRight = rightEdge - pad;
    cfg->talkRadius = cfg->gsIncRadius;
    if ((spaceRight - spaceLeft) * 0.5f < cfg->talkRadius) {
      cfg->talkRadius = (spaceRight - spaceLeft) * 0.5f;
    }
    if (cfg->talkRadius < 0.0f) cfg->talkRadius = 0.0f;
    cfg->talkCenterX = spaceRight - cfg->talkRadius;
    cfg->talkCenterY = gsRowY;
  }
#if defined(__IPHONEOS__)
  /* A narrow gutter (an iPad in the touch layout has about 311 units) leaves
     the gunsight row too little room, and the talk button came out about a
     third of a gunsight button's width. There it gets a row of its own, at
     the right edge just above the gunsight pair, at a gunsight button's
     size; the scroll joystick zone ends above it (below), so a press on it
     is the button's and not the joystick's. */
  bool talkOwnRow = false;
  if (cfg->talkRadius < cfg->gsIncRadius * 0.75f) {
    cfg->talkRadius = cfg->gsIncRadius;
    cfg->talkCenterX = rightEdge - pad - cfg->talkRadius;
    cfg->talkCenterY = gsRowY - cfg->gsIncRadius - pad - cfg->talkRadius;
    talkOwnRow = true;
  }
#endif

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
  /* The grids themselves are drawn by SDL in sdl3DrawMainScreen, at the
     status panels' own size for the effective zoom (STATUS_TANKS_WIDTH x
     STATUS_TANKS_HEIGHT times the zoom), 4 units in from the safe area with
     4 units between them. The frames drawn here must sit on exactly those
     rectangles, so the geometry is the same as sdl3draw.c's, not a scaled
     design-space size. */
  cfg->tanksGridX = cfg->safeLeft + 4.0f;
  cfg->tanksGridY = cfg->safeTop + 4.0f;
  cfg->statusGridScale = 1.0f;
  float gridH = (float)(STATUS_TANKS_HEIGHT * effectiveZoom);
  float gridGap = 4.0f;
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
#if defined(__IPHONEOS__)
  if (talkOwnRow) {
    scrollBottom = cfg->talkCenterY - cfg->talkRadius - pad * 2;
  }
  /* The resource bars are a fixed 280 units each, which on a phone ran the
     base bars down past the scroll joystick zone's top and over the view
     buttons (iPhone 17 Pro and SE-class screens alike). Shortened so both end
     a pad above the zone; a screen with the room keeps 280. */
  {
    float fit = (scrollTop - pad - buildRegionTop + 16.0f) * 0.5f;
    if (fit < cfg->barsH) {
      cfg->barsH = fit;
      cfg->baseBarsY = cfg->tankBarsY + cfg->barsH;
    }
  }
#endif
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

void tabletLayoutConfigureClassic(TabletLayoutConfig *cfg, int screenW, int screenH,
                                  float gameX, float gameY, float gameW, float gameH,
                                  bool mapUp, const OverviewHudLayout *hudIn) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->screenW = screenW;
  cfg->screenH = screenH;
  cfg->viewportX = (int)gameX;
  cfg->viewportY = (int)gameY;
  cfg->viewportW = (int)gameW;
  cfg->viewportH = (int)gameH;
  cfg->effectiveZoom = 1;
  tabletSafeInsets(cfg, screenW, screenH);

  /* Sizes in units of the shorter side over 480, so a control is the same
     size in either orientation: 2.15 points on a 13-inch iPad, 1.74 on an
     11-inch. */
  float u = (float)(screenW < screenH ? screenW : screenH) / 480.0f;
  float pad = 6.0f * u;

  cfg->idleOpacity   = 0.4f;
  cfg->activeOpacity = 1.0f;
  cfg->topBtnOpacity = 0.6f;
  cfg->joyFadeOutMs  = 1000;
  cfg->joyOuterRadius = 50.0f * u;
  cfg->joyInnerRadius = 21.0f * u;
  cfg->fireRadius  = 34.0f * u;
  cfg->mineRadius  = 24.0f * u;
  cfg->gsIncRadius = cfg->gsDecRadius = 18.0f * u;
  cfg->talkRadius  = 18.0f * u;
  cfg->pillViewRadius = cfg->baseViewRadius =
      cfg->allyViewRadius = cfg->tankViewRadius = 18.0f * u;
  cfg->viewBtnGap = pad;
  cfg->topBtnSize = 24.0f * u;
  /* No drawer: the classic screen and the map's HUD have status panels. */
  cfg->showStatusGrids = true;
  float side = 2.6f * cfg->joyOuterRadius;

  /* The safe area, and the box each group of controls goes in. */
  float safeL = cfg->safeLeft;
  float safeT = cfg->safeTop;
  float safeR = (float)screenW - cfg->safeRight;
  float safeB = (float)screenH - cfg->safeBottom;
  float clusterR = safeR;   /* fire cluster: right and bottom edges */
  float clusterB = safeB;
  float topT = safeT;       /* top buttons: top and right edges */
  float topR = safeR;
  bool inBand = false;
  OverviewHudLayout hud;
  bool haveHud = mapUp && hudIn != nullptr;
  if (haveHud) hud = *hudIn;

  if (mapUp) {
    /* The full screen map: the cluster keeps the screen's bottom right corner,
       over the status column and the newswire, which have nothing to press. */
  } else {
    /* The band below the classic screen, when it has room for the fire
       button, the gunsight row above it and the talk row above that. */
    float bandT = gameY + gameH;
    float clusterH = 2.0f * (cfg->fireRadius + cfg->gsIncRadius + cfg->talkRadius) + 4.0f * pad;
    inBand = (safeB - bandT) >= clusterH;
    if (inBand) topT = bandT;
  }

  /* Bottom right: fire, mine to its left, the gunsight pair above them, the
     talk button above the gunsight increase, and the view buttons in a column
     to the left of mine, stacked up from fire's row. */
  cfg->fireCenterX = clusterR - pad - cfg->fireRadius;
  cfg->fireCenterY = clusterB - pad - cfg->fireRadius;
  cfg->mineCenterX = cfg->fireCenterX - cfg->fireRadius - pad - cfg->mineRadius;
  cfg->mineCenterY = cfg->fireCenterY;
  float gsRowY = cfg->fireCenterY - cfg->fireRadius - pad - cfg->gsIncRadius;
  cfg->gsDecCenterX = cfg->mineCenterX;
  cfg->gsDecCenterY = gsRowY;
  cfg->gsIncCenterX = cfg->fireCenterX;
  cfg->gsIncCenterY = gsRowY;
  cfg->talkCenterX = cfg->fireCenterX;
  cfg->talkCenterY = gsRowY - cfg->gsIncRadius - pad - cfg->talkRadius;
  float viewX = cfg->mineCenterX - cfg->mineRadius - pad - cfg->pillViewRadius;
  cfg->pillViewCenterX = cfg->baseViewCenterX =
      cfg->allyViewCenterX = cfg->tankViewCenterX = viewX;
  cfg->showPillViewBtn = cfg->showBaseViewBtn = cfg->showAllyViewBtn = true;
  {
    float step = cfg->pillViewRadius * 2.0f + cfg->viewBtnGap;
    cfg->pillViewCenterY = cfg->fireCenterY;
    cfg->baseViewCenterY = cfg->pillViewCenterY - step;
    cfg->allyViewCenterY = cfg->baseViewCenterY - step;
    cfg->tankViewCenterY = cfg->allyViewCenterY - step;
  }

  /* Top right: messages, players, Settings, left to right, as in the touch
     layout (its field names have the first two the other way round), and
     the map button left of them. */
  cfg->topBtnY = topT + pad;
  cfg->cogBtnX = topR - pad - cfg->topBtnSize;
  cfg->msgBtnX = cfg->cogBtnX - pad - cfg->topBtnSize;
  cfg->playersBtnX = cfg->msgBtnX - pad - cfg->topBtnSize;
  cfg->mapBtnR = cfg->topBtnSize * 0.5f;
  cfg->mapBtnX = cfg->playersBtnX - pad - cfg->mapBtnR;
  cfg->mapBtnY = cfg->topBtnY + cfg->mapBtnR;

  /* The stick. In the band, its left half. Over the classic screen, the
     corner below the build column's last icon, where the chrome has nothing
     to press, so choosing what to build still works; over the map, the corner
     below the build tools and left of the newswire. The stick follows the
     finger out of either. Its resting ring sits in the zone's bottom left. */
  if (inBand) {
    cfg->joyZoneX = safeL;
    cfg->joyZoneY = topT;
    cfg->joyZoneW = (safeR - safeL) * 0.5f;
    cfg->joyZoneH = safeB - topT;
  } else {
    float colR, colB;
    if (haveHud) {
      colR = gameX + hud.newswireX - pad;
      colB = gameY + hud.buildY + hud.buildH + pad;
    } else {
      colR = gameX + (float)MAIN_OFFSET_X * gameW / (float)SDL3_SCREEN_W;
      colB = gameY + (float)(BS_MINE_OFFSET_Y + BS_ITEM_SIZE_Y) * gameH /
             (float)SDL3_SCREEN_H;
    }
    cfg->joyZoneX = safeL;
    cfg->joyZoneW = colR - safeL > side ? colR - safeL : side;
    cfg->joyZoneY = colB < safeB - side ? colB : safeB - side;
    cfg->joyZoneH = safeB - cfg->joyZoneY;
  }

  /* The scroll stick: right of the view buttons and above the talk button,
     up to a square the size of the stick's, or to the top buttons when they
     are nearer. Over the map it pans the map, as a drag does. */
  cfg->scrollJoyOuterRadius = 40.0f * u;
  cfg->scrollJoyInnerRadius = cfg->scrollJoyOuterRadius * 0.42f;
  {
    float bottom = cfg->talkCenterY - cfg->talkRadius - pad;
    float top = cfg->topBtnY + cfg->topBtnSize + pad;
    if (bottom - top > side) top = bottom - side;
    cfg->scrollJoyX = cfg->mineCenterX - cfg->mineRadius;
    cfg->scrollJoyW = clusterR - cfg->scrollJoyX;
    cfg->scrollJoyY = top;
    cfg->scrollJoyH = bottom > top ? bottom - top : 0.0f;
  }
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

  /* In full screen the panels lie over the map. A frame there is the bevel
     alone, at its panel's piece alpha: the backing under each panel is its
     own window's background, at the backing alpha, and a black fill here as
     well would darken the map through it twice. The button groups (top row,
     fire and mine, gunsight) and the newswire get no frame, since their
     buttons and the newswire's window carry their own backing. */
  ImU32 colNone = IM_COL32(0, 0, 0, 0);
  ImU32 statusLight = scaleAlpha(colLight, s_statusAlpha.piece);
  ImU32 statusDark  = scaleAlpha(colDark, s_statusAlpha.piece);
  ImU32 statusFill  = s_fullScreen ? colNone : colBlack;
  ImU32 buildLight  = scaleAlpha(colLight, s_buildAlpha.piece);
  ImU32 buildDark   = scaleAlpha(colDark, s_buildAlpha.piece);
  ImU32 buildFill   = s_fullScreen ? colNone : colBlack;

  /* --- Left gutter: status grids (tanks, pills, bases) --- */
  if (c.showStatusGrids) {
    /* The grids were drawn by SDL before this list, so the frame's fill is
       transparent: a black fill here is drawn over them and hides them,
       which is what the phone showed. The frame is one border wide outside
       the grid, so it covers no part of it. */
    float gridW = (float)(STATUS_TANKS_WIDTH * c.effectiveZoom);
    float gridH = (float)(STATUS_TANKS_HEIGHT * c.effectiveZoom);

    drawInsetRect(dl,
      c.tanksGridX - border, c.tanksGridY - border,
      gridW + border * 2, gridH + border * 2,
      border, statusLight, statusDark, colNone);

    drawInsetRect(dl,
      c.pillsGridX - border, c.pillsGridY - border,
      gridW + border * 2, gridH + border * 2,
      border, statusLight, statusDark, colNone);

    drawInsetRect(dl,
      c.basesGridX - border, c.basesGridY - border,
      gridW + border * 2, gridH + border * 2,
      border, statusLight, statusDark, colNone);
  }

  /* --- Right gutter: top buttons row --- */
  if (!s_fullScreen) {
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
      border, statusLight, statusDark, statusFill);

    float baseH = c.barsH - 16.0f;
    drawInsetRect(dl,
      c.baseBarsX - pad - border, c.baseBarsY - pad - border,
      c.topBtnSize + (pad + border) * 2, baseH + (pad + border) * 2,
      border, statusLight, statusDark, statusFill);
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
      border, buildLight, buildDark, buildFill);
  }

  /* --- Right gutter: fire/mine button area --- */
  if (!s_fullScreen) {
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
  if (!s_fullScreen) {
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
  if (!s_fullScreen) {
    float pad = 3.0f * ps;
    float msgH = 76.0f * ps;  /* matches renderMessagesOverlay height */
    /* The bevel alone: the strip SDL draws inside it carries its own
       backing, and slides away to show the game view under it. */
    drawInsetRect(dl,
      c.msgOverlayX - pad - border, c.msgOverlayY - pad - border,
      c.msgOverlayW + (pad + border) * 2, msgH + (pad + border) * 2,
      border, colLight, colDark, colNone);
  }
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

  ImDrawList *dl = controlsDrawList();
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
    ImDrawList *dl = controlsDrawList();
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

  ImDrawList *dl = controlsDrawList();
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
  ImDrawList *dl = controlsDrawList();
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
  ImDrawList *dl = controlsDrawList();
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
 * Voice talk button
 * ------------------------------------------------------- */

/* Whether the talk button is on screen: voice is switched on, the chosen
   mode sends audio, and this connection carries the player's voice to
   anyone.  That leaves it off in single player, on a server started with
   voice off, and for a viewer.  What the button does is decided in input.c
   with the keys it stands in for: held in push-to-talk mode, and the mute
   toggle in open mic mode. iOS only: a desktop or web client on a small
   touch screen also takes this layout, and keeps its own talk key there. */
static bool tabletTalkButtonShown(void) {
#if defined(WINBOLO_VOICE) && defined(__APPLE__) && TARGET_OS_IOS
  return s_cfg.talkRadius > 0.0f && voiceIsEnabled() &&
         voiceGetMode() != VOICE_MODE_OFF && voiceConnectionCarriesVoice();
#else
  return false;
#endif
}

static void renderTalkButton(void) {
#if defined(WINBOLO_VOICE)
  static bool s_prevTalkPressed = false;

  if (!tabletTalkButtonShown()) {
    s_prevTalkPressed = false;
    return;
  }

  ImDrawList *dl = controlsDrawList();
  float cx = s_cfg.talkCenterX;
  float cy = s_cfg.talkCenterY;
  float r = s_cfg.talkRadius;
  bool held = inputTouchIsButtonHeld(TOUCH_BTN_TALK);
  bool muted = voiceIsSelfMuted();
  bool sending = voiceIsTransmitting();

  if (!s_micIconsLoaded) {
    SDL_Renderer *rend = sdl3DrawGetRenderer();
    s_micIconsLoaded = true;
    s_iconMic      = imguiLoadSvgIconWhite(rend, "data/ui/mic.svg", (int)(r * 4.0f));
    s_iconMicMuted = imguiLoadSvgIconWhite(rend, "data/ui/mic-muted.svg", (int)(r * 4.0f));
  }

  if (held && !s_prevTalkPressed) {
    inputTouchTriggerHaptic(0.4f, 20);
  }
  s_prevTalkPressed = held;

  float alpha = (held || sending) ? s_cfg.activeOpacity : s_cfg.idleOpacity;
  /* Green while audio is going out, red outline while the player's own
     microphone is muted, the plain black and white of the other buttons
     otherwise. */
  ImU32 fill = sending ? IM_COL32(30, 130, 50, 200) : IM_COL32(0, 0, 0, 200);
  ImU32 outline = muted ? IM_COL32(230, 70, 70, 220) : IM_COL32(255, 255, 255, 220);
  dl->AddCircleFilled(ImVec2(cx, cy), r, scaleAlpha(fill, alpha), 32);
  dl->AddCircle(ImVec2(cx, cy), r, scaleAlpha(outline, alpha), 32, 2.0f);

  SDL_Texture *icon = muted ? s_iconMicMuted : s_iconMic;
  if (icon) {
    float iconHalf = r * 0.6f;
    dl->AddImage((ImTextureID)icon, ImVec2(cx - iconHalf, cy - iconHalf),
                 ImVec2(cx + iconHalf, cy + iconHalf), ImVec2(0, 0), ImVec2(1, 1),
                 scaleAlpha(IM_COL32(255, 255, 255, 255), alpha));
  }
#endif
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
  ImDrawList *dl = controlsDrawList();
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
  /* In full screen the background is the panel's backing over the map, and
     everything drawn on it below takes the panel's piece alpha. */
  ImGui::PushStyleColor(ImGuiCol_WindowBg,
                        ImVec4(0.0f, 0.0f, 0.0f,
                               s_fullScreen ? s_buildAlpha.backing : 0.5f));

  if (ImGui::Begin("##BuildBar", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_NoScrollWithMouse)) {
    ImDrawList *barDl = ImGui::GetWindowDrawList();
    int barVtxStart = barDl->VtxBuffer.Size;

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
    scaleDrawListAlphaSince(barDl, barVtxStart, s_buildAlpha.piece);
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

/* -------------------------------------------------------
 * Top bar buttons: Players (people icon), Message (chat), Settings (cog)
 * ------------------------------------------------------- */

static void renderTopBarButtons(ClientSim *cs) {
  ImDrawList *dl = controlsDrawList();
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
    /* The stock bars are part of the status panel in full screen, as they
       are in the desktop HUD's column. */
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImVec4(0.0f, 0.0f, 0.0f,
                                 s_fullScreen ? s_statusAlpha.backing : 0.5f));

    if (ImGui::Begin("##TankBars", nullptr, winFlags)) {
      ImDrawList *dl = ImGui::GetWindowDrawList();
      int vtxStart = dl->VtxBuffer.Size;
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
      scaleDrawListAlphaSince(dl, vtxStart, s_statusAlpha.piece);
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
    float baseBack = s_fullScreen ? s_statusAlpha.backing : 0.5f;
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImVec4(0.0f, 0.0f, 0.0f, hasBase ? baseBack : baseBack * 0.5f));

    if (ImGui::Begin("##BaseBars", nullptr, winFlags)) {
      ImDrawList *dl = ImGui::GetWindowDrawList();
      int vtxStart = dl->VtxBuffer.Size;
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
      scaleDrawListAlphaSince(dl, vtxStart, s_statusAlpha.piece);
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

/* The newswire, along the bottom of the game view's square. SDL draws it
   (sdl3DrawNewswireStrip), in the classic newswire's fixed-width face, so the
   scroll moves at one speed and the lines stay in step, and under every ImGui
   window. It slides on the shared slide (sdl3DrawNewswireSlideStep) that the
   desktop's full screen map uses: the same hold time and auto-hide setting.
   This hands the draw where the strip goes, the edge it slides off by (the
   bottom of the square, or of the screen in full screen, where the map
   carries on below the square), its size and its alphas; the draw uses them
   from the next frame. */
static void renderMessagesOverlay(void) {
  float edge = s_fullScreen ? (float)s_cfg.screenH
                            : (float)(s_cfg.viewportY + s_cfg.viewportH);
  /* A quarter above the overlay font, which is already sized for reading on
     the device (applyMainContextUiScale); 1.6 made two newswire lines take
     a seventh of a phone's screen. */
  int fontPx = (int)(ImGui::GetFontSize() * 1.25f + 0.5f);
  float textAlpha = s_fullScreen ? s_newsAlpha.piece : 220.0f / 255.0f;
  float backAlpha = s_fullScreen ? s_newsAlpha.backing : 0.5f;
  sdl3DrawSetTabletNewswire(s_cfg.msgOverlayX, s_cfg.msgOverlayY,
                            s_cfg.msgOverlayW, edge, fontPx, textAlpha,
                            backAlpha);
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
 * Map overview — the in-window map drawn inside the game
 * view (sdl3draw.c), opened and closed by a button in the
 * view's top-left corner. One finger drags the map, two
 * pinch to zoom, a tap builds at the square under it, the
 * scroll joystick pans and the Tank View button turns
 * follow back on: the touch forms of the desktop's right
 * drag, wheel, left click, scroll keys and follow key.
 * ------------------------------------------------------- */

/* The map button's icon, loaded on first use like the top bar's. */
static SDL_Texture *s_iconMap = nullptr;
static bool s_iconMapLoaded = false;

/* Pinch travel not yet turned into a zoom step, as the log of the spacing
   ratio. One step per 0.15, the threshold the map views use for the macOS
   trackpad pinch. */
#define OVERVIEW_PINCH_STEP 0.15f
static float s_overviewPinchAccum = 0.0f;

/* Scroll joystick pan speed on the map, in view pixels a frame. */
#define OVERVIEW_JOY_PAN_PX 8.0f

/* Where the map button sits: inside the game view's top-left corner, clear
   of the scenario panel in the top-right and the newswire along the bottom. */
static void overviewButtonRect(float *cx, float *cy, float *r) {
  if (s_cfg.mapBtnR > 0.0f) {  /* the classic screen's, beside the top buttons */
    *cx = s_cfg.mapBtnX;
    *cy = s_cfg.mapBtnY;
    *r  = s_cfg.mapBtnR;
    return;
  }
  float ps = (float)s_cfg.screenH / 480.0f;
  if (ps < 0.7f) ps = 0.7f;
  *r  = 14.0f * ps;
  *cx = (float)s_cfg.viewportX + 6.0f * ps + *r;
  *cy = (float)s_cfg.viewportY + 6.0f * ps + *r;
}

/* Only in a running game, and not while the server withholds the overview;
   the map stays closable whatever happens, so the button stays up while it
   is open. iOS only: Android and a desktop in the tablet layout have no
   in-window map to open (sdl3ImguiShowOverviewInWindow does nothing there). */
static bool overviewButtonShown(ClientSim *cs) {
#if defined(__APPLE__) && TARGET_OS_IOS
  if (sdl3DrawIsOverviewInWindow()) return true;
  return cs != nullptr && clientSimIsRunning(cs) && !clientSimIsInLobby(cs) &&
         sdl3ImguiIsOverviewOffered();
#else
  (void)cs;
  return false;
#endif
}

/* Ahead of the tap handlers below: the button lies inside the game view, so a
   tap on it is also a viewport tap, and that one is dropped here rather than
   left to build under the button. */
static void processOverviewButton(ClientSim *cs) {
  if (!overviewButtonShown(cs)) return;
  float cx, cy, r;
  overviewButtonRect(&cx, &cy, &r);
  if (inputTouchConsumeTapInRect(cx - r, cy - r, r * 2.0f, r * 2.0f)) {
    float tx, ty;
    (void)inputTouchGetViewportTapPos(&tx, &ty);
    if (sdl3ImguiIsDialogOpen()) return;
    sdl3ImguiShowOverviewInWindow(!sdl3ImguiIsOverviewInWindowOpen());
  }
}

static void renderOverviewButton(ClientSim *cs) {
  if (!overviewButtonShown(cs)) return;
  float cx, cy, r;
  overviewButtonRect(&cx, &cy, &r);
  if (!s_iconMapLoaded) {
    s_iconMapLoaded = true;
    s_iconMap = imguiLoadSvgIconWhite(sdl3DrawGetRenderer(),
                                      "data/ui/grid-view.svg", (int)(r * 4.0f));
  }
  bool on = sdl3DrawIsOverviewInWindow();
  float alpha = on ? s_cfg.activeOpacity : s_cfg.topBtnOpacity;
  ImDrawList *dl = controlsDrawList();
  dl->AddCircleFilled(ImVec2(cx, cy), r, scaleAlpha(IM_COL32(0, 0, 0, 200), alpha), 32);
  /* Outlined like the view buttons: yellow while the map is up. */
  ImU32 outline = on ? IM_COL32(255, 255, 100, 220) : IM_COL32(255, 255, 255, 220);
  dl->AddCircle(ImVec2(cx, cy), r, scaleAlpha(outline, alpha), 32, 2.0f);
  ImU32 tint = scaleAlpha(IM_COL32(255, 255, 255, 255), alpha);
  float half = r * 0.6f;
  if (s_iconMap) {
    dl->AddImage((ImTextureID)s_iconMap, ImVec2(cx - half, cy - half),
                 ImVec2(cx + half, cy + half), ImVec2(0, 0), ImVec2(1, 1), tint);
  } else {
    /* No icon file: four squares, a map grid. */
    float gap = half * 0.2f;
    for (int i = 0; i < 4; i++) {
      float x0 = (i % 2 == 0) ? cx - half : cx + gap * 0.5f;
      float y0 = (i < 2) ? cy - half : cy + gap * 0.5f;
      float side = half - gap * 0.5f;
      dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + side, y0 + side), tint, 0.0f, 0, 1.5f);
    }
  }

  /* The zoom and follow readout for a moment after either changes, beside
     the button. No keys are passed, so it names no keyboard key. */
  if (on) {
    char status[96];
    float noticeAlpha = 0.0f;
    OverviewView *view = sdl3DrawOverviewInWindowView();
    if (view && overviewViewCameraNotice(view, nullptr, status, sizeof(status),
                                         &noticeAlpha)) {
      const float pad = 4.0f;
      ImVec2 textSize = ImGui::CalcTextSize(status);
      ImVec2 boxMin(cx + r + pad * 2.0f, cy - textSize.y * 0.5f - pad);
      ImVec2 boxMax(boxMin.x + textSize.x + pad * 2.0f,
                    boxMin.y + textSize.y + pad * 2.0f);
      dl->AddRectFilled(boxMin, boxMax,
                        IM_COL32(0, 0, 0, (int)(160.0f * noticeAlpha)));
      dl->AddText(ImVec2(boxMin.x + pad, boxMin.y + pad),
                  IM_COL32(230, 230, 230, (int)(255.0f * noticeAlpha)), status);
    }
  }
}

/* The map's input for this frame, in place of the classic view's tap to build
   and drag scroll. Every way the camera changes here shows the zoom and follow
   readout, as the desktop's input does. */
/* scrollPansMap: the scroll stick pans the map (the touch layout); false on
   the classic screen's full screen map, where it moves the tank's view as it
   does over the classic screen (processScrollJoystick). */
static void processOverviewTouch(ClientSim *cs, bool scrollPansMap) {
  OverviewView *view = sdl3DrawOverviewInWindowView();
  OverviewCamera *cam = view ? overviewViewCamera(view) : nullptr;
  float rx = 0.0f, ry = 0.0f, rw = 0.0f, rh = 0.0f;
  float tx, ty;
  if (!cam || !sdl3DrawGetOverviewInWindowRect(&rx, &ry, &rw, &rh)) {
    /* Nothing drawn yet to map the touch onto, so the tap goes nowhere. */
    (void)inputTouchGetViewportTapPos(&tx, &ty);
    return;
  }
  int viewW = (int)rw;
  int viewH = (int)rh;
  int  zoomBefore   = cam->zoomIndex;
  bool followBefore = cam->follow;

  /* Negated: dragging right pulls the map right, which is the centre moving
     left. */
  float dx, dy;
  if (inputTouchConsumeViewportDragTravel(&dx, &dy)) {
    overviewCameraPan(cam, viewW, viewH, -dx, -dy);
  }

  int scrollX = 0, scrollY = 0;
  if (scrollPansMap && inputTouchGetScrollDirection(&scrollX, &scrollY)) {
    overviewCameraPan(cam, viewW, viewH, (float)scrollX * OVERVIEW_JOY_PAN_PX,
                      (float)scrollY * OVERVIEW_JOY_PAN_PX);
  }

  /* Spreading the fingers zooms in, about the point between them. */
  float scale, pcx, pcy;
  if (inputTouchConsumeViewportPinch(&scale, &pcx, &pcy) && scale > 0.0f) {
    s_overviewPinchAccum += logf(scale);
    while (s_overviewPinchAccum >= OVERVIEW_PINCH_STEP) {
      overviewCameraZoomAt(cam, viewW, viewH, pcx - rx, pcy - ry, 1);
      s_overviewPinchAccum -= OVERVIEW_PINCH_STEP;
    }
    while (s_overviewPinchAccum <= -OVERVIEW_PINCH_STEP) {
      overviewCameraZoomAt(cam, viewW, viewH, pcx - rx, pcy - ry, -1);
      s_overviewPinchAccum += OVERVIEW_PINCH_STEP;
    }
  }

  /* A tap builds at the square under it, as a left click does on the
     desktop's map. Dropped under a dialog, as the classic tap is. */
  if (inputTouchGetViewportTapPos(&tx, &ty) && !sdl3ImguiIsDialogOpen() && cs) {
    int mx = 0, my = 0;
    if (overviewCameraScreenToWorld(cam, viewW, viewH, tx - rx, ty - ry, &mx, &my)) {
      clientMutexWaitFor();
      clientSimManMoveToMap(cs, (BYTE)mx, (BYTE)my,
                            clientSimGetCurrentBuildSelect(cs));
      clientMutexRelease();
      buildCursorSetTile((BYTE)mx, (BYTE)my);
    }
  }

  if (cam->zoomIndex != zoomBefore || cam->follow != followBefore) {
    overviewViewShowCameraNotice(view);
  }
}

/* The Tank View button outside an item view: the map goes back to following
   the tank, which is what the desktop's follow key does. */
static void overviewFollowTank(void) {
  OverviewView *view = sdl3DrawOverviewInWindowView();
  OverviewCamera *cam = view ? overviewViewCamera(view) : nullptr;
  if (!cam || cam->follow) return;
  cam->follow = true;
  overviewViewShowCameraNotice(view);
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
  if (tabletTalkButtonShown()) {
    inputTouchRegisterButton(TOUCH_BTN_TALK, s_cfg.talkCenterX, s_cfg.talkCenterY,
                             s_cfg.talkRadius);
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
  s_prevArmour = 0;
  s_armourInitialized = false;
}

/* A buzz when the tank's armour drops. */
static void tabletDamageHaptic(void) {
  BYTE shells, mines, armour, trees;
  sdl3DrawGetCachedTankStats(&shells, &mines, &armour, &trees);
  if (s_armourInitialized && armour < s_prevArmour) {
    inputTouchTriggerHaptic(0.3f, 20);
  }
  s_prevArmour = armour;
  s_armourInitialized = true;
}

/* The view buttons' taps. A tap on the view you are already in is left
   alone, matching what the pill button has always done. overviewOn: the
   in-window map is up, where the tank button makes it follow the tank. */
static void tabletViewButtonTaps(ClientSim *cs, bool overviewOn) {
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
    } else if (overviewOn) {
      overviewFollowTank();
    }
  }
}

/* A faint ring where the stick's zone is, while no finger holds it, so the
   classic screen shows where to put a thumb. The touch layout's zone is the
   whole left gutter and needs none. */
static void renderJoystickZoneHint(void) {
  float ax, ay, tx, ty;
  bool active;
  Uint64 releaseTime;
  inputTouchGetJoystickState(&ax, &ay, &tx, &ty, &active, &releaseTime);
  if (active || s_cfg.joyZoneW <= 0.0f) return;
  ImDrawList *dl = controlsDrawList();
  float inset = s_cfg.joyOuterRadius * 1.15f;
  ImVec2 c(s_cfg.joyZoneX + inset, s_cfg.joyZoneY + s_cfg.joyZoneH - inset);
  dl->AddCircle(c, s_cfg.joyOuterRadius, scaleAlpha(IM_COL32(220, 220, 220, 255), s_cfg.idleOpacity * 0.6f), 32, 2.0f);
  dl->AddCircleFilled(c, s_cfg.joyInnerRadius, scaleAlpha(IM_COL32(220, 220, 220, 255), s_cfg.idleOpacity * 0.4f), 24);
}

/* A dark round backing under the scroll stick's marker on the classic
   screen while no finger holds it, at the buttons' idle strength. The marker alone is drawn for the
   touch layout's gray chrome and cannot be seen over the classic screen's
   panels or the map's status column. */
static void renderScrollZoneBacking(void) {
  if (s_cfg.scrollJoyH <= 10.0f) return;
  /* While a finger holds the stick, its own rings show where it is. */
  {
    float ax, ay, tx, ty;
    bool active;
    Uint64 releaseTime;
    inputTouchGetScrollJoystickState(&ax, &ay, &tx, &ty, &active, &releaseTime);
    if (active) return;
  }
  ImDrawList *dl = controlsDrawList();
  ImVec2 c(s_cfg.scrollJoyX + s_cfg.scrollJoyW * 0.5f,
           s_cfg.scrollJoyY + s_cfg.scrollJoyH * 0.5f);
  float r = s_cfg.scrollJoyOuterRadius;
  dl->AddCircleFilled(c, r, scaleAlpha(IM_COL32(0, 0, 0, 200), s_cfg.idleOpacity), 32);
  dl->AddCircle(c, r, scaleAlpha(IM_COL32(255, 255, 255, 220), s_cfg.idleOpacity), 32, 2.0f);
}

/* The touch controls on the classic screen (the desktop layout), where the
   platform offers them: the iPad. Laid out and registered every frame of a
   game, so the touch that brings them back lands on a control rather than
   the game under it; drawn and read only while the last input was a finger.
   The classic screen's own chrome does the rest: its build column, status
   panels and newswire, and taps on the game view, which reach it as SDL's
   mouse clicks. */
static void tabletClassicOverlay(ClientSim *cs) {
  ImGuiIO &io = ImGui::GetIO();
  int screenW = (int)io.DisplaySize.x;
  int screenH = (int)io.DisplaySize.y;
  if (screenW <= 0 || screenH <= 0) return;

  /* The full screen map when it is up, in its own rect; otherwise the
     classic screen's. */
  bool mapUp = sdl3DrawIsOverviewInWindow();
  float gx = 0.0f, gy = 0.0f, gw = 0.0f, gh = 0.0f;
  if (mapUp) {
    if (!sdl3DrawGetOverviewInWindowRect(&gx, &gy, &gw, &gh)) mapUp = false;
  }
  if (!mapUp) sdl3DrawGetGameRect(&gx, &gy, &gw, &gh, NULL);
  if (gw <= 0.0f || gh <= 0.0f) return;

  /* The HUD as the map drew it, voice row and all. */
  OverviewHudLayout hud;
  bool haveHud = mapUp && sdl3DrawGetOverviewHudLayout(&hud);
  tabletLayoutConfigureClassic(&s_cfg, screenW, screenH, gx, gy, gw, gh, mapUp,
                               haveHud ? &hud : nullptr);

  /* Over the map, the build tools are the mouse's, like a dialog: a tap on
     one reaches the desktop's click, and no finger there pans. The status
     column and the newswire have nothing to press, and the controls are laid
     over them, so they are not. */
  SDL_FRect hudRects[1];
  int hudCount = 0;
  if (haveHud) {
    hudRects[hudCount++] = { gx + hud.buildX, gy + hud.buildY, hud.buildW, hud.buildH };
  }
  tabletBlockDialogTouches(hudRects, hudCount);
  tabletViewButtonsApplyPolicies(&s_cfg, cs);
  registerTouchButtons();
  inputTouchSetJoystickZone(s_cfg.joyZoneX, s_cfg.joyZoneY, s_cfg.joyZoneW, s_cfg.joyZoneH);
  inputTouchSetScrollJoystickZone(s_cfg.scrollJoyX, s_cfg.scrollJoyY,
                                  s_cfg.scrollJoyW, s_cfg.scrollJoyH);
  {
    SDL_FRect ctl[2];
    int n = 0;
    ctl[n++] = { s_cfg.playersBtnX, s_cfg.topBtnY,
                 s_cfg.cogBtnX + s_cfg.topBtnSize - s_cfg.playersBtnX, s_cfg.topBtnSize };
    if (overviewButtonShown(cs)) {
      ctl[n++] = { s_cfg.mapBtnX - s_cfg.mapBtnR, s_cfg.mapBtnY - s_cfg.mapBtnR,
                   s_cfg.mapBtnR * 2.0f, s_cfg.mapBtnR * 2.0f };
    }
    inputTouchSetControlRects(ctl, n);
  }
  /* The map is the touch input's: drag pans, pinch zooms, a tap builds, as in
     the touch layout. Over the classic screen there is no touch game view: a
     tap there is the classic screen's mouse click. */
  if (mapUp) {
    inputTouchSetViewportBounds((int)gx, (int)gy, (int)gw, (int)gh, 1);
  } else {
    inputTouchSetViewportBounds(0, 0, 0, 0, 1);
  }
  inputTouchSetViewportPinchEnabled(mapUp);

  /* The map button first: a tap on it is not a tap on the map. */
  processOverviewButton(cs);
  if (mapUp) {
    processOverviewTouch(cs, false);
  } else {
    /* Nothing reads the viewport's taps over the classic screen; clear any. */
    float tx, ty;
    (void)inputTouchGetViewportTapPos(&tx, &ty);
  }

  if (!inputTouchControlsShown()) return;

  tabletDamageHaptic();
  tabletViewButtonTaps(cs, mapUp);
  processScrollJoystick(cs);

  /* Drawn at the fade's strength: full while touched, gone a few seconds
     after the last finger lifts (inputTouchClassicControlsAlpha). */
  float fade = inputTouchClassicControlsAlpha();
  if (fade <= 0.0f) return;
  ImDrawList *dl = controlsDrawList();
  int vtxStart = dl->VtxBuffer.Size;

  renderJoystickZoneHint();
  renderJoystickOverlay();
  renderScrollZoneBacking();
  renderScrollJoystickOverlay();
  renderFireMineButtons();
  renderGunsightButtons();
  renderTalkButton();
  renderViewButtons(cs);
  renderTopBarButtons(cs);
  renderOverviewButton(cs);
  scaleDrawListAlphaSince(dl, vtxStart, fade);
}

/* -------------------------------------------------------
 * Main overlay entry point
 * ------------------------------------------------------- */

void sdl3ImguiTabletOverlay(ClientSim *cs) {
  if (!uiModeIsTablet()) {
    if (inputTouchClassicControlsOffered()) tabletClassicOverlay(cs);
    return;
  }

  /* Get viewport bounds from sdl3draw — now in the same coordinate
     space as ImGui since we override DisplaySize to match. */
  int vpX, vpY, vpW, vpH, vpZoom;
  sdl3DrawGetTabletViewport(&vpX, &vpY, &vpW, &vpH, &vpZoom);

  ImGuiIO &io = ImGui::GetIO();
  int screenW = (int)io.DisplaySize.x;
  int screenH = (int)io.DisplaySize.y;

  /* Reconfigure layout each frame */
  tabletLayoutConfigure(&s_cfg, screenW, screenH, vpX, vpY, vpW, vpH, vpZoom);
  tabletPanelAlphasUpdate();
  tabletBlockDialogTouches(nullptr, 0);
  tabletViewButtonsApplyPolicies(&s_cfg, cs);

  /* Draw beveled chrome background behind everything */
  renderTabletBackground();

  /* Register button positions for touch hit-testing */
  registerTouchButtons();
  inputTouchSetJoystickZone(0.0f, 0.0f, 0.0f, 0.0f);

  /* Register scroll joystick zone */
  if (s_cfg.scrollJoyH > 10.0f) {
    inputTouchSetScrollJoystickZone(s_cfg.scrollJoyX, s_cfg.scrollJoyY,
                                    s_cfg.scrollJoyW, s_cfg.scrollJoyH);
  }

  /* Update viewport bounds for tap detection */
  inputTouchSetViewportBounds(vpX, vpY, vpW, vpH, vpZoom);

  /* The map button first: its tap lies inside the game view. */
  processOverviewButton(cs);

  /* With the map up, the game view's touches drive the map; two fingers on it
     pinch only then. iOS only, as for the button. */
#if defined(__APPLE__) && TARGET_OS_IOS
  bool overviewOn = sdl3DrawIsOverviewInWindow();
#else
  bool overviewOn = false;
#endif
  inputTouchSetViewportPinchEnabled(overviewOn);
  if (overviewOn) {
    processOverviewTouch(cs, true);
  } else {
    /* Process tap-to-build */
    handleTapToBuild(cs);

    /* Process scrolling (joystick + viewport drag) */
    processScrollJoystick(cs);
    processViewportDragScroll(cs);
  }

  tabletDamageHaptic();
  tabletViewButtonTaps(cs, overviewOn);

  renderScenarioSlot(cs);
  renderOverviewButton(cs);
  renderJoystickOverlay();
  renderScrollJoystickOverlay();
  renderFireMineButtons();
  renderGunsightButtons();
  renderTalkButton();
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
