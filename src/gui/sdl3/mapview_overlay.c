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
 * Name:          mapview_overlay.c
 * Purpose:
 *   mapViewDrawOverlay: the entity layer the classic
 *   view and the map overview both draw, in one order,
 *   placed by sprite_positions.c's arithmetic at the
 *   MapViewCtx's scale. See mapview_overlay.h.
 *********************************************************/

#include "mapview_overlay.h"
#include "../tiles.h"     /* MOUSE_SQUARE_X / Y, TILE_SIZE_X / Y */

#include <math.h>
#include <stdio.h>
#include <string.h>

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

  if (ov->labelCache == NULL || ov->labelFont == NULL) return;

  total = screenTanksGetNumEntries(tks);
  if (total == 0) return;

  overlayClipBegin(ctx, ov, &clip);
  for (count = 1; count <= total; count++) {
    BYTE mx, my, px, py, frame, playerNum;
    char name[256];
    float sx = 0.0f, sy = 0.0f;

    screenTanksGetItem(tks, count, &mx, &my, &px, &py, &frame, &playerNum,
                       name);
    if (name[0] == '\0') continue;

    spritePositionTankLabel(baseX, baseY, ctx->scale,
                            (int)mx, (int)my, (int)px, (int)py,
                            ov->clipLeft, &sx, &sy);
    if (sx > ov->clipRight || sy > ov->clipBottom) continue;

    tankLabelDraw(ov->labelCache, ctx->renderer, ov->labelFont, name,
                  playerNum, sx, sy, ov->labelDisplayScale);
  }
  overlayClipEnd(ctx, &clip);
}

/* The pill and base numbers. Each is rendered fresh, sized by the host's
   display scale, and drawn on a black backing one scaled pixel bigger all
   round: a base's in the top-left corner of its square, a pill's centred
   in it. */
static void overlayDrawItemLabels(MapViewCtx *ctx, const MapViewOverlay *ov,
                                  float baseX, float baseY,
                                  float tileW, float tileH) {
  OverlayClip clip;
  SDL_Color white = { 200, 200, 200, 255 };
  float ds = ov->labelDisplayScale;
  int i;

  if (ov->itemLabels == NULL || ov->itemLabelCount <= 0) return;
  if (!spritePositionItemLabelShown(ctx->scale, ov->itemLabelMinScale)) return;

  overlayClipBegin(ctx, ov, &clip);
  for (i = 0; i < ov->itemLabelCount; i++) {
    const OverviewItemLabel *l = &ov->itemLabels[i];
    TTF_Font *font = l->isBase ? ov->baseFont : ov->pillFont;
    SDL_FRect cell;
    char str[8];
    SDL_Surface *surf;

    if (font == NULL) continue;

    cell.x = spritePositionSquare(baseX, ctx->scale, (int)l->mapX);
    cell.y = spritePositionSquare(baseY, ctx->scale, (int)l->mapY);
    cell.w = tileW;
    cell.h = tileH;

    snprintf(str, sizeof(str), "%d", (int)l->number);
    surf = TTF_RenderText_Blended(font, str, 0, white);
    if (surf == NULL) continue;

    {
      SDL_Texture *tex = SDL_CreateTextureFromSurface(ctx->renderer, surf);
      if (tex) {
        float tw = (float)surf->w * ds;
        float th = (float)surf->h * ds;
        float tx, ty;
        if (l->isBase) {
          tx = cell.x;
          ty = cell.y;
        } else {
          tx = cell.x + (cell.w - tw) * 0.5f;
          ty = cell.y + (cell.h - th) * 0.5f;
        }
        SDL_FRect bgRect = { tx - ds, ty - ds, tw + 2.0f * ds, th + 2.0f * ds };
        SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 255);
        SDL_RenderFillRect(ctx->renderer, &bgRect);
        SDL_FRect d = { tx, ty, tw, th };
        SDL_RenderTexture(ctx->renderer, tex, NULL, &d);
        SDL_DestroyTexture(tex);
      }
    }
    SDL_DestroySurface(surf);
  }
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
  mapViewDrawTanks(ctx, tks, originX, originY, tileW, tileH, edgeX, edgeY);
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
