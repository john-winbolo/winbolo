/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          ImGui Theme
 *Filename:      imgui_theme.h
 *Purpose:
 *  Shared WinBolo ImGui theme applied after StyleColorsDark().
 *  Call imguiApplyBoloTheme() once per ImGui context,
 *  right after ImGui::StyleColorsDark().
 *********************************************************/

#ifndef IMGUI_THEME_H
#define IMGUI_THEME_H

#include "imgui.h"

static inline void imguiApplyBoloTheme(void) {
    ImGuiStyle &style = ImGui::GetStyle();

    /* --- Rounding --- */
    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 5.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 5.0f;

    /* --- Spacing & padding --- */
    style.WindowPadding  = ImVec2(10, 10);
    style.FramePadding   = ImVec2(8, 4);
    style.ItemSpacing    = ImVec2(8, 6);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.IndentSpacing  = 20.0f;

    /* --- Borders --- */
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize  = 0.0f;
    style.PopupBorderSize  = 1.0f;
    style.SeparatorTextBorderSize = 2.0f;

    /* --- Sizing --- */
    style.ScrollbarSize = 14.0f;
    style.GrabMinSize   = 12.0f;

    /* --- Colors: a refined dark palette with blue-teal accents --- */
    ImVec4 *c = style.Colors;

    /* Backgrounds */
    c[ImGuiCol_WindowBg]      = ImVec4(0.11f, 0.12f, 0.14f, 0.97f);
    c[ImGuiCol_ChildBg]       = ImVec4(0.10f, 0.11f, 0.13f, 0.00f);
    c[ImGuiCol_PopupBg]       = ImVec4(0.10f, 0.11f, 0.13f, 0.96f);

    /* Title bar */
    c[ImGuiCol_TitleBg]          = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    c[ImGuiCol_TitleBgActive]    = ImVec4(0.12f, 0.15f, 0.20f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.08f, 0.09f, 0.11f, 0.60f);

    /* Menu bar */
    c[ImGuiCol_MenuBarBg] = ImVec4(0.13f, 0.14f, 0.16f, 1.00f);

    /* Borders */
    c[ImGuiCol_Border]       = ImVec4(0.22f, 0.24f, 0.28f, 0.60f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    /* Frames (input boxes, checkboxes, etc.) */
    c[ImGuiCol_FrameBg]        = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
    c[ImGuiCol_FrameBgActive]  = ImVec4(0.24f, 0.27f, 0.33f, 1.00f);

    /* Tabs */
    c[ImGuiCol_Tab]                = ImVec4(0.14f, 0.15f, 0.18f, 1.00f);
    c[ImGuiCol_TabHovered]         = ImVec4(0.24f, 0.42f, 0.55f, 0.90f);
    c[ImGuiCol_TabSelected]        = ImVec4(0.19f, 0.35f, 0.48f, 1.00f);
    c[ImGuiCol_TabDimmed]          = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]  = ImVec4(0.15f, 0.22f, 0.30f, 1.00f);

    /* Buttons */
    c[ImGuiCol_Button]        = ImVec4(0.20f, 0.36f, 0.48f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.46f, 0.60f, 1.00f);
    c[ImGuiCol_ButtonActive]  = ImVec4(0.18f, 0.32f, 0.44f, 1.00f);

    /* Headers (collapsing headers, selectable, menu items) */
    c[ImGuiCol_Header]        = ImVec4(0.18f, 0.32f, 0.44f, 0.70f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.42f, 0.55f, 0.80f);
    c[ImGuiCol_HeaderActive]  = ImVec4(0.26f, 0.46f, 0.60f, 1.00f);

    /* Scrollbar */
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.10f, 0.11f, 0.13f, 0.50f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.24f, 0.26f, 0.30f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.33f, 0.38f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.36f, 0.40f, 0.46f, 1.00f);

    /* Slider grab */
    c[ImGuiCol_SliderGrab]       = ImVec4(0.24f, 0.42f, 0.55f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.30f, 0.50f, 0.65f, 1.00f);

    /* Check mark & radio */
    c[ImGuiCol_CheckMark] = ImVec4(0.40f, 0.72f, 0.88f, 1.00f);

    /* Separator */
    c[ImGuiCol_Separator]        = ImVec4(0.22f, 0.24f, 0.28f, 0.60f);
    c[ImGuiCol_SeparatorHovered] = ImVec4(0.24f, 0.42f, 0.55f, 0.80f);
    c[ImGuiCol_SeparatorActive]  = ImVec4(0.30f, 0.50f, 0.65f, 1.00f);

    /* Resize grip */
    c[ImGuiCol_ResizeGrip]        = ImVec4(0.24f, 0.42f, 0.55f, 0.25f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0.24f, 0.42f, 0.55f, 0.65f);
    c[ImGuiCol_ResizeGripActive]  = ImVec4(0.30f, 0.50f, 0.65f, 0.90f);

    /* Text */
    c[ImGuiCol_Text]         = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.47f, 0.50f, 1.00f);

    /* Misc */
    c[ImGuiCol_DockingPreview] = ImVec4(0.24f, 0.42f, 0.55f, 0.70f);
    /* Stock nav cursor is suppressed (alpha 0) so ImGui's unsnapped 2px
       stroke doesn't draw; dialogDrawNavOutline() renders a crisp,
       pixel-snapped replacement in the theme's light blue (0.40, 0.72,
       0.88) instead. */
    c[ImGuiCol_NavHighlight]   = ImVec4(0.40f, 0.72f, 0.88f, 0.00f);
}

#endif /* IMGUI_THEME_H */
