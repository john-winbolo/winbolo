/*
 * imgui_lv_panel.cpp - what every panel of the Log Viewer has: its options
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "imgui_lv_panel.h"
#include "imgui_lv_icons.h"
#include "imgui.h"
#include "imgui_internal.h"  /* the title bar, the dock node, the draw lists of child windows */
#include "platform_config.h"
#include "../../gui/lang.h"

#include <cstdio>
#include <cstring>

extern "C" {
    #include "../lv_panel_opts.h"
}

/* How far a block's backing reaches past what was drawn, and the gap up to
 * which two blocks count as one (twice this). */
#define LV_PANEL_BACKING_PAD 3.0f

/* How see-through the backing is: this much of the background's own
 * opacity. Dark enough to read text over the map's brightest squares, light
 * enough to tell the backing from a panel that is not transparent. */
#define LV_PANEL_BACKING_ALPHA 0.75f

/* The most blocks one panel gets a backing for; what is below the last one
 * becomes part of it. */
#define LV_PANEL_BACKING_MAX 24

/* How strong the gear is while the mouse is not over it. */
#define LV_PANEL_GEAR_DIM 0.45f

struct LvPanelState {
    bool transparent;
};

static LvPanelState s_panel[LV_PANEL_OPT_COUNT];
static bool         s_loaded = false;

/* The panel between lv_panel_begin and lv_panel_end. */
struct LvPanelCurrent {
    int          index;       /* in lvPanelOptNames, -1 for none */
    bool         begun;       /* Begin answered true */
    bool         split;       /* the draw list is split for the backing */
    int          colours;     /* style colours to pop at the end */
    ImGuiWindow *window;
    ImVec4       bg;          /* the background and the border as the style has them */
    ImVec4       border;
    bool         cornerGear;  /* the gear is drawn at the end, over the panel */
    bool         gearHot;
    LvPanelGear  gear;
};

static LvPanelCurrent s_cur = { -1, false, false, 0, nullptr, ImVec4(), ImVec4(), false, false,
                                { 0.0f, 0.0f, 0.0f } };

/* The rows of the panel that is being closed. Too big for the stack to be
 * comfortable, and one panel at a time uses it. */
static LvBacking s_backing;

static void load_once(void) {
    if (s_loaded) return;
    s_loaded = true;
    for (int i = 0; i < LV_PANEL_OPT_COUNT; i++) {
        char key[64];
        char val[16];
        s_panel[i].transparent = false;
        if (!lvPanelOptKey(i, key, sizeof(key))) continue;
        lv_platform_config_get_string("LOGVIEWER", key, "No", val, sizeof(val));
        s_panel[i].transparent = (val[0] == 'Y' || val[0] == 'y');
    }
}

void lv_panel_options_save(void) {
    /* Nothing was read, so nothing can have changed: the preferences are
     * still the truth. */
    if (!s_loaded) return;
    for (int i = 0; i < LV_PANEL_OPT_COUNT; i++) {
        char key[64];
        if (!lvPanelOptKey(i, key, sizeof(key))) continue;
        lv_platform_config_set_string("LOGVIEWER", key, s_panel[i].transparent ? "Yes" : "No");
    }
}

unsigned lv_panel_options_signature(void) {
    unsigned sig = 0;
    for (int i = 0; i < LV_PANEL_OPT_COUNT; i++) {
        if (s_panel[i].transparent) sig |= 1u << i;
    }
    return sig;
}

bool lv_panel_get_transparent(const char *id) {
    load_once();
    int i = lvPanelOptIndex(id);
    return i >= 0 && s_panel[i].transparent;
}

void lv_panel_set_transparent(const char *id, bool on) {
    load_once();
    int i = lvPanelOptIndex(id);
    if (i >= 0) s_panel[i].transparent = on;
}

/* The gear itself, in the box of g. */
static void draw_gear(ImDrawList *dl, const LvPanelGear &g, bool hot) {
    ImVec2 p0(g.x, g.y), p1(g.x + g.side, g.y + g.side);
    ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (hot) {
        /* The round patch the close button has under the mouse. */
        ImVec2 mid((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
        dl->AddCircleFilled(mid, g.side * 0.5f + 1.0f, ImGui::GetColorU32(ImGuiCol_ButtonHovered));
    } else {
        text.w *= LV_PANEL_GEAR_DIM;
    }
    /* A little smaller than its box, as the X of the close button is. */
    const float in = (float)(int)(g.side * 0.14f);
    lv_icon_draw(dl, LV_ICON_GEAR, ImVec2(p0.x + in, p0.y + in), ImVec2(p1.x - in, p1.y - in),
                 ImGui::ColorConvertFloat4ToU32(text));
}

/* Place the gear, see whether the mouse is on it, and run the popup it
 * opens. The window is the current one and Begin answered true. */
static void gear_and_options(ImGuiWindow *w, bool hasClose) {
    ImGuiContext     &g = *GImGui;
    const ImGuiStyle &style = g.Style;
    ImGuiDockNode    *node = w->DockIsActive ? w->DockNode : nullptr;
    ImGuiWindow      *host = nullptr;
    ImDrawList       *barList = nullptr;
    ImRect            bar;
    ImU32             barCol = 0;

    if (node != nullptr && node->HostWindow != nullptr && node->TabBar != nullptr &&
        !node->IsHiddenTabBar() && !node->IsNoTabBar()) {
        /* A tab: the bar of tabs of the node the panel is docked in. */
        host = node->HostWindow;
        bar = ImRect(node->Pos.x, node->Pos.y, node->Pos.x + node->Size.x,
                     node->Pos.y + g.FontSize + style.FramePadding.y * 2.0f);
        s_cur.gear = lvPanelGearInBar(bar.Min.x, bar.Min.y, bar.Max.x, g.FontSize,
                                      style.FramePadding.x, style.FramePadding.y,
                                      style.ItemInnerSpacing.x, style.WindowBorderSize,
                                      node->HasCloseButton);
        barList = host->DrawList;
        barCol = ImGui::GetColorU32(node->IsFocused ? ImGuiCol_TitleBgActive : ImGuiCol_TitleBg);
    } else if (node == nullptr && !(w->Flags & ImGuiWindowFlags_NoTitleBar)) {
        /* A window of its own: its title bar. */
        bar = w->TitleBarRect();
        s_cur.gear = lvPanelGearInBar(bar.Min.x, bar.Min.y, bar.Max.x, g.FontSize,
                                      style.FramePadding.x, style.FramePadding.y,
                                      style.ItemInnerSpacing.x, 0.0f, hasClose);
        barList = w->DrawList;
        bool focused = g.NavWindow != nullptr &&
                       g.NavWindow->RootWindowForTitleBarHighlight ==
                           w->RootWindowForTitleBarHighlight;
        barCol = ImGui::GetColorU32(focused ? ImGuiCol_TitleBgActive : ImGuiCol_TitleBg);
    } else {
        /* No bar: the corner of the panel, drawn last so it is over what the
         * panel shows. */
        s_cur.gear = lvPanelGearInCorner(w->InnerRect.Min.x, w->InnerRect.Min.y,
                                         w->InnerRect.Max.x, w->InnerRect.Max.y,
                                         g.FontSize, 2.0f);
        s_cur.cornerGear = true;
    }

    /* The mouse. The gear is not an item of the window: in a bar of tabs it
     * is not even inside the window. So the test is made here, and the gear
     * takes the "hovered" ID so the title bar under it does not start a
     * drag of the window on the click. */
    const ImRect  box(s_cur.gear.x, s_cur.gear.y, s_cur.gear.x + s_cur.gear.side,
                      s_cur.gear.y + s_cur.gear.side);
    const ImGuiID gearId = w->GetID("##lvpanelgear");
    bool overWindow = g.HoveredWindow != nullptr &&
                      (g.HoveredWindow == host || g.HoveredWindow == w ||
                       g.HoveredWindow->RootWindow == w);
    bool hot = overWindow && box.Contains(g.IO.MousePos) &&
               (g.ActiveId == 0 || g.ActiveId == gearId) &&
               (g.HoveredId == 0 || g.HoveredId == gearId || g.HoveredIdAllowOverlap) &&
               !g.DragDropActive && ImGui::IsWindowContentHoverable(w, ImGuiHoveredFlags_None);
    if (hot) {
        ImGui::SetHoveredID(gearId);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            ImGui::OpenPopup("##lvpanelopts");
        } else if (!ImGui::IsPopupOpen("##lvpanelopts")) {
            ImGui::SetTooltip("%s", langGetText(STR_LV_TIP_PANEL_OPTIONS));
        }
    }
    s_cur.gearHot = hot;

    if (barList != nullptr) {
        barList->PushClipRect(bar.Min, bar.Max, false);
        /* A patch in the bar's colour first: a long title, or a tab, can
         * reach under the gear. */
        barList->AddRectFilled(ImVec2(box.Min.x - 1.0f, box.Min.y - 1.0f),
                               ImVec2(box.Max.x + 1.0f, box.Max.y + 1.0f), barCol);
        draw_gear(barList, s_cur.gear, hot);
        barList->PopClipRect();
    }

    /* The options. The popup is a window of its own; it gets the border the
     * style has, also when the panel's own is switched off. */
    ImGui::PushStyleColor(ImGuiCol_Border, s_cur.border);
    if (ImGui::BeginPopup("##lvpanelopts")) {
        if (s_cur.index >= 0) {
            ImGui::MenuItem(langGetText(STR_LV_PANEL_TRANSPARENT), nullptr,
                            &s_panel[s_cur.index].transparent);
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
}

bool lv_panel_begin(const char *id, const char *title, bool *p_open, ImGuiWindowFlags flags) {
    load_once();

    const ImGuiStyle &style = ImGui::GetStyle();
    s_cur.index = lvPanelOptIndex(id);
    s_cur.begun = false;
    s_cur.split = false;
    s_cur.colours = 0;
    s_cur.window = nullptr;
    s_cur.bg = style.Colors[ImGuiCol_WindowBg];
    s_cur.border = style.Colors[ImGuiCol_Border];
    s_cur.cornerGear = false;
    s_cur.gearHot = false;

    /* As the frame started: a click on the option this frame shows from the
     * next one, so what is pushed here is what is popped at the end. */
    const bool transparent = s_cur.index >= 0 && s_panel[s_cur.index].transparent;
    if (transparent) {
        const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
        /* No background of a list or a table inside the panel, and no lines
         * that run the whole length of one: they would make all of it count
         * as used. The panel's own border is drawn at the end, when it is
         * to show. */
        ImGui::PushStyleColor(ImGuiCol_ChildBg, clear);
        ImGui::PushStyleColor(ImGuiCol_Border, clear);
        ImGui::PushStyleColor(ImGuiCol_TableRowBg, clear);
        ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, clear);
        ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, clear);
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, clear);
        s_cur.colours = 6;
        /* The window's own background is drawn by Begin, so this one is
         * only needed until Begin is back. */
        ImGui::PushStyleColor(ImGuiCol_WindowBg, clear);
    }

    bool open = ImGui::Begin(title, p_open, flags);
    if (transparent) ImGui::PopStyleColor();

    ImGuiWindow *w = ImGui::GetCurrentWindow();
    s_cur.window = w;
    s_cur.begun = open;
    if (!open) return false;

    gear_and_options(w, p_open != nullptr);

    if (transparent) {
        /* What the panel draws from here goes into the second of two
         * layers, so the backing can be put under it once it is known where
         * it is. */
        w->DrawList->ChannelsSplit(2);
        w->DrawList->ChannelsSetCurrent(1);
        s_cur.split = true;
    }
    return true;
}

/* Add the box of every triangle of a draw list to the rows, cut to what
 * shows of it (its command's clip rectangle, and the panel's inside). */
static void scan_draw_list(const ImDrawList *dl, const ImRect &area) {
    for (int c = 0; c < dl->CmdBuffer.Size; c++) {
        const ImDrawCmd &cmd = dl->CmdBuffer[c];
        if (cmd.UserCallback != nullptr || cmd.ElemCount < 3) continue;
        ImRect clip(cmd.ClipRect.x, cmd.ClipRect.y, cmd.ClipRect.z, cmd.ClipRect.w);
        clip.ClipWith(area);
        if (clip.Min.x >= clip.Max.x || clip.Min.y >= clip.Max.y) continue;
        if ((int)(cmd.IdxOffset + cmd.ElemCount) > dl->IdxBuffer.Size) continue;
        const ImDrawIdx *idx = dl->IdxBuffer.Data + cmd.IdxOffset;
        const int        vtxCount = dl->VtxBuffer.Size;
        for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
            ImRect box(1.0e30f, 1.0e30f, -1.0e30f, -1.0e30f);
            bool   ok = true;
            for (int k = 0; k < 3; k++) {
                int v = (int)cmd.VtxOffset + (int)idx[i + k];
                if (v < 0 || v >= vtxCount) { ok = false; break; }
                box.Add(dl->VtxBuffer.Data[v].pos);
            }
            if (!ok) continue;
            box.ClipWith(clip);
            lvBackingAdd(&s_backing, box.Min.x, box.Min.y, box.Max.x, box.Max.y);
        }
    }
}

/* The same for the lists and tables inside the panel: each is a window with
 * a draw list of its own, finished by the time the panel is. */
static void scan_children(const ImGuiWindow *w, const ImRect &area, int depth) {
    if (depth > 4) return;
    for (int i = 0; i < w->DC.ChildWindows.Size; i++) {
        const ImGuiWindow *child = w->DC.ChildWindows[i];
        if (child == nullptr || !child->Active || child->Hidden) continue;
        if (!(child->Flags & ImGuiWindowFlags_ChildWindow)) continue;
        scan_draw_list(child->DrawList, area);
        scan_children(child, area, depth + 1);
    }
}

void lv_panel_end(void) {
    ImGuiWindow *w = s_cur.window;

    if (s_cur.begun && w != nullptr && w == ImGui::GetCurrentWindow()) {
        ImGuiContext &g = *GImGui;
        ImDrawList   *dl = w->DrawList;

        if (s_cur.split) {
            /* The panel's inside: under the title bar, inside the scroll
             * bars. */
            const ImRect area = w->InnerRect;
            lvBackingReset(&s_backing, area.Min.y, area.GetHeight());
            /* While the list is split, its commands are those of the layer
             * the panel drew into. */
            scan_draw_list(dl, area);
            scan_children(w, area, 0);

            LvBackingRect blocks[LV_PANEL_BACKING_MAX];
            int count = lvBackingBlocks(&s_backing, LV_PANEL_BACKING_PAD, blocks,
                                        LV_PANEL_BACKING_MAX);
            ImVec4 bg = s_cur.bg;
            bg.w *= LV_PANEL_BACKING_ALPHA;
            const ImU32 col = ImGui::ColorConvertFloat4ToU32(bg);

            dl->ChannelsSetCurrent(0);
            dl->PushClipRect(area.Min, area.Max, false);
            for (int i = 0; i < count; i++) {
                ImRect r(blocks[i].x0, blocks[i].y0, blocks[i].x1, blocks[i].y1);
                r.ClipWith(area);
                if (r.Min.x < r.Max.x && r.Min.y < r.Max.y) {
                    dl->AddRectFilled(r.Min, r.Max, col, 3.0f);
                }
            }
            dl->PopClipRect();
            dl->ChannelsMerge();

            /* The border, only while the user is at the panel: the mouse is
             * over it, or it is being moved or resized. */
            bool atPanel = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                                  ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
                           s_cur.gearHot ||
                           (g.MovingWindow != nullptr && g.MovingWindow->RootWindow == w) ||
                           (g.ActiveId != 0 && g.ActiveIdWindow == w);
            if (atPanel) {
                dl->PushClipRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y), false);
                dl->AddRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y),
                            ImGui::ColorConvertFloat4ToU32(s_cur.border), w->WindowRounding, 0,
                            1.0f);
                dl->PopClipRect();
            }
        }

        if (s_cur.cornerGear) {
            dl->PushClipRect(w->InnerRect.Min, w->InnerRect.Max, false);
            draw_gear(dl, s_cur.gear, s_cur.gearHot);
            dl->PopClipRect();
        }
    }

    if (s_cur.colours > 0) ImGui::PopStyleColor(s_cur.colours);
    ImGui::End();

    s_cur.index = -1;
    s_cur.begun = false;
    s_cur.split = false;
    s_cur.colours = 0;
    s_cur.window = nullptr;
    s_cur.cornerGear = false;
}
