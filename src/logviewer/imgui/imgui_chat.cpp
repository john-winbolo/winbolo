/*
 * imgui_chat.cpp - ImGui Chat window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * The chat the players typed, at the playback time: the last lines, oldest
 * at the top, each with its time, its sender in the colour of the sender's
 * team, a note for a line to a team or to one player, and the words.
 *
 * The lines are the chat lines of the Events panel's list (lv_event_list.h),
 * which keeps each one's parts (lvEventListSetChat). Map pings are filed with
 * the chat in that list but are not typed, and are left out here.
 *
 * When no new line has come for a while the lines fade out, and a new line
 * shows at full opacity at once (lv_chat_view.h). All of it follows the
 * playback time, so a pause, a speed change or a seek shows what that time
 * shows. Docked in a sidebar, the panel keeps its frame and only the lines
 * fade. Floating, the whole window fades, title and background too. With
 * the mouse over the panel the lines are at full opacity, so they can be
 * read.
 */

#include "imgui_chat.h"
#include "imgui_main_menu.h"
#include "imgui_dock.h"
#include "imgui_context.h"
#include "imgui.h"
#include "../lv_event_list.h"
#include "../lv_chat_view.h"
#include "../../gui/lang.h"

#include <cstdint>
#include <cstdio>
#include <vector>

extern "C" {
    /* External functions from backend - using C types directly, as the
     * other panels do. */
    uint32_t lv_screenGetTimeRunning(void);
    uint32_t lv_screenWindowStartMs(void);
    void lv_screenFormatTime(uint32_t absMs, char *dest, size_t destSize);
    unsigned char lv_screenTeamColour(int team);
    void lv_drawTeamPanelColour(int palette, float *r, float *g, float *b);
    unsigned lv_imgui_events_version(void);
}

/* The floating window's default size. */
#define CHAT_DEFAULT_W 380.0f
#define CHAT_DEFAULT_H 190.0f

/* The chat lines of the list: each one's index in the list, and its time for
 * lvChatVisible. Rebuilt when the list changes (lv_imgui_events_version). */
static std::vector<int>      s_lines;
static std::vector<uint32_t> s_times;
static unsigned              s_lines_version = 0;

/* Whether the mouse was over the window last frame. A floating window's
 * opacity is set before it begins, so it takes last frame's answer. */
static bool s_hovered = false;

/* The end of the lines shown last frame, so the list scrolls to the newest
 * line only when a line comes or goes, and leaves hand scrolling alone in
 * between. */
static int s_last_end = -1;

static void rebuild_lines(void) {
    const unsigned version = lv_imgui_events_version();
    if (version == s_lines_version) return;
    s_lines_version = version;
    s_lines.clear();
    s_times.clear();
    const int count = lvEventListCount();
    for (int i = lvEventListPinnedCount(); i < count; i++) {
        const LvEvent *e = lvEventListAt(i);
        if (e == nullptr || e->chatFrom == nullptr || e->chatText == nullptr) continue;
        s_lines.push_back(i);
        s_times.push_back(e->timeMs);
    }
    s_last_end = -1;
}

/* The colour a sender's name is drawn in: the colour every panel draws the
 * team in (lv_drawTeamPanelColour). A sender with no team (a spectator)
 * is in the text colour. */
static ImVec4 sender_colour(int team) {
    if (team < 0) return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    float r = 0.0f, g = 0.0f, b = 0.0f;
    lv_drawTeamPanelColour(lv_screenTeamColour(team), &r, &g, &b);
    return ImVec4(r, g, b, 1.0f);
}

static void chat_line(const LvEvent &e) {
    char stamp[16];
    lv_screenFormatTime(e.timeMs, stamp, sizeof(stamp));
    ImGui::TextDisabled("%s", stamp);
    ImGui::SameLine();
    ImGui::TextColored(sender_colour(e.team), "%s", e.chatFrom);
    if (e.chatNote != nullptr) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", e.chatNote);
    }
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextUnformatted(":");
    ImGui::SameLine();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(e.chatText);
    ImGui::PopTextWrapPos();
}

void lv_imgui_chat_window(void) {
    if (!lv_g_show_chat_window) {
        s_hovered = false;
        return;
    }

    rebuild_lines();
    const uint32_t nowMs = lv_screenGetTimeRunning();
    int first = 0, end = 0;
    lvChatVisible(s_times.data(), (int)s_times.size(), nowMs, lv_screenWindowStartMs(),
                  LV_CHAT_MAX_LINES, &first, &end);
    const int shown = end - first;
    const uint32_t newestMs = (shown > 0) ? s_times[(size_t)end - 1] : 0;

    {
        /* A panel that sat in the dock when the dock was turned off goes
         * back to its default place, the same as a reset. */
        bool to_default = lv_g_reset_window_positions ||
                          lv_imgui_dock_take_float_reset("###chat");
        ImGuiCond cond = to_default ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        ImVec2 vp = ImGui::GetMainViewport()->Size;
        ImGui::SetNextWindowSize(ImVec2(CHAT_DEFAULT_W, CHAT_DEFAULT_H), cond);
        /* Low on the left of the map, over the Controls panel's place. */
        float y = vp.y - CHAT_DEFAULT_H - 200.0f;
        if (y < 30.0f) y = 30.0f;
        ImGui::SetNextWindowPos(ImVec2(230.0f, y), cond);
    }

    /* Docked or not as of last frame: a floating window fades whole, so its
     * opacity is set before it begins. */
    const bool docked = lv_imgui_dock_window_is_docked("###chat");
    const float baseAlpha = ImGui::GetStyle().Alpha;
    const bool fadeWindow = !docked;
    if (fadeWindow) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                            baseAlpha * lvChatShownAlpha(shown, newestMs, nowMs, s_hovered ? 1 : 0));
    }

    char title[128];
    snprintf(title, sizeof(title), "%s###chat", langGetText(STR_LV_WIN_CHAT));
    const bool open = ImGui::Begin(title, &lv_g_show_chat_window, ImGuiWindowFlags_NoCollapse);
    s_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    if (open) {
        lv_imgui_keep_window_on_screen();
        /* Docked: the frame stays and only the lines fade, by this frame's
         * hover. */
        if (!fadeWindow) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                baseAlpha * lvChatShownAlpha(shown, newestMs, nowMs, s_hovered ? 1 : 0));
        }
        if (shown <= 0) {
            ImGui::TextDisabled("%s", langGetText(STR_LV_CHAT_NONE));
        } else {
            for (int k = first; k < end; k++) {
                const LvEvent *e = lvEventListAt(s_lines[(size_t)k]);
                if (e != nullptr) chat_line(*e);
            }
        }
        if (end != s_last_end) {
            /* A line came or went: the newest is at the bottom. */
            ImGui::SetScrollHereY(1.0f);
            s_last_end = end;
        }
        if (!fadeWindow) {
            ImGui::PopStyleVar();
        }
    }
    ImGui::End();
    if (fadeWindow) {
        ImGui::PopStyleVar();
    }
}
