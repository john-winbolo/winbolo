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
    void lv_screenGetTime(char *buffer);
    uint32_t lv_screenGetTimeRunning(void);
}

/* Event with associated playback timestamp */
struct LogEvent {
    std::string text;
    uint32_t timeMs;
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
    char line[512] = {0};

    if (eventType == 0) {
        /* Add timestamp prefix */
        lv_screenGetTime(line);
        strncat(line, " - ", sizeof(line) - strlen(line) - 1);
    }

    strncat(line, msg, sizeof(line) - strlen(line) - 1);
    s_events.push_back({std::string(line), lv_screenGetTimeRunning()});

    /* Mark for auto-scroll */
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
            const char* event_text = s_events[i].text.c_str();
            
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
                }
            }
            
            /* Right-click context menu on item */
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem(langGetText(STR_LV_COPY))) {
                    copy_to_clipboard(event_text);
                }
                if (ImGui::MenuItem(langGetText(STR_LV_COPY_ALL))) {
                    /* Build string of all events */
                    std::string all_events;
                    for (const auto& e : s_events) {
                        all_events += e.text + "\r\n";
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
                    all_events += e.text + "\r\n";
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