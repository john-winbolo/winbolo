/*
 * imgui_bothosts.cpp - ImGui Bot Hosts window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * What the server's bot placement measured and chose for each host, from the
 * recording's log_BotHostData records (bothost_record.h, lv_bothost.h). A
 * server writes them only with -record-bothost-data.
 *
 * One graph for each host (the server's own process is "Server", a player's
 * Bot Host has the player's name and number), over the replay's time:
 *
 *  - the overrun rate of each 1 s probe as a bar, on its own scale (its top
 *    is in the legend); a bad probe has a red mark under its bar;
 *  - the free CPU of each reading and the host's bot CPU, 0 to 100 %;
 *  - the host's bot count and its limit as step lines, on a count scale
 *    whose top is written at the right;
 *  - a marker for each decision (trigger, move, cut, raise, refused raise,
 *    refused placement). With the mouse on a marker, its tooltip gives the
 *    decision and every number the server used. Elsewhere the tooltip gives
 *    the probe and the reading nearest the mouse;
 *  - the play head, at the playback time.
 */

#include "imgui_bothosts.h"
#include "imgui_main_menu.h"
#include "imgui_context.h"
#include "imgui.h"
#include "../lv_bothost.h"
#include "../../gui/lang.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
    /* External functions from backend - using C types directly, as the
     * other panels do. */
    uint32_t lv_screenGetTimeRunning(void);
    uint32_t lv_screenWindowStartMs(void);
    uint32_t lv_screenWindowLenMs(void);
    void lv_screenFormatTime(uint32_t absMs, char *dest, size_t destSize);
    const LvBotHostTrack *lv_screenBotHostTrack(void);
    void lv_clientMutexWaitFor(void);
    void lv_clientMutexRelease(void);
}

/* The floating window's default size. */
#define BOTHOSTS_DEFAULT_W 720.0f
#define BOTHOSTS_DEFAULT_H 420.0f
/* One host's graph. */
#define BOTHOSTS_GRAPH_H   150.0f
/* How near the mouse must be to a decision marker, in pixels. */
#define BOTHOSTS_HIT_PX    5.0f

/* The lines' colours. */
#define COL_BG        IM_COL32(24, 24, 28, 255)
#define COL_GRID      IM_COL32(70, 70, 80, 255)
#define COL_RATE      IM_COL32(230, 150, 40, 200)
#define COL_BAD       IM_COL32(235, 50, 50, 255)
#define COL_FREE      IM_COL32(80, 210, 90, 255)
#define COL_BOT_CPU   IM_COL32(80, 150, 240, 255)
#define COL_BOTS      IM_COL32(235, 235, 235, 255)
#define COL_LIMIT     IM_COL32(240, 220, 60, 255)
#define COL_PLAYHEAD  IM_COL32(255, 255, 255, 160)

/* One host's records, as indexes into s_items. */
struct BotHostView {
    uint8_t          host;
    std::string      label;
    std::vector<int> probes;
    std::vector<int> readings;
    std::vector<int> decisions;
    double           maxRate;
    int              maxCount;
};

/* A copy of the track, taken when it changes, so the graphs draw without the
 * client mutex. */
static std::vector<LvBotHostEntry> s_items;
static std::vector<BotHostView>    s_hosts;
static int                         s_count = -1;
static const void                 *s_ptr = nullptr;
static uint32_t                    s_first_ms = 0;
static uint32_t                    s_last_ms = 0;

/* langGetTextFmt's text, kept before the next call writes over it. */
static std::string fmt_text(langid id, const MessageArgs &args) {
    const char *t = langGetTextFmt(id, &args);
    return t != nullptr ? std::string(t) : std::string();
}

static std::string host_label(const std::vector<LvBotHostEntry> &items, uint8_t host) {
    MessageArgs args = {};
    const BotHostRecord *found = nullptr;
    if (host == 0) {
        return langGetText(STR_LV_BOTHOST_SERVER);
    }
    for (const LvBotHostEntry &e : items) {
        if (e.rec.kind == BOTHOST_RECORD_HOST && e.rec.host == host) found = &e.rec;
    }
    if (found != nullptr && found->player != BOTHOST_RECORD_NONE) {
        snprintf(args.string1, sizeof(args.string1), "%s", found->name);
        args.number = (int)found->player;
        return fmt_text(STR_LV_BOTHOST_PLAYER, args);
    }
    if (found != nullptr && found->name[0] != '\0') {
        snprintf(args.string1, sizeof(args.string1), "%s", found->name);
        return fmt_text(STR_LV_BOTHOST_LISTED, args);
    }
    args.number = (int)host;
    return fmt_text(STR_LV_BOTHOST_UNNAMED, args);
}

/* Take a new copy when the track changed: a load, or a live feed's new
 * records. */
static void refresh(void) {
    lv_clientMutexWaitFor();
    const LvBotHostTrack *t = lv_screenBotHostTrack();
    const bool changed = t != nullptr &&
        (t->count != s_count || (const void *)t->items != s_ptr ||
         (t->count > 0 && (t->items[0].ms != s_first_ms ||
                           t->items[t->count - 1].ms != s_last_ms)));
    if (changed) {
        s_items.assign(t->items, t->items + t->count);
        s_count = t->count;
        s_ptr = (const void *)t->items;
        s_first_ms = t->count > 0 ? t->items[0].ms : 0;
        s_last_ms = t->count > 0 ? t->items[t->count - 1].ms : 0;
    }
    lv_clientMutexRelease();
    if (!changed) return;

    s_hosts.clear();
    int slot[LV_BOTHOST_HOSTS];
    for (int h = 0; h < LV_BOTHOST_HOSTS; h++) slot[h] = -1;
    /* The server first, then the Bot Hosts in their order. */
    bool seen[LV_BOTHOST_HOSTS] = {};
    for (const LvBotHostEntry &e : s_items) seen[e.rec.host] = true;
    for (int h = 0; h < LV_BOTHOST_HOSTS; h++) {
        if (!seen[h]) continue;
        slot[h] = (int)s_hosts.size();
        BotHostView v;
        v.host = (uint8_t)h;
        v.label = host_label(s_items, (uint8_t)h);
        v.maxRate = 0.0;
        v.maxCount = 1;
        s_hosts.push_back(v);
    }
    for (int i = 0; i < (int)s_items.size(); i++) {
        const BotHostRecord &r = s_items[(size_t)i].rec;
        BotHostView &v = s_hosts[(size_t)slot[r.host]];
        switch (r.kind) {
        case BOTHOST_RECORD_PROBE: {
            v.probes.push_back(i);
            const double rate = lv_botHostProbeRate(&r);
            if (rate > v.maxRate) v.maxRate = rate;
            if (r.bots > v.maxCount) v.maxCount = r.bots;
            if (r.slots != BOTHOST_RECORD_NONE && r.slots > v.maxCount) v.maxCount = r.slots;
            if (r.limit != BOTHOST_RECORD_NONE && r.limit > v.maxCount) v.maxCount = r.limit;
            break;
        }
        case BOTHOST_RECORD_READING:
            v.readings.push_back(i);
            break;
        case BOTHOST_RECORD_DECISION:
            v.decisions.push_back(i);
            break;
        default:
            break;
        }
    }
}

static std::string decision_name(uint8_t d) {
    switch (d) {
    case BOTHOST_DECISION_TRIGGER:       return langGetText(STR_LV_BOTHOST_DEC_TRIGGER);
    case BOTHOST_DECISION_MOVE:          return langGetText(STR_LV_BOTHOST_DEC_MOVE);
    case BOTHOST_DECISION_CUT:           return langGetText(STR_LV_BOTHOST_DEC_CUT);
    case BOTHOST_DECISION_RAISE:         return langGetText(STR_LV_BOTHOST_DEC_RAISE);
    case BOTHOST_DECISION_RAISE_REFUSED: return langGetText(STR_LV_BOTHOST_DEC_RAISE_REFUSED);
    case BOTHOST_DECISION_PLACE_REFUSED: return langGetText(STR_LV_BOTHOST_DEC_PLACE_REFUSED);
    default: {
        MessageArgs args = {};
        args.number = (int)d;
        return fmt_text(STR_LV_BOTHOST_DEC_OTHER, args);
    }
    }
}

static ImU32 decision_colour(uint8_t d) {
    switch (d) {
    case BOTHOST_DECISION_TRIGGER:       return IM_COL32(255, 120, 60, 255);
    case BOTHOST_DECISION_MOVE:          return IM_COL32(200, 90, 255, 255);
    case BOTHOST_DECISION_CUT:           return IM_COL32(235, 50, 50, 255);
    case BOTHOST_DECISION_RAISE:         return IM_COL32(60, 230, 120, 255);
    case BOTHOST_DECISION_RAISE_REFUSED: return IM_COL32(150, 150, 150, 255);
    case BOTHOST_DECISION_PLACE_REFUSED: return IM_COL32(90, 200, 230, 255);
    default:                             return IM_COL32(200, 200, 200, 255);
    }
}

/* A byte field as text: its value with a unit, or "not known" for none. */
static void byte_text(uint8_t v, const char *unit, char *buf, size_t len) {
    if (v == BOTHOST_RECORD_NONE) {
        snprintf(buf, len, "%s", langGetText(STR_LV_BOTHOST_NOT_KNOWN));
    } else {
        snprintf(buf, len, "%u%s", (unsigned)v, unit);
    }
}

/* A limit as text: its value, or "none". */
static void limit_text(uint8_t v, char *buf, size_t len) {
    if (v == BOTHOST_RECORD_NONE) {
        snprintf(buf, len, "%s", langGetText(STR_LV_BOTHOST_NO_LIMIT));
    } else {
        snprintf(buf, len, "%u", (unsigned)v);
    }
}

/* The CTRL_BOT_HOST_CPU byte as text: 0..100 %, "<1%", or not known. */
static void bot_cpu_text(uint8_t v, char *buf, size_t len) {
    if (v <= 100) {
        snprintf(buf, len, "%u%%", (unsigned)v);
    } else if (v == 0xFEu) {
        snprintf(buf, len, "<1%%");
    } else {
        snprintf(buf, len, "%s", langGetText(STR_LV_BOTHOST_NOT_KNOWN));
    }
}

/* The bot CPU byte on the 0..100 scale; FALSE when not known. */
static bool bot_cpu_value(uint8_t v, float *out) {
    if (v <= 100) { *out = (float)v; return true; }
    if (v == 0xFEu) { *out = 0.5f; return true; }
    return false;
}

static void tooltip_time(uint32_t ms) {
    MessageArgs args = {};
    lv_screenFormatTime(ms, args.string1, sizeof(args.string1));
    ImGui::TextDisabled("%s", fmt_text(STR_LV_BOTHOST_TIP_TIME, args).c_str());
}

static const BotHostView *view_of(uint8_t host) {
    for (const BotHostView &v : s_hosts) {
        if (v.host == host) return &v;
    }
    return nullptr;
}

static void decision_tooltip(const LvBotHostEntry &e) {
    const BotHostRecord &r = e.rec;
    MessageArgs args = {};
    char a[32], b[32];

    ImGui::TextUnformatted(decision_name(r.decision).c_str());
    tooltip_time(e.ms);
    if (r.reason[0] != '\0') {
        snprintf(args.string1, sizeof(args.string1), "%s", r.reason);
        ImGui::TextUnformatted(fmt_text(STR_LV_BOTHOST_TIP_REASON, args).c_str());
    }
    limit_text(r.limitBefore, a, sizeof(a));
    limit_text(r.limitAfter, b, sizeof(b));
    args = MessageArgs{};
    snprintf(args.string1, sizeof(args.string1), "%s", a);
    snprintf(args.string2, sizeof(args.string2), "%s", b);
    ImGui::TextUnformatted(fmt_text(STR_LV_BOTHOST_TIP_LIMIT, args).c_str());

    args = MessageArgs{};
    args.number = (int)r.badProbes;
    args.number2 = (int)r.probes;
    ImGui::TextUnformatted(fmt_text(STR_LV_BOTHOST_TIP_BAD_SHARE, args).c_str());

    const bool costKnown = (r.flags & BOTHOST_RECORD_DEC_COST) != 0;
    const bool freeKnown = (r.flags & BOTHOST_RECORD_DEC_FREE) != 0;
    args = MessageArgs{};
    if (costKnown && freeKnown) {
        snprintf(args.string1, sizeof(args.string1), "%.1f%%", (double)r.predictedTenths / 10.0);
    } else {
        snprintf(args.string1, sizeof(args.string1), "%s", langGetText(STR_LV_BOTHOST_NOT_KNOWN));
    }
    ImGui::TextUnformatted(fmt_text(STR_LV_BOTHOST_TIP_PREDICTED, args).c_str());

    args = MessageArgs{};
    if (costKnown) {
        snprintf(args.string1, sizeof(args.string1), "%.1f%%", (double)r.costTenths / 10.0);
    } else {
        snprintf(args.string1, sizeof(args.string1), "%s", langGetText(STR_LV_BOTHOST_NOT_KNOWN));
    }
    ImGui::TextUnformatted(fmt_text(STR_LV_BOTHOST_TIP_COST, args).c_str());

    args = MessageArgs{};
    if (freeKnown) {
        snprintf(args.string1, sizeof(args.string1), "%.1f%%", (double)r.freeAvgTenths / 10.0);
    } else {
        snprintf(args.string1, sizeof(args.string1), "%s", langGetText(STR_LV_BOTHOST_NOT_KNOWN));
    }
    ImGui::TextUnformatted(fmt_text(STR_LV_BOTHOST_TIP_FREE_AVG, args).c_str());

    args = MessageArgs{};
    byte_text(r.lowestClean, "%", args.string1, sizeof(args.string1));
    ImGui::TextUnformatted(fmt_text(STR_LV_BOTHOST_TIP_LOWEST_CLEAN, args).c_str());

    args = MessageArgs{};
    byte_text(r.highestBad, "%", args.string1, sizeof(args.string1));
    ImGui::TextUnformatted(fmt_text(STR_LV_BOTHOST_TIP_HIGHEST_BAD, args).c_str());

    if (r.seat != BOTHOST_RECORD_NONE) {
        args = MessageArgs{};
        args.number = (int)r.seat;
        ImGui::TextUnformatted(fmt_text(STR_LV_BOTHOST_TIP_SEAT, args).c_str());
    }
    if (r.toHost != BOTHOST_RECORD_NONE) {
        const BotHostView *to = view_of(r.toHost);
        args = MessageArgs{};
        snprintf(args.string1, sizeof(args.string1), "%s",
                 to != nullptr ? to->label.c_str() : host_label(s_items, r.toHost).c_str());
        ImGui::TextUnformatted(fmt_text(STR_LV_BOTHOST_TIP_TO, args).c_str());
    }
}

/* The index in idx (indexes into s_items) of the record nearest ms, or -1. */
static int nearest(const std::vector<int> &idx, uint32_t ms) {
    int lo = 0, hi = (int)idx.size();
    if (hi == 0) return -1;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (s_items[(size_t)idx[(size_t)mid]].ms < ms) lo = mid + 1; else hi = mid;
    }
    if (lo >= (int)idx.size()) return idx.back();
    if (lo == 0) return idx[0];
    const uint32_t after = s_items[(size_t)idx[(size_t)lo]].ms - ms;
    const uint32_t before = ms - s_items[(size_t)idx[(size_t)lo - 1]].ms;
    return before <= after ? idx[(size_t)lo - 1] : idx[(size_t)lo];
}

static void point_tooltip(const BotHostView &v, uint32_t ms) {
    const int p = nearest(v.probes, ms);
    const int q = nearest(v.readings, ms);
    if (p < 0 && q < 0) return;
    ImGui::BeginTooltip();
    if (p >= 0) {
        const LvBotHostEntry &e = s_items[(size_t)p];
        const BotHostRecord &r = e.rec;
        MessageArgs args = {};
        tooltip_time(e.ms);
        snprintf(args.string1, sizeof(args.string1), "%.2f", lv_botHostProbeRate(&r));
        args.number = (int)r.overruns;
        args.number2 = (int)r.thinks;
        ImGui::TextUnformatted(fmt_text(STR_LV_BOTHOST_TIP_PROBE, args).c_str());
        if (r.flags & BOTHOST_RECORD_PROBE_BAD) {
            ImGui::TextColored(ImVec4(0.92f, 0.2f, 0.2f, 1.0f), "%s",
                               langGetText(STR_LV_BOTHOST_TIP_BAD_PROBE));
        }
        args = MessageArgs{};
        args.number = (int)r.badProbes;
        args.number2 = (int)r.probes;
        ImGui::TextUnformatted(fmt_text(STR_LV_BOTHOST_TIP_BAD_SHARE, args).c_str());
        args = MessageArgs{};
        args.number = (int)r.bots;
        args.number2 = (int)r.slots;
        limit_text(r.limit, args.string1, sizeof(args.string1));
        ImGui::TextUnformatted(fmt_text(STR_LV_BOTHOST_TIP_BOTS, args).c_str());
    }
    if (q >= 0) {
        const LvBotHostEntry &e = s_items[(size_t)q];
        MessageArgs args = {};
        if (p >= 0) ImGui::Separator();
        tooltip_time(e.ms);
        byte_text(e.rec.freePct, "%", args.string1, sizeof(args.string1));
        bot_cpu_text(e.rec.botCpu, args.string2, sizeof(args.string2));
        ImGui::TextUnformatted(fmt_text(STR_LV_BOTHOST_TIP_READING, args).c_str());
        if (e.rec.flags & BOTHOST_RECORD_READING_BAD) {
            ImGui::TextColored(ImVec4(0.92f, 0.2f, 0.2f, 1.0f), "%s",
                               langGetText(STR_LV_BOTHOST_TIP_BAD_PROBE));
        }
    }
    ImGui::EndTooltip();
}

static void legend_item(ImU32 col, const char *text) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetTextLineHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.3f), ImVec2(p.x + h * 0.8f, p.y + h * 0.7f), col);
    ImGui::Dummy(ImVec2(h * 0.9f, h));
    ImGui::SameLine();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(0.0f, 14.0f);
}

/* A polyline of the points, broken where a point is not known. */
static void draw_line(ImDrawList *dl, const std::vector<ImVec2> &pts, ImU32 col, float th) {
    /* Segment by segment: AddPolyline's argument order is not the same in
     * every ImGui version. */
    if (pts.size() >= 2) {
        for (size_t k = 1; k < pts.size(); k++) {
            dl->AddLine(pts[k - 1], pts[k], col, th);
        }
    } else if (pts.size() == 1) {
        dl->AddCircleFilled(pts[0], th + 1.0f, col);
    }
}

static void draw_host(const BotHostView &v, uint32_t x0, uint32_t x1, uint32_t nowMs) {
    ImGui::PushID((int)v.host);
    ImGui::SeparatorText(v.label.c_str());

    const double rateTop = v.maxRate * 1.1 > 2.0 ? (v.maxRate * 1.1 > 100.0 ? 100.0 : v.maxRate * 1.1) : 2.0;
    const int countTop = v.maxCount + 1;
    {
        MessageArgs args = {};
        snprintf(args.string1, sizeof(args.string1), "%.1f", rateTop);
        const std::string rate = fmt_text(STR_LV_BOTHOST_RATE, args);
        legend_item(COL_RATE, rate.c_str());
        legend_item(COL_FREE, langGetText(STR_LV_BOTHOST_FREE));
        legend_item(COL_BOT_CPU, langGetText(STR_LV_BOTHOST_BOT_CPU));
        legend_item(COL_BOTS, langGetText(STR_LV_BOTHOST_BOTS));
        legend_item(COL_LIMIT, langGetText(STR_LV_BOTHOST_LIMIT));
        ImGui::NewLine();
    }

    const float w = ImGui::GetContentRegionAvail().x;
    if (w < 40.0f) {
        ImGui::PopID();
        return;
    }
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 b(a.x + w, a.y + BOTHOSTS_GRAPH_H);
    ImGui::InvisibleButton("graph", ImVec2(w, BOTHOSTS_GRAPH_H));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(a, b, true);
    dl->AddRectFilled(a, b, COL_BG);
    /* The bottom strip holds the bad probe marks. */
    const float markH = 4.0f;
    const float top = a.y + 2.0f;
    const float bottom = b.y - markH - 1.0f;
    const float span = (float)(x1 > x0 ? x1 - x0 : 1u);
    auto xOf = [&](uint32_t ms) {
        float f = (float)((double)((int64_t)ms - (int64_t)x0) / span);
        return a.x + f * w;
    };
    auto yPct = [&](float pct) { return bottom - (bottom - top) * (pct / 100.0f); };
    auto yCount = [&](float n) { return bottom - (bottom - top) * (n / (float)countTop); };
    for (int g = 1; g < 4; g++) {
        const float y = yPct(25.0f * (float)g);
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), COL_GRID);
    }

    /* The overrun rate of each probe, and the bad probe marks. */
    for (int i : v.probes) {
        const LvBotHostEntry &e = s_items[(size_t)i];
        const float x = xOf(e.ms);
        const double rate = lv_botHostProbeRate(&e.rec);
        if (rate > 0.0) {
            const float f = (float)(rate >= rateTop ? 1.0 : rate / rateTop);
            dl->AddLine(ImVec2(x, bottom), ImVec2(x, bottom - (bottom - top) * f), COL_RATE);
        }
        if (e.rec.flags & BOTHOST_RECORD_PROBE_BAD) {
            dl->AddLine(ImVec2(x, b.y - markH), ImVec2(x, b.y), COL_BAD, 2.0f);
        }
    }

    /* Free CPU and bot CPU of each reading. */
    {
        std::vector<ImVec2> fr, bc;
        for (int i : v.readings) {
            const LvBotHostEntry &e = s_items[(size_t)i];
            const float x = xOf(e.ms);
            if (e.rec.freePct <= 100) {
                fr.push_back(ImVec2(x, yPct((float)e.rec.freePct)));
            } else {
                draw_line(dl, fr, COL_FREE, 1.5f);
                fr.clear();
            }
            float c;
            if (bot_cpu_value(e.rec.botCpu, &c)) {
                bc.push_back(ImVec2(x, yPct(c)));
            } else {
                draw_line(dl, bc, COL_BOT_CPU, 1.5f);
                bc.clear();
            }
        }
        draw_line(dl, fr, COL_FREE, 1.5f);
        draw_line(dl, bc, COL_BOT_CPU, 1.5f);
    }

    /* The bot count and the limit as step lines. */
    {
        std::vector<ImVec2> bots, lim;
        for (size_t k = 0; k < v.probes.size(); k++) {
            const LvBotHostEntry &e = s_items[(size_t)v.probes[k]];
            const float x = xOf(e.ms);
            const float yb = yCount((float)e.rec.bots);
            if (!bots.empty()) bots.push_back(ImVec2(x, bots.back().y));
            bots.push_back(ImVec2(x, yb));
            if (e.rec.limit != BOTHOST_RECORD_NONE) {
                const float yl = yCount((float)e.rec.limit);
                if (!lim.empty()) lim.push_back(ImVec2(x, lim.back().y));
                lim.push_back(ImVec2(x, yl));
            } else if (!lim.empty()) {
                lim.push_back(ImVec2(x, lim.back().y));
                draw_line(dl, lim, COL_LIMIT, 2.0f);
                lim.clear();
            }
        }
        draw_line(dl, bots, COL_BOTS, 1.5f);
        draw_line(dl, lim, COL_LIMIT, 2.0f);
    }

    /* The decisions. */
    int hit = -1;
    float hitDist = BOTHOSTS_HIT_PX + 1.0f;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (int i : v.decisions) {
        const LvBotHostEntry &e = s_items[(size_t)i];
        const float x = xOf(e.ms);
        const ImU32 col = decision_colour(e.rec.decision);
        dl->AddLine(ImVec2(x, top), ImVec2(x, bottom), col, 1.0f);
        dl->AddTriangleFilled(ImVec2(x - 5.0f, top), ImVec2(x + 5.0f, top),
                              ImVec2(x, top + 8.0f), col);
        if (hovered) {
            const float d = std::fabs(mouse.x - x);
            if (d <= BOTHOSTS_HIT_PX && d < hitDist) {
                hitDist = d;
                hit = i;
            }
        }
    }

    /* The play head. */
    if (nowMs >= x0 && nowMs <= x1) {
        const float x = xOf(nowMs);
        dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), COL_PLAYHEAD, 2.0f);
    }

    /* The count scale's top, at the right. */
    {
        char t[16];
        snprintf(t, sizeof(t), "%d", countTop);
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(b.x - ts.x - 3.0f, top), COL_BOTS, t);
    }
    dl->PopClipRect();

    if (hovered) {
        if (hit >= 0) {
            ImGui::BeginTooltip();
            decision_tooltip(s_items[(size_t)hit]);
            ImGui::EndTooltip();
        } else {
            const double f = (double)(mouse.x - a.x) / (double)w;
            const double msD = (double)x0 + f * (double)span;
            const uint32_t ms = msD <= 0.0 ? 0u : (uint32_t)msD;
            point_tooltip(v, ms);
        }
    }
    ImGui::PopID();
}

void lv_imgui_bothosts_window(void) {
    if (!lv_g_show_bot_hosts_window) {
        return;
    }
    refresh();

    {
        const ImGuiCond cond = lv_g_reset_window_positions ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        const ImVec2 vp = ImGui::GetMainViewport()->Size;
        ImGui::SetNextWindowSize(ImVec2(BOTHOSTS_DEFAULT_W, BOTHOSTS_DEFAULT_H), cond);
        float x = (vp.x - BOTHOSTS_DEFAULT_W) * 0.5f;
        float y = (vp.y - BOTHOSTS_DEFAULT_H) * 0.5f;
        if (x < 10.0f) x = 10.0f;
        if (y < 30.0f) y = 30.0f;
        ImGui::SetNextWindowPos(ImVec2(x, y), cond);
    }

    char title[128];
    snprintf(title, sizeof(title), "%s###bothosts", langGetText(STR_LV_WIN_BOT_HOSTS));
    if (ImGui::Begin(title, &lv_g_show_bot_hosts_window, ImGuiWindowFlags_NoCollapse)) {
        lv_imgui_keep_window_on_screen();
        if (s_hosts.empty()) {
            ImGui::TextWrapped("%s", langGetText(STR_LV_BOTHOST_NONE));
        } else {
            /* The replay's presented time, as the seek slider has it; the
             * records' own span when there is none. */
            uint32_t x0 = lv_screenWindowStartMs();
            uint32_t len = lv_screenWindowLenMs();
            uint32_t x1 = x0 + len;
            if (len == 0) {
                x0 = s_first_ms;
                x1 = s_last_ms > s_first_ms ? s_last_ms : s_first_ms + 1u;
            }
            const uint32_t nowMs = lv_screenGetTimeRunning();
            for (const BotHostView &v : s_hosts) {
                draw_host(v, x0, x1, nowMs);
            }
        }
    }
    ImGui::End();
}
