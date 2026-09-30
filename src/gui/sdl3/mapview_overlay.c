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
#include "gfx_settings.h"
#include "tilenum.h"   /* the tank and boat frames the marker pass reads */
#include "map_markers.h" /* the triangle a tank becomes */
#include "../tiles.h"     /* MOUSE_SQUARE_X / Y, TILE_SIZE_X / Y */

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

void itemLabelCacheFlush(ItemLabelCache *c) {
  int i;
  if (c == NULL) return;
  for (i = 0; i < ITEM_LABEL_MAX_NUM; i++) {
    if (c->pillTex[i]) {
      SDL_DestroyTexture(c->pillTex[i]);
      c->pillTex[i] = NULL;
    }
    if (c->baseTex[i]) {
      SDL_DestroyTexture(c->baseTex[i]);
      c->baseTex[i] = NULL;
    }
  }
  c->renderer     = NULL;
  c->pillFont     = NULL;
  c->baseFont     = NULL;
  c->pillFontSize = 0.0f;
  c->baseFontSize = 0.0f;
}

/* One number as a texture the caller owns. NULL when the face declines it. */
static SDL_Texture *itemLabelRender(SDL_Renderer *r, TTF_Font *font,
                                    int number) {
  SDL_Color    white = { 200, 200, 200, 255 };
  char         str[8];
  SDL_Surface *surf;
  SDL_Texture *tex;

  snprintf(str, sizeof(str), "%d", number);
  surf = TTF_RenderText_Blended(font, str, 0, white);
  if (surf == NULL) return NULL;
  tex = SDL_CreateTextureFromSurface(r, surf);
  SDL_DestroySurface(surf);
  return tex;
}

/* Drops the whole cache when the renderer or either face has changed since
   the textures were built — the shape tankLabelDraw uses, for the same
   reason: a zoom change reopens the fonts at a new pixel size and every
   glyph in hand was rendered at the old one. */
static void itemLabelCacheSync(ItemLabelCache *c, SDL_Renderer *r,
                               TTF_Font *pillFont, TTF_Font *baseFont) {
  float ps = pillFont ? TTF_GetFontSize(pillFont) : 0.0f;
  float bs = baseFont ? TTF_GetFontSize(baseFont) : 0.0f;

  if (r != c->renderer || pillFont != c->pillFont || baseFont != c->baseFont ||
      ps != c->pillFontSize || bs != c->baseFontSize) {
    itemLabelCacheFlush(c);
    c->renderer     = r;
    c->pillFont     = pillFont;
    c->baseFont     = baseFont;
    c->pillFontSize = ps;
    c->baseFontSize = bs;
  }
}

/* The pill and base numbers, sized by the host's display scale and drawn on
   a black backing one scaled pixel bigger all round: a base's in the top-left
   corner of its square, a pill's centred in it. The glyphs come from the
   host's cache where it has one, so a number that has been drawn before
   costs a blit rather than a rasterise, an upload and a texture destroy. */
static void overlayDrawItemLabels(MapViewCtx *ctx, const MapViewOverlay *ov,
                                  float baseX, float baseY,
                                  float tileW, float tileH) {
  OverlayClip clip;
  ItemLabelCache *c = ov->itemLabelCache;
  float ds = ov->labelDisplayScale;
  int i;

  if (ov->itemLabels == NULL || ov->itemLabelCount <= 0) return;
  if (!spritePositionItemLabelShown(ctx->scale, ov->itemLabelMinScale)) return;

  if (c != NULL) {
    itemLabelCacheSync(c, ctx->renderer, ov->pillFont, ov->baseFont);
  }

  overlayClipBegin(ctx, ov, &clip);
  for (i = 0; i < ov->itemLabelCount; i++) {
    const OverviewItemLabel *l = &ov->itemLabels[i];
    TTF_Font    *font = l->isBase ? ov->baseFont : ov->pillFont;
    int          num  = (int)l->number;
    SDL_Texture *tex  = NULL;
    bool         owned = false;   /* built for this draw, so destroy after it */
    SDL_FRect    cell;
    float        texW = 0.0f, texH = 0.0f;

    if (font == NULL) continue;

    if (c != NULL && num >= 0 && num < ITEM_LABEL_MAX_NUM) {
      SDL_Texture **slot = l->isBase ? &c->baseTex[num] : &c->pillTex[num];
      if (*slot == NULL) *slot = itemLabelRender(ctx->renderer, font, num);
      tex = *slot;
    } else {
      tex   = itemLabelRender(ctx->renderer, font, num);
      owned = (tex != NULL);
    }
    if (tex == NULL) continue;

    cell.x = spritePositionSquare(baseX, ctx->scale, (int)l->mapX);
    cell.y = spritePositionSquare(baseY, ctx->scale, (int)l->mapY);
    cell.w = tileW;
    cell.h = tileH;

    SDL_GetTextureSize(tex, &texW, &texH);
    {
      float tw = texW * ds;
      float th = texH * ds;
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
    }
    if (owned) SDL_DestroyTexture(tex);
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
