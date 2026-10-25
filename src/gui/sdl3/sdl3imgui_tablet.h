/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          SDL3 ImGui Tablet Overlay
*Filename:      sdl3imgui_tablet.h
*Purpose:
*  C-compatible header for the tablet-mode ImGui overlay.
*********************************************************/

#ifndef SDL3IMGUI_TABLET_H
#define SDL3IMGUI_TABLET_H

#include <stdbool.h>
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  /* Screen dimensions (set each frame) */
  int screenW, screenH;

  /* Safe area insets (pixels) */
  float safeTop, safeBottom, safeLeft, safeRight;

  /* Viewport info (computed from sdl3draw's tablet zoom) */
  int viewportX, viewportY, viewportW, viewportH;
  int effectiveZoom;

  /* Gutter widths */
  int leftGutter, rightGutter, topGutter, bottomGutter;

  /* Whether to show status grids directly (vs drawer) */
  bool showStatusGrids;   /* true if leftGutter >= 100 */

  /* Joystick */
  float joyOuterRadius;
  float joyInnerRadius;
  float joyZoneRight;
  /* The rect a finger takes the stick in. Zero size in the touch layout,
     which keeps the touch input's own rule (the left 40% outside the game
     view); the classic screen sets one. */
  float joyZoneX, joyZoneY, joyZoneW, joyZoneH;

  /* Action buttons — lower right */
  float fireRadius;
  float fireCenterX, fireCenterY;
  float mineRadius;
  float mineCenterX, mineCenterY;

  /* View buttons — one column, stacked up from the pill slot with the tank
     button on top. The show flags follow the server's visibility rules: a
     category turned off has no button, and the ones above it move down into
     its place. */
  float pillViewCenterX, pillViewCenterY, pillViewRadius;
  float baseViewCenterX, baseViewCenterY, baseViewRadius;
  float allyViewCenterX, allyViewCenterY, allyViewRadius;
  float tankViewCenterX, tankViewCenterY, tankViewRadius;
  float viewBtnGap;
  bool  showPillViewBtn, showBaseViewBtn, showAllyViewBtn;

  /* Gunsight +/- buttons (circles) */
  float gsIncCenterX, gsIncCenterY, gsIncRadius;
  float gsDecCenterX, gsDecCenterY, gsDecRadius;

  /* Voice talk button (circle) — right of the gunsight pair, above fire.
     Laid out every frame; drawn only while voice can reach anyone. */
  float talkCenterX, talkCenterY, talkRadius;

  /* Build bar — vertical right column */
  float buildIconSize;
  float buildSpacing;
  float buildBarX, buildBarY;  /* top-left of vertical strip */

  /* Status grids position (left gutter) */
  float tanksGridX, tanksGridY;
  float pillsGridX, pillsGridY;
  float basesGridX, basesGridY;
  float statusGridScale;

  /* Resource bars — tank (S/M/A/T) and base (S/M/A) */
  float tankBarsX, tankBarsY;
  float baseBarsX, baseBarsY;
  float barsW, barsH;

  /* Top bar buttons */
  float topBtnSize;
  float topBtnY;
  float cogBtnX;
  float playersBtnX;
  float msgBtnX;

  /* Button opacity */
  float idleOpacity;
  float activeOpacity;

  /* Messages overlay */
  float msgOverlayX;
  float msgOverlayY;
  float msgOverlayW;

  /* Top button idle opacity */
  float topBtnOpacity;

  /* Joystick fade-out duration in ms after release */
  Uint32 joyFadeOutMs;

  /* Scroll joystick — right side, between action buttons and build bar */
  float scrollJoyX, scrollJoyY, scrollJoyW, scrollJoyH;
  float scrollJoyOuterRadius;
  float scrollJoyInnerRadius;

  /* The map button. Zero radius in the touch layout, which places it at the
     game view's top left itself; the classic screen sets it beside the
     messages button. */
  float mapBtnX, mapBtnY, mapBtnR;

  /* Scenario panel — fixed square in the game view's top-right corner.
     scnSlotSide is 0 when the view has no room for one. */
  float scnSlotX, scnSlotY, scnSlotSide;
} TabletLayoutConfig;

void tabletLayoutConfigure(TabletLayoutConfig *cfg, int screenW, int screenH,
                           int viewportX, int viewportY, int viewportW, int viewportH,
                           int effectiveZoom);

/*********************************************************
*NAME:          tabletLayoutConfigureClassic
*PURPOSE:
*  Lays the touch controls out for the classic screen (the
*  desktop layout), which is drawn in the window at
*  gameX/Y/W/H. With a band below it tall enough, the
*  controls go in the band; otherwise over the screen: the
*  stick below the build column and the buttons at the
*  bottom right. The messages, players, Settings and map
*  buttons sit at the top right of the band or the screen.
*  mapUp: the full screen map is up instead, in gameX/Y/
*  W/H. hud, when not NULL, is the
*  layout its HUD panels were drawn with (relative to that
*  rect), and the stick keeps clear of the build tools,
*  below them; the buttons keep the screen's bottom right
*  corner, over the status column and the newswire.
*  Sets the fields the
*  classic overlay reads; the touch layout's build bar,
*  bars and grids are left unset.
*********************************************************/
struct OverviewHudLayout;
void tabletLayoutConfigureClassic(TabletLayoutConfig *cfg, int screenW, int screenH,
                                  float gameX, float gameY, float gameW, float gameH,
                                  bool mapUp, const struct OverviewHudLayout *hud);

/*********************************************************
*NAME:          sdl3ImguiTabletOverlay
*PURPOSE:
*  Renders the tablet-mode overlay (joystick, buttons,
*  build bar, status drawer, resource bars).
*  No-op if not in tablet mode.
*********************************************************/
void sdl3ImguiTabletOverlay(struct ClientSim *cs);

/*********************************************************
*NAME:          sdl3ImguiTabletNewGame
*PURPOSE:
*  Forgets the last game's messages and armour, so the
*  message fade and the damage buzz start from the new one.
*********************************************************/
void sdl3ImguiTabletNewGame(void);

#ifdef __cplusplus
}
#endif

#endif /* SDL3IMGUI_TABLET_H */
