/*
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
 * Name:          mapview.c
 * Purpose:
 *   Reusable map view renderer extracted from sdl3draw.c.
 *   Draws tiles, shells, tanks, and LGMs using a
 *   MapViewCtx (renderer + atlas + zoom). Can also build
 *   tile and sprite buffers directly from a GameSim for
 *   the bg_game welcome screen.
 *********************************************************/

#include "mapview.h"
#include "../tiles.h"
#include "../../bolo/tilenum.h"
#include "../../bolo/bolo_map.h"
#include "../../bolo/pillbox.h"
#include "../../bolo/bases.h"
#include "../../bolo/mines.h"
#include "../../bolo/tank.h"
#include "../../bolo/screencalc.h"
#include "../../bolo/players.h"
#include "../../bolo/shells.h"
#include "../../bolo/explosions.h"
#include "../../bolo/tankexp.h"
#include "../../bolo/util.h"

#include <string.h>

/* mapViewPosX/Y and mapViewInit() moved to sprite_positions.{c,h} so the
 * log viewer can link the table without dragging in the bolo screen
 * dependencies that mapview's draw helpers below need. */


/*********************************************************
 * mapViewDrawTiles — blit the tile buffer to the renderer.
 * Moved from sdl3draw.c tile blit loop.
 *********************************************************/
void mapViewDrawTiles(MapViewCtx *ctx, screen *value, screenMines *mineView,
                      int originX, int originY, int tileW, int tileH,
                      int edgeX, int edgeY) {
  int ss = ctx->sheetScale;
  BYTE x = 0, y = 0;
  bool done = FALSE;
  while (!done) {
    BYTE pos = screenGetPos(value, x, y);
    SDL_FRect src = {
      (float)(mapViewPosX[pos] * ss),
      (float)(mapViewPosY[pos] * ss),
      (float)(TILE_SIZE_X * ss),
      (float)(TILE_SIZE_Y * ss)
    };
    SDL_FRect dest = {
      (float)(originX + ((int)x - 1) * tileW - edgeX),
      (float)(originY + ((int)y - 1) * tileH - edgeY),
      (float)tileW,
      (float)tileH
    };
    SDL_RenderTexture(ctx->renderer, ctx->tilesTex, &src, &dest);

    if (screenIsMine(mineView, x, y)) {
      SDL_FRect mineSrc = { (float)(MINE_X * ss), (float)(MINE_Y * ss),
                            (float)(TILE_SIZE_X * ss), (float)(TILE_SIZE_Y * ss) };
      SDL_RenderTexture(ctx->renderer, ctx->tilesTex, &mineSrc, &dest);
    }

    x++;
    if (x == MAIN_BACK_BUFFER_SIZE_X) {
      x = 0; y++;
      if (y == MAIN_BACK_BUFFER_SIZE_Y) done = TRUE;
    }
  }
}

/*********************************************************
 * mapViewDrawShells — moved from sdl3DrawShells.
 *********************************************************/
void mapViewDrawShells(MapViewCtx *ctx, screenBullets *sBullets,
                       int originX, int originY, int tileW, int tileH,
                       int edgeX, int edgeY) {
  int total = screenBulletsGetNumEntries(sBullets);
  for (int count = 1; count <= total; count++) {
    BYTE mx, my, px, py, frame;
    screenBulletsGetItem(sBullets, count, &mx, &my, &px, &py, &frame);

    int srcX, srcY, srcW, srcH;
    switch (frame) {
      case SHELL_EXPLOSION8: srcX=EXPLOSION8_X; srcY=EXPLOSION8_Y; srcW=TILE_SIZE_X; srcH=TILE_SIZE_Y; break;
      case SHELL_EXPLOSION7: srcX=EXPLOSION7_X; srcY=EXPLOSION7_Y; srcW=TILE_SIZE_X; srcH=TILE_SIZE_Y; break;
      case SHELL_EXPLOSION6: srcX=EXPLOSION6_X; srcY=EXPLOSION6_Y; srcW=TILE_SIZE_X; srcH=TILE_SIZE_Y; break;
      case SHELL_EXPLOSION5: srcX=EXPLOSION5_X; srcY=EXPLOSION5_Y; srcW=TILE_SIZE_X; srcH=TILE_SIZE_Y; break;
      case SHELL_EXPLOSION4: srcX=EXPLOSION4_X; srcY=EXPLOSION4_Y; srcW=TILE_SIZE_X; srcH=TILE_SIZE_Y; break;
      case SHELL_EXPLOSION3: srcX=EXPLOSION3_X; srcY=EXPLOSION3_Y; srcW=TILE_SIZE_X; srcH=TILE_SIZE_Y; break;
      case SHELL_EXPLOSION2: srcX=EXPLOSION2_X; srcY=EXPLOSION2_Y; srcW=TILE_SIZE_X; srcH=TILE_SIZE_Y; break;
      case SHELL_EXPLOSION1: srcX=EXPLOSION1_X; srcY=EXPLOSION1_Y; srcW=TILE_SIZE_X; srcH=TILE_SIZE_Y; break;
      case SHELL_DIR0:  srcX=SHELL_0_X;  srcY=SHELL_0_Y;  srcW=SHELL_0_WIDTH;  srcH=SHELL_0_HEIGHT;  break;
      case SHELL_DIR1:  srcX=SHELL_1_X;  srcY=SHELL_1_Y;  srcW=SHELL_1_WIDTH;  srcH=SHELL_1_HEIGHT;  break;
      case SHELL_DIR2:  srcX=SHELL_2_X;  srcY=SHELL_2_Y;  srcW=SHELL_2_WIDTH;  srcH=SHELL_2_HEIGHT;  break;
      case SHELL_DIR3:  srcX=SHELL_3_X;  srcY=SHELL_3_Y;  srcW=SHELL_3_WIDTH;  srcH=SHELL_3_HEIGHT;  break;
      case SHELL_DIR4:  srcX=SHELL_4_X;  srcY=SHELL_4_Y;  srcW=SHELL_4_WIDTH;  srcH=SHELL_4_HEIGHT;  break;
      case SHELL_DIR5:  srcX=SHELL_5_X;  srcY=SHELL_5_Y;  srcW=SHELL_5_WIDTH;  srcH=SHELL_5_HEIGHT;  break;
      case SHELL_DIR6:  srcX=SHELL_6_X;  srcY=SHELL_6_Y;  srcW=SHELL_6_WIDTH;  srcH=SHELL_6_HEIGHT;  break;
      case SHELL_DIR7:  srcX=SHELL_7_X;  srcY=SHELL_7_Y;  srcW=SHELL_7_WIDTH;  srcH=SHELL_7_HEIGHT;  break;
      case SHELL_DIR8:  srcX=SHELL_8_X;  srcY=SHELL_8_Y;  srcW=SHELL_8_WIDTH;  srcH=SHELL_8_HEIGHT;  break;
      case SHELL_DIR9:  srcX=SHELL_9_X;  srcY=SHELL_9_Y;  srcW=SHELL_9_WIDTH;  srcH=SHELL_9_HEIGHT;  break;
      case SHELL_DIR10: srcX=SHELL_10_X; srcY=SHELL_10_Y; srcW=SHELL_10_WIDTH; srcH=SHELL_10_HEIGHT; break;
      case SHELL_DIR11: srcX=SHELL_11_X; srcY=SHELL_11_Y; srcW=SHELL_11_WIDTH; srcH=SHELL_11_HEIGHT; break;
      case SHELL_DIR12: srcX=SHELL_12_X; srcY=SHELL_12_Y; srcW=SHELL_12_WIDTH; srcH=SHELL_12_HEIGHT; break;
      case SHELL_DIR13: srcX=SHELL_13_X; srcY=SHELL_13_Y; srcW=SHELL_13_WIDTH; srcH=SHELL_13_HEIGHT; break;
      case SHELL_DIR14: srcX=SHELL_14_X; srcY=SHELL_14_Y; srcW=SHELL_14_WIDTH; srcH=SHELL_14_HEIGHT; break;
      case SHELL_DIR15: srcX=SHELL_15_X; srcY=SHELL_15_Y; srcW=SHELL_15_WIDTH; srcH=SHELL_15_HEIGHT; break;
      default: continue;
    }

    int bbx = (int)mx * TILE_SIZE_X + (int)px;
    int bby = (int)my * TILE_SIZE_Y + (int)py;
    float sx = (float)(originX - tileW + bbx * ctx->zoomFactor - edgeX);
    float sy = (float)(originY - tileH + bby * ctx->zoomFactor - edgeY);

    /* Anchor-pixel positioning: place the sprite so its leading pixel lands
     * exactly on the shell's world position (the collision point).
     *
     * Each entry is the (col, row) of the tip pixel within that direction's
     * sprite, read directly from the sprite shapes in tile.bmp.  We subtract
     * these from the top-left position so the tip — not the top-left corner —
     * sits at the shell coordinate.
     *
     * Indexed by shell direction 0-15 (N, NNE, NE, ENE, E, ESE, SE, SSE,
     *                                   S, SSW, SW, WSW, W, WNW, NW, NNW). */
    if (frame >= SHELL_DIR0 && frame <= SHELL_DIR15) {
      /* Symmetric diamond, matching brains/NewAutopilot/init.lua's
       * draw_shell_hitbox_viz mirror. Game-pixel offsets in 0..4
       * range (4 = right/bottom edge of the 4-px sprite). Float so
       * sub-pixel anchoring works at zoomFactor > 1. */
      static const float kTipCol[16] = {
        1.5f, 3.0f, 4.0f, 4.0f,    /* N   NNE  NE   ENE  */
        4.0f, 4.0f, 4.0f, 3.0f,    /* E   ESE  SE   SSE  */
        1.5f, 0.0f, 0.0f, 0.0f,    /* S   SSW  SW   WSW  */
        0.0f, 0.0f, 0.0f, 0.0f     /* W   WNW  NW   NNW  */
      };
      static const float kTipRow[16] = {
        0.0f, 0.0f, 0.0f, 0.0f,    /* N   NNE  NE   ENE  */
        1.5f, 3.0f, 4.0f, 4.0f,    /* E   ESE  SE   SSE  */
        4.0f, 4.0f, 3.0f, 3.0f,    /* S   SSW  SW   WSW  */
        1.5f, 0.0f, 0.0f, 0.0f     /* W   WNW  NW   NNW  */
      };
      int dir = frame - SHELL_DIR0;
      sx -= kTipCol[dir] * (float)ctx->zoomFactor;
      sy -= kTipRow[dir] * (float)ctx->zoomFactor;
    }

    int ss = ctx->sheetScale;
    SDL_FRect srcR = { (float)(srcX * ss), (float)(srcY * ss), (float)(srcW * ss), (float)(srcH * ss) };
    SDL_FRect dstR = { sx, sy, (float)(srcW * ctx->zoomFactor), (float)(srcH * ctx->zoomFactor) };
    SDL_RenderTexture(ctx->renderer, ctx->tilesTex, &srcR, &dstR);
  }
}

/*********************************************************
 * mapViewDrawTanks — moved from sdl3DrawTanks.
 * Note: does NOT draw tank labels (labels need fonts
 * which stay in sdl3draw.c).
 *********************************************************/
void mapViewDrawTanks(MapViewCtx *ctx, screenTanks *tks,
                      int originX, int originY, int tileW, int tileH,
                      int edgeX, int edgeY) {
  BYTE total = screenTanksGetNumEntries(tks);
  for (BYTE count = 1; count <= total; count++) {
    BYTE mx, my, px, py, frame, playerNum;
    char playerName[256];
    screenTanksGetItem(tks, count, &mx, &my, &px, &py, &frame, &playerNum, playerName);

    int srcX = 0, srcY = 0;
    switch (frame) {
      case TANK_SELF_0:      srcX=TANK_SELF_0_X;      srcY=TANK_SELF_0_Y;      break;
      case TANK_SELF_0+1:    srcX=TANK_SELF_1_X;      srcY=TANK_SELF_1_Y;      break;
      case TANK_SELF_0+2:    srcX=TANK_SELF_2_X;      srcY=TANK_SELF_2_Y;      break;
      case TANK_SELF_0+3:    srcX=TANK_SELF_3_X;      srcY=TANK_SELF_3_Y;      break;
      case TANK_SELF_0+4:    srcX=TANK_SELF_4_X;      srcY=TANK_SELF_4_Y;      break;
      case TANK_SELF_0+5:    srcX=TANK_SELF_5_X;      srcY=TANK_SELF_5_Y;      break;
      case TANK_SELF_0+6:    srcX=TANK_SELF_6_X;      srcY=TANK_SELF_6_Y;      break;
      case TANK_SELF_0+7:    srcX=TANK_SELF_7_X;      srcY=TANK_SELF_7_Y;      break;
      case TANK_SELF_0+8:    srcX=TANK_SELF_8_X;      srcY=TANK_SELF_8_Y;      break;
      case TANK_SELF_0+9:    srcX=TANK_SELF_9_X;      srcY=TANK_SELF_9_Y;      break;
      case TANK_SELF_0+10:   srcX=TANK_SELF_10_X;     srcY=TANK_SELF_10_Y;     break;
      case TANK_SELF_0+11:   srcX=TANK_SELF_11_X;     srcY=TANK_SELF_11_Y;     break;
      case TANK_SELF_0+12:   srcX=TANK_SELF_12_X;     srcY=TANK_SELF_12_Y;     break;
      case TANK_SELF_0+13:   srcX=TANK_SELF_13_X;     srcY=TANK_SELF_13_Y;     break;
      case TANK_SELF_0+14:   srcX=TANK_SELF_14_X;     srcY=TANK_SELF_14_Y;     break;
      case TANK_SELF_0+15:   srcX=TANK_SELF_15_X;     srcY=TANK_SELF_15_Y;     break;
      case TANK_SELFBOAT_0:  srcX=TANK_SELFBOAT_0_X;  srcY=TANK_SELFBOAT_0_Y;  break;
      case TANK_SELFBOAT_0+1:srcX=TANK_SELFBOAT_1_X;  srcY=TANK_SELFBOAT_1_Y;  break;
      case TANK_SELFBOAT_0+2:srcX=TANK_SELFBOAT_2_X;  srcY=TANK_SELFBOAT_2_Y;  break;
      case TANK_SELFBOAT_0+3:srcX=TANK_SELFBOAT_3_X;  srcY=TANK_SELFBOAT_3_Y;  break;
      case TANK_SELFBOAT_0+4:srcX=TANK_SELFBOAT_4_X;  srcY=TANK_SELFBOAT_4_Y;  break;
      case TANK_SELFBOAT_0+5:srcX=TANK_SELFBOAT_5_X;  srcY=TANK_SELFBOAT_5_Y;  break;
      case TANK_SELFBOAT_0+6:srcX=TANK_SELFBOAT_6_X;  srcY=TANK_SELFBOAT_6_Y;  break;
      case TANK_SELFBOAT_0+7:srcX=TANK_SELFBOAT_7_X;  srcY=TANK_SELFBOAT_7_Y;  break;
      case TANK_SELFBOAT_0+8:srcX=TANK_SELFBOAT_8_X;  srcY=TANK_SELFBOAT_8_Y;  break;
      case TANK_SELFBOAT_0+9:srcX=TANK_SELFBOAT_9_X;  srcY=TANK_SELFBOAT_9_Y;  break;
      case TANK_SELFBOAT_0+10:srcX=TANK_SELFBOAT_10_X;srcY=TANK_SELFBOAT_10_Y; break;
      case TANK_SELFBOAT_0+11:srcX=TANK_SELFBOAT_11_X;srcY=TANK_SELFBOAT_11_Y; break;
      case TANK_SELFBOAT_0+12:srcX=TANK_SELFBOAT_12_X;srcY=TANK_SELFBOAT_12_Y; break;
      case TANK_SELFBOAT_0+13:srcX=TANK_SELFBOAT_13_X;srcY=TANK_SELFBOAT_13_Y; break;
      case TANK_SELFBOAT_0+14:srcX=TANK_SELFBOAT_14_X;srcY=TANK_SELFBOAT_14_Y; break;
      case TANK_SELFBOAT_0+15:srcX=TANK_SELFBOAT_15_X;srcY=TANK_SELFBOAT_15_Y; break;
      case TANK_GOOD_0:      srcX=TANK_GOOD_0_X;      srcY=TANK_GOOD_0_Y;      break;
      case TANK_GOOD_0+1:    srcX=TANK_GOOD_1_X;      srcY=TANK_GOOD_1_Y;      break;
      case TANK_GOOD_0+2:    srcX=TANK_GOOD_2_X;      srcY=TANK_GOOD_2_Y;      break;
      case TANK_GOOD_0+3:    srcX=TANK_GOOD_3_X;      srcY=TANK_GOOD_3_Y;      break;
      case TANK_GOOD_0+4:    srcX=TANK_GOOD_4_X;      srcY=TANK_GOOD_4_Y;      break;
      case TANK_GOOD_0+5:    srcX=TANK_GOOD_5_X;      srcY=TANK_GOOD_5_Y;      break;
      case TANK_GOOD_0+6:    srcX=TANK_GOOD_6_X;      srcY=TANK_GOOD_6_Y;      break;
      case TANK_GOOD_0+7:    srcX=TANK_GOOD_7_X;      srcY=TANK_GOOD_7_Y;      break;
      case TANK_GOOD_0+8:    srcX=TANK_GOOD_8_X;      srcY=TANK_GOOD_8_Y;      break;
      case TANK_GOOD_0+9:    srcX=TANK_GOOD_9_X;      srcY=TANK_GOOD_9_Y;      break;
      case TANK_GOOD_0+10:   srcX=TANK_GOOD_10_X;     srcY=TANK_GOOD_10_Y;     break;
      case TANK_GOOD_0+11:   srcX=TANK_GOOD_11_X;     srcY=TANK_GOOD_11_Y;     break;
      case TANK_GOOD_0+12:   srcX=TANK_GOOD_12_X;     srcY=TANK_GOOD_12_Y;     break;
      case TANK_GOOD_0+13:   srcX=TANK_GOOD_13_X;     srcY=TANK_GOOD_13_Y;     break;
      case TANK_GOOD_0+14:   srcX=TANK_GOOD_14_X;     srcY=TANK_GOOD_14_Y;     break;
      case TANK_GOOD_0+15:   srcX=TANK_GOOD_15_X;     srcY=TANK_GOOD_15_Y;     break;
      case TANK_GOODBOAT_0:  srcX=TANK_GOODBOAT_0_X;  srcY=TANK_GOODBOAT_0_Y;  break;
      case TANK_GOODBOAT_0+1:srcX=TANK_GOODBOAT_1_X;  srcY=TANK_GOODBOAT_1_Y;  break;
      case TANK_GOODBOAT_0+2:srcX=TANK_GOODBOAT_2_X;  srcY=TANK_GOODBOAT_2_Y;  break;
      case TANK_GOODBOAT_0+3:srcX=TANK_GOODBOAT_3_X;  srcY=TANK_GOODBOAT_3_Y;  break;
      case TANK_GOODBOAT_0+4:srcX=TANK_GOODBOAT_4_X;  srcY=TANK_GOODBOAT_4_Y;  break;
      case TANK_GOODBOAT_0+5:srcX=TANK_GOODBOAT_5_X;  srcY=TANK_GOODBOAT_5_Y;  break;
      case TANK_GOODBOAT_0+6:srcX=TANK_GOODBOAT_6_X;  srcY=TANK_GOODBOAT_6_Y;  break;
      case TANK_GOODBOAT_0+7:srcX=TANK_GOODBOAT_7_X;  srcY=TANK_GOODBOAT_7_Y;  break;
      case TANK_GOODBOAT_0+8:srcX=TANK_GOODBOAT_8_X;  srcY=TANK_GOODBOAT_8_Y;  break;
      case TANK_GOODBOAT_0+9:srcX=TANK_GOODBOAT_9_X;  srcY=TANK_GOODBOAT_9_Y;  break;
      case TANK_GOODBOAT_0+10:srcX=TANK_GOODBOAT_10_X;srcY=TANK_GOODBOAT_10_Y; break;
      case TANK_GOODBOAT_0+11:srcX=TANK_GOODBOAT_11_X;srcY=TANK_GOODBOAT_11_Y; break;
      case TANK_GOODBOAT_0+12:srcX=TANK_GOODBOAT_12_X;srcY=TANK_GOODBOAT_12_Y; break;
      case TANK_GOODBOAT_0+13:srcX=TANK_GOODBOAT_13_X;srcY=TANK_GOODBOAT_13_Y; break;
      case TANK_GOODBOAT_0+14:srcX=TANK_GOODBOAT_14_X;srcY=TANK_GOODBOAT_14_Y; break;
      case TANK_GOODBOAT_0+15:srcX=TANK_GOODBOAT_15_X;srcY=TANK_GOODBOAT_15_Y; break;
      case TANK_EVIL_0:      srcX=TANK_EVIL_0_X;      srcY=TANK_EVIL_0_Y;      break;
      case TANK_EVIL_0+1:    srcX=TANK_EVIL_1_X;      srcY=TANK_EVIL_1_Y;      break;
      case TANK_EVIL_0+2:    srcX=TANK_EVIL_2_X;      srcY=TANK_EVIL_2_Y;      break;
      case TANK_EVIL_0+3:    srcX=TANK_EVIL_3_X;      srcY=TANK_EVIL_3_Y;      break;
      case TANK_EVIL_0+4:    srcX=TANK_EVIL_4_X;      srcY=TANK_EVIL_4_Y;      break;
      case TANK_EVIL_0+5:    srcX=TANK_EVIL_5_X;      srcY=TANK_EVIL_5_Y;      break;
      case TANK_EVIL_0+6:    srcX=TANK_EVIL_6_X;      srcY=TANK_EVIL_6_Y;      break;
      case TANK_EVIL_0+7:    srcX=TANK_EVIL_7_X;      srcY=TANK_EVIL_7_Y;      break;
      case TANK_EVIL_0+8:    srcX=TANK_EVIL_8_X;      srcY=TANK_EVIL_8_Y;      break;
      case TANK_EVIL_0+9:    srcX=TANK_EVIL_9_X;      srcY=TANK_EVIL_9_Y;      break;
      case TANK_EVIL_0+10:   srcX=TANK_EVIL_10_X;     srcY=TANK_EVIL_10_Y;     break;
      case TANK_EVIL_0+11:   srcX=TANK_EVIL_11_X;     srcY=TANK_EVIL_11_Y;     break;
      case TANK_EVIL_0+12:   srcX=TANK_EVIL_12_X;     srcY=TANK_EVIL_12_Y;     break;
      case TANK_EVIL_0+13:   srcX=TANK_EVIL_13_X;     srcY=TANK_EVIL_13_Y;     break;
      case TANK_EVIL_0+14:   srcX=TANK_EVIL_14_X;     srcY=TANK_EVIL_14_Y;     break;
      case TANK_EVIL_0+15:   srcX=TANK_EVIL_15_X;     srcY=TANK_EVIL_15_Y;     break;
      case TANK_EVILBOAT_0:  srcX=TANK_EVILBOAT_0_X;  srcY=TANK_EVILBOAT_0_Y;  break;
      case TANK_EVILBOAT_0+1:srcX=TANK_EVILBOAT_1_X;  srcY=TANK_EVILBOAT_1_Y;  break;
      case TANK_EVILBOAT_0+2:srcX=TANK_EVILBOAT_2_X;  srcY=TANK_EVILBOAT_2_Y;  break;
      case TANK_EVILBOAT_0+3:srcX=TANK_EVILBOAT_3_X;  srcY=TANK_EVILBOAT_3_Y;  break;
      case TANK_EVILBOAT_0+4:srcX=TANK_EVILBOAT_4_X;  srcY=TANK_EVILBOAT_4_Y;  break;
      case TANK_EVILBOAT_0+5:srcX=TANK_EVILBOAT_5_X;  srcY=TANK_EVILBOAT_5_Y;  break;
      case TANK_EVILBOAT_0+6:srcX=TANK_EVILBOAT_6_X;  srcY=TANK_EVILBOAT_6_Y;  break;
      case TANK_EVILBOAT_0+7:srcX=TANK_EVILBOAT_7_X;  srcY=TANK_EVILBOAT_7_Y;  break;
      case TANK_EVILBOAT_0+8:srcX=TANK_EVILBOAT_8_X;  srcY=TANK_EVILBOAT_8_Y;  break;
      case TANK_EVILBOAT_0+9:srcX=TANK_EVILBOAT_9_X;  srcY=TANK_EVILBOAT_9_Y;  break;
      case TANK_EVILBOAT_0+10:srcX=TANK_EVILBOAT_10_X;srcY=TANK_EVILBOAT_10_Y; break;
      case TANK_EVILBOAT_0+11:srcX=TANK_EVILBOAT_11_X;srcY=TANK_EVILBOAT_11_Y; break;
      case TANK_EVILBOAT_0+12:srcX=TANK_EVILBOAT_12_X;srcY=TANK_EVILBOAT_12_Y; break;
      case TANK_EVILBOAT_0+13:srcX=TANK_EVILBOAT_13_X;srcY=TANK_EVILBOAT_13_Y; break;
      case TANK_EVILBOAT_0+14:srcX=TANK_EVILBOAT_14_X;srcY=TANK_EVILBOAT_14_Y; break;
      case TANK_EVILBOAT_0+15:srcX=TANK_EVILBOAT_15_X;srcY=TANK_EVILBOAT_15_Y; break;
      default: continue;
    }

    /* Win32 adds 2 to px/py before computing position */
    int apx = (int)px;// + 2;
    int apy = (int)py;// + 2;
    int bbx = (int)mx * TILE_SIZE_X + apx;
    int bby = (int)my * TILE_SIZE_Y + apy;
    float sx = (float)(originX - tileW + bbx * ctx->zoomFactor - edgeX);
    float sy = (float)(originY - tileH + bby * ctx->zoomFactor - edgeY);

    {
      int ss = ctx->sheetScale;
      /* Inset the source rect by a tiny amount to prevent the GPU from
         sampling the adjacent atlas row due to float-to-UV precision
         errors.  BMP-sourced sprites above have green (0,255,0,0) in
         their transparent pixels which would otherwise bleed through. */
      float inset = 0.05f;
      SDL_FRect srcR = { (float)(srcX * ss) + inset, (float)(srcY * ss) + inset,
                         (float)(TILE_SIZE_X * ss) - 2.0f * inset, (float)(TILE_SIZE_Y * ss) - 2.0f * inset };
      SDL_FRect dstR = { sx, sy, (float)tileW, (float)tileH };
      SDL_RenderTexture(ctx->renderer, ctx->tilesTex, &srcR, &dstR);
    }
  }
}

/*********************************************************
 * mapViewDrawLGMs — moved from sdl3DrawLGMs.
 *********************************************************/
void mapViewDrawLGMs(MapViewCtx *ctx, screenLgm *lgms,
                     int originX, int originY, int tileW, int tileH,
                     int edgeX, int edgeY) {
  BYTE total = screenLgmGetNumEntries(lgms);
  for (BYTE count = 1; count <= total; count++) {
    BYTE mx, my, px, py, frame;
    screenLgmGetItem(lgms, count, &mx, &my, &px, &py, &frame);

    int srcX, srcY, srcW, srcH;
    switch (frame) {
      case LGM0:
        srcX=LGM0_X; srcY=LGM0_Y; srcW=LGM_WIDTH; srcH=LGM_HEIGHT; break;
      case LGM1:
        srcX=LGM1_X; srcY=LGM1_Y; srcW=LGM_WIDTH; srcH=LGM_HEIGHT; break;
      case LGM2:
        srcX=LGM2_X; srcY=LGM2_Y; srcW=LGM_WIDTH; srcH=LGM_HEIGHT; break;
      default:
        srcX=LGM_HELICOPTER_X; srcY=LGM_HELICOPTER_Y; srcW=TILE_SIZE_X; srcH=TILE_SIZE_Y; break;
    }

    int bbx = (int)mx * TILE_SIZE_X + (int)px;
    int bby = (int)my * TILE_SIZE_Y + (int)py;
    float sx = (float)(originX - tileW + bbx * ctx->zoomFactor - edgeX);
    float sy = (float)(originY - tileH + bby * ctx->zoomFactor - edgeY);

    /* Centre the LGM sprite on its authoritative hit pixel.
     * LGM_WIDTH=3 and LGM_HEIGHT=4, so the sprite-local centre is
     * (1.5, 2.0).  Without this offset the sprite top-left lands on
     * the body coord, putting the head ~2 game-pixels north-west of
     * where the engine actually thinks the LGM is — which makes
     * hit/death animations look offset from the sprite. */
    if (frame == LGM0 || frame == LGM1 || frame == LGM2) {
      sx -= 1.5f * (float)ctx->zoomFactor;
      sy -= 2.0f * (float)ctx->zoomFactor;
    }

    {
      int ss = ctx->sheetScale;
      SDL_FRect srcR = { (float)(srcX * ss), (float)(srcY * ss), (float)(srcW * ss), (float)(srcH * ss) };
      SDL_FRect dstR = { sx, sy, (float)(srcW * ctx->zoomFactor), (float)(srcH * ctx->zoomFactor) };
      SDL_RenderTexture(ctx->renderer, ctx->tilesTex, &srcR, &dstR);
    }
  }
}

/*********************************************************
 * mapViewCalcSquare — adjacency-aware tile calculation
 * from a GameSim. Same logic as screenCalcSquare in
 * screen.c but reads from sim instead of module-static cs.
 * Skips the invisiwall mapSetPos mutation (read-only).
 *********************************************************/

/* Read a neighbour tile, treating bases as ROAD and stripping mines. */
static BYTE mapViewNeighbour(GameSim *sim, BYTE nx, BYTE ny) {
  if (basesExistPos(&sim->bs, nx, ny) == TRUE) return ROAD;
  BYTE t = mapGetPos(&sim->mp, nx, ny);
  if (t >= MINE_START && t <= MINE_END) return (BYTE)(t - MINE_SUBTRACT);
  return t;
}

BYTE mapViewCalcSquare(GameSim *sim, BYTE xValue, BYTE yValue, bool *outMine, BYTE selfPlayer) {
  BYTE returnValue;
  *outMine = false;

  /* Pillbox check */
  if (pillsExistPos(&sim->pb, xValue, yValue) == TRUE) {
    returnValue = pillsGetScreenHealth(sim, &sim->pb, xValue, yValue);
    return returnValue;
  }

  /* Base check */
  if (basesExistPos(&sim->bs, xValue, yValue) == TRUE) {
    baseAlliance ba = basesGetAlliancePos(sim, xValue, yValue);
    switch (ba) {
    case baseOwnGood:
    case baseAllieGood:
      returnValue = BASE_GOOD;
      break;
    case baseNeutral:
      returnValue = BASE_NEUTRAL;
      break;
    case baseDead:
      if (selfPlayer != NEUTRAL && basesAmOwner(sim, selfPlayer, xValue, yValue) == TRUE) {
        returnValue = BASE_GOOD;
      } else {
        returnValue = BASE_EVIL;
      }
      break;
    case baseEvil:
    default:
      returnValue = BASE_EVIL;
      break;
    }
    return returnValue;
  }

  /* Regular terrain with adjacency */
  BYTE currentPos = mapGetPos(&sim->mp, xValue, yValue);

  /* Mine detection (skip invisiwall mutation) */
  if (mapIsMine(&sim->mp, xValue, yValue) == TRUE) {
    if (minesExistPos(&sim->mns, &sim->mp, xValue, yValue) == TRUE) {
      *outMine = true;
    }
    if (currentPos != DEEP_SEA) {
      currentPos = currentPos - MINE_SUBTRACT;
    }
  }

  /* Helper: read a neighbour position, treating bases as ROAD */
  BYTE aboveLeft  = mapViewNeighbour(sim, (BYTE)(xValue-1), (BYTE)(yValue-1));
  BYTE above      = mapViewNeighbour(sim, (BYTE)(xValue),   (BYTE)(yValue-1));
  BYTE aboveRight = mapViewNeighbour(sim, (BYTE)(xValue+1), (BYTE)(yValue-1));
  BYTE leftPos    = mapViewNeighbour(sim, (BYTE)(xValue-1), (BYTE)(yValue));
  BYTE rightPos   = mapViewNeighbour(sim, (BYTE)(xValue+1), (BYTE)(yValue));
  BYTE belowLeft  = mapViewNeighbour(sim, (BYTE)(xValue-1), (BYTE)(yValue+1));
  BYTE below      = mapViewNeighbour(sim, (BYTE)(xValue),   (BYTE)(yValue+1));
  BYTE belowRight = mapViewNeighbour(sim, (BYTE)(xValue+1), (BYTE)(yValue+1));

  switch (currentPos) {
  case ROAD:
    returnValue = screenCalcRoad(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    break;
  case BUILDING:
    returnValue = screenCalcBuilding(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    break;
  case FOREST:
    returnValue = screenCalcForest(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    break;
  case RIVER:
    returnValue = screenCalcRiver(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    break;
  case DEEP_SEA:
    returnValue = screenCalcDeepSea(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    break;
  case BOAT:
    returnValue = screenCalcBoat(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    break;
  case CRATER:
    returnValue = screenCalcCrater(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    break;
  default:
    returnValue = currentPos;
    break;
  }
  return returnValue;
}

/*********************************************************
 * Internal tile buffer for mapViewRenderCentered.
 * Uses larger dimensions than the client's 17x17 to
 * handle big viewports.
 *********************************************************/
typedef struct {
  BYTE tiles[MAPVIEW_MAX_TILES_W][MAPVIEW_MAX_TILES_H];
  bool mines[MAPVIEW_MAX_TILES_W][MAPVIEW_MAX_TILES_H];
} MapViewTileBuffer;

static void mapViewBuildTileBuffer(GameSim *sim, MapViewTileBuffer *buf,
                                   BYTE camMX, BYTE camMY,
                                   int tilesW, int tilesH, BYTE selfPlayer) {
  for (int x = 0; x < tilesW; x++) {
    for (int y = 0; y < tilesH; y++) {
      bool isMine = false;
      BYTE mapX = (BYTE)(camMX + x);
      BYTE mapY = (BYTE)(camMY + y);
      buf->tiles[x][y] = mapViewCalcSquare(sim, mapX, mapY, &isMine, selfPlayer);
      buf->mines[x][y] = isMine;
    }
  }
}

/*********************************************************
 * mapViewRenderCentered — all-in-one renderer for bg_game.
 * Computes camera, builds tile buffer from GameSim,
 * builds tanks from GameSim, draws everything.
 *********************************************************/
void mapViewRenderCentered(MapViewCtx *ctx, GameSim *sim,
                           WORLD centerWX, WORLD centerWY,
                           int originX, int originY,
                           int viewW, int viewH,
                           BYTE selfPlayer) {
  const int tileSize = TILE_SIZE_X;
  int zf = ctx->zoomFactor;
  int scaledTile = tileSize * zf;

  /* Convert world coords to pixel centre */
  int centerPX = ((int)centerWX * tileSize) >> 8;
  int centerPY = ((int)centerWY * tileSize) >> 8;

  /* Camera top-left in pixel space */
  int camPX = centerPX - viewW / (2 * zf);
  int camPY = centerPY - viewH / (2 * zf);

  /* Camera tile and sub-tile offset */
  int camMX = camPX / tileSize;
  int camMY = camPY / tileSize;
  if (camPX < 0) camMX--;
  if (camPY < 0) camMY--;
  int edgeX = (camPX - camMX * tileSize) * zf;
  int edgeY = (camPY - camMY * tileSize) * zf;

  /* Number of tiles needed to cover the viewport */
  int tilesW = viewW / scaledTile + 3;
  int tilesH = viewH / scaledTile + 3;
  if (tilesW > MAPVIEW_MAX_TILES_W) tilesW = MAPVIEW_MAX_TILES_W;
  if (tilesH > MAPVIEW_MAX_TILES_H) tilesH = MAPVIEW_MAX_TILES_H;

  /* Clamp camera to valid map range */
  if (camMX < 0) camMX = 0;
  if (camMY < 0) camMY = 0;

  /* Build tile buffer */
  static MapViewTileBuffer tileBuf;
  mapViewBuildTileBuffer(sim, &tileBuf, (BYTE)camMX, (BYTE)camMY, tilesW, tilesH, selfPlayer);

  /* Draw tiles */
  int ss = ctx->sheetScale;
  for (int x = 0; x < tilesW; x++) {
    for (int y = 0; y < tilesH; y++) {
      BYTE pos = tileBuf.tiles[x][y];
      SDL_FRect src = {
        (float)(mapViewPosX[pos] * ss),
        (float)(mapViewPosY[pos] * ss),
        (float)(tileSize * ss),
        (float)(tileSize * ss)
      };
      SDL_FRect dest = {
        (float)(originX + x * scaledTile - edgeX),
        (float)(originY + y * scaledTile - edgeY),
        (float)scaledTile,
        (float)scaledTile
      };
      SDL_RenderTexture(ctx->renderer, ctx->tilesTex, &src, &dest);

      if (tileBuf.mines[x][y]) {
        SDL_FRect mineSrc = { (float)(MINE_X * ss), (float)(MINE_Y * ss),
                              (float)(tileSize * ss), (float)(tileSize * ss) };
        SDL_RenderTexture(ctx->renderer, ctx->tilesTex, &mineSrc, &dest);
      }
    }
  }

  /* Draw tanks directly from GameSim */
  for (BYTE i = 0; i < MAX_TANKS; i++) {
    if (sim->tanks[i] == NULL) continue;
    if (tankGetDeathWait(&sim->tanks[i]) > 0) continue;
    WORLD twx, twy;
    tankGetWorld(&sim->tanks[i], &twx, &twy);

    /* Convert tank world pos to pixel, relative to camera */
    int tpx = ((int)twx * tileSize >> 8);
    int tpy = ((int)twy * tileSize >> 8);
    float dx = (float)((tpx - camMX * tileSize) * zf - edgeX + originX - scaledTile / 2);
    float dy = (float)((tpy - camMY * tileSize) * zf - edgeY + originY - scaledTile / 2);

    /* Cull off-screen */
    if (dx + scaledTile < originX || dx > originX + viewW ||
        dy + scaledTile < originY || dy > originY + viewH) continue;

    /* Determine tank frame: direction (0-15) + colour offset.
       Pick self/allie/evil sprite based on alliance with selfPlayer. */
    BYTE dir = tankGetDir(&sim->tanks[i]);
    bool onBoat = tankIsOnBoat(&sim->tanks[i]);
    tankAlliance al = playersScreenAllience(&sim->plyrs, selfPlayer, i);
    BYTE frameBase;
    switch (al) {
      case tankSelf:  frameBase = onBoat ? TANK_SELFBOAT_0 : TANK_SELF_0; break;
      case tankAllie: frameBase = onBoat ? TANK_GOODBOAT_0 : TANK_GOOD_0; break;
      default:        frameBase = onBoat ? TANK_EVILBOAT_0 : TANK_EVIL_0; break;
    }
    BYTE frame = frameBase + dir;

    /* Look up atlas coords using the same switch as mapViewDrawTanks.
       For efficiency, use the lookup table approach: the tank frame values
       0-95 don't have entries in mapViewPosX/Y (those are for terrain tiles
       and use different numbering). So we use a direct switch. */
    int srcX = 0, srcY = 0;
    switch (frame) {
      case TANK_SELF_0:      srcX=TANK_SELF_0_X;      srcY=TANK_SELF_0_Y;      break;
      case TANK_SELF_0+1:    srcX=TANK_SELF_1_X;      srcY=TANK_SELF_1_Y;      break;
      case TANK_SELF_0+2:    srcX=TANK_SELF_2_X;      srcY=TANK_SELF_2_Y;      break;
      case TANK_SELF_0+3:    srcX=TANK_SELF_3_X;      srcY=TANK_SELF_3_Y;      break;
      case TANK_SELF_0+4:    srcX=TANK_SELF_4_X;      srcY=TANK_SELF_4_Y;      break;
      case TANK_SELF_0+5:    srcX=TANK_SELF_5_X;      srcY=TANK_SELF_5_Y;      break;
      case TANK_SELF_0+6:    srcX=TANK_SELF_6_X;      srcY=TANK_SELF_6_Y;      break;
      case TANK_SELF_0+7:    srcX=TANK_SELF_7_X;      srcY=TANK_SELF_7_Y;      break;
      case TANK_SELF_0+8:    srcX=TANK_SELF_8_X;      srcY=TANK_SELF_8_Y;      break;
      case TANK_SELF_0+9:    srcX=TANK_SELF_9_X;      srcY=TANK_SELF_9_Y;      break;
      case TANK_SELF_0+10:   srcX=TANK_SELF_10_X;     srcY=TANK_SELF_10_Y;     break;
      case TANK_SELF_0+11:   srcX=TANK_SELF_11_X;     srcY=TANK_SELF_11_Y;     break;
      case TANK_SELF_0+12:   srcX=TANK_SELF_12_X;     srcY=TANK_SELF_12_Y;     break;
      case TANK_SELF_0+13:   srcX=TANK_SELF_13_X;     srcY=TANK_SELF_13_Y;     break;
      case TANK_SELF_0+14:   srcX=TANK_SELF_14_X;     srcY=TANK_SELF_14_Y;     break;
      case TANK_SELF_0+15:   srcX=TANK_SELF_15_X;     srcY=TANK_SELF_15_Y;     break;
      case TANK_SELFBOAT_0:  srcX=TANK_SELFBOAT_0_X;  srcY=TANK_SELFBOAT_0_Y;  break;
      case TANK_SELFBOAT_0+1:srcX=TANK_SELFBOAT_1_X;  srcY=TANK_SELFBOAT_1_Y;  break;
      case TANK_SELFBOAT_0+2:srcX=TANK_SELFBOAT_2_X;  srcY=TANK_SELFBOAT_2_Y;  break;
      case TANK_SELFBOAT_0+3:srcX=TANK_SELFBOAT_3_X;  srcY=TANK_SELFBOAT_3_Y;  break;
      case TANK_SELFBOAT_0+4:srcX=TANK_SELFBOAT_4_X;  srcY=TANK_SELFBOAT_4_Y;  break;
      case TANK_SELFBOAT_0+5:srcX=TANK_SELFBOAT_5_X;  srcY=TANK_SELFBOAT_5_Y;  break;
      case TANK_SELFBOAT_0+6:srcX=TANK_SELFBOAT_6_X;  srcY=TANK_SELFBOAT_6_Y;  break;
      case TANK_SELFBOAT_0+7:srcX=TANK_SELFBOAT_7_X;  srcY=TANK_SELFBOAT_7_Y;  break;
      case TANK_SELFBOAT_0+8:srcX=TANK_SELFBOAT_8_X;  srcY=TANK_SELFBOAT_8_Y;  break;
      case TANK_SELFBOAT_0+9:srcX=TANK_SELFBOAT_9_X;  srcY=TANK_SELFBOAT_9_Y;  break;
      case TANK_SELFBOAT_0+10:srcX=TANK_SELFBOAT_10_X;srcY=TANK_SELFBOAT_10_Y; break;
      case TANK_SELFBOAT_0+11:srcX=TANK_SELFBOAT_11_X;srcY=TANK_SELFBOAT_11_Y; break;
      case TANK_SELFBOAT_0+12:srcX=TANK_SELFBOAT_12_X;srcY=TANK_SELFBOAT_12_Y; break;
      case TANK_SELFBOAT_0+13:srcX=TANK_SELFBOAT_13_X;srcY=TANK_SELFBOAT_13_Y; break;
      case TANK_SELFBOAT_0+14:srcX=TANK_SELFBOAT_14_X;srcY=TANK_SELFBOAT_14_Y; break;
      case TANK_SELFBOAT_0+15:srcX=TANK_SELFBOAT_15_X;srcY=TANK_SELFBOAT_15_Y; break;
      case TANK_GOOD_0:      srcX=TANK_GOOD_0_X;      srcY=TANK_GOOD_0_Y;      break;
      case TANK_GOOD_0+1:    srcX=TANK_GOOD_1_X;      srcY=TANK_GOOD_1_Y;      break;
      case TANK_GOOD_0+2:    srcX=TANK_GOOD_2_X;      srcY=TANK_GOOD_2_Y;      break;
      case TANK_GOOD_0+3:    srcX=TANK_GOOD_3_X;      srcY=TANK_GOOD_3_Y;      break;
      case TANK_GOOD_0+4:    srcX=TANK_GOOD_4_X;      srcY=TANK_GOOD_4_Y;      break;
      case TANK_GOOD_0+5:    srcX=TANK_GOOD_5_X;      srcY=TANK_GOOD_5_Y;      break;
      case TANK_GOOD_0+6:    srcX=TANK_GOOD_6_X;      srcY=TANK_GOOD_6_Y;      break;
      case TANK_GOOD_0+7:    srcX=TANK_GOOD_7_X;      srcY=TANK_GOOD_7_Y;      break;
      case TANK_GOOD_0+8:    srcX=TANK_GOOD_8_X;      srcY=TANK_GOOD_8_Y;      break;
      case TANK_GOOD_0+9:    srcX=TANK_GOOD_9_X;      srcY=TANK_GOOD_9_Y;      break;
      case TANK_GOOD_0+10:   srcX=TANK_GOOD_10_X;     srcY=TANK_GOOD_10_Y;     break;
      case TANK_GOOD_0+11:   srcX=TANK_GOOD_11_X;     srcY=TANK_GOOD_11_Y;     break;
      case TANK_GOOD_0+12:   srcX=TANK_GOOD_12_X;     srcY=TANK_GOOD_12_Y;     break;
      case TANK_GOOD_0+13:   srcX=TANK_GOOD_13_X;     srcY=TANK_GOOD_13_Y;     break;
      case TANK_GOOD_0+14:   srcX=TANK_GOOD_14_X;     srcY=TANK_GOOD_14_Y;     break;
      case TANK_GOOD_0+15:   srcX=TANK_GOOD_15_X;     srcY=TANK_GOOD_15_Y;     break;
      case TANK_GOODBOAT_0:  srcX=TANK_GOODBOAT_0_X;  srcY=TANK_GOODBOAT_0_Y;  break;
      case TANK_GOODBOAT_0+1:srcX=TANK_GOODBOAT_1_X;  srcY=TANK_GOODBOAT_1_Y;  break;
      case TANK_GOODBOAT_0+2:srcX=TANK_GOODBOAT_2_X;  srcY=TANK_GOODBOAT_2_Y;  break;
      case TANK_GOODBOAT_0+3:srcX=TANK_GOODBOAT_3_X;  srcY=TANK_GOODBOAT_3_Y;  break;
      case TANK_GOODBOAT_0+4:srcX=TANK_GOODBOAT_4_X;  srcY=TANK_GOODBOAT_4_Y;  break;
      case TANK_GOODBOAT_0+5:srcX=TANK_GOODBOAT_5_X;  srcY=TANK_GOODBOAT_5_Y;  break;
      case TANK_GOODBOAT_0+6:srcX=TANK_GOODBOAT_6_X;  srcY=TANK_GOODBOAT_6_Y;  break;
      case TANK_GOODBOAT_0+7:srcX=TANK_GOODBOAT_7_X;  srcY=TANK_GOODBOAT_7_Y;  break;
      case TANK_GOODBOAT_0+8:srcX=TANK_GOODBOAT_8_X;  srcY=TANK_GOODBOAT_8_Y;  break;
      case TANK_GOODBOAT_0+9:srcX=TANK_GOODBOAT_9_X;  srcY=TANK_GOODBOAT_9_Y;  break;
      case TANK_GOODBOAT_0+10:srcX=TANK_GOODBOAT_10_X;srcY=TANK_GOODBOAT_10_Y; break;
      case TANK_GOODBOAT_0+11:srcX=TANK_GOODBOAT_11_X;srcY=TANK_GOODBOAT_11_Y; break;
      case TANK_GOODBOAT_0+12:srcX=TANK_GOODBOAT_12_X;srcY=TANK_GOODBOAT_12_Y; break;
      case TANK_GOODBOAT_0+13:srcX=TANK_GOODBOAT_13_X;srcY=TANK_GOODBOAT_13_Y; break;
      case TANK_GOODBOAT_0+14:srcX=TANK_GOODBOAT_14_X;srcY=TANK_GOODBOAT_14_Y; break;
      case TANK_GOODBOAT_0+15:srcX=TANK_GOODBOAT_15_X;srcY=TANK_GOODBOAT_15_Y; break;
      case TANK_EVIL_0:      srcX=TANK_EVIL_0_X;      srcY=TANK_EVIL_0_Y;      break;
      case TANK_EVIL_0+1:    srcX=TANK_EVIL_1_X;      srcY=TANK_EVIL_1_Y;      break;
      case TANK_EVIL_0+2:    srcX=TANK_EVIL_2_X;      srcY=TANK_EVIL_2_Y;      break;
      case TANK_EVIL_0+3:    srcX=TANK_EVIL_3_X;      srcY=TANK_EVIL_3_Y;      break;
      case TANK_EVIL_0+4:    srcX=TANK_EVIL_4_X;      srcY=TANK_EVIL_4_Y;      break;
      case TANK_EVIL_0+5:    srcX=TANK_EVIL_5_X;      srcY=TANK_EVIL_5_Y;      break;
      case TANK_EVIL_0+6:    srcX=TANK_EVIL_6_X;      srcY=TANK_EVIL_6_Y;      break;
      case TANK_EVIL_0+7:    srcX=TANK_EVIL_7_X;      srcY=TANK_EVIL_7_Y;      break;
      case TANK_EVIL_0+8:    srcX=TANK_EVIL_8_X;      srcY=TANK_EVIL_8_Y;      break;
      case TANK_EVIL_0+9:    srcX=TANK_EVIL_9_X;      srcY=TANK_EVIL_9_Y;      break;
      case TANK_EVIL_0+10:   srcX=TANK_EVIL_10_X;     srcY=TANK_EVIL_10_Y;     break;
      case TANK_EVIL_0+11:   srcX=TANK_EVIL_11_X;     srcY=TANK_EVIL_11_Y;     break;
      case TANK_EVIL_0+12:   srcX=TANK_EVIL_12_X;     srcY=TANK_EVIL_12_Y;     break;
      case TANK_EVIL_0+13:   srcX=TANK_EVIL_13_X;     srcY=TANK_EVIL_13_Y;     break;
      case TANK_EVIL_0+14:   srcX=TANK_EVIL_14_X;     srcY=TANK_EVIL_14_Y;     break;
      case TANK_EVIL_0+15:   srcX=TANK_EVIL_15_X;     srcY=TANK_EVIL_15_Y;     break;
      case TANK_EVILBOAT_0:  srcX=TANK_EVILBOAT_0_X;  srcY=TANK_EVILBOAT_0_Y;  break;
      case TANK_EVILBOAT_0+1:srcX=TANK_EVILBOAT_1_X;  srcY=TANK_EVILBOAT_1_Y;  break;
      case TANK_EVILBOAT_0+2:srcX=TANK_EVILBOAT_2_X;  srcY=TANK_EVILBOAT_2_Y;  break;
      case TANK_EVILBOAT_0+3:srcX=TANK_EVILBOAT_3_X;  srcY=TANK_EVILBOAT_3_Y;  break;
      case TANK_EVILBOAT_0+4:srcX=TANK_EVILBOAT_4_X;  srcY=TANK_EVILBOAT_4_Y;  break;
      case TANK_EVILBOAT_0+5:srcX=TANK_EVILBOAT_5_X;  srcY=TANK_EVILBOAT_5_Y;  break;
      case TANK_EVILBOAT_0+6:srcX=TANK_EVILBOAT_6_X;  srcY=TANK_EVILBOAT_6_Y;  break;
      case TANK_EVILBOAT_0+7:srcX=TANK_EVILBOAT_7_X;  srcY=TANK_EVILBOAT_7_Y;  break;
      case TANK_EVILBOAT_0+8:srcX=TANK_EVILBOAT_8_X;  srcY=TANK_EVILBOAT_8_Y;  break;
      case TANK_EVILBOAT_0+9:srcX=TANK_EVILBOAT_9_X;  srcY=TANK_EVILBOAT_9_Y;  break;
      case TANK_EVILBOAT_0+10:srcX=TANK_EVILBOAT_10_X;srcY=TANK_EVILBOAT_10_Y; break;
      case TANK_EVILBOAT_0+11:srcX=TANK_EVILBOAT_11_X;srcY=TANK_EVILBOAT_11_Y; break;
      case TANK_EVILBOAT_0+12:srcX=TANK_EVILBOAT_12_X;srcY=TANK_EVILBOAT_12_Y; break;
      case TANK_EVILBOAT_0+13:srcX=TANK_EVILBOAT_13_X;srcY=TANK_EVILBOAT_13_Y; break;
      case TANK_EVILBOAT_0+14:srcX=TANK_EVILBOAT_14_X;srcY=TANK_EVILBOAT_14_Y; break;
      case TANK_EVILBOAT_0+15:srcX=TANK_EVILBOAT_15_X;srcY=TANK_EVILBOAT_15_Y; break;
      default: continue;
    }

    SDL_FRect srcR = { (float)(srcX * ss), (float)(srcY * ss), (float)(tileSize * ss), (float)(tileSize * ss) };
    SDL_FRect dstR = { dx, dy, (float)scaledTile, (float)scaledTile };
    SDL_RenderTexture(ctx->renderer, ctx->tilesTex, &srcR, &dstR);
  }

  /* Draw shells directly from GameSim */
  {
    shells q = sim->shs;
    while (q != NULL) {
      if (!q->shellDead) {
        /* Convert shell world coords to pixel position relative to camera */
        int spx = ((int)q->x * tileSize) >> 8;
        int spy = ((int)q->y * tileSize) >> 8;
        BYTE dir = utilGetDir(q->angle);
        BYTE frame = dir + SHELL_START_EXPLODE + 1;  /* SHELL_DIR0 + dir */

        int srcX = 0, srcY = 0, srcW = 0, srcH = 0;
        switch (frame) {
          case SHELL_DIR0:  srcX=SHELL_0_X;  srcY=SHELL_0_Y;  srcW=SHELL_0_WIDTH;  srcH=SHELL_0_HEIGHT;  break;
          case SHELL_DIR1:  srcX=SHELL_1_X;  srcY=SHELL_1_Y;  srcW=SHELL_1_WIDTH;  srcH=SHELL_1_HEIGHT;  break;
          case SHELL_DIR2:  srcX=SHELL_2_X;  srcY=SHELL_2_Y;  srcW=SHELL_2_WIDTH;  srcH=SHELL_2_HEIGHT;  break;
          case SHELL_DIR3:  srcX=SHELL_3_X;  srcY=SHELL_3_Y;  srcW=SHELL_3_WIDTH;  srcH=SHELL_3_HEIGHT;  break;
          case SHELL_DIR4:  srcX=SHELL_4_X;  srcY=SHELL_4_Y;  srcW=SHELL_4_WIDTH;  srcH=SHELL_4_HEIGHT;  break;
          case SHELL_DIR5:  srcX=SHELL_5_X;  srcY=SHELL_5_Y;  srcW=SHELL_5_WIDTH;  srcH=SHELL_5_HEIGHT;  break;
          case SHELL_DIR6:  srcX=SHELL_6_X;  srcY=SHELL_6_Y;  srcW=SHELL_6_WIDTH;  srcH=SHELL_6_HEIGHT;  break;
          case SHELL_DIR7:  srcX=SHELL_7_X;  srcY=SHELL_7_Y;  srcW=SHELL_7_WIDTH;  srcH=SHELL_7_HEIGHT;  break;
          case SHELL_DIR8:  srcX=SHELL_8_X;  srcY=SHELL_8_Y;  srcW=SHELL_8_WIDTH;  srcH=SHELL_8_HEIGHT;  break;
          case SHELL_DIR9:  srcX=SHELL_9_X;  srcY=SHELL_9_Y;  srcW=SHELL_9_WIDTH;  srcH=SHELL_9_HEIGHT;  break;
          case SHELL_DIR10: srcX=SHELL_10_X; srcY=SHELL_10_Y; srcW=SHELL_10_WIDTH; srcH=SHELL_10_HEIGHT; break;
          case SHELL_DIR11: srcX=SHELL_11_X; srcY=SHELL_11_Y; srcW=SHELL_11_WIDTH; srcH=SHELL_11_HEIGHT; break;
          case SHELL_DIR12: srcX=SHELL_12_X; srcY=SHELL_12_Y; srcW=SHELL_12_WIDTH; srcH=SHELL_12_HEIGHT; break;
          case SHELL_DIR13: srcX=SHELL_13_X; srcY=SHELL_13_Y; srcW=SHELL_13_WIDTH; srcH=SHELL_13_HEIGHT; break;
          case SHELL_DIR14: srcX=SHELL_14_X; srcY=SHELL_14_Y; srcW=SHELL_14_WIDTH; srcH=SHELL_14_HEIGHT; break;
          case SHELL_DIR15: srcX=SHELL_15_X; srcY=SHELL_15_Y; srcW=SHELL_15_WIDTH; srcH=SHELL_15_HEIGHT; break;
          default: goto next_shell;
        }

        /* Anchor sprite by its TIP pixel (not by center) so the
         * leading-edge pixel lands on the shell's authoritative
         * world position — same convention as mapViewDrawShells.
         * Without this BrainTest's overlay (orange shell_hit_dot at
         * world coords) sits on the sprite *center* instead of the
         * tip pixel. */
        static const float kTipCol[16] = {
            1.5f, 3.0f, 4.0f, 4.0f,    /* N   NNE  NE   ENE  */
            4.0f, 4.0f, 4.0f, 3.0f,    /* E   ESE  SE   SSE  */
            1.5f, 0.0f, 0.0f, 0.0f,    /* S   SSW  SW   WSW  */
            0.0f, 0.0f, 0.0f, 0.0f     /* W   WNW  NW   NNW  */
        };
        static const float kTipRow[16] = {
            0.0f, 0.0f, 0.0f, 0.0f,    /* N   NNE  NE   ENE  */
            1.5f, 3.0f, 4.0f, 4.0f,    /* E   ESE  SE   SSE  */
            4.0f, 4.0f, 3.0f, 3.0f,    /* S   SSW  SW   WSW  */
            1.5f, 0.0f, 0.0f, 0.0f     /* W   WNW  NW   NNW  */
        };
        float sx = (float)((spx - camMX * tileSize) * zf - edgeX + originX) - kTipCol[dir] * (float)zf;
        float sy = (float)((spy - camMY * tileSize) * zf - edgeY + originY) - kTipRow[dir] * (float)zf;

        /* Cull off-screen */
        if (sx + srcW * zf >= originX && sx <= originX + viewW &&
            sy + srcH * zf >= originY && sy <= originY + viewH) {
          SDL_FRect sSrc = { (float)(srcX * ss), (float)(srcY * ss), (float)(srcW * ss), (float)(srcH * ss) };
          SDL_FRect sDst = { sx, sy, (float)(srcW * zf), (float)(srcH * zf) };
          SDL_RenderTexture(ctx->renderer, ctx->tilesTex, &sSrc, &sDst);
        }
      }
      next_shell:
      q = q->next;
    }
  }

  /* Draw explosions directly from GameSim */
  {
    explosions q = sim->expl;
    while (q != NULL) {
      int epx = (int)q->mx * tileSize + (int)q->px;
      int epy = (int)q->my * tileSize + (int)q->py;
      float ex = (float)((epx - camMX * tileSize) * zf - edgeX + originX);
      float ey = (float)((epy - camMY * tileSize) * zf - edgeY + originY);

      int srcX = 0, srcY = 0;
      /* Explosion frames count down from 8 (start) to 1 (end).
       * The SHELL_EXPLOSION constants invert this: SHELL_EXPLOSION8=1
       * maps to the big sprite and SHELL_EXPLOSION1=8 maps to the tiny
       * sprite.  Match the real game's mapping: length 8 (just started)
       * renders as the smallest sprite, growing toward the biggest. */
      switch (q->length) {
        case 8: srcX=EXPLOSION1_X; srcY=EXPLOSION1_Y; break;
        case 7: srcX=EXPLOSION2_X; srcY=EXPLOSION2_Y; break;
        case 6: srcX=EXPLOSION3_X; srcY=EXPLOSION3_Y; break;
        case 5: srcX=EXPLOSION4_X; srcY=EXPLOSION4_Y; break;
        case 4: srcX=EXPLOSION5_X; srcY=EXPLOSION5_Y; break;
        case 3: srcX=EXPLOSION6_X; srcY=EXPLOSION6_Y; break;
        case 2: srcX=EXPLOSION7_X; srcY=EXPLOSION7_Y; break;
        case 1: srcX=EXPLOSION8_X; srcY=EXPLOSION8_Y; break;
        default: goto next_explosion;
      }

      if (ex + scaledTile >= originX && ex <= originX + viewW &&
          ey + scaledTile >= originY && ey <= originY + viewH) {
        SDL_FRect eSrc = { (float)(srcX * ss), (float)(srcY * ss), (float)(tileSize * ss), (float)(tileSize * ss) };
        SDL_FRect eDst = { ex, ey, (float)scaledTile, (float)scaledTile };
        SDL_RenderTexture(ctx->renderer, ctx->tilesTex, &eSrc, &eDst);
      }
      next_explosion:
      q = q->next;
    }
  }

  /* Draw LGMs (builders) directly from GameSim */
  for (BYTE i = 0; i < MAX_TANKS; i++) {
    lgm *l = &sim->lgmen[i];
    if (*l == NULL) continue;
    if ((*l)->inTank || (*l)->isDead) continue;

    int lpx = ((int)(*l)->x * tileSize) >> 8;
    int lpy = ((int)(*l)->y * tileSize) >> 8;
    float lx = (float)((lpx - camMX * tileSize) * zf - edgeX + originX);
    float ly = (float)((lpy - camMY * tileSize) * zf - edgeY + originY);

    int srcX, srcY, srcW, srcH;
    switch ((*l)->frame) {
      case LGM0:
        srcX=LGM0_X; srcY=LGM0_Y; srcW=LGM_WIDTH; srcH=LGM_HEIGHT; break;
      case LGM1:
        srcX=LGM1_X; srcY=LGM1_Y; srcW=LGM_WIDTH; srcH=LGM_HEIGHT; break;
      case LGM2:
        srcX=LGM2_X; srcY=LGM2_Y; srcW=LGM_WIDTH; srcH=LGM_HEIGHT; break;
      default:
        srcX=LGM_HELICOPTER_X; srcY=LGM_HELICOPTER_Y; srcW=TILE_SIZE_X; srcH=TILE_SIZE_Y; break;
    }

    /* Centre the on-foot LGM sprite on its authoritative hit pixel
     * (l->x, l->y). Sprite-local centre is (LGM_WIDTH/2, LGM_HEIGHT/2)
     * = (1.5, 2.0) game pixels. Helicopter frame draws full-tile from
     * its own (0, 0) anchor so no offset there. */
    if ((*l)->frame == LGM0 || (*l)->frame == LGM1 || (*l)->frame == LGM2) {
      lx -= 1.5f * (float)zf;
      ly -= 2.0f * (float)zf;
    }

    if (lx + srcW * zf >= originX && lx <= originX + viewW &&
        ly + srcH * zf >= originY && ly <= originY + viewH) {
      SDL_FRect lSrc = { (float)(srcX * ss), (float)(srcY * ss), (float)(srcW * ss), (float)(srcH * ss) };
      SDL_FRect lDst = { lx, ly, (float)(srcW * zf), (float)(srcH * zf) };
      SDL_RenderTexture(ctx->renderer, ctx->tilesTex, &lSrc, &lDst);
    }
  }

  /* Draw tank explosions (flying debris) directly from GameSim */
  {
    tkExplosion q = sim->tankExplosions;
    while (q != NULL) {
      /* Anchor top-left, matching mapViewDrawShells (the real game's path
         for tank fireballs via screenBullets/SHELL_EXPLOSION1). Centering
         here would offset the head half a tile from the trail explosions
         and make it look like a separate spark traveling past the impact. */
      int tpx = ((int)q->x * tileSize) >> 8;
      int tpy = ((int)q->y * tileSize) >> 8;
      float tx = (float)((tpx - camMX * tileSize) * zf - edgeX + originX);
      float ty = (float)((tpy - camMY * tileSize) * zf - edgeY + originY);

      if (tx + scaledTile >= originX && tx <= originX + viewW &&
          ty + scaledTile >= originY && ty <= originY + viewH) {
        /* Fireball is TANK_EXPLOSION_FRAME (8) which in the real game maps
         * through SHELL_EXPLOSION1 (=8) to the EXPLOSION1 sprite (small spark). */
        SDL_FRect tSrc = { (float)(EXPLOSION1_X * ss), (float)(EXPLOSION1_Y * ss), (float)(tileSize * ss), (float)(tileSize * ss) };
        SDL_FRect tDst = { tx, ty, (float)scaledTile, (float)scaledTile };
        SDL_RenderTexture(ctx->renderer, ctx->tilesTex, &tSrc, &tDst);
      }
      q = q->next;
    }
  }
}
