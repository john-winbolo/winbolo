/*
 * imgui_lv_icons.cpp - the Log Viewer's drawn icons
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "imgui_lv_icons.h"

#include <cmath>

/* A triangle that points right (dir > 0) or left, in the box x0..x1 by
 * y0..y1. */
static void icon_triangle(ImDrawList *dl, float x0, float y0, float x1, float y1,
                          int dir, ImU32 col) {
    const float cy = (y0 + y1) * 0.5f;
    if (dir > 0) {
        dl->AddTriangleFilled(ImVec2(x0, y0), ImVec2(x1, cy), ImVec2(x0, y1), col);
    } else {
        dl->AddTriangleFilled(ImVec2(x1, y0), ImVec2(x1, y1), ImVec2(x0, cy), col);
    }
}

void lv_icon_draw(ImDrawList *dl, int icon, ImVec2 p0, ImVec2 p1, ImU32 col) {
    /* Whole pixels, so an upright edge is one sharp line. */
    const float x0 = floorf(p0.x), y0 = floorf(p0.y);
    const float x1 = floorf(p1.x), y1 = floorf(p1.y);
    const float w = x1 - x0, h = y1 - y0;
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    if (dl == nullptr || w < 2.0f || h < 2.0f) {
        return;
    }

    switch (icon) {
    case LV_ICON_PLAY: {
        /* A little narrower than the box, and a touch to the right: a
         * triangle's weight is at its back, so one drawn edge to edge looks
         * off centre to the left. */
        const float inset = floorf(w * 0.12f);
        icon_triangle(dl, x0 + inset + 1.0f, y0, x1 - inset + 1.0f, y1, +1, col);
        break;
    }
    case LV_ICON_PAUSE: {
        float bar = floorf(w * 0.30f);
        if (bar < 1.0f) bar = 1.0f;
        const float inset = floorf(w * 0.10f);
        dl->AddRectFilled(ImVec2(x0 + inset, y0), ImVec2(x0 + inset + bar, y1), col);
        dl->AddRectFilled(ImVec2(x1 - inset - bar, y0), ImVec2(x1 - inset, y1), col);
        break;
    }
    case LV_ICON_REWIND:
    case LV_ICON_FAST_FORWARD: {
        const int dir = (icon == LV_ICON_FAST_FORWARD) ? +1 : -1;
        const float mid = floorf(cx);
        const float inset = floorf(h * 0.10f);
        icon_triangle(dl, x0, y0 + inset, mid, y1 - inset, dir, col);
        icon_triangle(dl, mid, y0 + inset, x1, y1 - inset, dir, col);
        break;
    }
    case LV_ICON_TO_END: {
        float bar = floorf(w * 0.18f);
        if (bar < 1.0f) bar = 1.0f;
        icon_triangle(dl, x0, y0, x1 - bar - 1.0f, y1, +1, col);
        dl->AddRectFilled(ImVec2(x1 - bar, y0), ImVec2(x1, y1), col);
        break;
    }
    case LV_ICON_EYE: {
        /* Two lids, each one curve from corner to corner, and the pupil.
         * With the control points this far out a lid is about a third of
         * the box high at the middle. */
        const float half = w * 0.5f - 1.0f;
        const float lift = h * 0.50f;
        const float thick = (h >= 18.0f) ? 1.5f : 1.0f;
        ImVec2 left(cx - half, cy), right(cx + half, cy);
        dl->AddBezierCubic(left, ImVec2(cx - half * 0.45f, cy - lift),
                           ImVec2(cx + half * 0.45f, cy - lift), right, col, thick);
        dl->AddBezierCubic(left, ImVec2(cx - half * 0.45f, cy + lift),
                           ImVec2(cx + half * 0.45f, cy + lift), right, col, thick);
        dl->AddCircleFilled(ImVec2(cx, cy), h * 0.17f, col);
        break;
    }
    case LV_ICON_GEAR: {
        /* A ring with eight teeth and a hole: the teeth are short thick
         * strokes out from the ring. */
        const float r = ((w < h) ? w : h) * 0.5f;
        const float ring = r * 0.56f;
        float thick = r * 0.34f;
        if (thick < 1.5f) thick = 1.5f;
        dl->AddCircle(ImVec2(cx, cy), ring, col, 0, thick);
        for (int i = 0; i < 8; i++) {
            const float a = (float)i * (3.14159265f / 4.0f);
            const float dx = cosf(a), dy = sinf(a);
            dl->AddLine(ImVec2(cx + dx * ring, cy + dy * ring),
                        ImVec2(cx + dx * r, cy + dy * r), col, thick);
        }
        break;
    }
    default:
        break;
    }
}

float lv_icon_button_side(void) {
    return floorf(ImGui::GetFrameHeight());
}

bool lv_icon_button(const char *id, int icon, float side) {
    if (side <= 0.0f) {
        side = lv_icon_button_side();
    }
    ImGui::PushID(id);
    /* A real button with no label: the frame, the three looks and the
     * keyboard are ImGui's. The icon goes over it. */
    const bool pressed = ImGui::Button("##icon", ImVec2(side, side));
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImGui::PopID();

    /* The icon is a little over half the button, in its middle. */
    float box = floorf(side * 0.56f);
    if (box < 4.0f) box = 4.0f;
    const float ox = floorf(p0.x + (p1.x - p0.x - box) * 0.5f);
    const float oy = floorf(p0.y + (p1.y - p0.y - box) * 0.5f);
    lv_icon_draw(ImGui::GetWindowDrawList(), icon, ImVec2(ox, oy), ImVec2(ox + box, oy + box),
                 ImGui::GetColorU32(ImGuiCol_Text));
    return pressed;
}

float lv_icon_text_width(int icon) {
    const float h = ImGui::GetTextLineHeight();
    /* An eye is wider than it is high; the others are square. */
    return floorf(h * ((icon == LV_ICON_EYE) ? 1.35f : 1.0f));
}

void lv_icon_text(int icon) {
    const float h = ImGui::GetTextLineHeight();
    const float w = lv_icon_text_width(icon);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    lv_icon_draw(ImGui::GetWindowDrawList(), icon, p, ImVec2(p.x + w, p.y + h),
                 ImGui::GetColorU32(ImGuiCol_Text));
    ImGui::Dummy(ImVec2(w, h));
}
