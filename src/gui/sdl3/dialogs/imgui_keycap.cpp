/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "imgui_keycap.h"

#include <cstring>

#include "../tutorial_tokens.h"   /* tutorialKeycapWidth */

void drawProceduralKeycapAt(ImVec2 pos, float h, const char *label) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = pos;
    ImVec2 b = ImVec2(pos.x + h, pos.y + h);
    /* Hardcoded to match Xelu's *_Key_Light.png pack — white cap with
       dark grey ink — so procedural fallback caps sit visually flush
       with the PNG glyphs that surround them. */
    ImU32 bg     = IM_COL32(245, 245, 245, 255);
    ImU32 border = IM_COL32(40, 40, 40, 255);
    float rounding = h * 0.15f;
    dl->AddRectFilled(a, b, bg, rounding);
    dl->AddRect(a, b, border, rounding, 0, 1.5f);
    if (label && *label) {
        /* If the label is wide (e.g. "Left Shift"), draw just its
           first character so it stays inside the cap.  Callers of this
           square cap write the full name beside it (Configure Keys);
           text that names a key only by its cap uses
           drawProceduralKeycapWideAt instead. */
        char buf[8];
        const char *draw = label;
        if ((int)strlen(label) > 2) {
            buf[0] = label[0];
            buf[1] = '\0';
            draw = buf;
        }
        ImVec2 ts = ImGui::CalcTextSize(draw);
        ImVec2 p = ImVec2(a.x + (h - ts.x) * 0.5f,
                          a.y + (h - ts.y) * 0.5f);
        dl->AddText(p, border, draw);
    }
}

float proceduralKeycapWidth(float h, const char *label) {
    float labelW = (label && *label) ? ImGui::CalcTextSize(label).x : 0.0f;
    return tutorialKeycapWidth(h, labelW);
}

void drawProceduralKeycapWideAt(ImVec2 pos, float h, const char *label) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float w = proceduralKeycapWidth(h, label);
    ImVec2 a = pos;
    ImVec2 b = ImVec2(pos.x + w, pos.y + h);
    /* Same colours as drawProceduralKeycapAt (Xelu's light pack). */
    ImU32 bg     = IM_COL32(245, 245, 245, 255);
    ImU32 border = IM_COL32(40, 40, 40, 255);
    float rounding = h * 0.15f;
    dl->AddRectFilled(a, b, bg, rounding);
    dl->AddRect(a, b, border, rounding, 0, 1.5f);
    if (label && *label) {
        ImVec2 ts = ImGui::CalcTextSize(label);
        ImVec2 p = ImVec2(a.x + (w - ts.x) * 0.5f,
                          a.y + (h - ts.y) * 0.5f);
        dl->AddText(p, border, label);
    }
}
