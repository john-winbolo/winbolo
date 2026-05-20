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

#include "dialog_footer.h"
#include "imgui_dialog_utils.h"

#include <algorithm>

namespace WBUI {

/* Cancel button palette — muted slate, low contrast against the dark
 * theme so the eye lands on the primary (Confirm) action first.
 * Hardcoded here for the initial pass; can be promoted to wb_theme
 * if a Light theme later needs to override. */
static const ImVec4 kCancelBg       = ImVec4(0.25f, 0.25f, 0.28f, 1.0f);
static const ImVec4 kCancelHovered  = ImVec4(0.35f, 0.35f, 0.38f, 1.0f);
static const ImVec4 kCancelActive   = ImVec4(0.20f, 0.20f, 0.22f, 1.0f);

/* Minimum button width so a one-character label like "OK" doesn't
 * collapse to a tiny square inside the centered cluster. */
static const float kMinButtonWidth = 90.0f;

void PushCancelStyle() {
    ImGui::PushStyleColor(ImGuiCol_Button,        kCancelBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kCancelHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kCancelActive);
}

void PopCancelStyle() {
    ImGui::PopStyleColor(3);
}

bool DrawPanelCloseX() {
    /* Sized off the font so it scales with the dialog. Roughly the
     * height of one line of text, square. */
    float sz = ImGui::GetFontSize() * 1.4f;
    float pad = ImGui::GetStyle().WindowPadding.x;

    /* Anchor top-right of the window. GetWindowPos is the window's
     * top-left in screen space. */
    ImVec2 winPos  = ImGui::GetWindowPos();
    ImVec2 winSize = ImGui::GetWindowSize();
    ImVec2 btnPos  = ImVec2(winPos.x + winSize.x - sz - pad,
                            winPos.y + pad);

    /* Anchor an invisible button at top-right via cursor positioning,
     * relative to window content area. We save/restore the cursor so
     * subsequent widgets in the body draw normally. */
    ImVec2 savedCursor = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(winSize.x - sz - pad, pad));

    ImGui::PushID("##wb_panel_close_x");
    /* Keep the close X out of keyboard nav — otherwise on first frame
     * ImGui auto-focuses the first nav widget, which would be this X,
     * and Enter could activate it instead of going to the panel's
     * intended input field. */
    ImGui::PushTabStop(false);
    /* Transparent button — we'll draw the X glyph ourselves so we can
     * control its color/weight independently of the button background. */
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.35f, 0.38f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.20f, 0.20f, 0.22f, 1.0f));
    bool clicked = ImGui::Button("##close", ImVec2(sz, sz));
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopStyleColor(3);
    ImGui::PopTabStop();
    ImGui::PopID();

    /* Draw the X glyph on top of the button. Brighter on hover so the
     * affordance is obvious; muted otherwise to read as secondary. */
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 col = hovered ? IM_COL32(230, 230, 235, 255)
                        : IM_COL32(170, 170, 180, 255);
    float inset = sz * 0.28f;
    ImVec2 a = ImVec2(btnPos.x + inset,        btnPos.y + inset);
    ImVec2 b = ImVec2(btnPos.x + sz - inset,   btnPos.y + sz - inset);
    ImVec2 c = ImVec2(btnPos.x + sz - inset,   btnPos.y + inset);
    ImVec2 d = ImVec2(btnPos.x + inset,        btnPos.y + sz - inset);
    dl->AddLine(a, b, col, 1.5f);
    dl->AddLine(c, d, col, 1.5f);

    ImGui::SetCursorPos(savedCursor);
    return clicked;
}

/* Note: we intentionally don't gate these on IsPopupOpen — ImGui already
 * blocks key/click input to parent windows while a modal popup is open,
 * and an IsPopupOpen check would also disable us when *we* are being
 * called from inside a popup body (the popup containing us would count
 * as "open"). */
bool CancelKeyPressed() {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) return true;
    if (ImGui::IsKeyPressed(ImGuiKey_W) && IMGUI_PRIMARY_KEY_DOWN()) return true;
#ifdef __APPLE__
    if (ImGui::IsKeyPressed(ImGuiKey_Period) && ImGui::GetIO().KeySuper) return true;
#endif
    return false;
}

static bool confirmKeyPressed() {
    return ImGui::IsKeyPressed(ImGuiKey_Enter) ||
           ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
}

/* Compute button width that fits the widest label, with min clamp.
 * labels[] may contain NULLs (skipped). */
static float computeButtonWidth(const char *const labels[], int count) {
    float widest = 0.0f;
    for (int i = 0; i < count; ++i) {
        if (!labels[i]) continue;
        ImVec2 sz = ImGui::CalcTextSize(labels[i]);
        if (sz.x > widest) widest = sz.x;
    }
    float padded = widest + ImGui::GetStyle().FramePadding.x * 2.0f;
    float minW = kMinButtonWidth * (ImGui::GetFontSize() / 16.0f);
    return std::max(padded, minW);
}

static void drawSeparatorAbove() {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
}

int DialogFooter(const char *cancelLabel,
                 const char *confirmLabel,
                 bool enterConfirms,
                 bool showSeparator) {
    if (showSeparator) drawSeparatorAbove();

    int result = FOOTER_NONE;

    const char *labels[2] = { cancelLabel, confirmLabel };
    float btnW = computeButtonWidth(labels, 2);
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float availW = ImGui::GetContentRegionAvail().x;

    int btnCount = (cancelLabel ? 1 : 0) + (confirmLabel ? 1 : 0);
    float totalW = btnW * btnCount + spacing * (btnCount - 1);
    float startX = ImGui::GetCursorPosX() + (availW - totalW) * 0.5f;
    if (startX < ImGui::GetCursorPosX()) startX = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(startX);

    if (cancelLabel) {
        PushCancelStyle();
        if (ImGui::Button(cancelLabel, ImVec2(btnW, 0))) {
            result = FOOTER_CANCEL;
        }
        PopCancelStyle();
        imguiHandOnHover();
        if (confirmLabel) ImGui::SameLine(0.0f, spacing);
    }

    if (confirmLabel) {
        if (ImGui::Button(confirmLabel, ImVec2(btnW, 0))) {
            result = FOOTER_CONFIRM;
        }
        imguiHandOnHover();
    }

    /* Key bindings. Esc/Ctrl+W/Cmd+. always cancel (even on OK-only
     * dialogs — caller treats that as dismiss). Enter confirms only
     * when the caller has indicated the dialog has a text input. */
    if (result == FOOTER_NONE && CancelKeyPressed()) {
        result = FOOTER_CANCEL;
    }
    if (result == FOOTER_NONE && enterConfirms && confirmKeyPressed()) {
        result = FOOTER_CONFIRM;
    }

    return result;
}

int DialogFooter3(const char *cancelLabel,
                  const char *destructiveLabel,
                  const char *primaryLabel,
                  bool enterConfirms,
                  bool showSeparator) {
    if (showSeparator) drawSeparatorAbove();

    int result = FOOTER_NONE;

    const char *labels[3] = { cancelLabel, destructiveLabel, primaryLabel };
    float btnW = computeButtonWidth(labels, 3);
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float availW = ImGui::GetContentRegionAvail().x;

    float totalW = btnW * 3.0f + spacing * 2.0f;
    float startX = ImGui::GetCursorPosX() + (availW - totalW) * 0.5f;
    if (startX < ImGui::GetCursorPosX()) startX = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(startX);

    PushCancelStyle();
    if (ImGui::Button(cancelLabel, ImVec2(btnW, 0))) {
        result = FOOTER_CANCEL;
    }
    PopCancelStyle();
    imguiHandOnHover();

    ImGui::SameLine(0.0f, spacing);
    if (ImGui::Button(destructiveLabel, ImVec2(btnW, 0))) {
        result = FOOTER_DESTRUCTIVE;
    }
    imguiHandOnHover();

    ImGui::SameLine(0.0f, spacing);
    if (ImGui::Button(primaryLabel, ImVec2(btnW, 0))) {
        result = FOOTER_CONFIRM;
    }
    imguiHandOnHover();

    if (result == FOOTER_NONE && CancelKeyPressed()) {
        result = FOOTER_CANCEL;
    }
    if (result == FOOTER_NONE && enterConfirms && confirmKeyPressed()) {
        result = FOOTER_CONFIRM;
    }

    return result;
}

}  /* namespace WBUI */
