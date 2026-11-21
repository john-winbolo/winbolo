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
 * Name:          mapview_overlay.c
 * Purpose:
 *   mapViewDrawOverlay: the entity layer the classic
 *   view and the map overview both draw, in one order,
 *   placed by sprite_positions.c's arithmetic at the
 *   MapViewCtx's scale. See mapview_overlay.h.
 *********************************************************/

#include "mapview_overlay.h"
#include "gfx_settings.h"
#include "tilenum.h"   /* the tank and boat frames the marker pass reads */
#include "map_markers.h" /* the triangle a tank becomes */
#include "../tiles.h"     /* MOUSE_SQUARE_X / Y, TILE_SIZE_X / Y */
#include "../../logviewer/lv_pill_numbers.h" /* lvPillLabelLayout: where a number goes */
#include "../../logviewer/lv_builder_draw.h" /* the small digits, drawn */

#include <math.h>
#include <stdio.h>
#include <string.h>

/* A tank as a triangle pointing the way it faces, for the zooms where its
   sprite is too small to read: one shape, in the shared marker palette and
   stroke (map_colours.h), so a tank parked on a base is outlined the same way
   the base is. Positioned from the same sprite positions and allegiance
   frames as the normal draw, boats included. */
static void overlayDrawTankMarkers(MapViewCtx *ctx, screenTanks *tks,
                                   float baseX, float baseY, float tileW, float tileH) {
  int mode = (int)gfxGetAnimSmoothness();
  BYTE total = screenTanksGetNumEntries(tks);
  SDL_BlendMode oldBlend;
  SDL_GetRenderDrawBlendMode(ctx->renderer, &oldBlend);
  SDL_SetRenderDrawBlendMode(ctx->renderer, SDL_BLENDMODE_BLEND);
  for (BYTE count = 1; count <= total; count++) {
    BYTE mx, my, px, py, frame, playerNum, wx, wy, angle;
    char name[256];
    screenTanksGetItem(tks, count, &mx, &my, &px, &py, &frame, &playerNum, name);
    if (frame > TANK_EVILBOAT_15) continue;
    screenTanksGetSubPixel(tks, count, &wx, &wy, &angle);
    float cx = baseX + tileW / 2 + spritePositionOffset(mode, ctx->scale,
                                                       ctx->sheetScale, mx, px, wx);
    float cy = baseY + tileH / 2 + spritePositionOffset(mode, ctx->scale,
                                                       ctx->sheetScale, my, py, wy);
    float radius = SDL_max(3.0f, tileW * 0.45f);
    SDL_FColor color = { 1, 1, 1, 1 };
    if (frame >= TANK_EVIL_0) color = mapColourMarkerEvil();
    else if (frame >= TANK_GOOD_0) color = mapColourMarkerGood();
    /* The frame carries the allegiance offset as well as the heading;
       mapMarkerTank wraps, so it takes the frame as it stands. */
    mapMarkerTank(ctx->renderer, cx, cy, radius, (int)frame, color);
  }
  SDL_SetRenderDrawBlendMode(ctx->renderer, oldBlend);
}

/* The host's clip, put back after the labels are drawn. */
typedef struct {
  SDL_Rect rect;
  bool     enabled;
} OverlayClip;

/* Labels are drawn inside the overlay's box whatever the host's clip was:
   the classic view's is the game area already, and the overview's is the
   whole of its offscreen, so neither sees a change. The host's own clip is
   restored so what it draws after the pass is unaffected. */
static void overlayClipBegin(MapViewCtx *ctx, const MapViewOverlay *ov,
                             OverlayClip *saved) {
  SDL_Rect box;
  memset(&saved->rect, 0, sizeof(saved->rect));
  saved->enabled = SDL_RenderClipEnabled(ctx->renderer);
  if (saved->enabled) {
    SDL_GetRenderClipRect(ctx->renderer, &saved->rect);
  }
  box.x = (int)floorf(ov->clipLeft);
  box.y = (int)floorf(ov->clipTop);
  box.w = (int)ceilf(ov->clipRight) - box.x;
  box.h = (int)ceilf(ov->clipBottom) - box.y;
  SDL_SetRenderClipRect(ctx->renderer, &box);
}

static void overlayClipEnd(MapViewCtx *ctx, const OverlayClip *saved) {
  SDL_SetRenderClipRect(ctx->renderer, saved->enabled ? &saved->rect : NULL);
}

/* Tank names beside the sprites, through tank_label.c's drawer and the
   host's own cache: textures belong to the renderer that made them, and the
   overview may be on the pop-out's. The label string goes through whole, so
   the Tank Labels setting (none / short / long, and with it the icon)
   applies to every view. A name whose top-left is past the right or bottom
   of the box would draw nothing and is skipped. */
static void overlayDrawTankLabels(MapViewCtx *ctx, const MapViewOverlay *ov,
                                  screenTanks *tks,
                                  float baseX, float baseY) {
  OverlayClip clip;
  BYTE total;
  BYTE count;
  int mode = (int)gfxGetAnimSmoothness();

  if (ov->labelCache == NULL || ov->labelFont == NULL) return;

  total = screenTanksGetNumEntries(tks);
  if (total == 0) return;

  overlayClipBegin(ctx, ov, &clip);
  for (count = 1; count <= total; count++) {
    BYTE mx, my, px, py, frame, playerNum;
    BYTE wx, wy, angle = 0;
    char name[256];
    float sx = 0.0f, sy = 0.0f;

    screenTanksGetItem(tks, count, &mx, &my, &px, &py, &frame, &playerNum,
                       name);
    if (name[0] == '\0') continue;
    wx = (BYTE)(px << 4);
    wy = (BYTE)(py << 4);
    screenTanksGetSubPixel(tks, count, &wx, &wy, &angle);

    /* In Smooth the name follows the sprite's world position; the other
       modes keep the square-and-pixel placement. */
    spritePositionTankLabelAt(baseX, baseY, mode, ctx->scale, ctx->sheetScale,
                              (int)mx, (int)my, (int)px, (int)py,
                              (int)wx, (int)wy, ov->clipLeft, &sx, &sy);
    if (sx > ov->clipRight || sy > ov->clipBottom) continue;

    tankLabelDraw(ov->labelCache, ctx->renderer, ov->labelFont, name,
                  playerNum, sx, sy, ov->labelDisplayScale);
  }
  overlayClipEnd(ctx, &clip);
}

/* The pill and base numbers, as the Log Viewer draws its pillbox numbers
   (lvDrawPillNumbersOverBlit in logviewer/draw.c): the builder marker's small
   digits, one pixel a cell and two from LV_BUILDER_BIG_TILE pixels to a
   square, at the bottom right corner of the square (lvPillLabelLayout), in
   white over a black edge one pixel wide. Every edge goes first and every
   white after, so the edge of one number never covers the digits of a
   number beside it. The game's drawing is in its own pixels, so the scale
   of a pixel is 1. */
static void overlayDrawItemLabels(MapViewCtx *ctx, const MapViewOverlay *ov,
                                  float baseX, float baseY,
                                  float tileW, float tileH) {
  OverlayClip   clip;
  SDL_BlendMode oldBlend = SDL_BLENDMODE_NONE;
  Uint8         oldR, oldG, oldB, oldA;
  float         edge = lvTankBarPointsToPixels(LV_BUILDER_EDGE_POINTS, 1.0f);
  int           pass, i;

  (void)tileH;
  if (ov->itemLabels == NULL || ov->itemLabelCount <= 0) return;
  if (!spritePositionItemLabelShown(ctx->scale, ov->itemLabelMinScale)) return;

  overlayClipBegin(ctx, ov, &clip);
  SDL_GetRenderDrawBlendMode(ctx->renderer, &oldBlend);
  SDL_GetRenderDrawColor(ctx->renderer, &oldR, &oldG, &oldB, &oldA);
  SDL_SetRenderDrawBlendMode(ctx->renderer, SDL_BLENDMODE_BLEND);
  for (pass = 0; pass < 2; pass++) {
    LvBuilderColour c = (pass == 0) ? lvBuilderEdgeColour()
                                    : lvBuilderMarkColour(false, 0);
    SDL_SetRenderDrawColorFloat(ctx->renderer, c.r, c.g, c.b, c.a);
    for (i = 0; i < ov->itemLabelCount; i++) {
      const OverviewItemLabel *l = &ov->itemLabels[i];
      char               text[8];
      LvPillLabelLayout  lay;
      float              left = spritePositionSquare(baseX, ctx->scale, (int)l->mapX);
      float              top  = spritePositionSquare(baseY, ctx->scale, (int)l->mapY);

      snprintf(text, sizeof(text), "%d", (int)l->number);
      lay = lvPillLabelLayout(left, top, tileW, 1.0f, lvBuilderTextCells(text));
      if (!lay.visible) continue;
      lvBuilderDrawText(ctx->renderer, text, lay.x, lay.y, lay.cell,
                        (pass == 0) ? edge : 0.0f);
    }
  }
  SDL_SetRenderDrawColor(ctx->renderer, oldR, oldG, oldB, oldA);
  SDL_SetRenderDrawBlendMode(ctx->renderer, oldBlend);
  overlayClipEnd(ctx, &clip);
}

void mapViewDrawOverlay(MapViewCtx *ctx, const MapViewOverlay *ov,
                        screenTanks *tks, screenLgm *lgms, screenBullets *sb,
                        float originX, float originY,
                        float tileW, float tileH,
                        float edgeX, float edgeY) {
  float baseX = originX - tileW - edgeX;
  float baseY = originY - tileH - edgeY;

  /* The build cursor first, so tanks and men stand on top of it. From the
     same mouse_square sprite as everything else on the sheet. */
  if (ov->cursorShown) {
    SDL_FRect src = mapViewAtlasSrc(MOUSE_SQUARE_X, MOUSE_SQUARE_Y,
                                    TILE_SIZE_X, TILE_SIZE_Y, ctx->sheetScale);
    SDL_FRect dst = {
      spritePositionSquare(baseX, ctx->scale, (int)ov->cursorMapX),
      spritePositionSquare(baseY, ctx->scale, (int)ov->cursorMapY),
      tileW,
      tileH
    };
    /* Half alpha for the faint cursor, put back after so the sprite passes
       drawing from the same texture are unaffected. */
    if (ov->cursorFaint) SDL_SetTextureAlphaMod(ctx->tilesTex, 128);
    SDL_RenderTexture(ctx->renderer, ctx->tilesTex, &src, &dst);
    if (ov->cursorFaint) SDL_SetTextureAlphaMod(ctx->tilesTex, 255);
  }

  mapViewDrawShells(ctx, sb, originX, originY, tileW, tileH, edgeX, edgeY);
  if (ov->simpleTanks) overlayDrawTankMarkers(ctx, tks, baseX, baseY, tileW, tileH);
  else mapViewDrawTanks(ctx, tks, originX, originY, tileW, tileH, edgeX, edgeY);
  overlayDrawTankLabels(ctx, ov, tks, baseX, baseY);
  mapViewDrawLGMs(ctx, lgms, originX, originY, tileW, tileH, edgeX, edgeY);

  /* The reticle over the sprites, so it stays on top of tanks, boats, shells
     and men rather than being painted over by them. */
  if (ov->gunsightShown && ov->crosshairTex != NULL) {
    float side = (float)ov->crosshairPx * ctx->scale;
    SDL_FRect dst = {
      spritePositionGunsight(baseX, ctx->scale, (int)ov->gsMapX, (int)ov->gsPixelX),
      spritePositionGunsight(baseY, ctx->scale, (int)ov->gsMapY, (int)ov->gsPixelY),
      side,
      side
    };
    /* The ImGui SDL3 backend sets the sampler per draw, so the mode the
       host set at load time does not survive to here. */
    SDL_SetTextureScaleMode(ov->crosshairTex, SDL_SCALEMODE_NEAREST);
    SDL_RenderTexture(ctx->renderer, ov->crosshairTex, NULL, &dst);
  }

  overlayDrawItemLabels(ctx, ov, baseX, baseY, tileW, tileH);
}
