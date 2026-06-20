/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 * Name:          game_view
 * Filename:      game_view.h
 * Purpose:
 *   Standalone-only "trailer" game view for the LogViewer.
 *   Wires the live game's renderers (mapview.c +
 *   sdl3draw_status.c) to the logviewer's replayed state so
 *   recorded WBM logs render pixel-identical to live game
 *   footage. Phase D of plans/ctrailer.md.
 *
 *   game_view.c sees the bolo-side type names (screen,
 *   screenTanks, etc. via mapview.h) and so cannot include
 *   logviewer/backend.h. The frame entry point therefore
 *   takes the per-frame screen/tank/bullet/lgm pointers as
 *   void* — draw.c (which sees logviewer types) passes them
 *   through to be cast inside game_view.c. The pointers are
 *   ABI-safe because mapview.c only calls accessors that the
 *   bolo_shim.c routes to the logviewer-side implementations.
 *********************************************************/

#ifndef LV_GAME_VIEW_H
#define LV_GAME_VIEW_H

#ifdef __cplusplus
extern "C" {
#endif

void lv_drawGameViewSetup(int zoomFactor);
void lv_drawGameViewTeardown(void);

/* Returns the active zoom factor (1..4) while game view is up, or 0
 * when game view is inactive. Used by the Phase E zoom selector to
 * elide a no-op teardown+setup when the user picks the active zoom. */
int  lv_drawGameViewGetZoom(void);

/* Render one game-view frame. Called from lv_drawMainScreen when
 * gameView && ownsWindow. The pointers are the same screen* /
 * screenMines* / screenTanks* / screenBullets* / screenLgm* values
 * lv_drawMainScreen already received from lv_screenUpdate's locals. */
void lv_drawGameViewFrame(void *screenView, void *mineView,
                          void *tanks, void *bullets, void *lgms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LV_GAME_VIEW_H */
