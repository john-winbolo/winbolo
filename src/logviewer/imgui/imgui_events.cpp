/*
 * imgui_events.cpp - ImGui events window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "imgui_events.h"
#include "imgui_main_menu.h"
#include "imgui_context.h"
#include "imgui.h"
#include "../../gui/lang.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

/* External function from backend */
extern "C" {
    void lv_screenFormatTime(uint32_t absMs, char *dest, size_t destSize);
    uint32_t lv_screenGetTimeRunning(void);
    uint32_t lv_screenWindowStartMs(void);
    void lv_windowSeekToHighlight(uint32_t ms, int mapX, int mapY);
}

/* Event with associated playback timestamp, held in absolute log ms so the
 * displayed stamp can be rebuilt each frame against the presentation window.
 * Highlight clips also carry a seek target and a map cell so a click can jump
 * the scrubber and centre the view. */
struct LogEvent {
    std::string text;
    uint32_t timeMs;
    bool     showTime = false;  /* prefix the displayed line with its stamp */
    bool     pinned   = false;  /* load-time summary: never hidden with the lobby */
    bool     seekable = false;
    uint32_t seekMs = 0;
    int      mapX = 0;
    int      mapY = 0;
};

/* Event list storage */
static std::vector<LogEvent> s_events;
static bool s_auto_scroll = true;
static bool s_scroll_to_bottom = false;

/* Selection state for copy functionality */
static int s_selected_index = -1;
static bool s_select_all = false;

void lv_imgui_events_init(void) {
    s_events.clear();
    s_auto_scroll = true;
    s_scroll_to_bottom = false;
    s_selected_index = -1;
    s_select_all = false;
}

void lv_imgui_events_add(int eventType, const char *msg) {
    LogEvent e;
    e.text     = std::string(msg);
    e.timeMs   = lv_screenGetTimeRunning();
    e.showTime = (eventType == 0);
    s_events.push_back(e);

    /* Mark for auto-scroll */
    if (s_auto_scroll) {
        s_scroll_to_bottom = true;
    }
}

void lv_imgui_events_add_highlight(const char *msg, uint32_t seekMs, int mapX,
                                   int mapY) {
    LogEvent e;
    e.text = std::string(msg);
    e.timeMs = 0;   /* load-time summary line: never rewound away on a seek */
    e.pinned = true;
    e.seekable = true;
    e.seekMs = seekMs;
    e.mapX = mapX;
    e.mapY = mapY;
    s_events.push_back(e);

    if (s_auto_scroll) {
        s_scroll_to_bottom = true;
    }
}

/* Load-time round-summary line (awards, section headers): pinned above the feed
 * with no stamp of its own, so it is neither hidden with the lobby nor removed
 * by a rewind. */
void lv_imgui_events_add_summary(const char *msg) {
    LogEvent e;
    e.text   = std::string(msg);
    e.timeMs = 0;
    e.pinned = true;
    s_events.push_back(e);

    if (s_auto_scroll) {
        s_scroll_to_bottom = true;
    }
}

void lv_imgui_events_clear(void) {
    s_events.clear();
    s_selected_index = -1;
    s_select_all = false;
}

void lv_imgui_events_remove_after(unsigned int timeMs) {
    /* Remove events with timestamp strictly after timeMs */
    while (!s_events.empty() && s_events.back().timeMs > timeMs) {
        s_events.pop_back();
    }
    if (s_selected_index >= (int)s_events.size()) {
        s_selected_index = -1;
        s_select_all = false;
    }
}

int lv_imgui_events_get_count(void) {
    return (int)s_events.size();
}

const char *lv_imgui_events_get_text(int i) {
    if (i < 0 || i >= (int)s_events.size()) return "";
    return s_events[i].text.c_str();
}

uint32_t lv_imgui_events_get_time(int i) {
    if (i < 0 || i >= (int)s_events.size()) return 0;
    return s_events[i].timeMs;
}

/* Copy selected text to clipboard */
static void copy_to_clipboard(const char* text) {
    ImGui::SetClipboardText(text);
}

/* Build the line as displayed. Highlight clips read their stamp from the seek
 * target; ordinary timestamped lines from their own time. Both go through
 * lv_screenFormatTime, so the whole panel follows the presentation window. */
static std::string compose_line(const LogEvent &e) {
    char stamp[16];
    if (e.seekable) {
        lv_screenFormatTime(e.seekMs, stamp, sizeof(stamp));
        return std::string("Highlight ") + stamp + " \xe2\x80\x94 " + e.text;
    }
    if (e.showTime) {
        lv_screenFormatTime(e.timeMs, stamp, sizeof(stamp));
        return std::string(stamp) + " - " + e.text;
    }
    return e.text;
}

/* Lobby-period lines stay in the vector — un-ticking Hide Lobby brings them
 * straight back — but out of the panel, where they would sit at 00:00. */
static bool event_is_hidden(const LogEvent &e) {
    if (e.pinned) return false;
    return e.timeMs < lv_screenWindowStartMs();
}

void lv_imgui_events_window(void) {
    if (!lv_g_show_events_window) {
        return;
    }
    
    {
        ImGuiCond cond = lv_g_reset_window_positions ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        ImVec2 vp = ImGui::GetMainViewport()->Size;
        ImGui::SetNextWindowSize(ImVec2(320, 200), cond);
        ImGui::SetNextWindowPos(ImVec2(vp.x - 320 - 10, vp.y - 200 - 10), cond);
    }

    char ev_title[128];
    snprintf(ev_title, sizeof(ev_title), "%s###events", langGetText(STR_LV_WIN_EVENTS));
    if (ImGui::Begin(ev_title, &lv_g_show_events_window, ImGuiWindowFlags_NoCollapse)) {
        /* Reposition window relative to right/bottom edge when viewport is resized */
        float dx, dy;
        if (lv_imgui_context_get_resize_delta(&dx, &dy)) {
            ImVec2 pos = ImGui::GetWindowPos();
            float old_vp_x = ImGui::GetMainViewport()->Size.x - dx;
            float old_vp_y = ImGui::GetMainViewport()->Size.y - dy;
            ImVec2 size = ImGui::GetWindowSize();
            if (pos.x + size.x * 0.5f > old_vp_x * 0.5f) ImGui::SetWindowPos(ImVec2(pos.x + dx, pos.y + dy));
            else if (pos.y + size.y * 0.5f > old_vp_y * 0.5f) ImGui::SetWindowPos(ImVec2(pos.x, pos.y + dy));
        }
        /* Always clamp to keep fully on screen */
        {
            ImVec2 pos = ImGui::GetWindowPos();
            ImVec2 size = ImGui::GetWindowSize();
            ImVec2 vp = ImGui::GetMainViewport()->Size;
            ImVec2 new_pos = pos;
            if (new_pos.x + size.x > vp.x) new_pos.x = vp.x - size.x;
            if (new_pos.y + size.y > vp.y) new_pos.y = vp.y - size.y;
            if (new_pos.x < 0) new_pos.x = 0;
            if (new_pos.y < 0) new_pos.y = 0;
            if (new_pos.x != pos.x || new_pos.y != pos.y) ImGui::SetWindowPos(new_pos);
        }

        /* In-panel Auto Scroll toggle — always visible, mirrors the
         * right-click menu item below.  Reachable from controller/touch
         * where right-click is unavailable. */
        ImGui::Checkbox(langGetText(STR_LV_AUTO_SCROLL), &s_auto_scroll);
        ImGui::Separator();

        /* List box for events */
        ImGui::BeginChild("EventsList", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
        
        for (int i = 0; i < (int)s_events.size(); i++) {
            if (event_is_hidden(s_events[i])) {
                continue;
            }
            std::string composed = compose_line(s_events[i]);
            const char* event_text = composed.c_str();

            /* Check if this item is selected */
            bool is_selected = (s_select_all) || (s_selected_index == i);
            
            /* Selectable item */
            char selectId[544];
            snprintf(selectId, sizeof(selectId), "%s##evt%d", event_text, i);
            if (ImGui::Selectable(selectId, is_selected)) {
                /* Ctrl+Click for multi-select (simplified - just toggle selection) */
                if (ImGui::GetIO().KeyCtrl) {
                    if (s_selected_index == i) {
                        s_selected_index = -1;
                    } else {
                        s_selected_index = i;
                    }
                } else {
                    s_selected_index = i;
                    s_select_all = false;
                    /* A plain click on a highlight clip jumps the scrubber to a
                     * few seconds before the moment and centres the map on it. */
                    if (s_events[i].seekable) {
                        uint32_t target = s_events[i].seekMs > 5000u
                                              ? s_events[i].seekMs - 5000u
                                              : 0u;
                        lv_windowSeekToHighlight(target, s_events[i].mapX,
                                                 s_events[i].mapY);
                    }
                }
            }
            
            /* Right-click context menu on item */
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem(langGetText(STR_LV_COPY))) {
                    copy_to_clipboard(event_text);
                }
                if (ImGui::MenuItem(langGetText(STR_LV_COPY_ALL))) {
                    /* Build string of all shown events */
                    std::string all_events;
                    for (const auto& e : s_events) {
                        if (event_is_hidden(e)) continue;
                        all_events += compose_line(e) + "\r\n";
                    }
                    copy_to_clipboard(all_events.c_str());
                }
                if (ImGui::MenuItem(langGetText(STR_MENU_SELECT_ALL))) {
                    s_select_all = true;
                }
                ImGui::EndPopup();
            }
        }
        
        /* Auto-scroll to bottom if new event was added */
        if (s_scroll_to_bottom && s_auto_scroll) {
            ImGui::SetScrollHereY(1.0f);
            s_scroll_to_bottom = false;
        }
        
        ImGui::EndChild();
        
        /* Right-click context menu in empty space */
        if (ImGui::BeginPopupContextWindow()) {
            if (ImGui::MenuItem(langGetText(STR_MENU_SELECT_ALL), NULL, s_select_all)) {
                s_select_all = !s_select_all;
            }
            if (ImGui::MenuItem(langGetText(STR_LV_COPY_ALL), NULL, false, !s_events.empty())) {
                std::string all_events;
                for (const auto& e : s_events) {
                    if (event_is_hidden(e)) continue;
                    all_events += compose_line(e) + "\r\n";
                }
                copy_to_clipboard(all_events.c_str());
            }
            if (ImGui::MenuItem(langGetText(STR_LV_CLEAR_ALL), NULL, false, !s_events.empty())) {
                lv_imgui_events_clear();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(langGetText(STR_LV_AUTO_SCROLL), NULL, s_auto_scroll)) {
                s_auto_scroll = !s_auto_scroll;
            }
            ImGui::EndPopup();
        }
    }
    ImGui::End();
}