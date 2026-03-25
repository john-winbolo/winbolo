/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
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
  bool showStockBars;     /* true if rightGutter >= 60 */

  /* Joystick */
  float joyOuterRadius;
  float joyInnerRadius;
  float joyZoneRight;     /* fraction of screen for joystick zone (0.40) */

  /* Action buttons */
  float fireRadius;
  float fireCenterX, fireCenterY;
  float mineRadius;
  float mineCenterX, mineCenterY;

  /* Build bar */
  float buildIconSize;
  float buildSpacing;
  float buildBarY;

  /* Status grids position (left gutter) */
  float tanksGridX, tanksGridY;
  float pillsGridX, pillsGridY;
  float basesGridX, basesGridY;
  float statusGridScale;  /* multiplier on the 90x66 base size */

  /* Stock bars position (right gutter) */
  float stockBarsX, stockBarsY;

  /* View toggle button (pill/tank view) */
  float viewToggleCenterX, viewToggleCenterY;
  float viewToggleRadius;

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

  /* Message fade timing */
  float msgFadeSeconds;     /* seconds before messages start fading (5.0) */
  float msgFadeDuration;    /* seconds over which fade completes (1.0) */

  /* Top button idle opacity */
  float topBtnOpacity;      /* always-visible opacity for top buttons (0.6) */

  /* Joystick fade-out duration in ms after release */
  Uint32 joyFadeOutMs;      /* 1000 = 1 second */
} TabletLayoutConfig;

void tabletLayoutConfigure(TabletLayoutConfig *cfg, int screenW, int screenH,
                           int viewportX, int viewportY, int viewportW, int viewportH,
                           int effectiveZoom);

/*********************************************************
*NAME:          sdl3ImguiTabletOverlay
*PURPOSE:
*  Renders the tablet-mode overlay (joystick, buttons,
*  build bar, hamburger menu, status drawer).
*  No-op if not in tablet mode.
*********************************************************/
void sdl3ImguiTabletOverlay(struct ClientSim *cs);

#ifdef __cplusplus
}
#endif

#endif /* SDL3IMGUI_TABLET_H */
