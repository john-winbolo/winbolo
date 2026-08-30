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
 * Name:          imgui_star_rating.cpp
 * Purpose:       Shared 0-10 star strip for WinBolo.net
 *                ratings. Owns the three star textures.
 *********************************************************/

#include <cstdio>   /* FILENAME_MAX */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_dialog_utils.h"
#include "imgui_star_rating.h"

/* ---- Star icon textures ---- */
static SDL_Texture *s_starFull  = nullptr;
static SDL_Texture *s_starHalf  = nullptr;
static SDL_Texture *s_starEmpty = nullptr;
static bool s_starsLoaded = false;
/* Renderer the current textures were built against. */
static SDL_Renderer *s_starsRenderer = nullptr;

void imguiStarRatingLoadIcons(SDL_Renderer *renderer) {
    /* Loaded against this exact renderer already: nothing to do, and a
     * failed load stays failed rather than retrying every frame. If the
     * renderer pointer differs (game→lobby may have recreated it; SDL3
     * textures don't survive that), drop the stale textures and reload. */
    if (s_starsLoaded && s_starsRenderer == renderer) return;
    if (s_starsLoaded && s_starsRenderer != renderer) {
        imguiStarRatingDestroyIcons();
    }
    s_starsLoaded   = true;
    s_starsRenderer = renderer;

    int iconSize = 16;
    const char *paths[][2] = {
        { "data/ui/star_full.svg",  nullptr },
        { "data/ui/star_half.svg",  nullptr },
        { "data/ui/star_empty.svg", nullptr },
    };

    char baseBuf[FILENAME_MAX];
    const char *base = SDL_GetBasePath();
    SDL_Texture **targets[] = { &s_starFull, &s_starHalf, &s_starEmpty };

    for (int i = 0; i < 3; i++) {
        *targets[i] = imguiLoadSvgIcon(renderer, paths[i][0], iconSize);
        if (!*targets[i] && base) {
            SDL_snprintf(baseBuf, sizeof(baseBuf), "%s%s", base, paths[i][0]);
            *targets[i] = imguiLoadSvgIcon(renderer, baseBuf, iconSize);
        }
    }
}

void imguiStarRatingDestroyIcons(void) {
    if (s_starFull)  { SDL_DestroyTexture(s_starFull);  s_starFull = nullptr; }
    if (s_starHalf)  { SDL_DestroyTexture(s_starHalf);  s_starHalf = nullptr; }
    if (s_starEmpty) { SDL_DestroyTexture(s_starEmpty); s_starEmpty = nullptr; }
    s_starsLoaded   = false;
    s_starsRenderer = nullptr;
}

/* ---- Render star rating inline ---- */
void imguiStarRating(float rating10) {
    /* Convert 0-10 scale to 0-5 stars */
    float stars5 = rating10 / 2.0f;
    int full = (int)stars5;
    bool half = (stars5 - (float)full) >= 0.25f;
    int empty = 5 - full - (half ? 1 : 0);

    ImVec2 sz(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight());

    for (int i = 0; i < full; i++) {
        if (s_starFull) {
            ImGui::Image((ImTextureID)s_starFull, sz);
            if (i < full - 1 || half || empty > 0) ImGui::SameLine(0, 0);
        }
    }
    if (half && s_starHalf) {
        ImGui::Image((ImTextureID)s_starHalf, sz);
        if (empty > 0) ImGui::SameLine(0, 0);
    }
    for (int i = 0; i < empty; i++) {
        if (s_starEmpty) {
            ImGui::Image((ImTextureID)s_starEmpty, sz);
            if (i < empty - 1) ImGui::SameLine(0, 0);
        }
    }
}
