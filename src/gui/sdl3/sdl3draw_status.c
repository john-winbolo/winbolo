/*
 * $Id$
 *
 * Copyright (c) 1998-2008 John Morrison.
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
*Name:          SDL3 Draw — Status panel renderers
*Filename:      sdl3draw_status.c
*Purpose:
*  Phase C of plans/ctrailer.md — pure mechanical extract of
*  status / message / HUD rendering from sdl3draw.c so the
*  standalone log viewer can call the same renderers as the
*  live game.  Bodies are byte-identical to the originals;
*  the only changes are (a) two static helpers became static
*  in this module, and (b) module-private (a)-class state
*  (renderer/atlas/fonts/zoom/edge offsets) is populated by
*  setters from sdl3draw.c instead of being mutated in place.
*
*  Linked into both CLIENT_SOURCES and LOGVIEWER_IMGUI_SOURCES.
*********************************************************/

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <stdio.h>
#include <string.h>

#include "sdl3draw_status.h"
#include "../tiles.h"
#include "../ui_mode.h"
#include "../positions.h"
#include "../../bolo/global.h"
#include "../../bolo/viewport_types.h"  /* MAIN_SCREEN_SIZE_X/Y */
#include "../../bolo/screentank.h"
#include "../../bolo/bases.h"
#include "../../bolo/pillbox.h"
#include "../../bolo/tilenum.h"

/* Local copies of constants that sdl3draw.c keeps as file-local
 * #defines. Duplicating them is the simplest way to keep this
 * module self-contained. */
#define SDL3_MAX_PLAYERS 16
#define SDL3_MAX_NAME_LEN 64
#define SDL3_MSG_LEN 512

/* -----------------------------------------------------------------
 * (a)-class statics — populated by setters from sdl3draw.c.
 * sdl3draw.c keeps its own copies for its other uses; this module
 * has its own private copies for the moved renderers.
 * ----------------------------------------------------------------- */
static SDL_Renderer *gRenderer      = NULL;
static SDL_Texture  *gTilesTex      = NULL;

static int           gZoomFactor    = 1;
static int           gSheetScale    = 1;

static TTF_Font     *gFontMsg       = NULL;
static TTF_Font     *gFontKD        = NULL;
static TTF_Font     *gFontTiny      = NULL;
static TTF_Font     *gFontLabel     = NULL;

static TTF_Font     *gFallbackFontMsg   = NULL;
static TTF_Font     *gFallbackFontKD    = NULL;
static TTF_Font     *gFallbackFontTiny  = NULL;
static TTF_Font     *gFallbackFontLabel = NULL;

static int           gCurrentEdgeX  = 0;
static int           gCurrentEdgeY  = 0;

static SDL_Texture  *gTankBarsTex   = NULL;
static SDL_Texture  *gBaseBarsTex   = NULL;

static float         gStatusTanksOrgX = -1, gStatusTanksOrgY = -1;
static float         gStatusPillsOrgX = -1, gStatusPillsOrgY = -1;
static float         gStatusBasesOrgX = -1, gStatusBasesOrgY = -1;

/* -----------------------------------------------------------------
 * (b)-class statics — owned by this module, moved verbatim from
 * sdl3draw.c.
 * ----------------------------------------------------------------- */
static SDL_Texture *gLabelTex[SDL3_MAX_PLAYERS];
static char         gLabelStr[SDL3_MAX_PLAYERS][SDL3_MAX_NAME_LEN];

static char gMsgTop[SDL3_MSG_LEN];
static char gMsgBottom[SDL3_MSG_LEN];
static int  gCachedKills  = 0;
static int  gCachedDeaths = 0;

/* Cached SDL_ttf textures for the message lines and kills/deaths text */
static SDL_Texture *gTexMsgTop    = NULL;
static SDL_Texture *gTexMsgBot    = NULL;
static SDL_Texture *gTexKills     = NULL;
static SDL_Texture *gTexDeaths    = NULL;
static char         gTexMsgTopStr[SDL3_MSG_LEN];
static char         gTexMsgBotStr[SDL3_MSG_LEN];
static char         gTexKillsStr[16];
static char         gTexDeathsStr[16];
static int          gTexMsgTopW, gTexMsgTopH;
static int          gTexMsgBotW, gTexMsgBotH;
static int          gTexKillsW,  gTexKillsH;
static int          gTexDeathsW, gTexDeathsH;

/* Cached status bar values for tablet overlay */
static BYTE gCachedTankShells = 0, gCachedTankMines = 0, gCachedTankArmour = 0, gCachedTankTrees = 0;
static BYTE gCachedBaseShells = 0, gCachedBaseMines = 0, gCachedBaseArmour = 0;
static bool gCachedBaseValid = false;

/* -----------------------------------------------------------------
 * Local helpers (static — moved verbatim from sdl3draw.c).
 * sdl3RenderText is also kept as a static duplicate in sdl3draw.c
 * because non-moved code in that file (sdl3DrawStartDelay,
 * sdl3DrawNetFailed, sdl3DrawPillInView, sdl3DrawMainScreen) calls
 * it as well.
 * ----------------------------------------------------------------- */
/*********************************************************
*NAME:          sdl3RenderText
*PURPOSE:
*  Renders a text string with the game font directly onto
*  the current render target at (x, y) in screen pixels.
*  No-op if font is NULL or text is NULL/empty.
*********************************************************/
static void sdl3RenderText(TTF_Font *font, const char *text, SDL_Color fg, float x, float y) {
  if (!font || !gRenderer || !text || text[0] == '\0') return;

  SDL_Surface *sSurf = TTF_RenderText_Blended(font, text, 0, fg);
  if (!sSurf) return;
  SDL_Texture *tTex = SDL_CreateTextureFromSurface(gRenderer, sSurf);
  if (tTex) {
    SDL_FRect d = { x, y, (float)sSurf->w, (float)sSurf->h };
    SDL_RenderTexture(gRenderer, tTex, NULL, &d);
    SDL_DestroyTexture(tTex);
  }
  SDL_DestroySurface(sSurf);
}

/*********************************************************
*NAME:          sdl3UpdateTextCache
*PURPOSE:
*  Re-renders a text texture only when the string changes.
*  Compares text against prevStr; if different, destroys
*  the old texture and creates a new one. Stores the new
*  string, texture, and dimensions via the out-pointers.
*********************************************************/
static void sdl3UpdateTextCache(TTF_Font *font, const char *text,
                                SDL_Color fg,
                                SDL_Texture **tex, char *prevStr, size_t prevSize,
                                int *outW, int *outH) {
  if (!text || text[0] == '\0') {
    if (*tex) { SDL_DestroyTexture(*tex); *tex = NULL; }
    prevStr[0] = '\0';
    *outW = *outH = 0;
    return;
  }
  if (*tex && SDL_strcmp(text, prevStr) == 0) return; /* unchanged */

  if (*tex) { SDL_DestroyTexture(*tex); *tex = NULL; }
  SDL_Surface *surf = TTF_RenderText_Blended(font, text, 0, fg);
  if (!surf) { prevStr[0] = '\0'; *outW = *outH = 0; return; }
  *tex = SDL_CreateTextureFromSurface(gRenderer, surf);
  *outW = surf->w;
  *outH = surf->h;
  SDL_DestroySurface(surf);
  SDL_strlcpy(prevStr, text, prevSize);
}

/*********************************************************
*NAME:          sdl3RenderCachedText
*PURPOSE:
*  Renders the cached message lines and kills/deaths onto
*  the current render target. Called just before every
*  SDL_RenderPresent so text is composited in one pass.
*********************************************************/
void sdl3RenderCachedText(void) {
  if (!gRenderer) return;
  int zf = gZoomFactor;
  SDL_Color white = {200, 200, 200, 255};

  if (gFontMsg) {
    /* Clip to the newswire rectangle so text doesn't bleed outside the border */
    SDL_Rect msgClip = {
      zf * MESSAGE_LEFT,
      zf * MESSAGE_TOP,
      zf * MESSAGE_BOX_WIDTH,
      zf * MESSAGE_HEIGHT
    };
    SDL_SetRenderClipRect(gRenderer, &msgClip);

    if (gMsgTop[0] != '\0') {
      /* Show only the rightmost characters that fit in the display width. */
      size_t fitLen = 0;
      const char *topStr = gMsgTop;
      size_t topLen = SDL_strlen(gMsgTop);
      TTF_MeasureString(gFontMsg, gMsgTop, topLen, zf * MESSAGE_BOX_WIDTH, NULL, &fitLen);
      if (fitLen < topLen) {
        topStr = gMsgTop + (topLen - fitLen);
      }
      sdl3UpdateTextCache(gFontMsg, topStr, white,
                          &gTexMsgTop, gTexMsgTopStr, sizeof(gTexMsgTopStr),
                          &gTexMsgTopW, &gTexMsgTopH);
      if (gTexMsgTop) {
        SDL_FRect d = { (float)(zf * MESSAGE_TOP_LINE_X),
                        (float)(zf * MESSAGE_TOP_LINE_Y),
                        (float)gTexMsgTopW, (float)gTexMsgTopH };
        SDL_RenderTexture(gRenderer, gTexMsgTop, NULL, &d);
      }
    } else {
      sdl3UpdateTextCache(gFontMsg, "", white,
                          &gTexMsgTop, gTexMsgTopStr, sizeof(gTexMsgTopStr),
                          &gTexMsgTopW, &gTexMsgTopH);
    }

    if (gMsgBottom[0] != '\0') {
      size_t fitLen = 0;
      const char *botStr = gMsgBottom;
      size_t botLen = SDL_strlen(gMsgBottom);
      TTF_MeasureString(gFontMsg, gMsgBottom, botLen, zf * MESSAGE_BOX_WIDTH, NULL, &fitLen);
      if (fitLen < botLen) {
        botStr = gMsgBottom + (botLen - fitLen);
      }
      sdl3UpdateTextCache(gFontMsg, botStr, white,
                          &gTexMsgBot, gTexMsgBotStr, sizeof(gTexMsgBotStr),
                          &gTexMsgBotW, &gTexMsgBotH);
      if (gTexMsgBot) {
        SDL_FRect d = { (float)(zf * MESSAGE_BOTTOM_LINE_X),
                        (float)(zf * MESSAGE_BOTTOM_LINE_Y),
                        (float)gTexMsgBotW, (float)gTexMsgBotH };
        SDL_RenderTexture(gRenderer, gTexMsgBot, NULL, &d);
      }
    } else {
      sdl3UpdateTextCache(gFontMsg, "", white,
                          &gTexMsgBot, gTexMsgBotStr, sizeof(gTexMsgBotStr),
                          &gTexMsgBotW, &gTexMsgBotH);
    }

    SDL_SetRenderClipRect(gRenderer, NULL);
  }

  if (gFontKD) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", gCachedKills  > 99 ? 99 : gCachedKills);
    sdl3UpdateTextCache(gFontKD, buf, white,
                        &gTexKills, gTexKillsStr, sizeof(gTexKillsStr),
                        &gTexKillsW, &gTexKillsH);
    if (gTexKills) {
      SDL_FRect d = { (float)(zf * STATUS_KILLS_LEFT),
                      (float)(zf * STATUS_KILLS_TOP),
                      (float)gTexKillsW, (float)gTexKillsH };
      SDL_RenderTexture(gRenderer, gTexKills, NULL, &d);
    }

    snprintf(buf, sizeof(buf), "%d", gCachedDeaths > 99 ? 99 : gCachedDeaths);
    sdl3UpdateTextCache(gFontKD, buf, white,
                        &gTexDeaths, gTexDeathsStr, sizeof(gTexDeathsStr),
                        &gTexDeathsW, &gTexDeathsH);
    if (gTexDeaths) {
      SDL_FRect d = { (float)(zf * STATUS_DEATHS_LEFT),
                      (float)(zf * STATUS_DEATHS_TOP),
                      (float)gTexDeathsW, (float)gTexDeathsH };
      SDL_RenderTexture(gRenderer, gTexDeaths, NULL, &d);
    }
  }
}

/*--------------------------------------------------------
 * Helper: look up (dstX, dstY) within a status panel for
 * item number n (1-based) using the positions.h grid.
 * type: 0=base, 1=pill, 2=tank
 *--------------------------------------------------------*/
static void sdl3StatusItemPos(int n, int type, int *outX, int *outY) {
  /* All three panels share the same grid layout */
  int startX, startY, gapX, gapY, itemW;
  (void)type;
  startX = STATUS_BASE_1_X;  /* == STATUS_PILLBOX_1_X == STATUS_TANKS_1_X == 4 */
  startY = STATUS_BASE_1_Y;  /* == 4 */
  gapX   = STATUS_ITEM_GAP_X;
  gapY   = STATUS_ITEM_GAP_Y;
  itemW  = STATUS_ITEM_SIZE_X;

  /* Positions 1-6: row 0; 7-8 and 9-10: row 1 (with gap in middle for center icon);
     11-16: row 2. This matches the positions.h explicit macros. */
  int col, row;
  if (n <= 6) {
    col = n - 1;
    row = 0;
  } else if (n <= 8) {
    col = n - 7;
    row = 1;
  } else if (n <= 10) {
    col = (n - 9) + 4; /* cols 4,5 in row 1 */
    row = 1;
  } else {
    col = n - 11;
    row = 2;
  }

  *outX = startX + col * (gapX + itemW);
  *outY = startY + row * (gapY + STATUS_ITEM_SIZE_Y);
}

/* Helper: get panel origin in zoomed pixels, using override if set */
static void statusPanelOrigin(int panelType, int zf, float *ox, float *oy) {
  switch (panelType) {
    case 0: /* bases */
      *ox = (gStatusBasesOrgX >= 0) ? gStatusBasesOrgX : (float)(zf * STATUS_BASES_LEFT);
      *oy = (gStatusBasesOrgY >= 0) ? gStatusBasesOrgY : (float)(zf * STATUS_BASES_TOP);
      break;
    case 1: /* pills */
      *ox = (gStatusPillsOrgX >= 0) ? gStatusPillsOrgX : (float)(zf * STATUS_PILLS_LEFT);
      *oy = (gStatusPillsOrgY >= 0) ? gStatusPillsOrgY : (float)(zf * STATUS_PILLS_TOP);
      break;
    default: /* tanks */
      *ox = (gStatusTanksOrgX >= 0) ? gStatusTanksOrgX : (float)(zf * STATUS_TANKS_LEFT);
      *oy = (gStatusTanksOrgY >= 0) ? gStatusTanksOrgY : (float)(zf * STATUS_TANKS_TOP);
      break;
  }
}

void sdl3DrawSetBasesStatusClear(void) {
  if (!gRenderer) return;
  int zf = gZoomFactor;
  float orgX, orgY;
  statusPanelOrigin(0, zf, &orgX, &orgY);
  /* Black out bases panel area directly on the framebuffer */
  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_FRect area = { orgX, orgY,
                     (float)(zf * STATUS_BASES_WIDTH), (float)(zf * STATUS_BASES_HEIGHT) };
  SDL_RenderFillRect(gRenderer, &area);
  /* Centre base icon (full tile sprite, not the small status icon) */
  if (gTilesTex) {
    SDL_FRect src = { (float)(BASE_GOOD_X * gSheetScale), (float)(BASE_GOOD_Y * gSheetScale),
                      (float)(TILE_SIZE_X * gSheetScale),  (float)(TILE_SIZE_Y * gSheetScale) };
    SDL_FRect dst = {
      orgX + (float)(zf * STATUS_BASES_MIDDLE_ICON_X),
      orgY + (float)(zf * STATUS_BASES_MIDDLE_ICON_Y),
      (float)(zf * TILE_SIZE_X), (float)(zf * TILE_SIZE_Y)
    };
    SDL_RenderTexture(gRenderer, gTilesTex, &src, &dst);
  }
}

void sdl3DrawStatusBase(BYTE baseNum, baseAlliance ba, bool labels) {
  if (!gRenderer || !gTilesTex) return;
  if (baseNum < 1 || baseNum > 16) return;

  int srcX, srcY;
  switch (ba) {
    case baseDead:      srcX=STATUS_ITEM_DEAD_X;      srcY=STATUS_ITEM_DEAD_Y;      break;
    case baseNeutral:   srcX=STATUS_BASE_NEUTRAL_X;   srcY=STATUS_BASE_NEUTRAL_Y;   break;
    case baseOwnGood:   srcX=STATUS_BASE_GOOD_X;      srcY=STATUS_BASE_GOOD_Y;      break;
    case baseAllieGood: srcX=STATUS_BASE_ALLIEGOOD_X; srcY=STATUS_BASE_ALLIEGOOD_Y; break;
    case baseEvil:      srcX=STATUS_BASE_EVIL_X;      srcY=STATUS_BASE_EVIL_Y;      break;
    default:            srcX=STATUS_BASE_NEUTRAL_X;   srcY=STATUS_BASE_NEUTRAL_Y;   break;
  }

  int itemX, itemY;
  sdl3StatusItemPos(baseNum, 0, &itemX, &itemY);
  int zf = gZoomFactor;
  float orgX, orgY;
  statusPanelOrigin(0, zf, &orgX, &orgY);
  int ss = gSheetScale;
  SDL_FRect src = { (float)(srcX * ss), (float)(srcY * ss),
                    (float)(STATUS_ITEM_SIZE_X * ss), (float)(STATUS_ITEM_SIZE_Y * ss) };
  SDL_FRect dst = {
    orgX + (float)(zf * itemX),
    orgY + (float)(zf * itemY),
    (float)(zf * STATUS_ITEM_SIZE_X), (float)(zf * STATUS_ITEM_SIZE_Y)
  };
  SDL_SetTextureBlendMode(gTilesTex, SDL_BLENDMODE_NONE);
  SDL_RenderTexture(gRenderer, gTilesTex, &src, &dst);
  SDL_SetTextureBlendMode(gTilesTex, SDL_BLENDMODE_BLEND);

  if (labels == TRUE && gFontTiny) {
    char str[4];
    SDL_Color white = {200, 200, 200, 255};
    sprintf(str, "%d", baseNum - 1);
    sdl3RenderText(gFontTiny, str, white, dst.x, dst.y);
  }
}

void sdl3DrawCopyBasesStatus(int x, int y) {
  (void)x; (void)y;
  /* Icons drawn directly to framebuffer — no-op. */
}

void sdl3DrawSetPillsStatusClear(void) {
  if (!gRenderer) return;
  int zf = gZoomFactor;
  float orgX, orgY;
  statusPanelOrigin(1, zf, &orgX, &orgY);
  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_FRect area = { orgX, orgY,
                     (float)(zf * STATUS_PILLS_WIDTH), (float)(zf * STATUS_PILLS_HEIGHT) };
  SDL_RenderFillRect(gRenderer, &area);
  /* Centre pill icon (full tile PILL_GOOD15 sprite) */
  if (gTilesTex) {
    SDL_FRect src = { (float)(PILL_GOOD15_X * gSheetScale), (float)(PILL_GOOD15_Y * gSheetScale),
                      (float)(TILE_SIZE_X * gSheetScale),   (float)(TILE_SIZE_Y * gSheetScale) };
    SDL_FRect dst = {
      orgX + (float)(zf * STATUS_PILLS_MIDDLE_ICON_X),
      orgY + (float)(zf * STATUS_PILLS_MIDDLE_ICON_Y),
      (float)(zf * TILE_SIZE_X), (float)(zf * TILE_SIZE_Y)
    };
    SDL_RenderTexture(gRenderer, gTilesTex, &src, &dst);
  }
}

void sdl3DrawStatusPillbox(BYTE pillNum, pillAlliance pa, bool labels) {
  if (!gRenderer || !gTilesTex) return;
  if (pillNum < 1 || pillNum > 16) return;

  int srcX, srcY;
  switch (pa) {
    case pillDead:      srcX=STATUS_ITEM_DEAD_X;           srcY=STATUS_ITEM_DEAD_Y;           break;
    case pillNeutral:   srcX=STATUS_PILLBOX_NEUTRAL_X;     srcY=STATUS_PILLBOX_NEUTRAL_Y;     break;
    case pillGood:      srcX=STATUS_PILLBOX_GOOD_X;        srcY=STATUS_PILLBOX_GOOD_Y;        break;
    case pillAllie:     srcX=STATUS_PILLBOX_ALLIEGOOD_X;   srcY=STATUS_PILLBOX_ALLIEGOOD_Y;   break;
    case pillTankGood:  srcX=STATUS_PILLBOX_TANKGOOD_X;    srcY=STATUS_PILLBOX_TANKGOOD_Y;    break;
    case pillTankAllie: srcX=STATUS_PILLBOX_TANKALLIE_X;   srcY=STATUS_PILLBOX_TANKALLIE_Y;   break;
    case pillTankEvil:  srcX=STATUS_PILLBOX_TANKEVIL_X;    srcY=STATUS_PILLBOX_TANKEVIL_Y;    break;
    case pillEvil:      srcX=STATUS_PILLBOX_EVIL_X;        srcY=STATUS_PILLBOX_EVIL_Y;        break;
    default:            srcX=STATUS_PILLBOX_NEUTRAL_X;     srcY=STATUS_PILLBOX_NEUTRAL_Y;     break;
  }

  int itemX, itemY;
  sdl3StatusItemPos(pillNum, 1, &itemX, &itemY);
  int zf = gZoomFactor;
  float orgX, orgY;
  statusPanelOrigin(1, zf, &orgX, &orgY);
  int ss = gSheetScale;
  SDL_FRect src = { (float)(srcX * ss), (float)(srcY * ss),
                    (float)(STATUS_ITEM_SIZE_X * ss), (float)(STATUS_ITEM_SIZE_Y * ss) };
  SDL_FRect dst = {
    orgX + (float)(zf * itemX),
    orgY + (float)(zf * itemY),
    (float)(zf * STATUS_ITEM_SIZE_X), (float)(zf * STATUS_ITEM_SIZE_Y)
  };
  SDL_SetTextureBlendMode(gTilesTex, SDL_BLENDMODE_NONE);
  SDL_RenderTexture(gRenderer, gTilesTex, &src, &dst);
  SDL_SetTextureBlendMode(gTilesTex, SDL_BLENDMODE_BLEND);

  if (labels == TRUE && gFontTiny) {
    char str[4];
    SDL_Color white = {200, 200, 200, 255};
    sprintf(str, "%d", pillNum - 1);
    sdl3RenderText(gFontTiny, str, white, dst.x, dst.y);
  }
}

void sdl3DrawCopyPillsStatus(int x, int y) {
  (void)x; (void)y;
  /* Icons drawn directly to framebuffer — no-op. */
}

void sdl3DrawSetTanksStatusClear(void) {
  if (!gRenderer) return;
  int zf = gZoomFactor;
  float orgX, orgY;
  statusPanelOrigin(2, zf, &orgX, &orgY);
  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_FRect area = { orgX, orgY,
                     (float)(zf * STATUS_TANKS_WIDTH), (float)(zf * STATUS_TANKS_HEIGHT) };
  SDL_RenderFillRect(gRenderer, &area);
  /* Centre tank icon (full tile TANK_SELF_0 sprite) */
  if (gTilesTex) {
    SDL_FRect src = { (float)(TANK_SELF_0_X * gSheetScale), (float)(TANK_SELF_0_Y * gSheetScale),
                      (float)(TILE_SIZE_X * gSheetScale),   (float)(TILE_SIZE_Y * gSheetScale) };
    SDL_FRect dst = {
      orgX + (float)(zf * STATUS_TANKS_MIDDLE_ICON_X),
      orgY + (float)(zf * STATUS_TANKS_MIDDLE_ICON_Y),
      (float)(zf * TILE_SIZE_X), (float)(zf * TILE_SIZE_Y)
    };
    SDL_RenderTexture(gRenderer, gTilesTex, &src, &dst);
  }
}

void sdl3DrawStatusTank(BYTE tankNum, tankAlliance ta) {
  if (!gRenderer || !gTilesTex) return;
  if (tankNum < 1 || tankNum > 16) return;

  int srcX, srcY;
  switch (ta) {
    case tankNone: srcX=STATUS_TANK_NONE_X; srcY=STATUS_TANK_NONE_Y; break;
    case tankSelf: srcX=STATUS_TANK_SELF_X; srcY=STATUS_TANK_SELF_Y; break;
    case tankAllie:srcX=STATUS_TANK_GOOD_X; srcY=STATUS_TANK_GOOD_Y; break;
    case tankEvil: srcX=STATUS_TANK_EVIL_X; srcY=STATUS_TANK_EVIL_Y; break;
    default:       srcX=STATUS_TANK_NONE_X; srcY=STATUS_TANK_NONE_Y; break;
  }

  int itemX, itemY;
  sdl3StatusItemPos(tankNum, 2, &itemX, &itemY);
  int zf = gZoomFactor;
  float orgX, orgY;
  statusPanelOrigin(2, zf, &orgX, &orgY);
  int ss = gSheetScale;
  SDL_FRect src = { (float)(srcX * ss), (float)(srcY * ss),
                    (float)(STATUS_ITEM_SIZE_X * ss), (float)(STATUS_ITEM_SIZE_Y * ss) };
  SDL_FRect dst = {
    orgX + (float)(zf * itemX),
    orgY + (float)(zf * itemY),
    (float)(zf * STATUS_ITEM_SIZE_X), (float)(zf * STATUS_ITEM_SIZE_Y)
  };
  SDL_SetTextureBlendMode(gTilesTex, SDL_BLENDMODE_NONE);
  SDL_RenderTexture(gRenderer, gTilesTex, &src, &dst);
  SDL_SetTextureBlendMode(gTilesTex, SDL_BLENDMODE_BLEND);
}

void sdl3DrawCopyTanksStatus(int x, int y) {
  (void)x; (void)y;
  /* Icons drawn directly to framebuffer — no-op. */
}

void sdl3DrawStatusTankBars(int x, int y, BYTE shells, BYTE mines, BYTE armour, BYTE trees) {
  (void)x; (void)y;
  gCachedTankShells = shells;
  gCachedTankMines = mines;
  gCachedTankArmour = armour;
  gCachedTankTrees = trees;
  if (!gRenderer || !gTankBarsTex) return;

  SDL_SetRenderTarget(gRenderer, gTankBarsTex);
  SDL_SetTextureBlendMode(gTankBarsTex, SDL_BLENDMODE_NONE);

  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_RenderFillRect(gRenderer, NULL); /* avoid SDL3 Metal sampler bug in SDL_RenderClear */

  /* Green bars — heights in 1x pixels, bars grow from bottom */
  SDL_SetRenderDrawColor(gRenderer, 0, 255, 0, 255);

  int h = STATUS_TANK_BARS_HEIGHT;
  int bw = STATUS_TANK_BARS_WIDTH;

  SDL_FRect rShells = { 0.0f,
                        (float)(h - BAR_TANK_MULTIPLY * shells),
                        (float)bw,
                        (float)(BAR_TANK_MULTIPLY * shells) };
  SDL_FRect rMines  = { (float)STATUS_TANK_MINES,
                        (float)(h - BAR_TANK_MULTIPLY * mines),
                        (float)bw,
                        (float)(BAR_TANK_MULTIPLY * mines) };
  SDL_FRect rArmour = { (float)STATUS_TANK_ARMOUR,
                        (float)(h - BAR_TANK_MULTIPLY * armour),
                        (float)bw,
                        (float)(BAR_TANK_MULTIPLY * armour) };
  SDL_FRect rTrees  = { (float)STATUS_TANK_TREES,
                        (float)(h - BAR_TANK_MULTIPLY * trees),
                        (float)bw,
                        (float)(BAR_TANK_MULTIPLY * trees) };

  if (shells > 0) SDL_RenderFillRect(gRenderer, &rShells);
  if (mines  > 0) SDL_RenderFillRect(gRenderer, &rMines);
  if (armour > 0) SDL_RenderFillRect(gRenderer, &rArmour);
  if (trees  > 0) SDL_RenderFillRect(gRenderer, &rTrees);

  SDL_SetRenderTarget(gRenderer, NULL);
}

void sdl3DrawCopyTankStatusBars(int x, int y) {
  (void)x; (void)y;
  /* Panel texture is read by sdl3RenderStatusPanels each frame — no-op. */
}

void sdl3DrawStatusBaseBars(int x, int y, BYTE shells, BYTE mines, BYTE armour, bool redraw) {
  (void)x; (void)y; (void)redraw;
  gCachedBaseShells = shells;
  gCachedBaseMines = mines;
  gCachedBaseArmour = armour;
  gCachedBaseValid = (shells > 0 || mines > 0 || armour > 0);
  if (!gRenderer || !gBaseBarsTex) return;

  SDL_SetRenderTarget(gRenderer, gBaseBarsTex);
  SDL_SetTextureBlendMode(gBaseBarsTex, SDL_BLENDMODE_NONE);

  SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
  SDL_RenderFillRect(gRenderer, NULL); /* avoid SDL3 Metal sampler bug in SDL_RenderClear */

  if (shells != 0 || mines != 0 || armour != 0) {
    SDL_SetRenderDrawColor(gRenderer, 0, 255, 0, 255);

    int bh = STATUS_BASE_BARS_HEIGHT;
    float mult = (float)(2.0 / 3.0);

    SDL_FRect rShells = { 0.0f, 0.0f,
                          (float)(shells * mult), (float)bh };
    SDL_FRect rMines  = { 0.0f, (float)STATUS_BASE_MINES,
                          (float)(mines  * mult), (float)bh };
    SDL_FRect rArmour = { 0.0f, (float)STATUS_BASE_ARMOUR,
                          (float)(armour * mult), (float)bh };

    if (shells > 0) SDL_RenderFillRect(gRenderer, &rShells);
    if (mines  > 0) SDL_RenderFillRect(gRenderer, &rMines);
    if (armour > 0) SDL_RenderFillRect(gRenderer, &rArmour);
  }

  SDL_SetRenderTarget(gRenderer, NULL);
}

void sdl3DrawCopyBasesStatusBars(int x, int y) {
  (void)x; (void)y;
  /* Panel texture is read by sdl3RenderStatusPanels each frame — no-op. */
}

void sdl3DrawResetCachedText(void) {
  gMsgTop[0] = '\0';
  gMsgBottom[0] = '\0';
  gCachedKills  = 0;
  gCachedDeaths = 0;
  /* Invalidate texture caches so stale text isn't rendered */
  gTexMsgTopStr[0] = '\0';
  gTexMsgBotStr[0] = '\0';
  gTexKillsStr[0]  = '\0';
  gTexDeathsStr[0] = '\0';
  if (gTexMsgTop) { SDL_DestroyTexture(gTexMsgTop); gTexMsgTop = NULL; }
  if (gTexMsgBot) { SDL_DestroyTexture(gTexMsgBot); gTexMsgBot = NULL; }
  if (gTexKills)  { SDL_DestroyTexture(gTexKills);  gTexKills  = NULL; }
  if (gTexDeaths) { SDL_DestroyTexture(gTexDeaths); gTexDeaths = NULL; }
}

void sdl3DrawMessages(int x, int y, char *top, char *bottom) {
  (void)x; (void)y;
  /* Cache only — sdl3RenderCachedText() draws these into the next frame. */
  if (top)    SDL_strlcpy(gMsgTop,    top,    SDL3_MSG_LEN);
  else        gMsgTop[0] = '\0';
  if (bottom) SDL_strlcpy(gMsgBottom, bottom, SDL3_MSG_LEN);
  else        gMsgBottom[0] = '\0';
}

void sdl3DrawGetCachedMessages(const char **top, const char **bottom) {
  if (top)    *top    = gMsgTop;
  if (bottom) *bottom = gMsgBottom;
}

void sdl3DrawKillsDeaths(int x, int y, int kills, int deaths) {
  (void)x; (void)y;
  /* Cache only — sdl3RenderCachedText() draws these into the next frame. */
  gCachedKills  = kills;
  gCachedDeaths = deaths;
}

void sdl3DrawTankLabel(char *str, BYTE playerNum,
                       BYTE mx, BYTE my, BYTE px, BYTE py) {
  if (!gRenderer || !str || str[0] == '\0') return;
  if (playerNum >= SDL3_MAX_PLAYERS) return;

  /* Rebuild the cached texture if the name changed */
  if (strncmp(gLabelStr[playerNum], str, SDL3_MAX_NAME_LEN - 1) != 0) {
    if (gLabelTex[playerNum]) {
      SDL_DestroyTexture(gLabelTex[playerNum]);
      gLabelTex[playerNum] = NULL;
    }
    strncpy(gLabelStr[playerNum], str, SDL3_MAX_NAME_LEN - 1);
    gLabelStr[playerNum][SDL3_MAX_NAME_LEN - 1] = '\0';

    if (gFontMsg) {
      SDL_Color fg = {200, 200, 200, 255};
      SDL_Surface *sFg = TTF_RenderText_Blended(gFontMsg, str, 0, fg);
      if (sFg) {
        gLabelTex[playerNum] = SDL_CreateTextureFromSurface(gRenderer, sFg);
        SDL_DestroySurface(sFg);
      }
    }
  }

  if (!gLabelTex[playerNum]) return;

  /* Compute screen position matching sdl3DrawTanks placement.
     In tablet mode, gZoomFactor is temporarily set to the tablet scale
     and the origin is centred on screen rather than at MAIN_OFFSET. */
  int originX, originY;
  if (uiModeIsTablet()) {
    int ww, wh;
    {
      SDL_RendererLogicalPresentation logMode;
      SDL_GetRenderLogicalPresentation(gRenderer, &ww, &wh, &logMode);
      if (ww <= 0 || wh <= 0) {
        SDL_GetCurrentRenderOutputSize(gRenderer, &ww, &wh);
      }
    }
    int gamePixW = MAIN_SCREEN_SIZE_X * TILE_SIZE_X * gZoomFactor;
    int gamePixH = MAIN_SCREEN_SIZE_Y * TILE_SIZE_Y * gZoomFactor;
    originX = (ww - gamePixW) / 2;
    originY = (wh - gamePixH) / 2;
  } else {
    originX = MAIN_OFFSET_X * gZoomFactor;
    originY = MAIN_OFFSET_Y * gZoomFactor;
  }
  int tileW   = TILE_SIZE_X   * gZoomFactor;
  int tileH   = TILE_SIZE_Y   * gZoomFactor;
  int apx = (int)px + 2;
  int apy = (int)py + 2;
  int bbx = (int)mx * TILE_SIZE_X + apx;
  int bby = (int)my * TILE_SIZE_Y + apy;
  float sx = (float)(originX - tileW + bbx * gZoomFactor - gCurrentEdgeX);
  float sy = (float)(originY - tileH + bby * gZoomFactor - gCurrentEdgeY);

  /* Place label to the right of the tank sprite (matches Win32 behaviour) */
  float texW, texH;
  SDL_GetTextureSize(gLabelTex[playerNum], &texW, &texH);
  sx += (float)tileW;

  /* Clip label to game-area left edge */
  if (sx < (float)originX) sx = (float)originX;

  SDL_SetTextureBlendMode(gLabelTex[playerNum], SDL_BLENDMODE_BLEND);
  SDL_FRect d = { sx, sy, texW, texH };
  SDL_RenderTexture(gRenderer, gLabelTex[playerNum], NULL, &d);
}

void sdl3DrawStatusGetCachedTankStats(BYTE *shells, BYTE *mines, BYTE *armour, BYTE *trees) {
  *shells = gCachedTankShells;
  *mines = gCachedTankMines;
  *armour = gCachedTankArmour;
  *trees = gCachedTankTrees;
}

void sdl3DrawStatusGetCachedBaseStats(BYTE *shells, BYTE *mines, BYTE *armour, bool *hasBase) {
  *shells = gCachedBaseShells;
  *mines = gCachedBaseMines;
  *armour = gCachedBaseArmour;
  *hasBase = gCachedBaseValid;
}

/* -----------------------------------------------------------------
 * Lifecycle / setter API implementations.
 * ----------------------------------------------------------------- */
void sdl3DrawStatusInit(SDL_Renderer *renderer,
                        SDL_Texture *atlas,
                        int sheetScale,
                        int zoomFactor,
                        TTF_Font *fontTiny,
                        TTF_Font *fontMsg,
                        TTF_Font *fontKD,
                        TTF_Font *fontLabel,
                        TTF_Font *fallbackFontTiny,
                        TTF_Font *fallbackFontMsg,
                        TTF_Font *fallbackFontKD,
                        TTF_Font *fallbackFontLabel,
                        SDL_Texture *tankBarsTex,
                        SDL_Texture *baseBarsTex) {
  gRenderer          = renderer;
  gTilesTex          = atlas;
  gSheetScale        = sheetScale;
  gZoomFactor        = zoomFactor;
  gFontTiny          = fontTiny;
  gFontMsg           = fontMsg;
  gFontKD            = fontKD;
  gFontLabel         = fontLabel;
  gFallbackFontTiny  = fallbackFontTiny;
  gFallbackFontMsg   = fallbackFontMsg;
  gFallbackFontKD    = fallbackFontKD;
  gFallbackFontLabel = fallbackFontLabel;
  gTankBarsTex       = tankBarsTex;
  gBaseBarsTex       = baseBarsTex;
}

void sdl3DrawStatusSetZoom(int zoomFactor) {
  gZoomFactor = zoomFactor;
}

void sdl3DrawStatusSetAtlas(SDL_Texture *atlas, int sheetScale) {
  gTilesTex   = atlas;
  gSheetScale = sheetScale;
}

void sdl3DrawStatusSetFonts(TTF_Font *fontTiny, TTF_Font *fontMsg,
                            TTF_Font *fontKD,   TTF_Font *fontLabel,
                            TTF_Font *fallbackFontTiny,
                            TTF_Font *fallbackFontMsg,
                            TTF_Font *fallbackFontKD,
                            TTF_Font *fallbackFontLabel) {
  gFontTiny          = fontTiny;
  gFontMsg           = fontMsg;
  gFontKD            = fontKD;
  gFontLabel         = fontLabel;
  gFallbackFontTiny  = fallbackFontTiny;
  gFallbackFontMsg   = fallbackFontMsg;
  gFallbackFontKD    = fallbackFontKD;
  gFallbackFontLabel = fallbackFontLabel;
}

void sdl3DrawStatusSetEdgeOffset(int edgeX, int edgeY) {
  gCurrentEdgeX = edgeX;
  gCurrentEdgeY = edgeY;
}

void sdl3DrawStatusSetPanelOrigins(float tanksX, float tanksY,
                                   float pillsX, float pillsY,
                                   float basesX, float basesY) {
  gStatusTanksOrgX = tanksX;
  gStatusTanksOrgY = tanksY;
  gStatusPillsOrgX = pillsX;
  gStatusPillsOrgY = pillsY;
  gStatusBasesOrgX = basesX;
  gStatusBasesOrgY = basesY;
}

void sdl3DrawStatusShutdown(void) {
  /* Destroy owned (b)-class texture caches */
  for (int i = 0; i < SDL3_MAX_PLAYERS; i++) {
    if (gLabelTex[i]) { SDL_DestroyTexture(gLabelTex[i]); gLabelTex[i] = NULL; }
    gLabelStr[i][0] = '\0';
  }
  if (gTexMsgTop) { SDL_DestroyTexture(gTexMsgTop); gTexMsgTop = NULL; }
  if (gTexMsgBot) { SDL_DestroyTexture(gTexMsgBot); gTexMsgBot = NULL; }
  if (gTexKills)  { SDL_DestroyTexture(gTexKills);  gTexKills  = NULL; }
  if (gTexDeaths) { SDL_DestroyTexture(gTexDeaths); gTexDeaths = NULL; }
  gTexMsgTopStr[0] = '\0';
  gTexMsgBotStr[0] = '\0';
  gTexKillsStr[0]  = '\0';
  gTexDeathsStr[0] = '\0';

  /* Clear (a)-class handles — caller owns them and may destroy
   * them right after this call; null pointers stop us from
   * dereferencing dangling handles next render. */
  gRenderer          = NULL;
  gTilesTex          = NULL;
  gFontTiny          = NULL;
  gFontMsg           = NULL;
  gFontKD            = NULL;
  gFontLabel         = NULL;
  gFallbackFontTiny  = NULL;
  gFallbackFontMsg   = NULL;
  gFallbackFontKD    = NULL;
  gFallbackFontLabel = NULL;
  gTankBarsTex       = NULL;
  gBaseBarsTex       = NULL;
}
