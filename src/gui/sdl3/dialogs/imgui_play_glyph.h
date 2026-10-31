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
 * Name:          imgui_play_glyph.h
 * Purpose:       The play triangle, drawn with the draw list:
 *                the one drawing that the Log Viewer's play and
 *                pause button (lv_icon_draw, LV_ICON_PLAY) and
 *                the Preview clip button (clipGifDrawGlyph) both
 *                use. Header only, so every target that draws
 *                either button has it, the mobile clients too.
 *********************************************************/

#ifndef IMGUI_PLAY_GLYPH_H
#define IMGUI_PLAY_GLYPH_H

#include <math.h>

#include "imgui.h"

/* The play triangle in the box p0..p1, in colour col. The box is put on
 * whole pixels, so the upright edge is one sharp line. The triangle is a
 * little narrower than the box and a touch to the right: a triangle's
 * weight is at its back, so one drawn edge to edge looks off centre to the
 * left. */
static inline void imguiDrawPlayGlyph(ImDrawList *dl, ImVec2 p0, ImVec2 p1,
                                      ImU32 col) {
    const float x0 = floorf(p0.x), y0 = floorf(p0.y);
    const float x1 = floorf(p1.x), y1 = floorf(p1.y);
    const float w = x1 - x0;
    if (dl == nullptr || w < 2.0f || y1 - y0 < 2.0f) {
        return;
    }
    const float inset = floorf(w * 0.12f);
    const float l = x0 + inset + 1.0f;
    const float r = x1 - inset + 1.0f;
    const float cy = (y0 + y1) * 0.5f;
    dl->AddTriangleFilled(ImVec2(l, y0), ImVec2(r, cy), ImVec2(l, y1), col);
}

#endif /* IMGUI_PLAY_GLYPH_H */
