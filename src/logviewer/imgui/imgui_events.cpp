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
#include "imgui_event_overlay.h"
#include "imgui_main_menu.h"
#include "imgui_dock.h"
#include "imgui_context.h"
#include "imgui.h"
#include "platform_config.h"
#include "../lv_event_list.h"
#include "../lv_event_kind.h"
#include "../lv_event_overlay.h"
#include "../lv_builder_status.h"
#include "../../gui/lang.h"
#include "../../gui/sdl3/dialogs/imgui_dialog_utils.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
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
    void lv_windowSeekToEventKeepView(uint32_t ms, int hasCell, int mapX, int mapY, int team);
    void lv_windowCentreOnEvent(uint32_t ms, int hasCell, int mapX, int mapY, int team);
    void lv_screenEventTeams(int *team, int *otherTeam);
    int lv_screenEventPing(void);
    int lv_screenEventPlayer(void);
    int lv_screenBuilderAwayMs(int slot, uint32_t deathMs, uint32_t *awayMs);
}

/* The lines are kept by lv_event_list.c, in display order: the pinned
 * summary lines and highlight clips first, then the timed lines by time.
 * Each line carries what the filter, the markers and a click need (kind,
 * map cell, teams, ping). */
static bool s_auto_scroll = true;
static bool s_scroll_to_bottom = false;

/* The list has two phases. While collecting, it grows as the decoder posts
 * lines, and the panel shows those up to the playhead: that is all a live
 * feed can ever have. Once a host that read the whole file ahead calls
 * lv_imgui_events_set_complete, the list is the whole log and nothing adds
 * to it; playback then only moves the "now" divider through it. Clearing the
 * list (a new load) goes back to collecting. */
static bool s_complete = false;

/* Bumped on every change to the list, so the shown-lines cache below knows
 * when to rebuild. In the complete phase nothing bumps it, which is what
 * makes a seek cost the panel nothing however often the scrubber fires. */
static unsigned s_version = 1;

/* Selection state for copy functionality */
static int s_selected_index = -1;
/* The line the last plain click jumped to, -1 for none. The second click of
 * a double click is for this line: the first click's jump can move the rows
 * of the list (the "now" line, auto scroll), so the row under the mouse at
 * the second click need not be the one the user chose.
 * A plain click on the selected line (s_selected_index) is a centre click
 * too, with no limit of time between the two clicks. */
static int s_clicked_index = -1;
static bool s_select_all = false;

/* The kinds the filter shows, one bit a kind: the panel's own choice, apart
 * from the one of the "Overlay events" list of the Controls panel. */
static unsigned s_filter = LV_OVERLAY_ALL_MASK;

/* The selected line is to be brought into view: the filter changed and the
 * line is still shown. */
static bool s_scroll_to_selected = false;

/* The lines the panel shows, after the lobby hiding, the playhead (while
 * collecting) and the filter: indexes into the list in display order, and
 * each line's time for the binary search that places the "now" divider (a
 * pinned line counts as time 0, so it is always in the past). Rebuilt only
 * when one of its inputs changes. */
static std::vector<int>      s_view;
static std::vector<uint32_t> s_view_time;
static int      s_kind_count[LV_EVK_COUNT];  /* per kind, lobby hiding applied */
static int      s_total_count = 0;           /* every kind, lobby hiding applied */
static unsigned s_view_version = 0;          /* s_version the cache was built at */
static uint32_t s_view_window_start = 0;
static unsigned s_view_filter = LV_OVERLAY_ALL_MASK;
static int      s_view_now_end = -1;         /* end of the lines up to the playhead */

/* Row the "now" divider was last scrolled to, so auto scroll moves the list
 * only when the divider does and leaves hand scrolling alone in between. */
static int s_last_divider_row = -1;

/* The LOGVIEWER preference that keeps the ticked kinds: their saved names
 * with commas between them, or "none" (lvOverlayMaskToText). The panel used
 * to show one kind or all, saved under k_old_filter_pref; that choice is
 * taken over once (lvEventsMaskFromPrefs) and its key is left as it is. */
static const char *const k_kinds_pref = "Events.Kinds";
static const char *const k_old_filter_pref = "Events.Filter";

static void save_filter(void) {
    char val[256];
    lvOverlayMaskToText(s_filter, val, sizeof(val));
    lv_platform_config_set_string("LOGVIEWER", k_kinds_pref, val);
}

void lv_imgui_events_init(void) {
    char val[256];
    char old[32];
    lvEventListClear();
    s_complete = false;
    s_version++;
    s_auto_scroll = true;
    s_scroll_to_bottom = false;
    s_selected_index = -1;
    s_clicked_index = -1;
    s_select_all = false;
    s_last_divider_row = -1;
    s_scroll_to_selected = false;
    lv_platform_config_get_string("LOGVIEWER", k_kinds_pref, "", val, sizeof(val));
    lv_platform_config_get_string("LOGVIEWER", k_old_filter_pref, LV_EVK_KEY_ALL,
                                  old, sizeof(old));
    s_filter = lvEventsMaskFromPrefs(val, old);
    if (val[0] == '\0' && s_filter != LV_OVERLAY_ALL_MASK) {
        /* The old choice was taken over: keep it under the new key. */
        save_filter();
    }
}

void lv_imgui_events_add(int eventType, int kind, int mapX, int mapY,
                         const char *msg) {
    /* The whole log is already listed: playback is passing lines the panel
     * has, and adding them again would double every one. */
    if (s_complete) {
        return;
    }
    /* NULL for a line a replay after a seek back posts again. */
    LvEvent *e = lvEventListAdd(msg, lv_screenGetTimeRunning(), eventType == 0);
    if (e == nullptr) {
        return;
    }
    e->kind    = (kind >= 0 && kind < LV_EVK_COUNT) ? kind : LV_EVK_GAME;
    e->hasCell = (mapX >= 0 && mapY >= 0);
    e->mapX    = mapX;
    e->mapY    = mapY;
    lv_screenEventTeams(&e->team, &e->otherTeam);
    e->ping    = lv_screenEventPing();
    e->player  = lv_screenEventPlayer();
    s_version++;

    /* Mark for auto-scroll */
    if (s_auto_scroll) {
        s_scroll_to_bottom = true;
    }
}

/* A pinned line's fields that are not the list's own. */
static void pinned_fields(LvEvent *e, bool hasCell) {
    if (e == nullptr) {
        return;
    }
    e->kind      = LV_EVK_HIGHLIGHT;
    e->hasCell   = hasCell;
    e->team      = -1;
    e->otherTeam = -1;
    e->ping      = -1;
    e->player    = -1;
    s_version++;
    if (s_auto_scroll) {
        s_scroll_to_bottom = true;
    }
}

void lv_imgui_events_add_highlight(const char *msg, uint32_t seekMs, int mapX,
                                   int mapY) {
    /* Load-time summary line: pinned, so never hidden by the playhead. */
    pinned_fields(lvEventListAddPinned(msg, true, seekMs, mapX, mapY), true);
}

/* Load-time round-summary line (awards, section headers): pinned above the feed
 * with no stamp of its own, so it is hidden neither with the lobby nor by the
 * playhead. */
void lv_imgui_events_add_summary(const char *msg) {
    pinned_fields(lvEventListAddPinned(msg, false, 0, 0, 0), false);
}

void lv_imgui_events_clear(void) {
    lvEventListClear();
    s_complete = false;
    s_version++;
    s_selected_index = -1;
    s_clicked_index = -1;
    s_select_all = false;
    s_last_divider_row = -1;
}

void lv_imgui_events_rewound(void) {
    lvEventListRewound();
}

/* The line of a lost builder says how long he was away, "(0:42)", after
 * its text: from his death to the tick the new builder was in the tank. The
 * time is out of the table the load's play of the whole log has just
 * filled, so every line has it from the load on, the lines still to come
 * too, and playback does not change it. A builder who was not back when the
 * log ended, or when his player left, has no time and the line is as it was.
 * The text is part of the line, so the marker over the seek bar and Copy
 * say it too. */
static void add_builder_away_times(void) {
    const int count = lvEventListCount();
    for (int i = lvEventListPinnedCount(); i < count; i++) {
        LvEvent *e = lvEventListEdit(i);
        uint32_t awayMs = 0;
        char     away[LV_BUILDER_AWAY_TEXT_MAX];
        if (e == nullptr || e->kind != LV_EVK_BUILDER_LOST || e->player < 0 ||
            e->awayAdded) {
            continue;
        }
        if (!lv_screenBuilderAwayMs(e->player, e->timeMs, &awayMs)) continue;
        lvBuilderAwayText(true, awayMs, away, sizeof(away));
        lvEventListAppendText(i, away);
        /* Once only: the line has its time. */
        e->awayAdded = true;
    }
}

void lv_imgui_events_set_complete(void) {
    add_builder_away_times();
    /* The list keeps the pinned lines in front and the timed ones in time
     * order, which the binary search that places the divider needs. */
    s_complete = true;
    s_version++;
    s_selected_index = -1;
    s_clicked_index = -1;
    s_select_all = false;
    s_scroll_to_bottom = false;
    s_last_divider_row = -1;
}

int lv_imgui_events_get_count(void) {
    return lvEventListCount();
}

const char *lv_imgui_events_get_text(int i) {
    const LvEvent *e = lvEventListAt(i);
    return e != nullptr ? e->text : "";
}

uint32_t lv_imgui_events_get_time(int i) {
    const LvEvent *e = lvEventListAt(i);
    return e != nullptr ? e->timeMs : 0;
}

unsigned lv_imgui_events_version(void) {
    return s_version;
}

bool lv_imgui_events_get_info(int i, LvEventInfo *out) {
    const LvEvent *e = lvEventListAt(i);
    if (out == nullptr || e == nullptr) return false;
    /* A summary line is about the whole round, not about a moment. */
    if (e->pinned && !e->seekable) return false;
    if (!e->pinned && !e->showTime) return false;
    out->timeMs    = e->seekable ? e->seekMs : e->timeMs;
    out->kind      = e->kind;
    out->highlight = e->seekable ? 1 : 0;
    out->hasCell   = e->hasCell ? 1 : 0;
    out->mapX      = e->mapX;
    out->mapY      = e->mapY;
    out->team      = e->team;
    out->otherTeam = e->otherTeam;
    out->ping      = e->ping;
    out->text      = e->text;
    return true;
}

/* Copy selected text to clipboard */
static void copy_to_clipboard(const char* text) {
    ImGui::SetClipboardText(text);
}

/* The longest line compose_line writes whole; a longer one is cut. */
#define EVENT_LINE_MAX 1024

/* Build the line as displayed, into buf, and return it. Highlight clips read
 * their stamp from the seek target; ordinary timestamped lines from their own
 * time. Both go through lv_screenFormatTime, so the whole panel follows the
 * presentation window. A line with no stamp is its own text, and buf is not
 * used. Nothing is allocated: the panel calls this for every row it draws,
 * every frame. */
static const char *compose_line(const LvEvent &e, char *buf, size_t bufSize) {
    char stamp[16];
    if (e.seekable) {
        lv_screenFormatTime(e.seekMs, stamp, sizeof(stamp));
        snprintf(buf, bufSize, "Highlight %s \xe2\x80\x94 %s", stamp, e.text);
        return buf;
    }
    if (e.showTime) {
        lv_screenFormatTime(e.timeMs, stamp, sizeof(stamp));
        snprintf(buf, bufSize, "%s - %s", stamp, e.text);
        return buf;
    }
    return e.text;
}

/* Lobby-period lines stay in the list (un-ticking Hide Lobby brings them
 * straight back) but out of the panel, where they would sit at 00:00. While
 * the list is collecting, the lines after the playhead are out of it too:
 * a seek back keeps them, and they come back as the playhead reaches them. */
static bool event_is_hidden(const LvEvent &e, uint32_t windowStart, uint32_t nowMs) {
    if (e.pinned) return false;
    if (e.timeMs < windowStart) return true;
    return !s_complete && e.timeMs > nowMs;
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

/* Count the lines of each kind the lobby hiding (and, while collecting, the
 * playhead) leaves. The counts are the numbers in the filter's list, so they
 * are taken before the filter. */
static void count_kinds(uint32_t windowStart, uint32_t nowMs) {
    memset(s_kind_count, 0, sizeof(s_kind_count));
    s_total_count = 0;
    const int count = lvEventListCount();
    for (int i = 0; i < count; i++) {
        const LvEvent &e = *lvEventListAt(i);
        if (event_is_hidden(e, windowStart, nowMs)) continue;
        if (e.kind >= 0 && e.kind < LV_EVK_COUNT) s_kind_count[e.kind]++;
        s_total_count++;
    }
}

/* Bring the shown-lines cache up to date. Cheap to call every frame: it
 * rebuilds only when the list, the presentation window, the filter or (while
 * collecting) the last line the playhead has reached has changed since the
 * last build. */
static void refresh_view(void) {
    uint32_t windowStart = lv_screenWindowStartMs();
    uint32_t nowMs       = lv_screenGetTimeRunning();
    int      nowEnd      = -1;
    if (!s_complete) {
        int first;
        lvEventListRange(0, nowMs, &first, &nowEnd);
    }
    if (s_view_version != s_version || s_view_window_start != windowStart ||
        s_view_now_end != nowEnd) {
        /* A seek that moves the last line shown scrolls like a new line. */
        if (s_view_now_end != nowEnd && s_auto_scroll) {
            s_scroll_to_bottom = true;
        }
        count_kinds(windowStart, nowMs);
    } else if (s_view_filter == s_filter) {
        return;
    }
    const unsigned filter = s_filter;
    const int      count  = lvEventListCount();
    s_view.clear();
    s_view_time.clear();
    for (int i = 0; i < count; i++) {
        const LvEvent &e = *lvEventListAt(i);
        if (event_is_hidden(e, windowStart, nowMs)) continue;
        if (!lvEventKindTicked(filter, e.kind)) continue;
        s_view.push_back(i);
        s_view_time.push_back(e.pinned ? 0u : e.timeMs);
    }
    s_view_version      = s_version;
    s_view_window_start = windowStart;
    s_view_filter       = filter;
    s_view_now_end      = nowEnd;
    /* The rows moved under the divider: let auto scroll find it again. */
    s_last_divider_row = -1;
}

/* Every shown line, one per row, as Copy All puts it on the clipboard. */
static std::string shown_lines_text(void) {
    std::string all_events;
    char        buf[EVENT_LINE_MAX];
    for (int idx : s_view) {
        all_events += compose_line(*lvEventListAt(idx), buf, sizeof(buf));
        all_events += "\r\n";
    }
    return all_events;
}

/* The filter: the list of kinds with a check box each, the same control as
 * the "Overlay events" list of the Controls panel (lv_event_kinds_combo),
 * with the panel's own choice. Closed, it says what the list shows: "All
 * events (412)" with every kind ticked, "Tank killed (37)" with one, and
 * "37 of 412 events" with any other choice. In a narrow panel it says only
 * the number, "(37)", and in a narrower one it is the arrow alone. */
static void draw_filter_combo(void) {
    const ImGuiStyle &style = ImGui::GetStyle();
    char full[200];
    char count[32];
    const int shown = lvEventKindsTickedCount(s_filter, s_kind_count);
    int only = -1;
    for (int kind = 0; kind < LV_EVK_COUNT; kind++) {
        if (s_filter == (1u << kind)) only = kind;
    }
    if (s_filter == LV_OVERLAY_ALL_MASK) {
        snprintf(full, sizeof(full), "%s (%d)", langGetText(STR_LV_EVT_FILTER_ALL), shown);
    } else if (only >= 0) {
        snprintf(full, sizeof(full), "%s (%d)", langGetText(kind_label_id(only)), shown);
    } else {
        MessageArgs args = {};
        snprintf(args.string1, sizeof(args.string1), "%d", shown);
        snprintf(args.string2, sizeof(args.string2), "%d", s_total_count);
        snprintf(full, sizeof(full), "%s", langGetTextFmt(STR_LV_EVT_FILTER_SOME, &args));
    }
    snprintf(count, sizeof(count), "(%d)", shown);

    const float availW = ImGui::GetContentRegionAvail().x;
    const float arrowW = ImGui::GetFrameHeight();
    const LvEventsLabel pick = lvEventsLabelPick(availW, ImGui::CalcTextSize(full).x,
                                                 ImGui::CalcTextSize(count).x,
                                                 style.FramePadding.x * 2.0f + arrowW);
    char tip[512];
    if (pick == LV_EVENTS_LABEL_FULL) {
        snprintf(tip, sizeof(tip), "%s", langGetText(STR_LV_EVT_FILTER_TIP));
    } else {
        /* What the closed list has no room to say goes in front of the tip. */
        snprintf(tip, sizeof(tip), "%s\n%s", full, langGetText(STR_LV_EVT_FILTER_TIP));
    }
    ImGui::SetNextItemWidth((pick == LV_EVENTS_LABEL_ARROW) ? arrowW : -FLT_MIN);
    if (lv_event_kinds_combo("##evtfilter",
                             (pick == LV_EVENTS_LABEL_FULL) ? full :
                             (pick == LV_EVENTS_LABEL_COUNT) ? count : nullptr,
                             tip, &s_filter, s_kind_count)) {
        save_filter();
        /* The selected line stays selected, and is brought into view, when
         * its kind is still ticked. A line the filter now hides cannot stay
         * selected: nothing would show it. Select All covers the lines that
         * are shown, so it stays as it is. */
        if (lvEventListAt(s_selected_index) != nullptr &&
            lvEventKindTicked(s_filter, lvEventListAt(s_selected_index)->kind)) {
            s_scroll_to_selected = true;
        } else {
            s_selected_index = -1;
            s_clicked_index = -1;
        }
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
        lv_imgui_keep_window_on_screen();

        refresh_view();

        /* The filter (drawn from the counts of the frame; a change to it is
         * taken in before the list is drawn), then the in-panel Auto Scroll
         * toggle — always visible, mirrors the right-click menu item below.
         * Reachable from controller/touch where right-click is unavailable. */
        draw_filter_combo();
        refresh_view();
        ImGui::Checkbox(langGetText(STR_LV_AUTO_SCROLL), &s_auto_scroll);
        ImGui::Separator();

        /* A click that seeks is acted on after the list is drawn: while the
         * list is still collecting, the seek itself posts lines into it,
         * which moves the rows under the loop that is drawing them. */
        bool     jump          = false;
        bool     jumpHighlight = false;
        bool     jumpCentre    = false; /* a click on the selected line */
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
        /* Ordinary lines seek on a click, on a live feed too: the click parks
         * the view in the past, and the Live button (or End or L) goes back
         * to the head. */
        bool     linesSeek = true;

        /* Same hue as a past line, most of the way to transparent. */
        ImVec4 futureCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        futureCol.w *= 0.45f;

        /* Nothing to list though the log has lines: the filter hides them
         * all. The panel says so, so an empty list is not taken for an empty
         * log. */
        if (shown == 0 && s_total_count > 0) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", langGetText(STR_LV_EVT_FILTER_EMPTY));
            ImGui::PopTextWrapPos();
        }

        /* The selected line into the middle of the list, once, after a
         * change of the filter moved the rows. It is found by its place in
         * the shown lines, which are in the order of the list. */
        bool selectedScrolled = false;
        if (s_scroll_to_selected) {
            s_scroll_to_selected = false;
            const auto it = std::lower_bound(s_view.begin(), s_view.end(), s_selected_index);
            if (it != s_view.end() && *it == s_selected_index) {
                const int viewIdx = (int)(it - s_view.begin());
                const int row = viewIdx + ((divider && viewIdx >= past) ? 1 : 0);
                const float rowH = ImGui::GetTextLineHeightWithSpacing();
                const float target = ((float)row + 0.5f) * rowH - ImGui::GetWindowHeight() * 0.5f;
                ImGui::SetScrollY(target > 0.0f ? target : 0.0f);
                selectedScrolled = true;
            }
        }

        /* The column of the markers: a marker is a square as high as the
         * text of a line, and the text starts a little after it. */
        const float rowTextH = ImGui::GetTextLineHeight();
        const float iconSide = floorf(ImGui::GetFontSize());
        const float iconCol  = iconSide + ImGui::GetStyle().ItemInnerSpacing.x;
        const float listLeft = ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x;
        const bool  scrolledAcross = ImGui::GetScrollX() > 0.0f;

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
                const LvEvent &ev = *lvEventListAt(i);
                char line_buf[EVENT_LINE_MAX];
                const char* event_text = compose_line(ev, line_buf, sizeof(line_buf));

                /* Check if this item is selected */
                bool is_selected = (s_select_all) || (s_selected_index == i);

                /* The line: the marker of the event in a column of its own
                 * at the left, then the time and the text. The whole row is
                 * one item, the marker's column too, so a click anywhere on
                 * it does the same thing. The row is as wide as the list, or
                 * as its text when that is longer (the list then scrolls
                 * sideways, as it always did), and as high as a line of
                 * text: the marker adds nothing to it. */
                char selectId[32];
                snprintf(selectId, sizeof(selectId), "##evt%d", i);
                const ImVec2 rowPos = ImGui::GetCursorScreenPos();
                const float  rowW   = (std::max)(iconCol + ImGui::CalcTextSize(event_text).x,
                                               ImGui::GetContentRegionAvail().x);
                bool clicked = ImGui::Selectable(selectId, is_selected, 0, ImVec2(rowW, 0.0f));
                const bool rowHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
                {
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    /* The marker keeps its place at the left of the list
                     * when the list is scrolled sideways; the text goes
                     * under it and is cut off at the column's edge. */
                    const float iconX = floorf(listLeft);
                    const float iconY = floorf(rowPos.y + (rowTextH - iconSide) * 0.5f);
                    /* A line still to come is dim, and so is its marker. */
                    lv_event_marker_draw(dl, ev.kind, ev.team, ev.ping, ImVec2(iconX, iconY),
                                         ImVec2(iconX + iconSide, iconY + iconSide),
                                         isFuture ? 0.45f : 1.0f);
                    if (scrolledAcross) {
                        dl->PushClipRect(ImVec2(listLeft + iconCol, rowPos.y),
                                         ImVec2(dl->GetClipRectMax().x, rowPos.y + rowTextH), true);
                    }
                    dl->AddText(ImVec2(rowPos.x + iconCol, rowPos.y),
                                ImGui::GetColorU32(isFuture ? futureCol
                                                            : ImGui::GetStyleColorVec4(ImGuiCol_Text)),
                                event_text);
                    if (scrolledAcross) {
                        dl->PopClipRect();
                    }
                    /* The marker says what kind of event the line is. */
                    if (rowHovered && ImGui::GetIO().MousePos.x < listLeft + iconCol) {
                        /* A ping says which kind of ping it is. */
                        ImGui::SetTooltip("%s", (ev.kind == LV_EVK_CHAT && ev.ping >= 0)
                                                    ? lv_event_ping_name(ev.ping)
                                                    : lv_imgui_events_kind_label(ev.kind));
                    }
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
                        s_clicked_index = -1;
                    } else {
                        /* A click on the line that is selected already
                         * puts its place in the middle of the map that
                         * shows. The second click of a double click does the
                         * same, for the line the first one chose. The first
                         * click has done the plain jump already: there is no
                         * wait (lvEventClickPick). */
                        const LvEventClick pick = lvEventClickPick(
                            i, s_select_all ? -1 : s_selected_index, s_clicked_index,
                            ImGui::GetIO().MouseClickedLastCount[ImGuiMouseButton_Left],
                            lvEventListCount());
                        const bool twice = pick.centre != 0;
                        const int target = pick.target;
                        const LvEvent &tev = *lvEventListAt(target);
                        s_selected_index = target;
                        s_clicked_index = target;
                        s_select_all = false;
                        if (tev.seekable) {
                            /* A click on a highlight clip jumps the
                             * scrubber to a few seconds before the moment. */
                            jump          = true;
                            jumpHighlight = true;
                            jumpCentre    = twice;
                            jumpMs        = tev.seekMs > 5000u ? tev.seekMs - 5000u : 0u;
                            jumpX         = tev.mapX;
                            jumpY         = tev.mapY;
                        } else if (tev.showTime && linesSeek) {
                            /* Any other timed line jumps to its own moment. */
                            jump        = true;
                            jumpCentre  = twice;
                            jumpMs      = tev.timeMs;
                            jumpHasCell = tev.hasCell;
                            jumpX       = tev.mapX;
                            jumpY       = tev.mapY;
                            jumpTeam    = tev.team;
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
            if (s_auto_scroll && past != s_last_divider_row && !selectedScrolled) {
                float rowH = ImGui::GetTextLineHeightWithSpacing();
                float target = ((float)past + 0.5f) * rowH
                               - ImGui::GetWindowHeight() * (2.0f / 3.0f);
                ImGui::SetScrollY(target > 0.0f ? target : 0.0f);
            }
            s_last_divider_row = past;
            s_scroll_to_bottom = false;
        } else if (s_scroll_to_bottom && s_auto_scroll) {
            /* Auto-scroll to bottom if new event was added */
            if (!selectedScrolled) ImGui::SetScrollHereY(1.0f);
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
                                lvEventListCount() > 0 && !s_complete)) {
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
            /* A plain click only moves playback: the view stays where the
             * user put it, and the ring (or its arrow at the view's edge)
             * shows the place. A click on the selected line, or the second
             * click of a double click, also centres the view on the place
             * (lvEventJumpFor). */
            const LvEventJump j = lvEventJumpFor(jumpCentre ? 1 : 0, jumpHighlight ? 1 : 0,
                                                 jumpHasCell ? 1 : 0, jumpTeam);
            if (j.view == LV_EVENT_JUMP_CENTRE) {
                lv_windowCentreOnEvent(jumpMs, j.hasCell, jumpX, jumpY, j.team);
            } else {
                lv_windowSeekToEventKeepView(jumpMs, j.hasCell, jumpX, jumpY, j.team);
            }
        }
    }
    ImGui::End();
}
