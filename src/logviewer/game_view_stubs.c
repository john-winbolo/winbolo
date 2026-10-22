/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 * Name:          game_view_stubs
 * Filename:      game_view_stubs.c
 * Purpose:
 *   Empty-body stubs for the standalone-only game-view API
 *   (Phase D of plans/ctrailer.md). Linked into the embedded
 *   WinBolo client via LOGVIEWER_LIB_SOURCES; the real
 *   implementations live in game_view.c which is restricted
 *   to LOGVIEWER_IMGUI_SOURCES because it includes bolo
 *   headers directly and would conflict with the host
 *   client's bolo struct layouts.
 *
 *   These stubs are unreachable at runtime: logviewer.c
 *   gates the backtick handler on `ownsWindow`, and
 *   lv_drawMainScreen's branch checks `gameView && ownsWindow`
 *   — both false in embedded mode. The stubs exist only to
 *   satisfy the linker.
 *********************************************************/

#include "game_view.h"

void lv_drawGameViewSetup(int zoomFactor) {
  (void)zoomFactor;
}

void lv_drawGameViewTeardown(void) {
}

int lv_drawGameViewGetZoom(void) {
  return 0;
}

int lv_drawGameViewPixelZoom(void) {
  return 0;
}

void lv_drawGameViewFrame(void *screenView, void *mineView,
                          void *tanks, void *bullets, void *lgms) {
  (void)screenView; (void)mineView; (void)tanks; (void)bullets; (void)lgms;
}

/* Where the game's ping marker code is not linked, the overview map draws
 * no ping markers. */
void lv_drawOverviewPing(float cx, float cy, float tilePx, unsigned char kind,
                         unsigned char sender, const char *name,
                         unsigned int ageMs, void *font) {
  (void)cx; (void)cy; (void)tilePx; (void)kind; (void)sender; (void)name;
  (void)ageMs; (void)font;
}

void lv_drawOverviewPingsFlush(void) {
}

void lv_drawIsoFrame(const void *parts, int w, int h) {
  (void)parts; (void)w; (void)h;
}
void *lv_drawIsoTexture(void) {
  return 0;
}
void lv_drawIsoTeardown(void) {
}
