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

  /* Joystick */
  float joyOuterRadius;
  float joyInnerRadius;
  float joyZoneRight;

  /* Action buttons — lower right */
  float fireRadius;
  float fireCenterX, fireCenterY;
  float mineRadius;
  float mineCenterX, mineCenterY;

  /* View buttons — pill view and tank view */
  float pillViewCenterX, pillViewCenterY, pillViewRadius;
  float tankViewCenterX, tankViewCenterY, tankViewRadius;

  /* Gunsight +/- buttons (circles) */
  float gsIncCenterX, gsIncCenterY, gsIncRadius;
  float gsDecCenterX, gsDecCenterY, gsDecRadius;

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

  /* Message fade timing */
  float msgFadeSeconds;
  float msgFadeDuration;

  /* Top button idle opacity */
  float topBtnOpacity;

  /* Joystick fade-out duration in ms after release */
  Uint32 joyFadeOutMs;

  /* Scroll joystick — right side, between action buttons and build bar */
  float scrollJoyX, scrollJoyY, scrollJoyW, scrollJoyH;
  float scrollJoyOuterRadius;
  float scrollJoyInnerRadius;
} TabletLayoutConfig;

void tabletLayoutConfigure(TabletLayoutConfig *cfg, int screenW, int screenH,
                           int viewportX, int viewportY, int viewportW, int viewportH,
                           int effectiveZoom);

/*********************************************************
*NAME:          sdl3ImguiTabletOverlay
*PURPOSE:
*  Renders the tablet-mode overlay (joystick, buttons,
*  build bar, status drawer, resource bars).
*  No-op if not in tablet mode.
*********************************************************/
void sdl3ImguiTabletOverlay(struct ClientSim *cs);

#ifdef __cplusplus
}
#endif

#endif /* SDL3IMGUI_TABLET_H */
