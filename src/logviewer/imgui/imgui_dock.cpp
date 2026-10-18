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
#include "imgui_controls.h"
#include "imgui_hud.h"
#include "imgui_overview.h"
#include "imgui_tank.h"
#include "imgui_lv_icons.h"
#include "imgui.h"
#include "imgui_internal.h"  /* DockBuilder*, FindWindowByName */
#include "platform_config.h"
#include "../../gui/lang.h"
#include "../../gui/ui_mode.h"
#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
    #include "logviewer.h"
    #include "../backend.h"
    #include "../lv_dock_fit.h"
    #include "../lv_dock_layout.h"
    #include "../lv_dock_clear.h"
    #include "../lv_dock_default.h"
    #include "../lv_panel_set.h"
}

/* How thick the strip on a sidebar's inner edge is that the user drags to
 * make the sidebar bigger or smaller. */
#define LV_DOCK_GRIP_W 6.0f

/* The most ImGui settings text read back from the preferences. The settings
 * of the panels and four sidebars are a few KB; text that does not fit
 * is not this viewer's. */
#define LV_DOCK_LAYOUT_MAX 32768

/* The panels the sidebars know, in the order the right sidebar stacks them
 * from the top the first time it shows for a user who had only that
 * sidebar. Each name is the part of the window's title that makes its ID ("Controls###controls" and "###controls" are one window), so it
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
 * its rows the same way, as does the Tank panel with its
 * four gauges. */
static const LvDockPanel k_panels[] = {
    { "###controls",   &lv_g_show_controls_window,       0.0f,               0.0f, false, lv_imgui_controls_dock_height },
    { "###hud",        &lv_g_show_hud_window,            0.0f,               0.0f, false, lv_imgui_hud_dock_height },
    { "###overview",   &lv_g_show_overview_window,       0.0f,               0.0f, false, lv_imgui_overview_dock_height },
    { "###tank",       &lv_g_show_tank_window,           0.0f,               0.0f, false, lv_imgui_tank_dock_height },
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

/* The option "Make unused sidebar portions transparent" of each sidebar, and
 * with it on the length each panel has in that sidebar (a height in the
 * left and right ones, a width in the top and bottom ones; 0 until the
 * panel has been in it). Indexed as k_panels, which is the order of
 * lvDockClearPanels. */
static bool s_clear[LV_DOCK_SIDES] = { false, false, false, false };
static int  s_len[LV_DOCK_SIDES][LV_DOCK_CLEAR_PANELS];

/* The part of each sidebar its panels took the last time it drew: all of
 * the strip for a solid one, and for one with no panel in it. */
static LvDockRect s_used[LV_DOCK_SIDES];

/* The user is dragging the grip of a panel in this sidebar, and where on
 * the grip the mouse took hold. */
static bool  s_slot_drag[LV_DOCK_SIDES] = { false, false, false, false };
static float s_slot_grab = 0.0f;

/* Whether this host may show sidebars at all. The viewer (logViewerRun) and
 * the live spectator (spectatorRun) both say yes, and share the same saved
 * layout. The reel in the game says no. */
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

/* No layout of the sidebars was ever saved: the standalone viewer starts
 * with the default arrangement (lv_dock_default.h). */
static bool s_fresh = false;

/* Put the default arrangement in before the next frame. */
static bool s_reset_default = false;

/* Build the default arrangement's panels in each sidebar the next time it
 * draws. */
static bool s_want_default[LV_DOCK_SIDES] = { false, false, false, false };

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
    s_fresh = false;
    s_reset_default = false;
    s_float_reset_mask = 0;
    s_ini.clear();
    s_ini_is_legacy = false;
    for (int i = 0; i < LV_DOCK_SIDES; i++) {
        s_side[i].active = false;
        s_side[i].wantRebuild = false;
        s_side[i].gripHeld = false;
        s_side[i].tree.clear();
        s_slot_drag[i] = false;
        s_want_default[i] = false;
        s_used[i].x = s_used[i].y = s_used[i].w = s_used[i].h = 0;
    }

    /* Which sidebars are clear past their panels, and the panels' lengths
     * in each. Text that does not read gives off, and no length. */
    {
        int  clear[LV_DOCK_SIDES];
        char key[64];
        lv_platform_config_get_string("LOGVIEWER", "Dock.Clear", "", val, sizeof(val));
        lvDockClearFlagsParse(val, clear);
        for (int i = 0; i < LV_DOCK_SIDES; i++) {
            char lens[256];
            s_clear[i] = clear[i] != 0;
            snprintf(key, sizeof(key), "Dock.Len.%s", s_side[i].key);
            lv_platform_config_get_string("LOGVIEWER", key, "", lens, sizeof(lens));
            lvDockClearLensParse(lens, s_len[i]);
        }
    }

    lv_platform_config_get_string("LOGVIEWER", "Dock.Sides", "", val, sizeof(val));
    bool have_sides = lvDockSidesParse(val, &sides) != 0;
    bool have_any = val[0] != '\0';
    if (!have_sides) {
        lv_platform_config_get_string("LOGVIEWER", "Dock.Enabled", "", val, sizeof(val));
        have_any = have_any || val[0] != '\0';
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
    /* Nothing of the sidebars in the preferences, under the names of now or
     * of before: a new user. */
    s_fresh = !have_any && s_ini.empty();

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

    int clear[LV_DOCK_SIDES];
    for (int i = 0; i < LV_DOCK_SIDES; i++) clear[i] = s_clear[i] ? 1 : 0;
    if (lvDockClearFlagsFormat(clear, line, sizeof(line)) > 0) {
        lv_platform_config_set_string("LOGVIEWER", "Dock.Clear", line);
    }
    for (int i = 0; i < LV_DOCK_SIDES; i++) {
        char lens[256];
        snprintf(key, sizeof(key), "Dock.Len.%s", s_side[i].key);
        if (lvDockClearLensFormat(s_len[i], lens, sizeof(lens)) > 0) {
            lv_platform_config_set_string("LOGVIEWER", key, lens);
        }
    }
}

void lv_imgui_dock_set_standalone(int standalone) {
    s_standalone = standalone != 0;
    /* A host with sidebars starts a new user with the default
     * arrangement. */
    if (s_standalone && s_fresh) s_reset_default = true;
    s_fresh = false;
}

int lv_imgui_dock_get_side_enabled(int side) {
    if (side < 0 || side >= LV_DOCK_SIDES) return 0;
    return s_side[side].enabled ? 1 : 0;
}

void lv_imgui_dock_set_side_enabled(int side, int enabled) {
    if (side < 0 || side >= LV_DOCK_SIDES) return;
    s_side[side].enabled = enabled != 0;
}

int lv_imgui_dock_get_side_clear(int side) {
    if (side < 0 || side >= LV_DOCK_SIDES) return 0;
    return s_clear[side] ? 1 : 0;
}

void lv_imgui_dock_set_side_clear(int side, int clear) {
    if (side < 0 || side >= LV_DOCK_SIDES) return;
    s_clear[side] = clear != 0;
}

int lv_imgui_dock_get_enabled(void) {
    return lv_imgui_dock_get_side_enabled(LV_DOCK_RIGHT);
}

void lv_imgui_dock_set_enabled(int enabled) {
    lv_imgui_dock_set_side_enabled(LV_DOCK_RIGHT, enabled);
}

void lv_imgui_dock_request_layout(void) {
    /* Put in before the next frame (lv_imgui_dock_pre_frame), outside the
     * frame the menu is drawn in. */
    s_reset_default = true;
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

/* Put the default arrangement in: the sidebars' options and sizes, which of
 * them are clear and the lengths of their panels, and the panels themselves,
 * built in each sidebar the next time it draws. Every sidebar's dock space
 * is taken away first, so no panel stays where the user had it. A panel the
 * default does not dock (one that is hidden) goes to its default floating
 * place when it next shows. */
static void apply_default(void) {
    LvDockSides sides;
    lvDockDefaultSides(&sides);
    s_reset_default = false;
    s_want_layout = false;
    s_right_laid_out = sides.rightLaidOut != 0;
    for (int i = 0; i < LV_DOCK_SIDES; i++) {
        LvDockSide &s = s_side[i];
        ImGuiID     id = dock_space_id(i);
        if (ImGui::DockBuilderGetNode(id) != nullptr) ImGui::DockBuilderRemoveNode(id);
        s.tree.clear();
        s.wantRebuild = false;
        s.gripHeld = false;
        s.enabled = sides.enabled[i] != 0;
        s.size = sides.size[i];
        s_clear[i] = lvDockDefaultClear(i) != 0;
        lvDockDefaultLens(i, s_len[i]);
        s_slot_drag[i] = false;
        s_want_default[i] = s.enabled && lvDockDefaultStackOf(i).count > 0;
    }
    s_float_reset_mask = (1u << LV_DOCK_PANEL_COUNT) - 1u;
}

void lv_imgui_dock_pre_frame(void) {
    if (s_lv == nullptr || lv_imgui_context_get() == nullptr) return;

    bool mode_ok = lvPanelSetSidebarsOn(s_standalone ? 1 : 0, s_lv->gameView ? 1 : 0,
                                        (uiModeIsTablet() || uiModeIsSteamDeck()) ? 1 : 0) != 0;

    if (s_standalone && !s_booted) {
        ImGui::SetCurrentContext(lv_imgui_context_get());
        boot(mode_ok);
    }
    if (s_standalone && s_booted && s_reset_default) {
        ImGui::SetCurrentContext(lv_imgui_context_get());
        apply_default();
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

void lv_imgui_dock_map_margins(int *left, int *top, int *right, int *bottom) {
    LvDockMargins m = { 0, 0, 0, 0 };
    if (any_active()) {
        int clear[LV_DOCK_SIDES];
        for (int i = 0; i < LV_DOCK_SIDES; i++) {
            clear[i] = (s_side[i].active && s_clear[i]) ? 1 : 0;
        }
        m = current_margins(nullptr, nullptr);
        m = lvDockClearMapMargins(&m, clear);
    }
    if (left)   *left = m.left;
    if (top)    *top = m.top;
    if (right)  *right = m.right;
    if (bottom) *bottom = m.bottom;
}

void lv_imgui_dock_open_margins(int *left, int *top, int *right, int *bottom) {
    LvDockMargins m = { 0, 0, 0, 0 };
    if (any_active()) {
        int clear[LV_DOCK_SIDES];
        for (int i = 0; i < LV_DOCK_SIDES; i++) {
            clear[i] = (s_side[i].active && s_clear[i]) ? 1 : 0;
        }
        m = current_margins(nullptr, nullptr);
        m = lvDockClearOpenMargins(&m, clear);
    }
    if (left)   *left = m.left;
    if (top)    *top = m.top;
    if (right)  *right = m.right;
    if (bottom) *bottom = m.bottom;
}

int lv_imgui_dock_point_on_map(float px, float py) {
    int w = 0, areaH = 0, h = 0;
    int clear[LV_DOCK_SIDES];

    if (!any_active()) return 1;
    LvDockMargins m = current_margins(&w, &areaH);
    if (s_lv && s_lv->window) SDL_GetWindowSize(s_lv->window, &w, &h);
    for (int i = 0; i < LV_DOCK_SIDES; i++) {
        clear[i] = (s_side[i].active && s_clear[i]) ? 1 : 0;
    }
    return lvDockClearPointOnMap(&m, clear, s_used, w, h, h - areaH, px, py);
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
        dragging = dragging || s_side[i].gripHeld || s_slot_drag[i];
        /* The option of each sidebar and the lengths of its panels. */
        sig = (sig ^ (unsigned)(s_clear[i] ? 1 : 0)) * 16777619u;
        for (int k = 0; k < LV_DOCK_CLEAR_PANELS; k++) {
            sig = (sig ^ (unsigned)s_len[i][k]) * 16777619u;
        }
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

/* Put the panels of slot that are showing into a sidebar, one after the
 * other from its start: down the left and right sidebars, across the top
 * and bottom ones. Called inside the sidebar's host window, just before the
 * dock space is submitted for the frame.
 *
 * The dock space is split once a panel. Splitting toward the start gives the
 * new node to the panel and leaves the central node beyond it; splitting
 * toward the end does the reverse. So the panels before the anchor are
 * peeled off the start in order, the ones after it off the end in reverse,
 * and the anchor is docked into what is left, which is the central node. A
 * node keeps the length it is given here; the central node takes whatever
 * remains, then and whenever the window changes size.
 *
 * A panel's length is the slot's, or else its height for the sidebar's
 * width (k_panels), or else a share by weight of what the others leave
 * (lvDockStackLens). A panel that is hidden is left out and gets no node,
 * so there is no gap for it. Nothing here forbids splitting or undocking:
 * the user can still drag a panel by its tab to another place in the dock
 * or out of it. */
static void build_stack(int side, ImGuiID dockId, ImVec2 size,
                        const LvDockDefaultSlot *slot, int slotCount) {
    int         shown[LV_DOCK_PANEL_COUNT];
    float       fixed[LV_DOCK_PANEL_COUNT];
    float       weight[LV_DOCK_PANEL_COUNT];
    float       length[LV_DOCK_PANEL_COUNT];
    int         count = 0;
    int         anchor = -1;
    const bool  down = lvDockSideIsWide(side) != 0;
    const float extent = down ? size.y : size.x;

    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, size);

    for (int k = 0; k < slotCount && count < LV_DOCK_PANEL_COUNT; k++) {
        int i = panel_index(slot[k].id);
        if (i < 0 || !*k_panels[i].show) continue;
        if (slot[k].anchor) anchor = count;
        if (slot[k].len > 0) {
            fixed[count] = (float)slot[k].len;
        } else if (down && k_panels[i].heightFor != nullptr) {
            fixed[count] = k_panels[i].heightFor(size.x);
        } else {
            fixed[count] = k_panels[i].fixedH;
        }
        weight[count] = (fixed[count] > 0.0f) ? 0.0f : k_panels[i].weight;
        shown[count++] = i;
    }
    if (count == 0) {
        ImGui::DockBuilderFinish(dockId);
        return;
    }
    /* With the anchor panel hidden the last one shown stands in for it. */
    if (anchor < 0) anchor = count - 1;

    lvDockStackLens(fixed, weight, count, extent, length);

    const ImGuiDir toStart = down ? ImGuiDir_Up : ImGuiDir_Left;
    const ImGuiDir toEnd = down ? ImGuiDir_Down : ImGuiDir_Right;
    ImGuiID rest = dockId;
    float   restLen = extent;
    for (int n = 0; n < anchor; n++) {
        ImGuiID at = 0;
        float ratio = (restLen > 1.0f) ? length[n] / restLen : 0.5f;
        ImGui::DockBuilderSplitNode(rest, toStart, ratio, &at, &rest);
        ImGui::DockBuilderDockWindow(k_panels[shown[n]].id, at);
        restLen -= length[n];
    }
    for (int n = count - 1; n > anchor; n--) {
        ImGuiID at = 0;
        float ratio = (restLen > 1.0f) ? length[n] / restLen : 0.5f;
        ImGui::DockBuilderSplitNode(rest, toEnd, ratio, &at, &rest);
        ImGui::DockBuilderDockWindow(k_panels[shown[n]].id, at);
        restLen -= length[n];
    }
    ImGui::DockBuilderDockWindow(k_panels[shown[anchor]].id, rest);

    ImGui::DockBuilderFinish(dockId);

    /* The panels docked here must not be sent to their floating places,
     * which would take them straight out again. */
    for (int n = 0; n < count; n++) s_float_reset_mask &= ~(1u << shown[n]);
}

/* Put every panel that is showing into the right sidebar, one above the
 * other, in the order of k_panels: the first layout of a right sidebar for
 * a user who had only that one. It runs only while the right sidebar has no
 * dock space yet, so a panel that is docked is in another sidebar: it stays
 * there (panel_is_free), as rebuild_side leaves it. */
static void build_layout(ImGuiID dockId, ImVec2 size) {
    LvDockDefaultSlot slot[LV_DOCK_PANEL_COUNT];
    int count = 0;
    for (int i = 0; i < LV_DOCK_PANEL_COUNT; i++) {
        if (!panel_is_free(k_panels[i].id)) continue;
        slot[count].id = k_panels[i].id;
        slot[count].len = 0;
        slot[count].anchor = k_panels[i].anchor ? 1 : 0;
        count++;
    }
    build_stack(LV_DOCK_RIGHT, dockId, size, slot, count);

    /* Every panel showing is docked now, here or in another sidebar; none
     * is to be sent to its floating place. */
    s_float_reset_mask = 0;
}

/* ---- A sidebar whose unused part is clear ---------------------------------- */

/* One panel's place along a sidebar: the part of the dock space it is in.
 * That is one dock node, or, where the user has put panels side by side
 * across the sidebar, the node that holds them all. panel is the first
 * panel in it by the order of k_panels, whose saved length the part has;
 * -1 for a part with no panel in it. */
struct LvClearSlot {
    ImGuiDockNode *node;
    ImGuiID        id;
    int            panel;
};

static int node_key_panel(const ImGuiDockNode *n, int depth) {
    if (n == nullptr || depth > LV_DOCK_TREE_DEPTH) return -1;
    if (n->IsSplitNode()) {
        int a = node_key_panel(n->ChildNodes[0], depth + 1);
        int b = node_key_panel(n->ChildNodes[1], depth + 1);
        if (a < 0) return b;
        if (b < 0) return a;
        return a < b ? a : b;
    }
    int best = -1;
    for (int i = 0; i < n->Windows.Size; i++) {
        int p = panel_index(n->Windows[i]->Name);
        if (p >= 0 && (best < 0 || p < best)) best = p;
    }
    return best;
}

/* The parts of a dock space in order along axis: what its splits along
 * that axis divide it into. A part that does not show is left out. */
static void collect_slots(ImGuiDockNode *n, ImGuiAxis axis, LvClearSlot *out, int *count,
                          int depth) {
    if (n == nullptr || !n->IsVisible || depth > LV_DOCK_TREE_DEPTH) return;
    if (n->IsSplitNode() && n->SplitAxis == axis) {
        collect_slots(n->ChildNodes[0], axis, out, count, depth + 1);
        collect_slots(n->ChildNodes[1], axis, out, count, depth + 1);
        return;
    }
    if (*count >= LV_DOCK_CLEAR_SLOTS) return;
    out[*count].node = n;
    out[*count].id = n->ID;
    out[*count].panel = node_key_panel(n, 0);
    (*count)++;
}

/* The length along axis of what a part shows, 0 when that is not known
 * (a panel that has not drawn yet). A panel with a height that follows from
 * its width (k_panels) is asked for it; any other gives the size of what it
 * last drew. */
static float node_content(ImGuiDockNode *n, ImGuiAxis axis, int depth) {
    if (n == nullptr || !n->IsVisible || depth > LV_DOCK_TREE_DEPTH) return 0.0f;
    if (n->IsSplitNode()) {
        ImGuiDockNode *a = n->ChildNodes[0], *b = n->ChildNodes[1];
        float ca = node_content(a, axis, depth + 1);
        float cb = node_content(b, axis, depth + 1);
        if ((a->IsVisible && ca <= 0.0f) || (b->IsVisible && cb <= 0.0f)) return 0.0f;
        if (n->SplitAxis == axis) {
            return ca + cb + ((a->IsVisible && b->IsVisible) ? (float)LV_DOCK_CLEAR_GRIP : 0.0f);
        }
        return ca > cb ? ca : cb;
    }
    ImGuiWindow *w = n->VisibleWindow;
    if (w == nullptr) return 0.0f;
    if (axis == ImGuiAxis_Y) {
        int p = panel_index(w->Name);
        if (p >= 0 && k_panels[p].heightFor != nullptr && n->Size.x > 1.0f) {
            return k_panels[p].heightFor(n->Size.x);
        }
        if (w->ContentSize.y <= 0.0f) return 0.0f;
        bool bar = n->TabBar != nullptr && !n->IsHiddenTabBar() && !n->IsNoTabBar();
        return (bar ? ImGui::GetFrameHeight() : 0.0f) + w->ContentSize.y +
               w->WindowPadding.y * 2.0f;
    }
    if (w->ContentSize.x <= 0.0f) return 0.0f;
    return w->ContentSize.x + w->WindowPadding.x * 2.0f +
           (w->ScrollbarY ? ImGui::GetStyle().ScrollbarSize : 0.0f);
}

/* Tell ImGui the length of each part: the size it works from when it shares
 * a dock space out among its parts. The dock space is then submitted exactly
 * as long as the parts and the grips between them, so each part gets its
 * own length. Returns the length of n and what is under it. */
static float apply_lens(ImGuiDockNode *n, ImGuiAxis axis, const LvDockClearPlan *plan, int *at,
                        int depth) {
    if (n == nullptr || !n->IsVisible || depth > LV_DOCK_TREE_DEPTH) return 0.0f;
    if (n->IsSplitNode() && n->SplitAxis == axis) {
        float a = apply_lens(n->ChildNodes[0], axis, plan, at, depth + 1);
        float b = apply_lens(n->ChildNodes[1], axis, plan, at, depth + 1);
        float total = a + b + ((a > 0.0f && b > 0.0f) ? (float)LV_DOCK_CLEAR_GRIP : 0.0f);
        if (total > 0.0f) n->SizeRef[axis] = total;
        return total;
    }
    if (*at >= plan->count) return n->SizeRef[axis];
    float len = (float)plan->len[(*at)++];
    n->SizeRef[axis] = len;
    return len;
}

/* The grip on a sidebar's inner edge, in the window that is current: the
 * user drags it to make the sidebar bigger or smaller. gripPos and gripSize
 * are the part of the edge this window has; pos and size are the whole
 * strip. Dragging sets the size from where the mouse is, between the
 * sidebar's smallest size and what the sidebar facing it leaves of two
 * thirds of the window. The map follows on the next frame. With always
 * false the grip is drawn only while the mouse is on it or holds it.
 * Returns true while it is held. */
static bool edge_grip(int side, ImVec2 gripPos, ImVec2 gripSize, ImVec2 pos, ImVec2 size,
                      int other, int extent, bool always) {
    LvDockSide &s = s_side[side];
    bool        wide = lvDockSideIsWide(side) != 0;

    if (gripSize.x < 1.0f || gripSize.y < 1.0f) return false;
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
    if (always || gripHot || gripHeld) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            gripPos, ImVec2(gripPos.x + gripSize.x, gripPos.y + gripSize.y),
            ImGui::GetColorU32(gripHeld ? ImGuiCol_SeparatorActive
                               : gripHot ? ImGuiCol_SeparatorHovered
                                         : ImGuiCol_Separator));
    }
    return gripHeld;
}

/* The flags of a bare window the sidebars put on the screen: nothing to
 * grab, move or close, never in front of a floating panel, never itself
 * dockable, and with no settings of its own (its place is worked out every
 * frame). */
static const ImGuiWindowFlags k_bare_flags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;

/* Draw one sidebar: its strip, the grip on its inner edge and its dock space.
 * pos and size are the strip's place, other the size of the sidebar facing
 * it and extent the length the two share.
 *
 * A sidebar whose unused part is clear (s_clear) is drawn another way. Its
 * strip has no background, and is only as long as its panels: each panel
 * has its own length, they are packed from the start of the sidebar, and a
 * grip on the far edge of each changes the length of that panel and of no
 * other (lv_dock_clear.h). Past the last grip there is no window at all, so
 * the map shows there and gets the mouse. The grip on the sidebar's inner
 * edge still runs the whole length: the part of it beside the clear part is
 * a window of its own that shows only under the mouse. */
static void draw_side(int side, ImVec2 pos, ImVec2 size, int other, int extent) {
    LvDockSide      &s = s_side[side];
    bool             wide = lvDockSideIsWide(side) != 0;
    const ImGuiAxis  axis = wide ? ImGuiAxis_Y : ImGuiAxis_X;
    const ImGuiStyle &style = ImGui::GetStyle();
    const bool       clear = s_clear[side];
    const float      slotGrip = (float)LV_DOCK_CLEAR_GRIP;

    /* With no room at all (a window a few points high) the strip is still
     * made: the dock space has to be submitted every frame or its panels
     * would be undocked for good. */
    if (size.x < 1.0f) size.x = 1.0f;
    if (size.y < 1.0f) size.y = 1.0f;

    ImGuiID        dockId = dock_space_id(side);
    ImGuiDockNode *root = ImGui::DockBuilderGetNode(dockId);
    bool building = (side == LV_DOCK_RIGHT && s_want_layout) || s.wantRebuild ||
                    s_want_default[side];

    /* The panels and their lengths, when they are packed. */
    LvClearSlot     slots[LV_DOCK_CLEAR_SLOTS];
    int             slotCount = 0;
    int             wanted[LV_DOCK_CLEAR_SLOTS], mins[LV_DOCK_CLEAR_SLOTS];
    LvDockClearPlan plan;
    bool            packed = false;
    const int       stripLen = (int)size[axis];
    plan.count = 0;
    if (clear && root != nullptr && !building) {
        bool anyPanel = false;
        int  minLen = lvDockClearMin(side);
        if (minLen < (int)style.WindowMinSize[axis]) minLen = (int)style.WindowMinSize[axis];
        collect_slots(root, axis, slots, &slotCount, 0);
        for (int i = 0; i < slotCount; i++) {
            ImGuiDockNode *n = slots[i].node;
            int cur = (int)(n->Size[axis] + 0.5f);
            int p = slots[i].panel;
            if (cur <= 0) cur = (int)(n->SizeRef[axis] + 0.5f);
            if (p < 0) {
                /* A part with no panel in it: the least there can be. */
                bool hollow = n->IsLeafNode() && n->Windows.Size == 0;
                mins[i] = (int)style.WindowMinSize[axis];
                wanted[i] = (hollow || cur < mins[i]) ? mins[i] : cur;
                continue;
            }
            anyPanel = true;
            mins[i] = minLen;
            if (s_len[side][p] <= 0 && cur > 0) {
                /* The first time the panel is here: its content's length,
                 * once that is known. Until then it stays as it is. */
                int content = (int)ceilf(node_content(n, axis, 0));
                /* A panel whose height follows from its width shows a
                 * fixed amount: all of it. */
                bool exact = wide && n->IsLeafNode() && n->VisibleWindow != nullptr &&
                             k_panels[p].heightFor != nullptr &&
                             panel_index(n->VisibleWindow->Name) == p;
                if (content > 0) {
                    s_len[side][p] = lvDockClearFirstLen(cur, content, minLen, exact ? 1 : 0);
                }
            }
            wanted[i] = (s_len[side][p] > 0) ? s_len[side][p] : cur;
        }
        if (anyPanel) {
            lvDockClearLayout(wanted, mins, slotCount, stripLen, LV_DOCK_CLEAR_GRIP, &plan);
            packed = true;
        }
    }

    /* The strip is all of the sidebar, or only what the packed panels and
     * their grips take of it. */
    ImVec2 hostSize = size;
    if (packed) {
        float used = (float)(plan.used < stripLen ? plan.used : stripLen);
        if (used < 1.0f) used = 1.0f;
        hostSize[axis] = used;
    }
    s_used[side].x = (int)pos.x;
    s_used[side].y = (int)pos.y;
    s_used[side].w = (int)hostSize.x;
    s_used[side].h = (int)hostSize.y;

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(hostSize, ImGuiCond_Always);

    ImGuiWindowFlags flags = k_bare_flags;
    /* A clear sidebar has no backdrop: each panel in it paints its own. */
    if (clear) flags |= ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
    /* The panels docked here look for the dock space every frame, so it is
     * submitted whatever Begin answers. */
    ImGui::Begin(s.hostName, nullptr, flags);
    ImGui::PopStyleVar(5);
    ImGuiWindow *outer = ImGui::GetCurrentWindow();

    /* The grip along the inner edge, the dock space in the rest. */
    ImVec2 gripPos = pos, gripSize = hostSize, dockPos = pos, dockSize = hostSize;
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
    /* Packed: the last panel's own grip is past the dock space. */
    if (packed) dockSize[axis] -= slotGrip;
    if (dockSize.x < 1.0f) dockSize.x = 1.0f;
    if (dockSize.y < 1.0f) dockSize.y = 1.0f;

    s.gripHeld = edge_grip(side, gripPos, gripSize, pos, size, other, extent, true);

    if (dockSize.x > 1.0f && dockSize.y > 1.0f) {
        if (s_want_default[side]) {
            LvDockDefaultStack stack = lvDockDefaultStackOf(side);
            build_stack(side, dockId, dockSize, stack.slot, stack.count);
            s_want_default[side] = false;
            s.wantRebuild = false;
        } else if (side == LV_DOCK_RIGHT && s_want_layout) {
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
    root = ImGui::DockBuilderGetNode(dockId);
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
        if (clear) {
            /* The words are over the map: a plate under them. */
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(at.x - 6.0f, at.y - 4.0f), ImVec2(at.x + text.x + 6.0f, at.y + text.y + 4.0f),
                ImGui::GetColorU32(ImGuiCol_WindowBg), 3.0f);
        }
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), at,
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            hint, nullptr, wrap);
    }
    if (empty || clear) {
        ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    }
    ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_None;
    if (packed) {
        /* Each part its own length, and ImGui's own splitters along the
         * sidebar off: a drag of one of those takes from one panel what it
         * gives the next. The grips drawn below take their place. */
        int at = 0;
        apply_lens(root, axis, &plan, &at, 0);
        dockFlags = wide ? ImGuiDockNodeFlags_NoResizeY : ImGuiDockNodeFlags_NoResizeX;
        ImGui::PushStyleVar(ImGuiStyleVar_DockingSeparatorSize, slotGrip);
    }
    ImGui::SetCursorScreenPos(dockPos);
    ImGui::DockSpace(dockId, dockSize, dockFlags);
    if (packed) ImGui::PopStyleVar();
    if (empty || clear) ImGui::PopStyleColor();

    /* The grips of the packed panels: one on the far edge of each. They are
     * not items of a window. Between two panels the place is ImGui's own
     * (its dock space is a window inside this one), so the mouse is tested
     * here, and a grip that has it takes the "hovered" and "active" IDs so
     * nothing under it acts on the click. */
    bool slotDrag = false;
    if (packed) {
        ImGuiContext &g = *GImGui;
        LvClearSlot   now[LV_DOCK_CLEAR_SLOTS];
        int           nowCount = 0;
        bool          same;

        /* The dock space has just been updated and may have dropped or
         * joined parts: only what is still there, in the same order. */
        root = ImGui::DockBuilderGetNode(dockId);
        collect_slots(root, axis, now, &nowCount, 0);
        same = nowCount == slotCount;
        for (int i = 0; same && i < nowCount; i++) same = now[i].id == slots[i].id;

        ImGuiWindow *inner = (root != nullptr) ? root->HostWindow : nullptr;
        bool overHost = g.HoveredWindow != nullptr &&
                        (g.HoveredWindow == outer || g.HoveredWindow == inner);
        for (int i = 0; same && i < nowCount; i++) {
            ImGuiDockNode *n = now[i].node;
            const int      p = now[i].panel;
            const float    farEdge = n->Pos[axis] + n->Size[axis];
            ImRect         box;
            if (wide) {
                box = ImRect(dockPos.x, farEdge, dockPos.x + dockSize.x, farEdge + slotGrip);
            } else {
                box = ImRect(farEdge, dockPos.y, farEdge + slotGrip, dockPos.y + dockSize.y);
            }
            ImGui::PushID(i);
            const ImGuiID id = outer->GetID("##lv_slot_grip");
            ImGui::PopID();

            bool held = g.ActiveId == id;
            bool hot = p >= 0 && overHost && box.Contains(g.IO.MousePos) &&
                       (g.ActiveId == 0 || held) && (g.HoveredId == 0 || g.HoveredId == id) &&
                       !g.DragDropActive;
            if (hot) {
                ImGui::SetHoveredID(id);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    /* As long as what it shows. */
                    int content = (int)ceilf(node_content(n, axis, 0));
                    s_len[side][p] = lvDockClearSnapLen((int)(n->Size[axis] + 0.5f), content, mins[i]);
                    s_len[side][p] = lvDockClearDragLen(&plan, mins, i, s_len[side][p], stripLen,
                                                        LV_DOCK_CLEAR_GRIP);
                } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    ImGui::SetActiveID(id, outer);
                    s_slot_grab = g.IO.MousePos[axis] - farEdge;
                    held = true;
                }
            }
            if (held) {
                if (p >= 0 && g.IO.MouseDown[ImGuiMouseButton_Left]) {
                    ImGui::KeepAliveID(id);
                    float len = g.IO.MousePos[axis] - s_slot_grab - n->Pos[axis];
                    s_len[side][p] = lvDockClearDragLen(&plan, mins, i, (int)(len + 0.5f),
                                                        stripLen, LV_DOCK_CLEAR_GRIP);
                    slotDrag = true;
                } else {
                    ImGui::ClearActiveID();
                    held = false;
                }
            }
            if (hot || held) {
                ImGui::SetMouseCursor(wide ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
            }
            /* Between two panels ImGui has drawn the line already, in its
             * own window; the last panel's is past it, in this one. */
            ImDrawList *dl = (i < nowCount - 1 && inner != nullptr) ? inner->DrawList
                                                                    : outer->DrawList;
            if (hot || held || dl == outer->DrawList) {
                dl->PushClipRect(box.Min, box.Max, false);
                dl->AddRectFilled(box.Min, box.Max,
                                  ImGui::GetColorU32(held ? ImGuiCol_SeparatorActive
                                                     : hot ? ImGuiCol_SeparatorHovered
                                                           : ImGuiCol_Separator));
                dl->PopClipRect();
            }
        }
    }
    s_slot_drag[side] = slotDrag;

    ImGui::End();

    /* The rest of the inner edge's grip, beside the clear part. */
    if (packed && plan.clearLen > 0) {
        ImVec2 restPos = gripPos, restSize = gripSize;
        restPos[axis] = pos[axis] + hostSize[axis];
        restSize[axis] = size[axis] - hostSize[axis];
        if (restSize[axis] >= 1.0f) {
            char name[64];
            snprintf(name, sizeof(name), "%s_edge", s.hostName);
            ImGui::SetNextWindowPos(restPos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(restSize, ImGuiCond_Always);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
            ImGui::Begin(name, nullptr, k_bare_flags | ImGuiWindowFlags_NoBackground |
                                            ImGuiWindowFlags_NoFocusOnAppearing);
            ImGui::PopStyleVar(3);
            if (edge_grip(side, restPos, restSize, pos, size, other, extent, false)) {
                s.gripHeld = true;
            }
            ImGui::End();
        }
    }
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
