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
 * Name:          imgui_star_rating.h
 * Purpose:       Shared 0-10 star strip for WinBolo.net
 *                ratings — drawn by the log browser and by
 *                the lobby's post-round recap.
 *
 *                The three star textures are statics of the
 *                matching .cpp, so the strip lives in one
 *                translation unit rather than inline in this
 *                header: two copies would rasterise the SVGs
 *                twice and each surface would then own a set
 *                the other's teardown could not release.
 *********************************************************/

#ifndef IMGUI_STAR_RATING_H
#define IMGUI_STAR_RATING_H

struct SDL_Renderer;

/* Rasterise the star SVGs against this renderer. Cheap to call every
 * frame: it returns immediately once it has attempted the load, and
 * reloads only when handed a renderer other than the one the current
 * textures were built against. */
void imguiStarRatingLoadIcons(SDL_Renderer *renderer);

/* Release the textures. The next load call rebuilds them. */
void imguiStarRatingDestroyIcons(void);

/* Draw a 0-10 rating as five stars at text height, on the current line.
 * Submits nothing when the textures are missing. */
void imguiStarRating(float rating10);

#endif /* IMGUI_STAR_RATING_H */
