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
#include <cstdio>
#include <cstring>

#include <SDL3/SDL.h>

extern "C" {
    #include "../lv_name_header.h"
    #include "../lv_team_sprite.h"
    bool lv_drawItemPicture(int isBase, int team, struct SDL_Texture **tex, float *sx, float *sy,
                            float *sw, float *sh);
}

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
    case LV_ICON_CLOSE: {
        const float thick = (h >= 12.0f) ? 2.0f : 1.5f;
        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), col, thick);
        dl->AddLine(ImVec2(x0, y1), ImVec2(x1, y0), col, thick);
        break;
    }
    case LV_ICON_SQUARE:
        dl->AddRectFilled(ImVec2(x0 + 1.0f, y0 + 1.0f), ImVec2(x1 - 1.0f, y1 - 1.0f), col);
        break;
    case LV_ICON_DISC:
        dl->AddCircleFilled(ImVec2(cx, cy), ((w < h) ? w : h) * 0.5f - 0.5f, col);
        break;
    case LV_ICON_PERSON: {
        /* The head, and under it the shoulders: a box with a round top. */
        const float head = h * 0.21f;
        dl->AddCircleFilled(ImVec2(cx, y0 + head + 0.5f), head, col);
        dl->AddRectFilled(ImVec2(cx - w * 0.36f, y0 + h * 0.52f), ImVec2(cx + w * 0.36f, y1),
                          col, w * 0.28f, ImDrawFlags_RoundCornersTop);
        break;
    }
    case LV_ICON_RING: {
        const float r = ((w < h) ? w : h) * 0.5f - 1.5f;
        dl->AddCircle(ImVec2(cx, cy), (r > 1.0f) ? r : 1.0f, col, 0, (h >= 12.0f) ? 2.0f : 1.5f);
        break;
    }
    case LV_ICON_BUBBLE: {
        /* A box with round corners and a tail at the bottom left. */
        const float base = floorf(y1 - h * 0.28f);
        dl->AddRectFilled(ImVec2(x0, y0 + 1.0f), ImVec2(x1, base), col, 2.0f);
        dl->AddTriangleFilled(ImVec2(x0 + w * 0.18f, base - 1.0f), ImVec2(x0 + w * 0.55f, base - 1.0f),
                              ImVec2(x0 + w * 0.18f, y1), col);
        break;
    }
    case LV_ICON_DIAMOND:
        dl->AddQuadFilled(ImVec2(cx, y0), ImVec2(x1, cy), ImVec2(cx, y1), ImVec2(x0, cy), col);
        break;
    case LV_ICON_STAR: {
        /* Ten corners, out and in by turns, as ten triangles from the
         * middle. */
        const float outer = ((w < h) ? w : h) * 0.5f;
        const float inner = outer * 0.42f;
        ImVec2 p[10];
        for (int i = 0; i < 10; i++) {
            const float a = -3.14159265f * 0.5f + (float)i * (3.14159265f / 5.0f);
            const float r = (i % 2 == 0) ? outer : inner;
            p[i] = ImVec2(cx + cosf(a) * r, cy + 0.5f + sinf(a) * r);
        }
        for (int i = 0; i < 10; i++) {
            dl->AddTriangleFilled(ImVec2(cx, cy + 0.5f), p[i], p[(i + 1) % 10], col);
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

bool lv_icon_button(const char *id, int icon, float side, float width) {
    if (side <= 0.0f) {
        side = lv_icon_button_side();
    }
    if (width <= 0.0f) {
        width = side;
    }
    ImGui::PushID(id);
    /* A real button with no label: the frame, the three looks and the
     * keyboard are ImGui's. The icon goes over it. */
    const bool pressed = ImGui::Button("##icon", ImVec2(width, side));
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImGui::PopID();

    /* The icon is a little over half the button's height, in its middle. */
    float box = floorf(side * 0.56f);
    if (box < 4.0f) box = 4.0f;
    const float ox = floorf(p0.x + (p1.x - p0.x - box) * 0.5f);
    const float oy = floorf(p0.y + (p1.y - p0.y - box) * 0.5f);
    lv_icon_draw(ImGui::GetWindowDrawList(), icon, ImVec2(ox, oy), ImVec2(ox + box, oy + box),
                 ImGui::GetColorU32(ImGuiCol_Text));
    return pressed;
}

bool lv_item_picture(ImDrawList *dl, bool isBase, int team, ImVec2 p0, ImVec2 p1, float alpha) {
    SDL_Texture *tex = nullptr;
    float sx = 0.0f, sy = 0.0f, sw = 0.0f, sh = 0.0f, tw = 0.0f, th = 0.0f;
    if (!lv_drawItemPicture(isBase ? 1 : 0, team, &tex, &sx, &sy, &sw, &sh) || tex == nullptr ||
        !SDL_GetTextureSize(tex, &tw, &th) || tw <= 0.0f || th <= 0.0f) {
        return false;
    }
    /* Whole pixels, so the picture is not smeared over two. */
    p0.x = floorf(p0.x);
    p0.y = floorf(p0.y);
    p1.x = floorf(p1.x);
    p1.y = floorf(p1.y);
    /* The size on the display: a point is more than a pixel on a dense one. */
    const float sidePx = (p1.x - p0.x) * ImGui::GetIO().DisplayFramebufferScale.x;
    const bool sharp = lvItemPictureSharp(sidePx);
    const ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
    const ImDrawCallback toSharp = sharp ? pio.DrawCallback_SetSamplerNearest
                                         : pio.DrawCallback_SetSamplerLinear;
    /* The renderer's own setting comes back after the picture. */
    if (toSharp != nullptr) dl->AddCallback(toSharp, nullptr);
    dl->AddImage((ImTextureID)(size_t)tex, p0, p1, ImVec2(sx / tw, sy / th),
                 ImVec2((sx + sw) / tw, (sy + sh) / th),
                 ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, alpha)));
    if (toSharp != nullptr && pio.DrawCallback_SetSamplerLinear != nullptr) {
        dl->AddCallback(pio.DrawCallback_SetSamplerLinear, nullptr);
    }
    return true;
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

/* name cut to at most maxW wide, with "..." for what is gone. Whole UTF-8
 * characters are taken off the end. */
static void icon_cut_name(const char *name, float maxW, char *out, size_t outSize) {
    snprintf(out, outSize, "%s", name);
    if (ImGui::CalcTextSize(out).x <= maxW) {
        return;
    }
    size_t len = strlen(out);
    char tried[256];
    while (len > 0) {
        len--;
        /* Not in the middle of a character. */
        while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80) {
            len--;
        }
        out[len] = '\0';
        snprintf(tried, sizeof(tried), "%s...", out);
        if (ImGui::CalcTextSize(tried).x <= maxW) {
            snprintf(out, outSize, "%s", tried);
            return;
        }
    }
    /* Not even "..." goes in: nothing. */
    out[0] = '\0';
}

bool lv_name_header(const char *id, bool hasStripe, ImU32 stripe, int leadIcon,
                    const char *name, const char *nameTip, const char *closeTip) {
    const ImGuiStyle &style = ImGui::GetStyle();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float lh = ImGui::GetTextLineHeight();
    const float gap = style.ItemInnerSpacing.x;
    const float closeW = floorf(lh);
    float lead = LV_NAME_HEADER_STRIPE_W + gap;
    if (leadIcon >= 0) {
        lead += lv_icon_text_width(leadIcon) + gap;
    }
    if (name == nullptr) name = "";
    const LvNameHeader lay = lvNameHeaderLayout(ImGui::GetContentRegionAvail().x, lead,
                                                ImGui::CalcTextSize(name).x, closeW, gap);

    if (hasStripe) {
        dl->AddRectFilled(ImVec2(top.x, top.y + 1.0f),
                          ImVec2(top.x + LV_NAME_HEADER_STRIPE_W, top.y + lh - 1.0f), stripe, 1.0f);
    }
    ImGui::BeginGroup();
    if (leadIcon >= 0) {
        ImGui::SetCursorScreenPos(ImVec2(top.x + LV_NAME_HEADER_STRIPE_W + gap, top.y));
        lv_icon_text(leadIcon);
    }
    char shown[256];
    if (lay.nameCut) {
        icon_cut_name(name, lay.nameW, shown, sizeof(shown));
    } else {
        snprintf(shown, sizeof(shown), "%s", name);
    }
    ImGui::SetCursorScreenPos(ImVec2(top.x + lay.nameX, top.y));
    ImGui::TextUnformatted(shown);
    ImGui::EndGroup();
    if (nameTip == nullptr && lay.nameCut) nameTip = name;
    if (nameTip != nullptr && nameTip[0] != '\0' &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("%s", nameTip);
    }

    /* The X: a button with no frame until the mouse is over it, so the line
     * reads as a name and not as a row of controls. */
    ImGui::SetCursorScreenPos(ImVec2(top.x + lay.closeX, top.y));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    const bool pressed = lv_icon_button(id, LV_ICON_CLOSE, closeW);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (closeTip != nullptr && closeTip[0] != '\0' &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("%s", closeTip);
    }

    ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + ceilf(ImGui::GetTextLineHeightWithSpacing())));
    return pressed;
}
