/*
 * imgui_events.cpp - ImGui events window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "imgui_events.h"
#include "../lv_event_list.h"
#include "imgui_main_menu.h"
#include "imgui_context.h"
#include "imgui.h"
#include "../../gui/lang.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

/* External function from backend */
extern "C" {
    void lv_screenFormatTime(uint32_t absMs, char *dest, size_t destSize);
    uint32_t lv_screenGetTimeRunning(void);
    uint32_t lv_screenWindowStartMs(void);
    void lv_windowSeekToHighlight(uint32_t ms, int mapX, int mapY);
}

/* Auto-scroll state */
static bool s_auto_scroll = true;
static bool s_scroll_to_bottom = false;
/* The end of the shown lines last frame, so a seek that moves it scrolls
   like a new line does. */
static int s_last_shown_end = -1;

/* Selection state for copy functionality */
static int s_selected_index = -1;
static bool s_select_all = false;

void lv_imgui_events_init(void) {
    lvEventListClear();
    s_auto_scroll = true;
    s_scroll_to_bottom = false;
    s_last_shown_end = -1;
    s_selected_index = -1;
    s_select_all = false;
}

void lv_imgui_events_add(int eventType, const char *msg) {
    lvEventListAdd(msg, lv_screenGetTimeRunning(), eventType == 0);

    /* Mark for auto-scroll */
    if (s_auto_scroll) {
        s_scroll_to_bottom = true;
    }
}

void lv_imgui_events_add_highlight(const char *msg, uint32_t seekMs, int mapX,
                                   int mapY) {
    /* Load-time summary line: pinned, so never hidden by the playhead. */
    lvEventListAddPinned(msg, true, seekMs, mapX, mapY);

    if (s_auto_scroll) {
        s_scroll_to_bottom = true;
    }
}

/* Load-time round-summary line (awards, section headers): pinned above the feed
 * with no stamp of its own, so it is hidden neither with the lobby nor by the
 * playhead. */
void lv_imgui_events_add_summary(const char *msg) {
    lvEventListAddPinned(msg, false, 0, 0, 0);

    if (s_auto_scroll) {
        s_scroll_to_bottom = true;
    }
}

void lv_imgui_events_clear(void) {
    lvEventListClear();
    s_last_shown_end = -1;
    s_selected_index = -1;
    s_select_all = false;
}

void lv_imgui_events_rewound(void) {
    lvEventListRewound();
}

int lv_imgui_events_get_count(void) {
    return lvEventListCount();
}

const char *lv_imgui_events_get_text(int i) {
    const LvEvent *e = lvEventListAt(i);
    return e != NULL ? e->text : "";
}

uint32_t lv_imgui_events_get_time(int i) {
    const LvEvent *e = lvEventListAt(i);
    return e != NULL ? e->timeMs : 0;
}

/* Copy selected text to clipboard */
static void copy_to_clipboard(const char* text) {
    ImGui::SetClipboardText(text);
}

/* Build the line as displayed. Highlight clips read their stamp from the seek
 * target; ordinary timestamped lines from their own time. Both go through
 * lv_screenFormatTime, so the whole panel follows the presentation window. */
static std::string compose_line(const LvEvent &e) {
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

/* The timed lines shown: from the start of the presentation window up to the
 * playhead. Lobby-period lines stay in the list — un-ticking Hide Lobby brings
 * them straight back — and so do lines ahead of the playhead, which come back
 * as it reaches them. */
static void shown_range(int *first, int *end) {
    lvEventListRange(lv_screenWindowStartMs(), lv_screenGetTimeRunning(), first, end);
}

/* Every shown line, pinned first, one per line. */
static std::string compose_all_shown(void) {
    std::string all_events;
    int first, end, i;
    for (i = 0; i < lvEventListPinnedCount(); i++) {
        all_events += compose_line(*lvEventListAt(i)) + "\r\n";
    }
    shown_range(&first, &end);
    for (i = first; i < end; i++) {
        all_events += compose_line(*lvEventListAt(i)) + "\r\n";
    }
    return all_events;
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
        
        int shown_first, shown_end;
        shown_range(&shown_first, &shown_end);
        /* A seek moves the end of what is shown; follow it as a new line. */
        if (shown_end != s_last_shown_end) {
            if (s_auto_scroll) {
                s_scroll_to_bottom = true;
            }
            s_last_shown_end = shown_end;
        }

        /* Pinned lines, then the timed ones the playhead has reached. */
        int pinned = lvEventListPinnedCount();
        bool seeked = false;
        for (int i = 0; i < shown_end && !seeked; i++) {
            if (i == pinned && i < shown_first) {
                i = shown_first;
                if (i >= shown_end) {
                    break;
                }
            }
            const LvEvent *e = lvEventListAt(i);
            if (e == NULL) {
                break;
            }
            std::string composed = compose_line(*e);
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
                     * few seconds before the moment and centres the map on it.
                     * The seek posts lines into the list, so this frame's
                     * range is stale after it and the loop stops. */
                    if (e->seekable) {
                        uint32_t target = e->seekMs > 5000u ? e->seekMs - 5000u : 0u;
                        int mapX = e->mapX;
                        int mapY = e->mapY;
                        lv_windowSeekToHighlight(target, mapX, mapY);
                        seeked = true;
                    }
                }
            }
            
            /* Right-click context menu on item */
            if (!seeked && ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem(langGetText(STR_LV_COPY))) {
                    copy_to_clipboard(event_text);
                }
                if (ImGui::MenuItem(langGetText(STR_LV_COPY_ALL))) {
                    copy_to_clipboard(compose_all_shown().c_str());
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
            if (ImGui::MenuItem(langGetText(STR_LV_COPY_ALL), NULL, false, lvEventListCount() > 0)) {
                copy_to_clipboard(compose_all_shown().c_str());
            }
            if (ImGui::MenuItem(langGetText(STR_LV_CLEAR_ALL), NULL, false, lvEventListCount() > 0)) {
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