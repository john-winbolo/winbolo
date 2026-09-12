/*
 * $Id$
 *
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

/* The four amounts and the four caps they are drawn against; a bar fills
   at its cap. The caller supplies both because this file has no sim. */
void sdl3DrawStatusTankBars(int x, int y,
                            BYTE shells, BYTE mines, BYTE armour, BYTE trees,
                            BYTE fullShells, BYTE fullMines,
                            BYTE fullArmour, BYTE fullTrees);
void sdl3DrawCopyTankStatusBars(int x, int y);

void sdl3DrawStatusBaseBars(int x, int y,
                            BYTE shells, BYTE mines, BYTE armour, bool redraw);
void sdl3DrawCopyBasesStatusBars(int x, int y);

/* Render-thread-only rebuilds of the resource-bar textures from the cached
 * values set by sdl3DrawStatus{Tank,Base}Bars. Called each frame from
 * sdl3RenderStatusPanels so the sim-tick thread never touches the GPU. */
void sdl3RenderTankBarsTex(void);
void sdl3RenderBaseBarsTex(void);

/* TRUE if the caller is on the thread that created the renderer. All GPU
 * work must run there; drawing helpers assert on it. */
bool sdl3DrawOnRenderThread(void);

void sdl3DrawResetCachedText(void);
void sdl3DrawMessages(int x, int y, char *top, char *bottom);
void sdl3DrawGetCachedMessages(const char **top, const char **bottom);
/* The newswire's TTF face (13 px times the main window's zoom), NULL until
   the fonts are loaded. Fonts are renderer-independent, so the overview
   hosts hand it to their own tank-label caches (tank_label.h), which build
   textures from it on whichever renderer hosts them — the classic pass's
   cache is the main window's and cannot be shared. The pointer changes
   when a zoom change reopens the fonts; the label caches treat that as a
   flush. */
TTF_Font *sdl3DrawGetMessageFont(void);
/* The classic view's tank-label cache, for the shared overlay pass. */
struct TankLabelCache *sdl3DrawGetTankLabelCache(void);
/* The faces the pill and base numbers are drawn in — the label font for
   pills, the tiny font for bases — opened at the main window's zoom. The
   overview reads them through here and scales what they render to its own
   zoom. */
TTF_Font *sdl3DrawGetLabelFont(void);
TTF_Font *sdl3DrawGetTinyFont(void);
/* When the newswire text last changed (SDL_GetTicks ms), 0 for never since
   the last reset. The full screen map uses it to slide its newswire strip
   on and off; the classic frame ignores it. */
Uint64 sdl3DrawGetMessageActivityTick(void);
void sdl3DrawKillsDeaths(int x, int y, int kills, int deaths);
void sdl3DrawTankLabel(char *str, BYTE playerNum,
                       BYTE mx, BYTE my, BYTE px, BYTE py);

#if defined(WINBOLO_VOICE)
/* Whether sdl3DrawTankLabel draws a microphone icon beside the label
 * of a player whose voice is being heard. Owned here, by the module
 * that reads it, the way the voice module owns the other voice
 * settings; winbolo.c wraps it for the settings dialog and prefs. */
void sdl3DrawStatusSetShowMicIcons(bool on);
bool sdl3DrawStatusGetShowMicIcons(void);
#endif

/* Lifted from static — flushes the message-line + kills/deaths text
 * caches into the current render target. */
void sdl3RenderCachedText(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SDL3DRAW_STATUS_H */
