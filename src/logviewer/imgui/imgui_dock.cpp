/*
 * imgui_dock.cpp - the Log Viewer's right-side dock for its panels
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "imgui_dock.h"
#include "imgui_context.h"
#include "imgui_main_menu.h"
#include "imgui.h"
#include "imgui_internal.h"  /* DockBuilder*, FindWindowByName */
#include "platform_config.h"
#include "../../gui/ui_mode.h"
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
    #include "logviewer.h"
    #include "../backend.h"
    #include "../lv_dock_fit.h"
}

/* How wide the strip on the dock's left edge is that the user drags to make
 * the dock wider or narrower. */
#define LV_DOCK_GRIP_W 6.0f

/* The height the Controls panel gets in the built layout: its tab, the button
 * row, the seek bar and the time row, the same as its floating window. */
#define LV_DOCK_CONTROLS_H 125.0f

/* The most layout text read back from the preferences. A layout of the six
 * panels is under 2 KB; one that does not fit is not this viewer's. */
#define LV_DOCK_LAYOUT_MAX 32768

/* The panels the dock knows, in the order "Reset Window Positions into Dock"
 * stacks them from the top. Each name is the part of the window's title that
 * makes its ID ("Controls###controls" and "###controls" are one window), so it
 * has to match the title the panel passes to Begin.
 *
 * A panel with fixedH takes that many points; the others share what is left
 * by weight. The anchor is the panel left in the dock space's central node,
 * the one that takes up the slack when the window changes height. */
struct LvDockPanel {
    const char *id;
    bool       *show;
    float       fixedH;
    float       weight;
    bool        anchor;
};

static const LvDockPanel k_panels[] = {
    { "###controls",   &lv_g_show_controls_window,       LV_DOCK_CONTROLS_H, 0.0f, false },
    { "###gameinfo",   &lv_g_show_game_info_window,      0.0f,               3.0f, false },
    { "###lvscnpanel", &lv_g_show_scenario_panel_window, 0.0f,               3.0f, false },
    { "###iteminfo",   &lv_g_show_item_info_window,      0.0f,               2.0f, false },
    { "###events",     &lv_g_show_events_window,         0.0f,               5.0f, true  },
    { "###comments",   &lv_g_show_comments_window,       0.0f,               3.0f, false },
};
#define LV_DOCK_PANEL_COUNT ((int)(sizeof(k_panels) / sizeof(k_panels[0])))

static LogViewerState *s_lv = nullptr;

/* The menu option, and whether this host may show the dock at all. Only the
 * standalone viewer says yes; the spectator host shares this code and the
 * same preferences and must stay as it was. */
static bool s_enabled    = false;
static bool s_standalone = false;

/* Whether the dock is in the layout this frame: the option is on AND the
 * viewer is in the mode that has room for it. Everything the rest of the
 * viewer asks (reserved width, is a panel docked) answers from this. */
static bool s_active = false;

static int  s_width = LV_DOCK_WIDTH_DEFAULT;

/* The layout as ImGui settings text. The viewer runs ImGui with no ini file,
 * so this is the only place a layout outlives the dock space: it is taken
 * when the dock goes away or the viewer closes, and put back when the dock
 * comes in. Empty means the dock was never laid out. */
static std::string s_layout_ini;

/* Build the stacked layout the next time the dock draws. */
static bool s_want_layout = false;

/* One bit a panel: it was in the dock when the dock went away and has not yet
 * been sent back to its default floating place. */
static unsigned s_float_reset_mask = 0;

static ImGuiID dock_space_id(void) {
    return ImHashStr("LvRightDock");
}

static int window_width(void) {
    int w = 0, h = 0;
    if (s_lv && s_lv->window) SDL_GetWindowSize(s_lv->window, &w, &h);
    return w;
}

/* The layout text for the context as it is now. */
static void capture_layout(void) {
    size_t len = 0;
    const char *ini = ImGui::SaveIniSettingsToMemory(&len);
    if (ini != nullptr) s_layout_ini.assign(ini, len);
}

void lv_imgui_dock_init(struct LogViewerState *lv) {
    char val[64];

    s_lv = lv;
    s_standalone = false;
    s_active = false;
    s_want_layout = false;
    s_float_reset_mask = 0;

    lv_platform_config_get_string("LOGVIEWER", "Dock.Enabled", "No", val, sizeof(val));
    s_enabled = (val[0] == 'Y' || val[0] == 'y');

    s_width = lv_platform_config_get_int("LOGVIEWER", "Dock.Width", LV_DOCK_WIDTH_DEFAULT);
    if (s_width < LV_DOCK_WIDTH_MIN) s_width = LV_DOCK_WIDTH_MIN;

    /* A value that fills the buffer was cut short; half a layout is worse
     * than none, so it is dropped and the dock lays itself out afresh. */
    s_layout_ini.clear();
    char *buf = (char *)malloc(LV_DOCK_LAYOUT_MAX);
    if (buf != nullptr) {
        lv_platform_config_get_string("LOGVIEWER", "Dock.Layout", "", buf, LV_DOCK_LAYOUT_MAX);
        if (strlen(buf) < (size_t)LV_DOCK_LAYOUT_MAX - 1) s_layout_ini = buf;
        free(buf);
    }
}

void lv_imgui_dock_save(void) {
    /* A host that never shows the dock has nothing of its own to write, and
     * must not write the standalone viewer's layout over with an empty one. */
    if (!s_standalone) return;

    if (s_active && lv_imgui_context_get() != nullptr) {
        ImGui::SetCurrentContext(lv_imgui_context_get());
        capture_layout();
    }
    lv_platform_config_set_string("LOGVIEWER", "Dock.Enabled", s_enabled ? "Yes" : "No");
    lv_platform_config_set_int("LOGVIEWER", "Dock.Width", s_width);
    if (!s_layout_ini.empty() && s_layout_ini.size() < (size_t)LV_DOCK_LAYOUT_MAX - 1) {
        lv_platform_config_set_string("LOGVIEWER", "Dock.Layout", s_layout_ini.c_str());
    }
}

void lv_imgui_dock_set_standalone(int standalone) {
    s_standalone = standalone != 0;
}

int lv_imgui_dock_get_enabled(void) {
    return s_enabled ? 1 : 0;
}

void lv_imgui_dock_set_enabled(int enabled) {
    s_enabled = enabled != 0;
}

void lv_imgui_dock_request_layout(void) {
    if (s_enabled) s_want_layout = true;
}

/* Bring the dock in. The saved layout goes back into the context, which docks
 * the panels it names as they next draw. */
static void dock_activate(void) {
    s_float_reset_mask = 0;
    s_active = true;

    if (s_layout_ini.empty()) {
        /* Never laid out: start with every panel in the dock. */
        s_want_layout = true;
        return;
    }

    /* Loading settings also puts every window the text names back where the
     * text says. That is wanted for the docked panels and at start-up, but a
     * window the user has moved since the text was taken would jump back, so
     * the windows that stay floating keep the place they have now. */
    ImGuiContext &g = *ImGui::GetCurrentContext();
    struct Kept { ImGuiWindow *window; ImVec2 pos; ImVec2 size; };
    ImVector<Kept> kept;
    for (ImGuiWindow *window : g.Windows) {
        Kept k = { window, window->Pos, window->SizeFull };
        kept.push_back(k);
    }

    ImGui::LoadIniSettingsFromMemory(s_layout_ini.c_str(), s_layout_ini.size());

    for (const Kept &k : kept) {
        if (k.window->DockId != 0) continue;
        k.window->Pos = k.pos;
        k.window->Size = k.window->SizeFull = k.size;
    }
}

/* Take the dock away. Its panels float again; the ones that were docked are
 * marked to go back to their default places, because where a slot in the dock
 * was is no place for a floating window. */
static void dock_deactivate(void) {
    capture_layout();

    for (int i = 0; i < LV_DOCK_PANEL_COUNT; i++) {
        ImGuiWindow *window = ImGui::FindWindowByName(k_panels[i].id);
        ImGuiDockNode *node = window ? ImGui::DockBuilderGetNode(window->DockId) : nullptr;
        while (node != nullptr && node->ParentNode != nullptr) node = node->ParentNode;
        if (node != nullptr && node->ID == dock_space_id()) s_float_reset_mask |= 1u << i;
    }

    ImGui::DockBuilderRemoveNode(dock_space_id());
    s_want_layout = false;
    s_active = false;
}

void lv_imgui_dock_pre_frame(void) {
    if (s_lv == nullptr || lv_imgui_context_get() == nullptr) return;

    bool want = s_enabled && s_standalone && !s_lv->gameView &&
                !lv_screenSpecIsLiveMode() &&
                !(uiModeIsTablet() || uiModeIsSteamDeck());
    if (want == s_active) return;

    ImGui::SetCurrentContext(lv_imgui_context_get());
    if (want) dock_activate();
    else      dock_deactivate();
}

int lv_imgui_dock_reserved_width(void) {
    if (!s_active) return 0;
    return lvDockClampWidth(s_width, window_width());
}

bool lv_imgui_dock_take_float_reset(const char *window_id) {
    for (int i = 0; i < LV_DOCK_PANEL_COUNT; i++) {
        if (strcmp(k_panels[i].id, window_id) != 0) continue;
        if ((s_float_reset_mask & (1u << i)) == 0) return false;
        s_float_reset_mask &= ~(1u << i);
        return true;
    }
    return false;
}

bool lv_imgui_dock_window_is_docked(const char *window_id) {
    if (!s_active) return false;
    ImGuiWindow *window = ImGui::FindWindowByName(window_id);
    return window != nullptr && window->DockIsActive;
}

/* Put every panel that is showing into the dock, one above the other, in the
 * order of k_panels. Called inside the host window, just before the dock
 * space is submitted for the frame.
 *
 * The dock space is split once a panel. Splitting upward gives the new top
 * node to the panel and leaves the central node below; splitting downward
 * does the reverse. So the panels above the anchor are peeled off the top in
 * order, the ones below it off the bottom in reverse, and the anchor is
 * docked into what is left, which is the central node. A node keeps the
 * height it is given here; the central node takes whatever remains, then and
 * whenever the window changes height.
 *
 * A panel that is hidden is left out and gets no node, so there is no gap
 * for it. Nothing here forbids splitting or undocking: the user can still
 * drag a panel by its tab to another place in the dock or out of it. */
static void build_layout(ImGuiID dockId, ImVec2 size) {
    int   shown[LV_DOCK_PANEL_COUNT];
    float height[LV_DOCK_PANEL_COUNT];
    int   count = 0;
    int   anchor = -1;
    float fixedSum = 0.0f, weightSum = 0.0f;

    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, size);

    for (int i = 0; i < LV_DOCK_PANEL_COUNT; i++) {
        if (!*k_panels[i].show) continue;
        if (k_panels[i].anchor) anchor = count;
        fixedSum += k_panels[i].fixedH;
        weightSum += k_panels[i].weight;
        shown[count++] = i;
    }
    if (count == 0) {
        ImGui::DockBuilderFinish(dockId);
        return;
    }
    /* With the anchor panel hidden the last one shown stands in for it. */
    if (anchor < 0) anchor = count - 1;

    /* A dock too short for the fixed heights shares everything by weight. */
    float spare = size.y - fixedSum;
    for (int n = 0; n < count; n++) {
        const LvDockPanel &p = k_panels[shown[n]];
        if (spare > 0.0f && p.fixedH > 0.0f) {
            height[n] = p.fixedH;
        } else if (spare > 0.0f && weightSum > 0.0f) {
            height[n] = spare * p.weight / weightSum;
        } else {
            height[n] = size.y / (float)count;
        }
    }

    ImGuiID rest = dockId;
    float   restH = size.y;
    for (int n = 0; n < anchor; n++) {
        ImGuiID at = 0;
        float ratio = (restH > 1.0f) ? height[n] / restH : 0.5f;
        ImGui::DockBuilderSplitNode(rest, ImGuiDir_Up, ratio, &at, &rest);
        ImGui::DockBuilderDockWindow(k_panels[shown[n]].id, at);
        restH -= height[n];
    }
    for (int n = count - 1; n > anchor; n--) {
        ImGuiID at = 0;
        float ratio = (restH > 1.0f) ? height[n] / restH : 0.5f;
        ImGui::DockBuilderSplitNode(rest, ImGuiDir_Down, ratio, &at, &rest);
        ImGui::DockBuilderDockWindow(k_panels[shown[n]].id, at);
        restH -= height[n];
    }
    ImGui::DockBuilderDockWindow(k_panels[shown[anchor]].id, rest);

    ImGui::DockBuilderFinish(dockId);
}

void lv_imgui_dock_host(void) {
    if (!s_active) return;

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    /* The same sum lv_imgui_dock_reserved_width does, so the strip and the
     * part of the window the map leaves alone are one and the same. */
    float w = (float)lvDockClampWidth(s_width, (int)vp->Size.x);
    float right = vp->WorkPos.x + vp->WorkSize.x;

    /* WorkPos / WorkSize are the viewport less the menu bar. */
    ImGui::SetNextWindowPos(ImVec2(right - w, vp->WorkPos.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, vp->WorkSize.y), ImGuiCond_Always);

    /* A bare strip: nothing to grab, move or close, never in front of a
     * floating panel, never itself dockable, and with no settings of its own
     * (its place is worked out every frame). */
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    bool open = ImGui::Begin("###lv_dock_host", nullptr, flags);
    ImGui::PopStyleVar(4);

    if (open) {
        /* The grip down the left edge. Dragging it sets the width from where
         * the mouse is; the clamp keeps the dock between its smallest width
         * and two thirds of the window. The map follows on the next frame. */
        ImVec2 gripPos = ImGui::GetCursorScreenPos();
        float  gripH = ImGui::GetContentRegionAvail().y;
        ImGui::InvisibleButton("##lv_dock_grip", ImVec2(LV_DOCK_GRIP_W, gripH > 1.0f ? gripH : 1.0f));
        bool gripHot = ImGui::IsItemHovered();
        bool gripHeld = ImGui::IsItemActive();
        if (gripHot || gripHeld) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (gripHeld) {
            float wanted = right - ImGui::GetIO().MousePos.x + LV_DOCK_GRIP_W * 0.5f;
            s_width = lvDockClampWidth((int)(wanted + 0.5f), (int)vp->Size.x);
        }
        ImGui::GetWindowDrawList()->AddRectFilled(
            gripPos, ImVec2(gripPos.x + LV_DOCK_GRIP_W, gripPos.y + gripH),
            ImGui::GetColorU32(gripHeld ? ImGuiCol_SeparatorActive
                               : gripHot ? ImGuiCol_SeparatorHovered
                                         : ImGuiCol_Separator));

        ImGui::SameLine(0.0f, 0.0f);

        ImGuiID dockId = dock_space_id();
        ImVec2  avail = ImGui::GetContentRegionAvail();
        if (s_want_layout && avail.x > 1.0f && avail.y > 1.0f) {
            build_layout(dockId, avail);
            s_want_layout = false;
        }
        ImGui::DockSpace(dockId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    }
    ImGui::End();
}
