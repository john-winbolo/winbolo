/*
 * imgui_dock.cpp - the Log Viewer's sidebars: the docks its panels go in
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
#include "imgui_hud.h"
#include "imgui_overview.h"
#include "imgui.h"
#include "imgui_internal.h"  /* DockBuilder*, FindWindowByName */
#include "platform_config.h"
#include "../../gui/lang.h"
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
    #include "../lv_dock_layout.h"
}

/* How thick the strip on a sidebar's inner edge is that the user drags to
 * make the sidebar bigger or smaller. */
#define LV_DOCK_GRIP_W 6.0f

/* The height the Controls panel gets in the built layout: its tab, the button
 * row, the seek bar and the time row, the same as its floating window. */
#define LV_DOCK_CONTROLS_H 125.0f

/* The most ImGui settings text read back from the preferences. The settings
 * of the panels and four sidebars are a few KB; text that does not fit
 * is not this viewer's. */
#define LV_DOCK_LAYOUT_MAX 32768

/* The panels the sidebars know, in the order "Reset Window Positions into
 * Dock" stacks them from the top of the right sidebar. Each name is the part of the window's title that
 * makes its ID ("Controls###controls" and "###controls" are one window), so it
 * has to match the title the panel passes to Begin.
 *
 * A panel with fixedH takes that many points; the others share what is left
 * by weight. A panel with heightFor has a fixed height too, but one that
 * depends on how wide the dock is, so it is asked for when the layout is
 * built. The anchor is the panel left in the dock space's central node, the
 * one that takes up the slack when the window changes height. */
struct LvDockPanel {
    const char *id;
    bool       *show;
    float       fixedH;
    float       weight;
    bool        anchor;
    float     (*heightFor)(float dockWidth);
};

/* The HUD is second, under the controls: its grids are a fixed size, so it
 * gets just the height they need (one row of three in a wide dock, three
 * rows in a narrow one) and the lists below it share the rest. A weight
 * would hand it room it can only fill by drawing its icons larger. The
 * Overview panel under it is a table of a few rows, and gets the height of
 * its rows the same way. */
static const LvDockPanel k_panels[] = {
    { "###controls",   &lv_g_show_controls_window,       LV_DOCK_CONTROLS_H, 0.0f, false, nullptr },
    { "###hud",        &lv_g_show_hud_window,            0.0f,               0.0f, false, lv_imgui_hud_dock_height },
    { "###overview",   &lv_g_show_overview_window,       0.0f,               0.0f, false, lv_imgui_overview_dock_height },
    { "###gameinfo",   &lv_g_show_game_info_window,      0.0f,               3.0f, false, nullptr },
    { "###lvscnpanel", &lv_g_show_scenario_panel_window, 0.0f,               3.0f, false, nullptr },
    { "###iteminfo",   &lv_g_show_item_info_window,      0.0f,               2.0f, false, nullptr },
    { "###events",     &lv_g_show_events_window,         0.0f,               5.0f, true,  nullptr },
    { "###comments",   &lv_g_show_comments_window,       0.0f,               3.0f, false, nullptr },
};
#define LV_DOCK_PANEL_COUNT ((int)(sizeof(k_panels) / sizeof(k_panels[0])))

static LogViewerState *s_lv = nullptr;

/* One sidebar. The names are ImGui's: spaceName makes the dock space's ID and
 * hostName the strip's window. The right sidebar keeps the names it had when
 * it was the only one. key is the side's part of the preference names. */
struct LvDockSide {
    const char *spaceName;
    const char *hostName;
    const char *key;
    /* The menu option, and the size asked for: a width for the left and
     * right sidebars, a height for the top and bottom ones. */
    bool        enabled;
    int         size;
    /* Whether the sidebar is in the layout this frame: the option is on AND
     * the viewer is in the mode that has room for sidebars. */
    bool        active;
    /* The arrangement the sidebar had when it last went away (lv_dock_layout.h
     * text), put back when it next comes in. Empty while the sidebar shows:
     * its arrangement is then in ImGui's settings. */
    std::string tree;
    /* Put the arrangement back the next time the sidebar draws. */
    bool        wantRebuild;
    /* The user is dragging the sidebar's edge. */
    bool        gripHeld;
};

static LvDockSide s_side[LV_DOCK_SIDES] = {
    { "LvRightDock",  "###lv_dock_host",        "Right",  false, LV_DOCK_WIDTH_DEFAULT,  false, "", false, false },
    { "LvLeftDock",   "###lv_dock_host_left",   "Left",   false, LV_DOCK_WIDTH_DEFAULT,  false, "", false, false },
    { "LvTopDock",    "###lv_dock_host_top",    "Top",    false, LV_DOCK_HEIGHT_DEFAULT, false, "", false, false },
    { "LvBottomDock", "###lv_dock_host_bottom", "Bottom", false, LV_DOCK_HEIGHT_DEFAULT, false, "", false, false },
};

/* Whether this host may show sidebars at all. Only the standalone viewer
 * says yes; the spectator host shares this code and the same preferences and
 * must stay as it was. */
static bool s_standalone = false;

/* ImGui's settings text: where every floating panel is and how every sidebar
 * that shows is arranged. The viewer runs ImGui with no ini file, so the
 * preferences are the only place this outlives the context: it is taken when
 * the settings change and when the viewer closes, and put back before the
 * first frame of the next run. */
static std::string s_ini;

/* The settings text came from the key the right-side dock used before there
 * were four sidebars. */
static bool s_ini_is_legacy = false;

/* The settings text has been put into the context (or there was none). */
static bool s_booted = false;

/* The right sidebar has had its first layout, with every panel in it. */
static bool s_right_laid_out = false;

/* Build the stacked layout in the right sidebar the next time it draws. */
static bool s_want_layout = false;

/* One bit a panel: it was in a sidebar when the sidebar went away and has not
 * yet been sent back to its default floating place. */
static unsigned s_float_reset_mask = 0;

/* What lv_imgui_dock_autosave_due last saw, to tell a change from no change,
 * and when it first saw the change it has not yet reported. */
static unsigned s_sig_saved = 0;
static unsigned s_sig_pending = 0;
static Uint64   s_sig_pending_at = 0;

static ImGuiID dock_space_id(int side) {
    return ImHashStr(s_side[side].spaceName);
}

static bool any_active(void) {
    for (int i = 0; i < LV_DOCK_SIDES; i++) {
        if (s_side[i].active) return true;
    }
    return false;
}

/* The part of the window each sidebar that shows takes. The one sum for the
 * strips this file draws and for the map, the mouse and everything else that
 * asks lv_imgui_dock_margins, so the two never disagree. */
static LvDockMargins current_margins(int *outW, int *outAreaH) {
    int w = 0, h = 0, menuH = 0;
    int wanted[LV_DOCK_SIDES], showing[LV_DOCK_SIDES];

    if (s_lv && s_lv->window) SDL_GetWindowSize(s_lv->window, &w, &h);
    if (any_active()) menuH = (int)lv_imgui_get_menu_bar_height();
    for (int i = 0; i < LV_DOCK_SIDES; i++) {
        wanted[i] = s_side[i].size;
        showing[i] = s_side[i].active ? 1 : 0;
    }
    if (outW) *outW = w;
    if (outAreaH) *outAreaH = h - menuH;
    return lvDockFitMargins(wanted, showing, w, h - menuH);
}

/* Which k_panels entry a window is, by the part of its name that makes its
 * ID, or -1. The name may be a window's whole title ("Controls###controls"),
 * the ID part ("###controls") or that part as ImGui keeps it in a window's
 * settings, with the marker taken off ("controls"). */
static int panel_index(const char *name) {
    const char *id = strstr(name, "###");
    id = (id != nullptr) ? id + 3 : name;
    for (int i = 0; i < LV_DOCK_PANEL_COUNT; i++) {
        if (strcmp(k_panels[i].id + 3, id) == 0) return i;
    }
    return -1;
}

/* The settings text for the context as it is now. Taking it also brings
 * ImGui's record of each window (where it is docked, its tab's place) up to
 * date, which side_to_tree reads. */
static void capture_ini(void) {
    size_t len = 0;
    const char *ini = ImGui::SaveIniSettingsToMemory(&len);
    if (ini != nullptr) s_ini.assign(ini, len);
    ImGui::GetIO().WantSaveIniSettings = false;
}

void lv_imgui_dock_init(struct LogViewerState *lv) {
    char val[128];
    LvDockSides sides;

    s_lv = lv;
    s_standalone = false;
    s_booted = false;
    s_want_layout = false;
    s_float_reset_mask = 0;
    s_ini.clear();
    s_ini_is_legacy = false;
    for (int i = 0; i < LV_DOCK_SIDES; i++) {
        s_side[i].active = false;
        s_side[i].wantRebuild = false;
        s_side[i].gripHeld = false;
        s_side[i].tree.clear();
    }

    lv_platform_config_get_string("LOGVIEWER", "Dock.Sides", "", val, sizeof(val));
    bool have_sides = lvDockSidesParse(val, &sides) != 0;
    if (!have_sides) {
        /* Preferences from before there were four sidebars: the right one's
         * option and width under their own names. */
        lv_platform_config_get_string("LOGVIEWER", "Dock.Enabled", "No", val, sizeof(val));
        sides.enabled[LV_DOCK_RIGHT] = (val[0] == 'Y' || val[0] == 'y');
        sides.size[LV_DOCK_RIGHT] = lvDockSidesClampSize(
            LV_DOCK_RIGHT,
            lv_platform_config_get_int("LOGVIEWER", "Dock.Width", LV_DOCK_WIDTH_DEFAULT));
    }
    for (int i = 0; i < LV_DOCK_SIDES; i++) {
        s_side[i].enabled = sides.enabled[i] != 0;
        s_side[i].size = sides.size[i];
    }
    s_right_laid_out = sides.rightLaidOut != 0;

    /* A value that fills the buffer was cut short; half the settings are
     * worse than none, so they are dropped and every panel starts at its
     * default place. */
    char *buf = (char *)malloc(LV_DOCK_LAYOUT_MAX);
    if (buf != nullptr) {
        lv_platform_config_get_string("LOGVIEWER", have_sides ? "Dock.Ini" : "Dock.Layout",
                                      "", buf, LV_DOCK_LAYOUT_MAX);
        if (strlen(buf) < (size_t)LV_DOCK_LAYOUT_MAX - 1) s_ini = buf;
        free(buf);
    }
    if (!have_sides && !s_ini.empty()) {
        /* The old key held text only once the dock had been laid out. */
        s_ini_is_legacy = true;
        s_right_laid_out = true;
    }

    if (have_sides) {
        static LvDockTree parsed;
        char tree[LV_DOCK_TREE_TEXT_MAX];
        char key[64];
        for (int i = 0; i < LV_DOCK_SIDES; i++) {
            snprintf(key, sizeof(key), "Dock.Tree.%s", s_side[i].key);
            lv_platform_config_get_string("LOGVIEWER", key, "", tree, sizeof(tree));
            /* Kept only if it reads; a damaged one is the same as none. */
            if (lvDockTreeParse(tree, &parsed)) s_side[i].tree = tree;
        }
    }
}

void lv_imgui_dock_save(void) {
    /* A host that never shows sidebars has nothing of its own to write, and
     * must not write the standalone viewer's layout over with an empty one.
     * Nor is there anything to write before the saved settings were put into
     * the context: what is in the preferences is still the truth. */
    if (!s_standalone || !s_booted) return;

    if (lv_imgui_context_get() != nullptr) {
        ImGui::SetCurrentContext(lv_imgui_context_get());
        capture_ini();
    }

    LvDockSides sides;
    char line[128];
    char key[64];
    for (int i = 0; i < LV_DOCK_SIDES; i++) {
        sides.enabled[i] = s_side[i].enabled ? 1 : 0;
        sides.size[i] = s_side[i].size;
    }
    sides.rightLaidOut = s_right_laid_out ? 1 : 0;
    if (lvDockSidesFormat(&sides, line, sizeof(line)) > 0) {
        lv_platform_config_set_string("LOGVIEWER", "Dock.Sides", line);
    }
    /* The right sidebar's option and width also under the names they had, so
     * a viewer from before the four sidebars still finds them. */
    lv_platform_config_set_string("LOGVIEWER", "Dock.Enabled",
                                  s_side[LV_DOCK_RIGHT].enabled ? "Yes" : "No");
    lv_platform_config_set_int("LOGVIEWER", "Dock.Width", s_side[LV_DOCK_RIGHT].size);

    if (s_ini.size() < (size_t)LV_DOCK_LAYOUT_MAX - 1) {
        lv_platform_config_set_string("LOGVIEWER", "Dock.Ini", s_ini.c_str());
    }
    for (int i = 0; i < LV_DOCK_SIDES; i++) {
        snprintf(key, sizeof(key), "Dock.Tree.%s", s_side[i].key);
        lv_platform_config_set_string("LOGVIEWER", key,
                                      s_side[i].active ? "" : s_side[i].tree.c_str());
    }
}

void lv_imgui_dock_set_standalone(int standalone) {
    s_standalone = standalone != 0;
}

int lv_imgui_dock_get_side_enabled(int side) {
    if (side < 0 || side >= LV_DOCK_SIDES) return 0;
    return s_side[side].enabled ? 1 : 0;
}

void lv_imgui_dock_set_side_enabled(int side, int enabled) {
    if (side < 0 || side >= LV_DOCK_SIDES) return;
    s_side[side].enabled = enabled != 0;
}

int lv_imgui_dock_get_enabled(void) {
    return lv_imgui_dock_get_side_enabled(LV_DOCK_RIGHT);
}

void lv_imgui_dock_set_enabled(int enabled) {
    lv_imgui_dock_set_side_enabled(LV_DOCK_RIGHT, enabled);
}

void lv_imgui_dock_request_layout(void) {
    /* The layout goes in the right sidebar, so asking for it turns that on. */
    s_side[LV_DOCK_RIGHT].enabled = true;
    s_want_layout = true;
}

/* ---- A sidebar's arrangement, out of ImGui and back ------------------------ */

/* Add node and what is under it to the tree. Returns the new part's index,
 * or -1 when the tree cannot hold it. */
static int node_to_tree(ImGuiDockNode *node, LvDockTree *tree, int depth) {
    ImGuiContext &g = *ImGui::GetCurrentContext();

    if (node == nullptr || depth > LV_DOCK_TREE_DEPTH) return -1;
    int idx = lvDockTreeAdd(tree);
    if (idx < 0) return -1;

    if (node->IsSplitNode()) {
        int   axis = (node->SplitAxis == ImGuiAxis_Y) ? 1 : 0;
        float a = node->ChildNodes[0]->SizeRef[axis];
        float b = node->ChildNodes[1]->SizeRef[axis];
        if (a + b <= 0.0f) {
            a = node->ChildNodes[0]->Size[axis];
            b = node->ChildNodes[1]->Size[axis];
        }
        int share = (a + b > 0.0f) ? (int)(a * 1000.0f / (a + b) + 0.5f) : 500;
        if (share < 1) share = 1;
        if (share > 999) share = 999;
        tree->node[idx].axis = axis ? 'Y' : 'X';
        tree->node[idx].permille = share;
        for (int k = 0; k < 2; k++) {
            int child = node_to_tree(node->ChildNodes[k], tree, depth + 1);
            if (child < 0) return -1;
            tree->node[idx].child[k] = child;
        }
        return idx;
    }

    /* The panels in this part, in tab order. Read from ImGui's record of
     * each window and not from the node's own list: the record also has a
     * panel that is hidden just now, and it is all there is before the
     * first frame, when no window has been made yet. */
    struct Tab { const char *id; short order; ImGuiID window; };
    Tab tabs[LV_DOCK_TREE_WINS];
    int count = 0;
    for (ImGuiWindowSettings *ws = g.SettingsWindows.begin(); ws != nullptr;
         ws = g.SettingsWindows.next_chunk(ws)) {
        if (ws->DockId != node->ID) continue;
        int panel = panel_index(ws->GetName());
        if (panel < 0 || count >= LV_DOCK_TREE_WINS) continue;
        if (strlen(k_panels[panel].id) >= LV_DOCK_TREE_NAME) continue;
        int at = count++;
        /* A tab with no place yet (-1) goes after the ones that have one. */
        short order = (ws->DockOrder < 0) ? (short)0x7FFF : ws->DockOrder;
        while (at > 0 && tabs[at - 1].order > order) {
            tabs[at] = tabs[at - 1];
            at--;
        }
        tabs[at].id = k_panels[panel].id;
        tabs[at].order = order;
        tabs[at].window = ws->ID;
    }
    LvDockTreeNode &leaf = tree->node[idx];
    leaf.central = node->IsCentralNode() ? 1 : 0;
    leaf.winCount = count;
    for (int n = 0; n < count; n++) {
        snprintf(leaf.win[n], sizeof(leaf.win[n]), "%s", tabs[n].id);
        /* A window's tab is "#TAB" in the window's own ID space. */
        if (node->SelectedTabId != 0 &&
            ImHashStr("#TAB", 0, tabs[n].window) == node->SelectedTabId) {
            leaf.selected = n;
        }
    }
    return idx;
}

/* The arrangement of a sidebar as text, or empty when it holds no panel. */
static std::string side_to_tree(int side) {
    static LvDockTree tree;
    char text[LV_DOCK_TREE_TEXT_MAX];

    ImGuiDockNode *root = ImGui::DockBuilderGetNode(dock_space_id(side));
    tree.count = 0;
    if (root == nullptr || node_to_tree(root, &tree, 0) != 0) return std::string();
    if (lvDockTreeWindowCount(&tree) == 0) return std::string();
    if (lvDockTreeFormat(&tree, text, sizeof(text)) <= 0) return std::string();
    return std::string(text);
}

/* True when the panel is not docked anywhere, so a sidebar that comes back
 * may take it. A panel the user has since put in another sidebar stays. */
static bool panel_is_free(const char *id) {
    ImGuiID window_id = ImHashStr(id);
    if (ImGuiWindow *window = ImGui::FindWindowByID(window_id)) return window->DockId == 0;
    if (ImGuiWindowSettings *ws = ImGui::FindWindowSettingsByID(window_id)) return ws->DockId == 0;
    return true;
}

/* Build the part idx of the tree in the dock node at. Called inside the host
 * window, between DockBuilderAddNode and DockBuilderFinish. */
static void tree_to_node(const LvDockTree *tree, int idx, ImGuiID at, int depth) {
    if (idx < 0 || idx >= tree->count || depth > LV_DOCK_TREE_DEPTH) return;
    const LvDockTreeNode &n = tree->node[idx];

    if (n.axis != 0) {
        /* DockBuilderSplitNode gives the first ID it returns to the new part
         * on the side asked for, and leaves the central node in the other.
         * So the part asked for is the one WITHOUT the central node. */
        bool    first_keeps_central = lvDockTreeHasCentral(tree, n.child[0], depth + 1) != 0;
        float   share = (float)n.permille / 1000.0f;
        ImGuiID first = 0, second = 0;
        if (first_keeps_central) {
            ImGui::DockBuilderSplitNode(at, n.axis == 'X' ? ImGuiDir_Right : ImGuiDir_Down,
                                        1.0f - share, &second, &first);
        } else {
            ImGui::DockBuilderSplitNode(at, n.axis == 'X' ? ImGuiDir_Left : ImGuiDir_Up,
                                        share, &first, &second);
        }
        tree_to_node(tree, n.child[0], first, depth + 1);
        tree_to_node(tree, n.child[1], second, depth + 1);
        return;
    }

    short order = 0;
    for (int k = 0; k < n.winCount; k++) {
        /* A name that is not one of the viewer's panels is from a damaged
         * text or a panel that is gone: left out. */
        int panel = panel_index(n.win[k]);
        if (panel < 0 || !panel_is_free(k_panels[panel].id)) continue;
        ImGui::DockBuilderDockWindow(k_panels[panel].id, at);
        ImGuiID window_id = ImHashStr(k_panels[panel].id);
        if (ImGuiWindow *window = ImGui::FindWindowByID(window_id)) {
            window->DockOrder = order;
        } else if (ImGuiWindowSettings *ws = ImGui::FindWindowSettingsByID(window_id)) {
            ws->DockOrder = order;
        }
        order++;
        /* It is docked again, so it must not be sent to its floating place:
         * a window that is told where to be leaves its dock. */
        s_float_reset_mask &= ~(1u << panel);
        if (k == n.selected) {
            if (ImGuiDockNode *node = ImGui::DockBuilderGetNode(at)) {
                node->SelectedTabId = ImHashStr("#TAB", 0, window_id);
            }
        }
    }
}

/* Put a sidebar's remembered arrangement back. Called inside the host window,
 * just before the dock space is submitted for the frame. */
static void rebuild_side(int side, ImGuiID dockId, ImVec2 size) {
    static LvDockTree tree;
    if (!lvDockTreeParse(s_side[side].tree.c_str(), &tree)) return;
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, size);
    tree_to_node(&tree, 0, dockId, 0);
    ImGui::DockBuilderFinish(dockId);
}

/* Bring a sidebar in. */
static void side_activate(int side) {
    LvDockSide &s = s_side[side];
    s.active = true;
    s.gripHeld = false;

    if (ImGui::DockBuilderGetNode(dock_space_id(side)) != nullptr) {
        /* Already in the context: it came with the settings text. */
    } else if (!s.tree.empty()) {
        s.wantRebuild = true;
    } else if (side == LV_DOCK_RIGHT && !s_right_laid_out) {
        /* Never laid out: start with every panel in it. */
        s_want_layout = true;
    }
    if (side == LV_DOCK_RIGHT) s_right_laid_out = true;
}

/* Take a sidebar away. Its panels float again; they are marked to go back to
 * their default places, because where a slot in a sidebar was is no place
 * for a floating window. The arrangement is kept for when it comes back. */
static void side_deactivate(int side) {
    LvDockSide &s = s_side[side];
    ImGuiID     id = dock_space_id(side);

    if (ImGui::DockBuilderGetNode(id) != nullptr) {
        capture_ini();
        s.tree = side_to_tree(side);

        for (int i = 0; i < LV_DOCK_PANEL_COUNT; i++) {
            ImGuiWindow *window = ImGui::FindWindowByName(k_panels[i].id);
            ImGuiDockNode *node = window ? ImGui::DockBuilderGetNode(window->DockId) : nullptr;
            while (node != nullptr && node->ParentNode != nullptr) node = node->ParentNode;
            if (node != nullptr && node->ID == id) s_float_reset_mask |= 1u << i;
        }
        ImGui::DockBuilderRemoveNode(id);
    }
    if (side == LV_DOCK_RIGHT) s_want_layout = false;
    s.wantRebuild = false;
    s.gripHeld = false;
    s.active = false;
}

/* Put the saved settings into the context, once, before the first frame: the
 * floating panels are then made where they were left and the sidebars that
 * show are made as they were arranged, with nothing to jump afterwards.
 *
 * What the text says is not trusted further than ImGui reads it. A sidebar
 * it arranges that is not going to show (the option is off, or the viewer is
 * in a mode with no sidebars) is taken out again here, its arrangement kept
 * for later, so no panel is left tied to a dock space that is never drawn. */
static void boot(bool mode_ok) {
    s_booted = true;
    /* Tell of a change two seconds after it, not ImGui's five: a viewer that
     * is killed then loses that much at most. */
    ImGui::GetIO().IniSavingRate = 2.0f;

    if (!s_ini.empty()) {
        ImGui::LoadIniSettingsFromMemory(s_ini.c_str(), s_ini.size());
    }
    for (int i = 0; i < LV_DOCK_SIDES; i++) {
        ImGuiID id = dock_space_id(i);
        if (ImGui::DockBuilderGetNode(id) == nullptr) continue;
        if (s_side[i].enabled && mode_ok) continue;
        s_side[i].tree = side_to_tree(i);
        ImGui::DockBuilderRemoveNode(id);
    }
    if (s_ini_is_legacy && !s_side[LV_DOCK_RIGHT].enabled) {
        /* The old text was the dock's alone: with the dock off the viewer
         * never used it to place a floating panel, and its places for the
         * panels are slots in the dock. Only the arrangement is wanted. */
        ImGui::ClearIniSettings();
    }
    s_ini_is_legacy = false;
}

void lv_imgui_dock_pre_frame(void) {
    if (s_lv == nullptr || lv_imgui_context_get() == nullptr) return;

    bool mode_ok = s_standalone && !s_lv->gameView &&
                   !lv_screenSpecIsLiveMode() &&
                   !(uiModeIsTablet() || uiModeIsSteamDeck());

    if (s_standalone && !s_booted) {
        ImGui::SetCurrentContext(lv_imgui_context_get());
        boot(mode_ok);
    }

    for (int i = 0; i < LV_DOCK_SIDES; i++) {
        bool want = mode_ok && s_side[i].enabled;
        if (want == s_side[i].active) continue;
        ImGui::SetCurrentContext(lv_imgui_context_get());
        if (want) side_activate(i);
        else      side_deactivate(i);
    }
}

void lv_imgui_dock_margins(int *left, int *top, int *right, int *bottom) {
    LvDockMargins m = { 0, 0, 0, 0 };
    if (any_active()) m = current_margins(nullptr, nullptr);
    if (left)   *left = m.left;
    if (top)    *top = m.top;
    if (right)  *right = m.right;
    if (bottom) *bottom = m.bottom;
}

int lv_imgui_dock_autosave_due(void) {
    if (!s_standalone || !s_booted || lv_imgui_context_get() == nullptr) return 0;
    ImGui::SetCurrentContext(lv_imgui_context_get());

    int due = 0;
    /* ImGui's own notice: a window moved, was resized, docked or undocked, a
     * split was dragged, a tab chosen. */
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantSaveIniSettings) {
        io.WantSaveIniSettings = false;
        due = 1;
    }

    /* What ImGui does not know: the sidebar options and sizes, and which
     * panels are open. Reported once it has been still for a second, so a
     * drag of a sidebar's edge is one save and not one a frame. */
    unsigned sig = 2166136261u;
    bool     dragging = false;
    for (int i = 0; i < LV_DOCK_SIDES; i++) {
        sig = (sig ^ (unsigned)(s_side[i].enabled ? 1 : 0)) * 16777619u;
        sig = (sig ^ (unsigned)s_side[i].size) * 16777619u;
        dragging = dragging || s_side[i].gripHeld;
    }
    for (int i = 0; i < LV_DOCK_PANEL_COUNT; i++) {
        sig = (sig ^ (unsigned)(*k_panels[i].show ? 1 : 0)) * 16777619u;
    }
    if (s_sig_saved == 0) s_sig_saved = sig;  /* the first look: as loaded */
    if (sig != s_sig_saved) {
        Uint64 now = SDL_GetTicks();
        if (sig != s_sig_pending) {
            s_sig_pending = sig;
            s_sig_pending_at = now;
        } else if (!dragging && now - s_sig_pending_at >= 1000) {
            s_sig_saved = sig;
            due = 1;
        }
    }
    return due;
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
    if (!any_active()) return false;
    ImGuiWindow *window = ImGui::FindWindowByName(window_id);
    return window != nullptr && window->DockIsActive;
}

/* Put every panel that is showing into the dock, one above the other, in the
 * order of k_panels. Called inside the right sidebar's host window, just
 * before the dock space is submitted for the frame.
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
    float fixed[LV_DOCK_PANEL_COUNT];
    int   count = 0;
    int   anchor = -1;
    float fixedSum = 0.0f, weightSum = 0.0f;

    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, size);

    for (int i = 0; i < LV_DOCK_PANEL_COUNT; i++) {
        if (!*k_panels[i].show) continue;
        if (k_panels[i].anchor) anchor = count;
        fixed[count] = k_panels[i].heightFor ? k_panels[i].heightFor(size.x)
                                             : k_panels[i].fixedH;
        fixedSum += fixed[count];
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
        if (spare > 0.0f && fixed[n] > 0.0f) {
            height[n] = fixed[n];
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

    /* Every panel showing is docked now; none is to be sent to its floating
     * place, which would take it straight out again. */
    s_float_reset_mask = 0;
}

/* Draw one sidebar: its strip, the grip on its inner edge and its dock space.
 * pos and size are the strip's place, other the size of the sidebar facing
 * it and extent the length the two share. */
static void draw_side(int side, ImVec2 pos, ImVec2 size, int other, int extent) {
    LvDockSide &s = s_side[side];
    bool        wide = lvDockSideIsWide(side) != 0;

    /* With no room at all (a window a few points high) the strip is still
     * made: the dock space has to be submitted every frame or its panels
     * would be undocked for good. */
    if (size.x < 1.0f) size.x = 1.0f;
    if (size.y < 1.0f) size.y = 1.0f;
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

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
    /* The panels docked here look for the dock space every frame, so it is
     * submitted whatever Begin answers. */
    ImGui::Begin(s.hostName, nullptr, flags);
    ImGui::PopStyleVar(4);

    /* The grip along the inner edge, the dock space in the rest. */
    ImVec2 gripPos = pos, gripSize = size, dockPos = pos, dockSize = size;
    switch (side) {
        case LV_DOCK_RIGHT:
            gripSize.x = LV_DOCK_GRIP_W;
            dockPos.x += LV_DOCK_GRIP_W;
            dockSize.x -= LV_DOCK_GRIP_W;
            break;
        case LV_DOCK_LEFT:
            gripSize.x = LV_DOCK_GRIP_W;
            gripPos.x = pos.x + size.x - LV_DOCK_GRIP_W;
            dockSize.x -= LV_DOCK_GRIP_W;
            break;
        case LV_DOCK_TOP:
            gripSize.y = LV_DOCK_GRIP_W;
            gripPos.y = pos.y + size.y - LV_DOCK_GRIP_W;
            dockSize.y -= LV_DOCK_GRIP_W;
            break;
        default: /* LV_DOCK_BOTTOM */
            gripSize.y = LV_DOCK_GRIP_W;
            dockPos.y += LV_DOCK_GRIP_W;
            dockSize.y -= LV_DOCK_GRIP_W;
            break;
    }
    if (dockSize.x < 1.0f) dockSize.x = 1.0f;
    if (dockSize.y < 1.0f) dockSize.y = 1.0f;

    /* Dragging the grip sets the size from where the mouse is, between the
     * sidebar's smallest size and what the sidebar facing it leaves of two
     * thirds of the window. The map follows on the next frame. */
    ImGui::SetCursorScreenPos(gripPos);
    ImGui::InvisibleButton("##lv_dock_grip", gripSize);
    bool gripHot = ImGui::IsItemHovered();
    bool gripHeld = ImGui::IsItemActive();
    if (gripHot || gripHeld) {
        ImGui::SetMouseCursor(wide ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    }
    if (gripHeld) {
        ImVec2 mouse = ImGui::GetIO().MousePos;
        float  wanted;
        switch (side) {
            case LV_DOCK_RIGHT: wanted = pos.x + size.x - mouse.x; break;
            case LV_DOCK_LEFT:  wanted = mouse.x - pos.x; break;
            case LV_DOCK_TOP:   wanted = mouse.y - pos.y; break;
            default:            wanted = pos.y + size.y - mouse.y; break;
        }
        wanted += LV_DOCK_GRIP_W * 0.5f;
        s.size = lvDockDragSize(side, (int)(wanted + 0.5f), other, extent);
    }
    s.gripHeld = gripHeld;
    ImGui::GetWindowDrawList()->AddRectFilled(
        gripPos, ImVec2(gripPos.x + gripSize.x, gripPos.y + gripSize.y),
        ImGui::GetColorU32(gripHeld ? ImGuiCol_SeparatorActive
                           : gripHot ? ImGuiCol_SeparatorHovered
                                     : ImGuiCol_Separator));

    ImGuiID dockId = dock_space_id(side);
    if (dockSize.x > 1.0f && dockSize.y > 1.0f) {
        if (side == LV_DOCK_RIGHT && s_want_layout) {
            build_layout(dockId, dockSize);
            s_want_layout = false;
            s.wantRebuild = false;
        } else if (s.wantRebuild) {
            rebuild_side(side, dockId, dockSize);
            s.wantRebuild = false;
        }
    }

    /* A sidebar with nothing in it says what it is for. ImGui paints an
     * empty dock space over; with that colour clear, the words drawn on the
     * strip under it show. */
    ImGuiDockNode *root = ImGui::DockBuilderGetNode(dockId);
    bool empty = root == nullptr || (root->IsLeafNode() && root->Windows.Size == 0);
    if (empty) {
        const char *hint = langGetText(STR_LV_DOCK_EMPTY_HINT);
        float  wrap = dockSize.x - 16.0f;
        if (wrap < 40.0f) wrap = 40.0f;
        ImVec2 text = ImGui::CalcTextSize(hint, nullptr, false, wrap);
        ImVec2 at(dockPos.x + (dockSize.x - text.x) * 0.5f,
                  dockPos.y + (dockSize.y - text.y) * 0.5f);
        if (at.x < dockPos.x + 8.0f) at.x = dockPos.x + 8.0f;
        if (at.y < dockPos.y + 4.0f) at.y = dockPos.y + 4.0f;
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), at,
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            hint, nullptr, wrap);
        ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    }
    ImGui::SetCursorScreenPos(dockPos);
    ImGui::DockSpace(dockId, dockSize, ImGuiDockNodeFlags_None);
    if (empty) ImGui::PopStyleColor();

    ImGui::End();
}

void lv_imgui_dock_host(void) {
    if (!any_active()) return;

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    int windowW = 0, areaH = 0;
    /* The same sum lv_imgui_dock_margins does, so the strips and the part of
     * the window the map leaves alone are one and the same. */
    LvDockMargins m = current_margins(&windowW, &areaH);

    /* WorkPos / WorkSize are the viewport less the menu bar. The left and
     * right sidebars run the full height under the menu bar; the top and
     * bottom ones sit between them. */
    float x0 = vp->Pos.x, x1 = vp->Pos.x + vp->Size.x;
    float y0 = vp->WorkPos.y, y1 = vp->WorkPos.y + vp->WorkSize.y;
    float midW = vp->Size.x - (float)(m.left + m.right);

    if (s_side[LV_DOCK_RIGHT].active) {
        draw_side(LV_DOCK_RIGHT, ImVec2(x1 - (float)m.right, y0),
                  ImVec2((float)m.right, y1 - y0), m.left, windowW);
    }
    if (s_side[LV_DOCK_LEFT].active) {
        draw_side(LV_DOCK_LEFT, ImVec2(x0, y0),
                  ImVec2((float)m.left, y1 - y0), m.right, windowW);
    }
    if (s_side[LV_DOCK_TOP].active) {
        draw_side(LV_DOCK_TOP, ImVec2(x0 + (float)m.left, y0),
                  ImVec2(midW, (float)m.top), m.bottom, areaH);
    }
    if (s_side[LV_DOCK_BOTTOM].active) {
        draw_side(LV_DOCK_BOTTOM, ImVec2(x0 + (float)m.left, y1 - (float)m.bottom),
                  ImVec2(midW, (float)m.bottom), m.top, areaH);
    }
}
