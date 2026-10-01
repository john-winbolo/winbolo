/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "imgui_keycap.h"

#include <cstring>

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
           first character so it stays inside the cap.  The full name
           still appears in the dialog text where the player needs it. */
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
