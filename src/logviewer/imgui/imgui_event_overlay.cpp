/*
 * imgui_event_overlay.cpp - event markers over the Log Viewer's seek slider
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "imgui_event_overlay.h"
#include "imgui_events.h"
#include "imgui_lv_icons.h"
#include "imgui.h"
#include "platform_config.h"
#include "../../gui/lang.h"
#include "../../gui/sdl3/dialogs/imgui_dialog_utils.h"
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
    #include "../lv_event_overlay.h"
    #include "../lv_hud.h"
    #include "../../gui/tiles.h"
    #include "../../gui/sdl3/map_colours.h"  /* mapColourTeam */

    void lv_screenFormatTime(uint32_t absMs, char *dest, size_t destSize);
    uint32_t lv_screenGetTimeRunning(void);
    uint32_t lv_screenWindowStartMs(void);
    uint32_t lv_screenWindowLenMs(void);
    uint32_t lv_screenServerTickAt(uint32_t ms);
    bool lv_screenHasServerTick(void);
    bool lv_screenSpecIsLiveMode(void);
    unsigned char lv_screenTeamColour(int team);
    void lv_windowSeekToHighlight(uint32_t ms, int mapX, int mapY);
    void lv_windowSeekToEvent(uint32_t ms, int hasCell, int mapX, int mapY, int team);
    SDL_Texture *lv_drawGetTilesTexture(void);
}

/* The LOGVIEWER preference that keeps the ticked kinds: their saved names
 * (lvEventKindKey) with commas between them, or "none". */
static const char *const k_pref = "Controls.OverlayEvents";

/* How much of its colour a marker playback has passed keeps. */
#define OVERLAY_PAST_ALPHA 0.45f

/* The lines of a marker's tooltip when the marker stands for several
 * events; the rest are counted in a last line. */
#define OVERLAY_TIP_LINES 5

/* The gap between the lane and the slider under it. */
#define OVERLAY_LANE_GAP 1.0f

/* The ticked kinds, one bit a kind. */
static unsigned s_mask = LV_OVERLAY_DEFAULT_MASK;
static bool s_loaded = false;

/* What the lane draws, worked out from the list of the Events panel and
 * kept until one of the things it depends on changes. The places are
 * measured from the left end of the slider, so a panel that only moves
 * keeps them. */
struct OverlayMarker {
    float    x;        /* middle of the marker, from the slider's left end */
    int      first;    /* index into s_items of its first event */
    int      count;
    int      kind;     /* the kind of all its events, or -1 when they differ */
    int      team;     /* the team most of them name, or -1 */
    int      otherTeam;/* a lone event's second team, or -1 */
    uint32_t timeMs;   /* the time of its first event */
};
static std::vector<int>              s_items;       /* event indexes, in time order */
static std::vector<uint32_t>         s_item_time;
static std::vector<float>            s_item_x;
static std::vector<LvOverlayCluster> s_clusters;
static std::vector<OverlayMarker>    s_markers;
static int      s_kind_count[LV_EVK_COUNT];  /* every kind, ticked or not */
static bool     s_built = false;
static unsigned s_built_version = 0;
static unsigned s_built_mask = 0;
static float    s_built_width = 0.0f;
static float    s_built_grab = 0.0f;
static float    s_built_gap = 0.0f;
static uint32_t s_built_start = 0;
static uint32_t s_built_len = 0;

static void overlay_load(void) {
    char def[256], val[256];
    if (s_loaded) return;
    s_loaded = true;
    lvOverlayMaskToText(LV_OVERLAY_DEFAULT_MASK, def, sizeof(def));
    lv_platform_config_get_string("LOGVIEWER", k_pref, def, val, sizeof(val));
    s_mask = lvOverlayMaskFromText(val);
}

static void overlay_save(void) {
    char val[256];
    lvOverlayMaskToText(s_mask, val, sizeof(val));
    lv_platform_config_set_string("LOGVIEWER", k_pref, val);
}

void lv_event_overlay_init(void) {
    s_loaded = false;
    s_built = false;
    overlay_load();
}

/* The side of a marker: the height of a line of text. */
static float overlay_side(void) {
    return floorf(ImGui::GetFontSize());
}

float lv_event_overlay_lane_height(void) {
    overlay_load();
    if (s_mask == 0) return 0.0f;
    /* The marker, then two points for the tick of a second team. */
    return overlay_side() + 3.0f + OVERLAY_LANE_GAP;
}

/* The drawn icon of a kind. */
static int overlay_kind_icon(int kind) {
    switch (kind) {
    case LV_EVK_PILL_CAPTURED:
    case LV_EVK_PILL_STOLEN:   return LV_ICON_SQUARE;
    case LV_EVK_BASE_CAPTURED:
    case LV_EVK_BASE_STOLEN:   return LV_ICON_DISC;
    case LV_EVK_BUILDER_LOST:  return LV_ICON_PERSON;
    case LV_EVK_TANK_KILLED:   return LV_ICON_CLOSE;
    case LV_EVK_PLAYER:        return LV_ICON_RING;
    case LV_EVK_CHAT:          return LV_ICON_BUBBLE;
    case LV_EVK_HIGHLIGHT:     return LV_ICON_STAR;
    default:                   return LV_ICON_DIAMOND;
    }
}

/* A pillbox or a base changed hands: the marker is the HUD panel's shape
 * for an owned one, in the colour of the team that now owns it. Any other
 * kind is a plain icon with the team's colour round it. */
static bool overlay_kind_is_owned_shape(int kind) {
    return kind == LV_EVK_PILL_CAPTURED || kind == LV_EVK_PILL_STOLEN ||
           kind == LV_EVK_BASE_CAPTURED || kind == LV_EVK_BASE_STOLEN;
}

/* The colour of a team, as the HUD panel draws it: lifted where it would be
 * too dark to see on the panel, and a red team orange. */
static ImVec4 overlay_team_colour(int team) {
    SDL_FColor c = mapColourTeam(lv_screenTeamColour(team));
    if (!lvHudRedToOrange(&c.r, &c.g, &c.b)) {
        lvHudReadableColour(&c.r, &c.g, &c.b);
    }
    return ImVec4(c.r, c.g, c.b, 1.0f);
}

static ImU32 overlay_u32(ImVec4 c, float alpha) {
    c.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

/* Bring what the lane draws up to date for a slider `width` wide. Cheap to
 * call every frame: it reads the list only when the list, the ticked
 * kinds, the shown stretch of the log or the width has changed. */
static void overlay_refresh(float width, float grabMin, float minGap) {
    const unsigned version = lv_imgui_events_version();
    const uint32_t start = lv_screenWindowStartMs();
    const uint32_t len = lv_screenWindowLenMs();
    if (s_built && s_built_version == version && s_built_mask == s_mask &&
        s_built_width == width && s_built_grab == grabMin && s_built_gap == minGap &&
        s_built_start == start && s_built_len == len) {
        return;
    }
    s_built = true;
    s_built_version = version;
    s_built_mask = s_mask;
    s_built_width = width;
    s_built_grab = grabMin;
    s_built_gap = minGap;
    s_built_start = start;
    s_built_len = len;

    s_items.clear();
    s_item_time.clear();
    s_item_x.clear();
    s_markers.clear();
    memset(s_kind_count, 0, sizeof(s_kind_count));

    const int total = lv_imgui_events_get_count();
    bool ordered = true;
    uint32_t last = 0;
    for (int i = 0; i < total; i++) {
        LvEventInfo info;
        if (!lv_imgui_events_get_info(i, &info)) continue;
        /* The lobby is hidden: so are its events, as in the Events panel. */
        if (info.timeMs < start) continue;
        if (info.kind < 0 || info.kind >= LV_EVK_COUNT) continue;
        s_kind_count[info.kind]++;
        if ((s_mask & (1u << info.kind)) == 0) continue;
        if (info.timeMs < last) ordered = false;
        last = info.timeMs;
        s_items.push_back(i);
        s_item_time.push_back(info.timeMs);
    }
    /* The highlight clips are in front of the list, whatever their times. */
    if (!ordered) {
        std::stable_sort(s_items.begin(), s_items.end(), [](int a, int b) {
            LvEventInfo ia, ib;
            lv_imgui_events_get_info(a, &ia);
            lv_imgui_events_get_info(b, &ib);
            return ia.timeMs < ib.timeMs;
        });
        for (size_t n = 0; n < s_items.size(); n++) {
            LvEventInfo info;
            lv_imgui_events_get_info(s_items[n], &info);
            s_item_time[n] = info.timeMs;
        }
    }
    s_item_x.resize(s_items.size());
    for (size_t n = 0; n < s_items.size(); n++) {
        s_item_x[n] = lvOverlaySliderX(0.0f, width, grabMin,
                                       lvOverlayRatio(s_item_time[n], start, len));
    }
    if (s_items.empty()) return;

    /* No two markers are nearer than minGap, so this many is the most. */
    int most = (int)(width / ((minGap > 1.0f) ? minGap : 1.0f)) + 2;
    if (most < 1) most = 1;
    s_clusters.resize((size_t)most);
    const int made = lvOverlayClusters(s_item_x.data(), (int)s_items.size(), minGap,
                                       s_clusters.data(), most);
    s_markers.resize((size_t)made);
    for (int m = 0; m < made; m++) {
        const LvOverlayCluster &c = s_clusters[m];
        OverlayMarker &out = s_markers[m];
        int teams[LV_OVERLAY_TEAMS];
        memset(teams, 0, sizeof(teams));
        out.x = c.x;
        out.first = c.first;
        out.count = c.count;
        out.kind = -1;
        out.otherTeam = -1;
        out.timeMs = s_item_time[c.first];
        for (int k = 0; k < c.count; k++) {
            LvEventInfo info;
            if (!lv_imgui_events_get_info(s_items[c.first + k], &info)) continue;
            if (k == 0) {
                out.kind = info.kind;
                out.otherTeam = info.otherTeam;
            } else if (out.kind != info.kind) {
                out.kind = -1;
            }
            teams[(info.team >= 0 && info.team < LV_OVERLAY_TEAMS - 1) ? info.team
                                                                       : LV_OVERLAY_TEAMS - 1]++;
        }
        out.team = lvOverlayMajorityTeam(teams);
        if (c.count > 1) out.otherTeam = -1;
    }
}

/* The game's own "dead" picture of the status panels, over p0..p1. False
 * when the tile sheet is not there to take it from. */
static bool overlay_dead_picture(ImDrawList *dl, ImVec2 p0, ImVec2 p1, float alpha) {
    SDL_Texture *tex = lv_drawGetTilesTexture();
    float tw = 0.0f, th = 0.0f;
    if (tex == nullptr || !SDL_GetTextureSize(tex, &tw, &th) || tw <= 0.0f || th <= 0.0f) {
        return false;
    }
    dl->AddImage((ImTextureID)(size_t)tex, p0, p1,
                 ImVec2((float)STATUS_ITEM_DEAD_X / tw, (float)STATUS_ITEM_DEAD_Y / th),
                 ImVec2((float)(STATUS_ITEM_DEAD_X + STATUS_ITEM_SIZE_X) / tw,
                        (float)(STATUS_ITEM_DEAD_Y + STATUS_ITEM_SIZE_Y) / th),
                 overlay_u32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), alpha));
    return true;
}

/* The marker of one kind of event in the box p0..p1 (a square). team is
 * the team that did it, or -1. */
static void overlay_draw_kind(ImDrawList *dl, int kind, int team, ImVec2 p0, ImVec2 p1,
                              float alpha) {
    const ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    const ImU32 dark = overlay_u32(ImVec4(0.0f, 0.0f, 0.0f, 0.8f), alpha);
    if (overlay_kind_is_owned_shape(kind)) {
        /* The shape in the colour of its new owner, on a dark edge so a
         * dark colour still shows on a dark panel. */
        const ImVec4 fill = (team >= 0) ? overlay_team_colour(team) : text;
        lv_icon_draw(dl, overlay_kind_icon(kind), p0, p1, dark);
        lv_icon_draw(dl, overlay_kind_icon(kind), ImVec2(p0.x + 1.0f, p0.y + 1.0f),
                     ImVec2(p1.x - 1.0f, p1.y - 1.0f), overlay_u32(fill, alpha));
        return;
    }
    /* A plain icon, with the colour of the team as a line round it. */
    const ImVec2 i0(p0.x + 2.0f, p0.y + 2.0f), i1(p1.x - 2.0f, p1.y - 2.0f);
    dl->AddRectFilled(p0, p1, dark, 2.0f);
    if (kind != LV_EVK_TANK_KILLED || !overlay_dead_picture(dl, i0, i1, alpha)) {
        lv_icon_draw(dl, overlay_kind_icon(kind), ImVec2(i0.x + 1.0f, i0.y + 1.0f),
                     ImVec2(i1.x - 1.0f, i1.y - 1.0f), overlay_u32(text, alpha));
    }
    if (team >= 0) {
        dl->AddRect(p0, p1, overlay_u32(overlay_team_colour(team), alpha), 2.0f, 0, 1.0f);
    }
}

/* A marker that stands for several events: a box in the colour of the team
 * most of them name, with how many they are. */
static void overlay_draw_count(ImDrawList *dl, const OverlayMarker &m, ImVec2 p0, ImVec2 p1,
                               float alpha) {
    const ImVec4 fill = (m.team >= 0) ? overlay_team_colour(m.team)
                                      : ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    const float luma = 0.299f * fill.x + 0.587f * fill.y + 0.114f * fill.z;
    const ImVec4 ink = (luma > 0.55f) ? ImVec4(0.0f, 0.0f, 0.0f, 1.0f)
                                      : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    char num[8];
    if (m.count > 99) {
        snprintf(num, sizeof(num), "99+");
    } else {
        snprintf(num, sizeof(num), "%d", m.count);
    }
    dl->AddRectFilled(p0, p1, overlay_u32(fill, alpha), 3.0f);
    /* The number as large as fits in the box. */
    ImFont *font = ImGui::GetFont();
    float size = ImGui::GetFontSize();
    ImVec2 ts = ImGui::CalcTextSize(num);
    const float room = (p1.x - p0.x) - 2.0f;
    if (ts.x > room && ts.x > 0.0f) {
        const float scale = room / ts.x;
        size *= scale;
        ts.x *= scale;
        ts.y *= scale;
    }
    dl->AddText(font, size,
                ImVec2(floorf(p0.x + ((p1.x - p0.x) - ts.x) * 0.5f),
                       floorf(p0.y + ((p1.y - p0.y) - ts.y) * 0.5f)),
                overlay_u32(ink, alpha), num);
}

/* "12:34  Tick 37020": when an event was, as the Controls panel says the
 * time and the tick of playback. */
static void overlay_format_when(uint32_t ms, char *out, size_t outSize) {
    char stamp[24];
    const uint32_t tick = lv_screenHasServerTick() ? lv_screenServerTickAt(ms) : ms / 20u;
    lv_screenFormatTime(ms, stamp, sizeof(stamp));
    snprintf(out, outSize, "%s  %s %u", stamp, langGetText(STR_DLGSYSINFO_TICK), (unsigned)tick);
}

/* What the mouse over a marker says. Brief: one event is its time, its
 * tick and its kind, then its line of the Events panel; several are their
 * number, then the first few lines with their times. */
static void overlay_tooltip(const OverlayMarker &m) {
    char when[96];
    LvEventInfo info;
    if (!ImGui::BeginTooltip()) return;
    if (m.count == 1) {
        if (lv_imgui_events_get_info(s_items[m.first], &info)) {
            overlay_format_when(info.timeMs, when, sizeof(when));
            ImGui::TextDisabled("%s  %s", when, lv_imgui_events_kind_label(info.kind));
            ImGui::TextUnformatted(info.text);
        }
    } else {
        MessageArgs args = {};
        snprintf(args.string1, sizeof(args.string1), "%d", m.count);
        ImGui::TextDisabled("%s", langGetTextFmt(STR_LV_OVERLAY_COUNT, &args));
        const int lines = (m.count < OVERLAY_TIP_LINES) ? m.count : OVERLAY_TIP_LINES;
        for (int k = 0; k < lines; k++) {
            char stamp[24];
            if (!lv_imgui_events_get_info(s_items[m.first + k], &info)) continue;
            lv_screenFormatTime(info.timeMs, stamp, sizeof(stamp));
            ImGui::Text("%s  %s", stamp, info.text);
        }
        if (m.count > lines) {
            MessageArgs more = {};
            snprintf(more.string1, sizeof(more.string1), "%d", m.count - lines);
            ImGui::TextDisabled("%s", langGetTextFmt(STR_LV_OVERLAY_MORE, &more));
        }
    }
    ImGui::EndTooltip();
}

void lv_event_overlay_lane(void) {
    overlay_load();
    if (s_mask == 0) return;

    const ImGuiStyle &style = ImGui::GetStyle();
    const float side = overlay_side();
    const float laneH = side + 3.0f;
    /* The width the slider under the lane gets: the same call it makes. */
    const float width = ImGui::CalcItemWidth();
    if (width < 1.0f) return;

    /* The lane is an item of its own, so a press in it is not a press on
     * the window (which would drag the panel) and not one on the slider. */
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, OVERLAY_LANE_GAP));
    const bool pressed = ImGui::InvisibleButton("##evlane", ImVec2(width, laneH));
    ImGui::PopStyleVar();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 lane0 = ImGui::GetItemRectMin();

    overlay_refresh(width, style.GrabMinSize, side);
    if (s_markers.empty()) return;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const uint32_t nowMs = lv_screenGetTimeRunning();
    const float half = side * 0.5f;
    int hot = -1;
    if (hovered) {
        hot = lvOverlayHit(s_clusters.data(), (int)s_markers.size(),
                           ImGui::GetIO().MousePos.x - lane0.x, half);
    }

    for (int i = 0; i < (int)s_markers.size(); i++) {
        const OverlayMarker &m = s_markers[i];
        /* Whole pixels, so the shapes are sharp. */
        const float left = floorf(lane0.x + m.x - half);
        const ImVec2 p0(left, lane0.y), p1(left + side, lane0.y + side);
        /* Playback has passed it: dimmer, as the lines to come are in the
         * Events panel the other way round. The one under the mouse is
         * always at full strength. */
        const float alpha = (i == hot || m.timeMs > nowMs) ? 1.0f : OVERLAY_PAST_ALPHA;
        if (m.count == 1) {
            overlay_draw_kind(dl, m.kind, m.team, p0, p1, alpha);
            /* The team it was done to: a short tick under the marker. */
            if (m.otherTeam >= 0) {
                dl->AddRectFilled(ImVec2(p0.x + 2.0f, p1.y + 1.0f), ImVec2(p1.x - 2.0f, p1.y + 3.0f),
                                  overlay_u32(overlay_team_colour(m.otherTeam), alpha));
            }
        } else {
            overlay_draw_count(dl, m, p0, p1, alpha);
        }
        if (i == hot) {
            dl->AddRect(ImVec2(p0.x - 1.0f, p0.y - 1.0f), ImVec2(p1.x + 1.0f, p1.y + 1.0f),
                        IM_COL32_WHITE, 3.0f, 0, 1.0f);
        }
    }

    if (hot < 0) return;
    const OverlayMarker m = s_markers[hot];
    overlay_tooltip(m);
    /* A click goes to the event, as a click on its line of the Events panel
     * does; and as there, not on a live feed, where a stray click would
     * throw the viewer off the live head. */
    if (lv_screenSpecIsLiveMode()) return;
    imguiHandOnHover();
    if (pressed) {
        LvEventInfo info;
        if (lv_imgui_events_get_info(s_items[m.first], &info)) {
            if (info.highlight) {
                lv_windowSeekToHighlight(info.timeMs > 5000u ? info.timeMs - 5000u : 0u,
                                         info.mapX, info.mapY);
            } else {
                lv_windowSeekToEvent(info.timeMs, info.hasCell, info.mapX, info.mapY,
                                     info.team);
            }
        }
    }
}

float lv_event_overlay_combo_width(bool full) {
    const ImGuiStyle &style = ImGui::GetStyle();
    const float arrow = ImGui::GetFrameHeight();
    if (!full) return arrow;
    return ImGui::CalcTextSize(langGetText(STR_LV_OVERLAY_EVENTS)).x + style.FramePadding.x * 2.0f +
           arrow;
}

void lv_event_overlay_combo(bool full) {
    overlay_load();
    const char *name = langGetText(STR_LV_OVERLAY_EVENTS);
    ImGui::SetNextItemWidth(lv_event_overlay_combo_width(full));
    const bool open = ImGui::BeginCombo("##evoverlay", name,
                                        ImGuiComboFlags_HeightLargest |
                                        (full ? 0 : ImGuiComboFlags_NoPreview));
    if (!open) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            /* The short one has no name on it. */
            if (full) {
                ImGui::SetTooltip("%s", langGetText(STR_LV_OVERLAY_TIP));
            } else {
                ImGui::SetTooltip("%s\n%s", name, langGetText(STR_LV_OVERLAY_TIP));
            }
        }
        return;
    }
    /* The counts are those of the log as the lane last saw it; with no
     * kind ticked the lane does not look, so look here. */
    if (s_mask == 0 || !s_built) {
        overlay_refresh(s_built_width, s_built_grab, (s_built_gap > 0.0f) ? s_built_gap : 13.0f);
    }
    unsigned mask = s_mask;
    /* Buttons and check boxes leave the list open, so several kinds can be
     * ticked in one go. */
    if (ImGui::SmallButton(langGetText(STR_DLGPLAYERS_ALL))) {
        mask = LV_OVERLAY_ALL_MASK;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(langGetText(STR_DLGPLAYERS_NONE))) {
        mask = 0;
    }
    ImGui::Separator();
    for (int kind = 0; kind < LV_EVK_COUNT; kind++) {
        char label[160];
        bool on = (mask & (1u << kind)) != 0;
        /* The marker of the kind, so the list is also the key to the lane.
         * It is as tall as the text, in the middle of the check box's row. */
        {
            const float side = overlay_side();
            const float rowH = ImGui::GetFrameHeight();
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float top = floorf(at.y + (rowH - side) * 0.5f);
            lv_icon_draw(ImGui::GetWindowDrawList(), overlay_kind_icon(kind),
                         ImVec2(at.x + 1.0f, top + 1.0f), ImVec2(at.x + side - 1.0f, top + side - 1.0f),
                         ImGui::GetColorU32(ImGuiCol_Text));
            ImGui::Dummy(ImVec2(side, rowH));
            ImGui::SameLine();
        }
        snprintf(label, sizeof(label), "%s (%d)###evk%d", lv_imgui_events_kind_label(kind),
                 s_kind_count[kind], kind);
        if (ImGui::Checkbox(label, &on)) {
            mask = on ? (mask | (1u << kind)) : (mask & ~(1u << kind));
        }
    }
    ImGui::EndCombo();
    if (mask != s_mask) {
        s_mask = mask;
        overlay_save();
    }
}
