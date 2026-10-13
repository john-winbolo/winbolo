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
#include "imgui_main_menu.h"
#include "imgui_dock.h"
#include "imgui_context.h"
#include "imgui.h"
#include "platform_config.h"
#include "../lv_event_kind.h"
#include "../../gui/lang.h"
#include "../../gui/sdl3/dialogs/imgui_dialog_utils.h"

#include <algorithm>
#include <cfloat>
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
    bool lv_screenSpecIsLiveMode(void);
    void lv_windowSeekToHighlight(uint32_t ms, int mapX, int mapY);
    void lv_windowSeekToEvent(uint32_t ms, int hasCell, int mapX, int mapY, int team);
    void lv_screenEventTeams(int *team, int *otherTeam);
}

/* Event with associated playback timestamp, held in absolute log ms so the
 * displayed stamp can be rebuilt each frame against the presentation window.
 * Highlight clips also carry a seek target and a map cell so a click can jump
 * the scrubber and centre the view; an ordinary line jumps to its own time,
 * and carries a cell when its call site knew one. */
struct LogEvent {
    std::string text;
    uint32_t timeMs;
    int      kind     = LV_EVK_GAME;  /* lvEventKind, for the filter */
    bool     showTime = false;  /* prefix the displayed line with its stamp */
    bool     pinned   = false;  /* load-time summary: never hidden with the lobby */
    bool     seekable = false;
    bool     hasCell  = false;  /* mapX/mapY name a real map cell */
    uint32_t seekMs = 0;
    int      mapX = 0;
    int      mapY = 0;
    /* The teams of the players the line is about, as they were when it
     * happened (lv_messageTagPlayers), -1 for none. */
    int      team = -1;
    int      otherTeam = -1;
};

/* Event list storage */
static std::vector<LogEvent> s_events;
static bool s_auto_scroll = true;
static bool s_scroll_to_bottom = false;

/* The list has two phases. While collecting, it grows as the decoder posts
 * lines and a seek back takes the later ones off again: it holds what
 * playback has passed, and that is all a live feed can ever have. Once a
 * host that read the whole file ahead calls lv_imgui_events_set_complete, the
 * list is the whole log and nothing adds to it or removes from it; playback
 * then only moves the "now" divider through it. Clearing the list (a new
 * load) goes back to collecting. */
static bool s_complete = false;

/* Bumped on every change to s_events, so the shown-lines cache below knows
 * when to rebuild. In the complete phase nothing bumps it, which is what
 * makes a seek cost the panel nothing however often the scrubber fires. */
static unsigned s_version = 1;

/* Selection state for copy functionality */
static int s_selected_index = -1;
static bool s_select_all = false;

/* The one kind the filter shows, or -1 for all of them. This is the user's
 * choice as saved; a log with no line of that kind shows all (see
 * effective_filter) without forgetting the choice. */
static int s_filter = -1;

/* The lines the panel shows, after the lobby hiding and the filter: indexes
 * into s_events in display order, and each line's time for the binary search
 * that places the "now" divider (a pinned line counts as time 0, so it is
 * always in the past). Rebuilt only when one of its inputs changes. */
static std::vector<int>      s_view;
static std::vector<uint32_t> s_view_time;
static int      s_kind_count[LV_EVK_COUNT];  /* per kind, lobby hiding applied */
static int      s_total_count = 0;           /* every kind, lobby hiding applied */
static unsigned s_view_version = 0;          /* s_version the cache was built at */
static uint32_t s_view_window_start = 0;
static int      s_view_filter = -1;

/* Row the "now" divider was last scrolled to, so auto scroll moves the list
 * only when the divider does and leaves hand scrolling alone in between. */
static int s_last_divider_row = -1;

static const char *const k_filter_pref = "Events.Filter";

void lv_imgui_events_init(void) {
    char val[32];
    s_events.clear();
    s_complete = false;
    s_version++;
    s_auto_scroll = true;
    s_scroll_to_bottom = false;
    s_selected_index = -1;
    s_select_all = false;
    s_last_divider_row = -1;
    lv_platform_config_get_string("LOGVIEWER", k_filter_pref, LV_EVK_KEY_ALL,
                                  val, sizeof(val));
    s_filter = lvEventKindFromKey(val);
}

void lv_imgui_events_add(int eventType, int kind, int mapX, int mapY,
                         const char *msg) {
    /* The whole log is already listed: playback is passing lines the panel
     * has, and adding them again would double every one. */
    if (s_complete) {
        return;
    }
    LogEvent e;
    e.text     = std::string(msg);
    e.timeMs   = lv_screenGetTimeRunning();
    e.kind     = (kind >= 0 && kind < LV_EVK_COUNT) ? kind : LV_EVK_GAME;
    e.showTime = (eventType == 0);
    e.hasCell  = (mapX >= 0 && mapY >= 0);
    e.mapX     = mapX;
    e.mapY     = mapY;
    lv_screenEventTeams(&e.team, &e.otherTeam);
    s_events.push_back(e);
    s_version++;

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
    e.kind = LV_EVK_HIGHLIGHT;
    e.pinned = true;
    e.seekable = true;
    e.hasCell = true;
    e.seekMs = seekMs;
    e.mapX = mapX;
    e.mapY = mapY;
    s_events.push_back(e);
    s_version++;

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
    e.kind   = LV_EVK_HIGHLIGHT;
    e.pinned = true;
    s_events.push_back(e);
    s_version++;

    if (s_auto_scroll) {
        s_scroll_to_bottom = true;
    }
}

void lv_imgui_events_clear(void) {
    s_events.clear();
    s_complete = false;
    s_version++;
    s_selected_index = -1;
    s_select_all = false;
    s_last_divider_row = -1;
}

void lv_imgui_events_remove_after(unsigned int timeMs) {
    /* The whole log is listed and stays listed: a rewind moves the "now"
     * divider back, it does not take the later lines away. */
    if (s_complete) {
        return;
    }
    /* Remove events with timestamp strictly after timeMs */
    while (!s_events.empty() && s_events.back().timeMs > timeMs) {
        s_events.pop_back();
        s_version++;
    }
    if (s_selected_index >= (int)s_events.size()) {
        s_selected_index = -1;
        s_select_all = false;
    }
}

void lv_imgui_events_set_complete(void) {
    /* The divider is placed by a binary search on time, so the lines must be
     * in time order with the pinned summary lines in front. The decoder posts
     * them that way already; the stable sort then changes nothing, and it
     * keeps the order of lines that share a tick. */
    std::stable_sort(s_events.begin(), s_events.end(),
                     [](const LogEvent &a, const LogEvent &b) {
                         if (a.pinned != b.pinned) return a.pinned;
                         if (a.pinned) return false;
                         return a.timeMs < b.timeMs;
                     });
    s_complete = true;
    s_version++;
    s_selected_index = -1;
    s_select_all = false;
    s_scroll_to_bottom = false;
    s_last_divider_row = -1;
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

unsigned lv_imgui_events_version(void) {
    return s_version;
}

bool lv_imgui_events_get_info(int i, LvEventInfo *out) {
    if (out == nullptr || i < 0 || i >= (int)s_events.size()) return false;
    const LogEvent &e = s_events[i];
    /* A summary line is about the whole round, not about a moment. */
    if (e.pinned && !e.seekable) return false;
    if (!e.pinned && !e.showTime) return false;
    out->timeMs    = e.seekable ? e.seekMs : e.timeMs;
    out->kind      = e.kind;
    out->highlight = e.seekable ? 1 : 0;
    out->hasCell   = e.hasCell ? 1 : 0;
    out->mapX      = e.mapX;
    out->mapY      = e.mapY;
    out->team      = e.team;
    out->otherTeam = e.otherTeam;
    out->text      = e.text.c_str();
    return true;
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
static bool event_is_hidden(const LogEvent &e, uint32_t windowStart) {
    if (e.pinned) return false;
    return e.timeMs < windowStart;
}

static langid kind_label_id(int kind) {
    switch (kind) {
        case LV_EVK_PILL_CAPTURED: return STR_LV_EVT_KIND_PILL_CAPTURED;
        case LV_EVK_PILL_STOLEN:   return STR_LV_EVT_KIND_PILL_STOLEN;
        case LV_EVK_BASE_CAPTURED: return STR_LV_EVT_KIND_BASE_CAPTURED;
        case LV_EVK_BASE_STOLEN:   return STR_LV_EVT_KIND_BASE_STOLEN;
        case LV_EVK_BUILDER_LOST:  return STR_LV_EVT_KIND_BUILDER_LOST;
        case LV_EVK_TANK_KILLED:   return STR_LV_EVT_KIND_TANK_KILLED;
        case LV_EVK_PLAYER:        return STR_LV_EVT_KIND_PLAYER;
        case LV_EVK_CHAT:          return STR_LV_EVT_KIND_CHAT;
        case LV_EVK_GAME:          return STR_LV_EVT_KIND_GAME;
        case LV_EVK_HIGHLIGHT:     return STR_LV_EVT_KIND_HIGHLIGHT;
        default:                   return STR_LV_EVT_FILTER_ALL;
    }
}

const char *lv_imgui_events_kind_label(int kind) {
    return langGetText(kind_label_id(kind));
}

/* Count the lines of each kind the lobby hiding leaves. The counts decide
 * which kinds the filter offers, so they are taken before the filter. */
static void count_kinds(uint32_t windowStart) {
    memset(s_kind_count, 0, sizeof(s_kind_count));
    s_total_count = 0;
    for (const LogEvent &e : s_events) {
        if (event_is_hidden(e, windowStart)) continue;
        s_kind_count[e.kind]++;
        s_total_count++;
    }
}

/* The filter in force: the saved choice when this log has a line of that
 * kind, otherwise all. The choice itself is kept, so it comes back with the
 * next log that has such lines. */
static int effective_filter(void) {
    if (s_filter >= 0 && s_filter < LV_EVK_COUNT && s_kind_count[s_filter] > 0) {
        return s_filter;
    }
    return -1;
}

/* Bring the shown-lines cache up to date. Cheap to call every frame: it
 * rebuilds only when the list, the presentation window or the filter has
 * changed since the last build. */
static void refresh_view(void) {
    uint32_t windowStart = lv_screenWindowStartMs();
    if (s_view_version != s_version || s_view_window_start != windowStart) {
        count_kinds(windowStart);
    } else if (s_view_filter == effective_filter()) {
        return;
    }
    int filter = effective_filter();
    s_view.clear();
    s_view_time.clear();
    for (int i = 0; i < (int)s_events.size(); i++) {
        const LogEvent &e = s_events[i];
        if (event_is_hidden(e, windowStart)) continue;
        if (filter >= 0 && e.kind != filter) continue;
        s_view.push_back(i);
        s_view_time.push_back(e.pinned ? 0u : e.timeMs);
    }
    s_view_version      = s_version;
    s_view_window_start = windowStart;
    s_view_filter       = filter;
    /* The rows moved under the divider: let auto scroll find it again. */
    s_last_divider_row = -1;
}

/* Every shown line, one per row, as Copy All puts it on the clipboard. */
static std::string shown_lines_text(void) {
    std::string all_events;
    for (int idx : s_view) {
        all_events += compose_line(s_events[idx]) + "\r\n";
    }
    return all_events;
}

/* "<label> (<count>)" for a filter entry; kind -1 is the All entry. */
static void filter_entry_label(int kind, char *dest, size_t destSize) {
    if (kind < 0) {
        snprintf(dest, destSize, "%s (%d)", langGetText(STR_LV_EVT_FILTER_ALL),
                 s_total_count);
    } else {
        snprintf(dest, destSize, "%s (%d)", langGetText(kind_label_id(kind)),
                 s_kind_count[kind]);
    }
}

/* The filter: one kind at a time, All first. A kind this log has no line of
 * is not offered. */
static void draw_filter_combo(void) {
    char label[160];
    int current = effective_filter();
    filter_entry_label(current, label, sizeof(label));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##evtfilter", label)) {
        int chosen = current;
        filter_entry_label(-1, label, sizeof(label));
        if (ImGui::Selectable(label, current < 0)) {
            chosen = -1;
        }
        for (int kind = 0; kind < LV_EVK_COUNT; kind++) {
            if (s_kind_count[kind] <= 0) continue;
            filter_entry_label(kind, label, sizeof(label));
            if (ImGui::Selectable(label, current == kind)) {
                chosen = kind;
            }
        }
        if (chosen != current) {
            s_filter = chosen;
            lv_platform_config_set_string("LOGVIEWER", k_filter_pref,
                                          lvEventKindKey(s_filter));
            /* A selection is an index into the full list and may now be a
             * line the filter hides. */
            s_selected_index = -1;
            s_select_all = false;
        }
        ImGui::EndCombo();
    }
}

/* The "now" divider: a thin line across the visible list with the current
 * time on it, between the lines playback has passed and those to come. It
 * takes one row of the same height as a line, which the list clipper relies
 * on. Drawn against the window, not the content, so it stays in view when the
 * list is scrolled sideways. */
static void draw_now_divider(uint32_t nowMs) {
    char stamp[16];
    char label[96];
    lv_screenFormatTime(nowMs, stamp, sizeof(stamp));
    snprintf(label, sizeof(label), "%s %s", langGetText(STR_LV_EVT_NOW), stamp);

    const ImGuiStyle &style = ImGui::GetStyle();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 winPos  = ImGui::GetWindowPos();
    ImVec2 winSize = ImGui::GetWindowSize();
    float rowH = ImGui::GetTextLineHeight();
    float y    = ImGui::GetCursorScreenPos().y;
    float x0   = winPos.x + style.WindowPadding.x;
    float x1   = winPos.x + winSize.x - style.WindowPadding.x;
    if (ImGui::GetScrollMaxY() > 0.0f) {
        x1 -= style.ScrollbarSize;
    }
    ImU32 col     = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
    ImVec2 textSz = ImGui::CalcTextSize(label);
    float lead    = ImGui::GetFontSize();  /* short stroke before the label */
    float midY    = y + rowH * 0.5f;
    float textX   = x0 + lead + style.ItemInnerSpacing.x;
    float afterX  = textX + textSz.x + style.ItemInnerSpacing.x;

    dl->AddLine(ImVec2(x0, midY), ImVec2(x0 + lead, midY), col, 1.0f);
    dl->AddText(ImVec2(textX, y), col, label);
    if (afterX < x1) {
        dl->AddLine(ImVec2(afterX, midY), ImVec2(x1, midY), col, 1.0f);
    }
    ImGui::Dummy(ImVec2(1.0f, rowH));
}

void lv_imgui_events_window(void) {
    if (!lv_g_show_events_window) {
        return;
    }

    {
        /* A panel that sat in the dock when the dock was turned off goes
         * back to its default place, the same as a reset. */
        bool to_default = lv_g_reset_window_positions ||
                          lv_imgui_dock_take_float_reset("###events");
        ImGuiCond cond = to_default ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        ImVec2 vp = ImGui::GetMainViewport()->Size;
        ImGui::SetNextWindowSize(ImVec2(320, 200), cond);
        ImGui::SetNextWindowPos(ImVec2(vp.x - 320 - 10, vp.y - 200 - 10), cond);
    }

    char ev_title[128];
    snprintf(ev_title, sizeof(ev_title), "%s###events", langGetText(STR_LV_WIN_EVENTS));
    if (ImGui::Begin(ev_title, &lv_g_show_events_window, ImGuiWindowFlags_NoCollapse)) {
        /* In the dock the dock places the window; moving it here would pull
         * it straight back out. */
        if (!ImGui::IsWindowDocked()) {
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
        }

        refresh_view();

        /* The filter, then the in-panel Auto Scroll toggle — always visible,
         * mirrors the right-click menu item below.  Reachable from
         * controller/touch where right-click is unavailable. */
        draw_filter_combo();
        ImGui::Checkbox(langGetText(STR_LV_AUTO_SCROLL), &s_auto_scroll);
        ImGui::Separator();

        /* A click that seeks is acted on after the list is drawn: while the
         * list is still collecting, the seek itself takes lines off it and
         * posts them again, under the loop that is drawing them. */
        bool     jump          = false;
        bool     jumpHighlight = false;
        bool     jumpHasCell   = false;
        uint32_t jumpMs        = 0;
        int      jumpX         = 0;
        int      jumpY         = 0;
        int      jumpTeam      = -1;
        bool     copyAll       = false;

        /* List box for events */
        ImGui::BeginChild("EventsList", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

        /* With the whole log listed, the lines after the playhead are still
         * to come: they are drawn dim, below a divider at the current time.
         * A list that is still collecting holds only what has happened. */
        uint32_t nowMs   = lv_screenGetTimeRunning();
        int      shown   = (int)s_view.size();
        bool     divider = s_complete && shown > 0;
        int      past    = divider
                               ? lvEventPastCount(s_view_time.data(), shown, nowMs)
                               : shown;
        int      rows    = shown + (divider ? 1 : 0);
        /* Ordinary lines seek on a click, except on a live feed, where a
         * stray click would throw the viewer off the live head. */
        bool     linesSeek = !lv_screenSpecIsLiveMode();

        /* Same hue as a past line, most of the way to transparent. */
        ImVec4 futureCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        futureCol.w *= 0.45f;

        ImGuiListClipper clipper;
        clipper.Begin(rows, ImGui::GetTextLineHeightWithSpacing());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                if (divider && row == past) {
                    draw_now_divider(nowMs);
                    continue;
                }
                int  viewIdx  = (divider && row > past) ? row - 1 : row;
                int  i        = s_view[viewIdx];
                bool isFuture = divider && viewIdx >= past;
                const LogEvent &ev = s_events[i];
                std::string composed = compose_line(ev);
                const char* event_text = composed.c_str();

                /* Check if this item is selected */
                bool is_selected = (s_select_all) || (s_selected_index == i);

                /* Selectable item */
                char selectId[544];
                snprintf(selectId, sizeof(selectId), "%s##evt%d", event_text, i);
                if (isFuture) {
                    ImGui::PushStyleColor(ImGuiCol_Text, futureCol);
                }
                bool clicked = ImGui::Selectable(selectId, is_selected);
                if (isFuture) {
                    ImGui::PopStyleColor();
                }
                /* A line that jumps when clicked shows the hand. */
                if (ev.seekable || (ev.showTime && linesSeek)) {
                    imguiHandOnHover();
                }
                if (clicked) {
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
                        if (ev.seekable) {
                            /* A plain click on a highlight clip jumps the
                             * scrubber to a few seconds before the moment and
                             * centres the map on it. */
                            jump          = true;
                            jumpHighlight = true;
                            jumpMs        = ev.seekMs > 5000u ? ev.seekMs - 5000u : 0u;
                            jumpX         = ev.mapX;
                            jumpY         = ev.mapY;
                        } else if (ev.showTime && linesSeek) {
                            /* Any other timed line jumps to its own moment,
                             * and to its place when it has one. */
                            jump        = true;
                            jumpMs      = ev.timeMs;
                            jumpHasCell = ev.hasCell;
                            jumpX       = ev.mapX;
                            jumpY       = ev.mapY;
                            jumpTeam    = ev.team;
                        }
                    }
                }

                /* Right-click context menu on item */
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem(langGetText(STR_LV_COPY))) {
                        copy_to_clipboard(event_text);
                    }
                    if (ImGui::MenuItem(langGetText(STR_LV_COPY_ALL))) {
                        copyAll = true;
                    }
                    if (ImGui::MenuItem(langGetText(STR_MENU_SELECT_ALL))) {
                        s_select_all = true;
                    }
                    ImGui::EndPopup();
                }
            }
        }
        clipper.End();

        if (divider) {
            /* Auto scroll follows the divider, held about a third of the list
             * up from the bottom so the lines about to happen show under it.
             * It moves the list only when the divider changes row, so the
             * list can be scrolled by hand between events. */
            if (s_auto_scroll && past != s_last_divider_row) {
                float rowH = ImGui::GetTextLineHeightWithSpacing();
                float target = ((float)past + 0.5f) * rowH
                               - ImGui::GetWindowHeight() * (2.0f / 3.0f);
                ImGui::SetScrollY(target > 0.0f ? target : 0.0f);
            }
            s_last_divider_row = past;
            s_scroll_to_bottom = false;
        } else if (s_scroll_to_bottom && s_auto_scroll) {
            /* Auto-scroll to bottom if new event was added */
            ImGui::SetScrollHereY(1.0f);
            s_scroll_to_bottom = false;
        }

        ImGui::EndChild();

        /* Right-click context menu in empty space */
        if (ImGui::BeginPopupContextWindow()) {
            if (ImGui::MenuItem(langGetText(STR_MENU_SELECT_ALL), NULL, s_select_all)) {
                s_select_all = !s_select_all;
            }
            if (ImGui::MenuItem(langGetText(STR_LV_COPY_ALL), NULL, false, !s_view.empty())) {
                copyAll = true;
            }
            /* Clear All empties a list that playback then refills. The
             * whole-log list is filled once, at load, so there it would
             * only throw the log's events away until the next load. */
            if (ImGui::MenuItem(langGetText(STR_LV_CLEAR_ALL), NULL, false,
                                !s_events.empty() && !s_complete)) {
                lv_imgui_events_clear();
                jump = false;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(langGetText(STR_LV_AUTO_SCROLL), NULL, s_auto_scroll)) {
                s_auto_scroll = !s_auto_scroll;
            }
            ImGui::EndPopup();
        }

        /* Copy All takes the lines the panel shows: the filter and the lobby
         * hiding both apply. */
        if (copyAll) {
            refresh_view();
            copy_to_clipboard(shown_lines_text().c_str());
        }

        if (jump) {
            if (jumpHighlight) {
                lv_windowSeekToHighlight(jumpMs, jumpX, jumpY);
            } else {
                lv_windowSeekToEvent(jumpMs, jumpHasCell ? 1 : 0, jumpX, jumpY, jumpTeam);
            }
        }
    }
    ImGui::End();
}
