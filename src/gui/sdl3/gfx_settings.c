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
 * Name:          gfx_settings.c
 * Purpose:
 *   The graphics settings and their accessors.  The four
 *   drawing modes start at 0 - Classic tile detail, Classic
 *   animation, no smooth shells, nearest filtering - which
 *   is what the game drew before any of them existed.  A
 *   value outside its enum is stored as 0 rather than kept,
 *   so a hand-edited prefs file cannot leave the game in a
 *   mode nothing knows how to draw.
 *
 *   The simplified view is a pair of booleans rather than a
 *   mode, and starts on with its sub-option clear.  See
 *   gfx_settings.h for what the pair means.
 *********************************************************/

#include "gfx_settings.h"
#include "skin_source.h"
#include "tileloader.h"

#include "platform_types.h"   /* BOLO_STATIC_ASSERT */

/* tileloader.h spells the same three modes out as TILE_DETAIL_* so the tile
   loader needs no include of this header.  The two sets have to agree. */
BOLO_STATIC_ASSERT((int)GFX_TILE_DETAIL_CLASSIC    == TILE_DETAIL_CLASSIC &&
                   (int)GFX_TILE_DETAIL_MATCH_ZOOM == TILE_DETAIL_MATCH_ZOOM &&
                   (int)GFX_TILE_DETAIL_HIGH       == TILE_DETAIL_HIGH,
                   gfx_tile_detail_matches_tileloader);

/* skin_source.h spells the same three filters out as SKIN_FILTER_*, so a
   skin.ini can name one without skin_source depending on this header.  The
   two sets have to agree. */
BOLO_STATIC_ASSERT((int)GFX_FILTER_NEAREST  == SKIN_FILTER_NEAREST &&
                   (int)GFX_FILTER_LINEAR   == SKIN_FILTER_LINEAR &&
                   (int)GFX_FILTER_PIXELART == SKIN_FILTER_PIXELART,
                   gfx_texture_filter_matches_skin_source);

static GfxTileDetail     s_tileDetail     = GFX_TILE_DETAIL_CLASSIC;
static GfxAnimSmoothness s_animSmoothness = GFX_ANIM_CLASSIC;
static bool              s_smoothShells   = false;
static GfxTextureFilter  s_textureFilter  = GFX_FILTER_NEAREST;
static bool              s_simplifiedZoomOut     = true;
static bool              s_simplifiedOverviewOnly = false;
static FogStyle          s_fogStyle       = FOG_STYLE_GREY;

GfxTileDetail gfxGetTileDetail(void) { return s_tileDetail; }

/* The setters clamp through int: an enum with no negative members can be an
   unsigned type, and comparing one against a negative bound is a warning
   waiting to happen. */
void gfxSetTileDetail(GfxTileDetail v) {
    int n = (int)v;
    if (n < (int)GFX_TILE_DETAIL_CLASSIC || n > (int)GFX_TILE_DETAIL_HIGH) {
        n = (int)GFX_TILE_DETAIL_CLASSIC;
    }
    s_tileDetail = (GfxTileDetail)n;
}

GfxAnimSmoothness gfxGetAnimSmoothness(void) { return s_animSmoothness; }

void gfxSetAnimSmoothness(GfxAnimSmoothness v) {
    int n = (int)v;
    if (n < (int)GFX_ANIM_CLASSIC || n > (int)GFX_ANIM_SMOOTH) {
        n = (int)GFX_ANIM_CLASSIC;
    }
    s_animSmoothness = (GfxAnimSmoothness)n;
}

bool gfxGetSmoothShells(void) { return s_smoothShells; }

void gfxSetSmoothShells(bool v) { s_smoothShells = v; }

GfxTextureFilter gfxGetTextureFilter(void) { return s_textureFilter; }

void gfxSetTextureFilter(GfxTextureFilter v) {
    int n = (int)v;
    if (n < (int)GFX_FILTER_NEAREST || n > (int)GFX_FILTER_PIXELART) {
        n = (int)GFX_FILTER_NEAREST;
    }
    s_textureFilter = (GfxTextureFilter)n;
}

bool gfxGetSimplifiedZoomOut(void) { return s_simplifiedZoomOut; }

void gfxSetSimplifiedZoomOut(bool v) { s_simplifiedZoomOut = v; }

bool gfxGetSimplifiedOverviewOnly(void) { return s_simplifiedOverviewOnly; }

void gfxSetSimplifiedOverviewOnly(bool v) { s_simplifiedOverviewOnly = v; }

FogStyle gfxGetFogStyle(void) { return s_fogStyle; }

void gfxSetFogStyle(FogStyle v) {
    int n = (int)v;
    if (n < (int)FOG_STYLE_GREY || n >= FOG_STYLE_COUNT) {
        n = (int)FOG_STYLE_GREY;
    }
    s_fogStyle = (FogStyle)n;
}
