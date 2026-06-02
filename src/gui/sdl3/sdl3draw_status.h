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
*Filename:      sdl3draw_status.h
*Purpose:
*  Status-panel and message/HUD rendering split out of
*  sdl3draw.c so the standalone log viewer can reuse the
*  exact same SDL renderers as the live game (Phase C of
*  plans/ctrailer.md). Linked into both the live game
*  client and LOGVIEWER_IMGUI_SOURCES.
*
*  The module owns its own copies of the renderer / atlas
*  / fonts / zoom state. The live game's sdl3draw.c keeps
*  its own copies for its other uses; updates flow into
*  this module via the Init/Set* setter API below.
*********************************************************/

#ifndef SDL3DRAW_STATUS_H
#define SDL3DRAW_STATUS_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "global.h"
#include "alliance_enums.h"
#include "screentank.h"

/* -----------------------------------------------------------------
 * Lifecycle / setter API.
 *
 * Call Init once after sdl3DrawSetup builds renderer/atlas/fonts/
 * bar textures.  Push subsequent state changes via the Set* calls
 * (zoom mutations, font reloads, atlas reloads, per-frame edge
 * offsets).  Call Shutdown from sdl3DrawCleanup to destroy this
 * module's owned texture caches and clear all handles.
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
                        SDL_Texture *baseBarsTex);

void sdl3DrawStatusSetZoom(int zoomFactor);

void sdl3DrawStatusSetAtlas(SDL_Texture *atlas, int sheetScale);

void sdl3DrawStatusSetFonts(TTF_Font *fontTiny, TTF_Font *fontMsg,
                            TTF_Font *fontKD,   TTF_Font *fontLabel,
                            TTF_Font *fallbackFontTiny,
                            TTF_Font *fallbackFontMsg,
                            TTF_Font *fallbackFontKD,
                            TTF_Font *fallbackFontLabel);

void sdl3DrawStatusSetEdgeOffset(int edgeX, int edgeY);

void sdl3DrawStatusSetPanelOrigins(float tanksX, float tanksY,
                                   float pillsX, float pillsY,
                                   float basesX, float basesY);

void sdl3DrawStatusShutdown(void);

void sdl3DrawStatusGetCachedTankStats(BYTE *shells, BYTE *mines,
                                      BYTE *armour, BYTE *trees);
void sdl3DrawStatusGetCachedBaseStats(BYTE *shells, BYTE *mines,
                                      BYTE *armour, bool *hasBase);

/* -----------------------------------------------------------------
 * Moved status / message / HUD renderers (bodies unchanged).
 * Signatures match sdl3draw.h prior to Phase C; sdl3draw.h now
 * includes this header so existing callers compile unchanged.
 * ----------------------------------------------------------------- */
void sdl3DrawSetBasesStatusClear(void);
void sdl3DrawStatusBase(BYTE baseNum, baseAlliance ba, bool labels);
void sdl3DrawCopyBasesStatus(int x, int y);

void sdl3DrawSetPillsStatusClear(void);
void sdl3DrawStatusPillbox(BYTE pillNum, pillAlliance pa, bool labels);
void sdl3DrawCopyPillsStatus(int x, int y);

void sdl3DrawSetTanksStatusClear(void);
void sdl3DrawStatusTank(BYTE tankNum, tankAlliance ta);
void sdl3DrawCopyTanksStatus(int x, int y);

void sdl3DrawStatusTankBars(int x, int y,
                            BYTE shells, BYTE mines, BYTE armour, BYTE trees);
void sdl3DrawCopyTankStatusBars(int x, int y);

void sdl3DrawStatusBaseBars(int x, int y,
                            BYTE shells, BYTE mines, BYTE armour, bool redraw);
void sdl3DrawCopyBasesStatusBars(int x, int y);

void sdl3DrawResetCachedText(void);
void sdl3DrawMessages(int x, int y, char *top, char *bottom);
void sdl3DrawGetCachedMessages(const char **top, const char **bottom);
void sdl3DrawKillsDeaths(int x, int y, int kills, int deaths);
void sdl3DrawTankLabel(char *str, BYTE playerNum,
                       BYTE mx, BYTE my, BYTE px, BYTE py);

/* Lifted from static — flushes the message-line + kills/deaths text
 * caches into the current render target. */
void sdl3RenderCachedText(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SDL3DRAW_STATUS_H */
