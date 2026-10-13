/*
 * imgui_overview.cpp - ImGui Overview window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

/*
 * Who owns what, in one small table: a row for each team in the game, with
 * the number of pillboxes and of bases it owns at the playback time, then
 * the ones nobody owns, then the totals. Under the table (beside it when the
 * window is wide) two thin bars show the same numbers as shares: one for the
 * pillboxes and one for the bases, each cut into the teams' colours.
 *
 * The colours are the ones the map and the HUD panel use, so a red team is
 * orange here too. The two count columns are headed by the game's own
 * status icons for a pillbox and a base.
 *
 * The numbers are read off the game's state every frame
 * (lv_screenOverviewSnapshot), so they follow a seek in either direction.
 */

#include "imgui_overview.h"
#include "imgui_main_menu.h"
#include "imgui_dialogs.h"
#include "imgui_dock.h"
#include "imgui_context.h"
#include "imgui.h"
#include "../../gui/lang.h"
#include "../../gui/sdl3/dialogs/imgui_dialog_utils.h"
#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
    #include "../lv_hud.h"
    #include "../lv_overview.h"
    #include "../../gui/tiles.h"
    #include "../../gui/sdl3/map_colours.h"  /* mapColourTeam */

    /* External functions from backend - using C types directly. The viewer's
     * own headers cannot be included here beside lang.h: its lv_global.h and
     * the game's global.h, which lang.h brings in, share an include guard. */
    void lv_clientMutexWaitFor(void);
    void lv_clientMutexRelease(void);
    void lv_screenOverviewSnapshot(LvOverview *out);
    SDL_Texture *lv_drawGetTilesTexture(void);
}

/* The floating window's default width, and the widest the table is drawn:
 * in a window wider than that plus OVERVIEW_BARS_MIN_W the share bars go
 * beside the table and not under it. */
#define OVERVIEW_DEFAULT_W   250.0f
#define OVERVIEW_TABLE_MAX_W 300.0f
#define OVERVIEW_BARS_MIN_W  160.0f

/* The colour stripe at the start of a team's row, and a share bar. */
#define OVERVIEW_STRIPE_W 4.0f
#define OVERVIEW_BAR_H    6.0f

/* The colour of things nobody owns, in the stripe and in the share bars. */
#define OVERVIEW_NEUTRAL_COLOUR IM_COL32(120, 120, 120, 255)

/* How many teams the table had the last time it was drawn: the dock asks for
 * a height before the window is drawn in it. Two until a log says. */
static int s_last_rows = 2;

/* The colour a team is drawn in: the HUD panel's (imgui_hud.cpp). A red team
 * is orange, and a colour too dark to see on a dark panel is lifted. */
static ImU32 overview_team_colour(int colour) {
    SDL_FColor c = mapColourTeam(colour);
    if (!lvHudRedToOrange(&c.r, &c.g, &c.b)) {
        lvHudReadableColour(&c.r, &c.g, &c.b);
    }
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 1.0f));
}

/* The whole-number scale the 12x12 status icons are drawn at: as large as
 * fits in a line of text. */
static float overview_icon_size(void) {
    int scale = (int)(ImGui::GetTextLineHeight() / (float)STATUS_ITEM_SIZE_Y);
    if (scale < 1) scale = 1;
    return (float)(scale * STATUS_ITEM_SIZE_Y);
}

/* The game's neutral status icon for a pillbox or a base, with its top left
 * at p. */
static void overview_icon(ImDrawList *dl, bool isBase, ImVec2 p) {
    SDL_Texture *atlas = lv_drawGetTilesTexture();
    float tw = 0.0f, th = 0.0f;
    const float size = overview_icon_size();
    const float sx = isBase ? (float)STATUS_BASE_NEUTRAL_X : (float)STATUS_PILLBOX_NEUTRAL_X;
    const float sy = isBase ? (float)STATUS_BASE_NEUTRAL_Y : (float)STATUS_PILLBOX_NEUTRAL_Y;

    p.x = floorf(p.x);
    p.y = floorf(p.y);
    if (atlas == nullptr || !SDL_GetTextureSize(atlas, &tw, &th) || tw <= 0.0f || th <= 0.0f) {
        /* No atlas: a plain shape still tells the two columns apart. */
        ImU32 col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        if (isBase) {
            dl->AddCircleFilled(ImVec2(p.x + size * 0.5f, p.y + size * 0.5f), size * 0.5f, col);
        } else {
            dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), col);
        }
        return;
    }
    imguiPushNearestSamplingOn(dl);
    dl->AddImage((ImTextureID)(size_t)atlas, p, ImVec2(p.x + size, p.y + size),
                 ImVec2(sx / tw, sy / th),
                 ImVec2((sx + STATUS_ITEM_SIZE_X) / tw, (sy + STATUS_ITEM_SIZE_Y) / th));
    imguiPopNearestSamplingOn(dl);
}

/* The width a count column needs: three digits, or the icon above them. */
static float overview_count_w(void) {
    float w = ImGui::CalcTextSize("000").x;
    float icon = overview_icon_size();
    return (icon > w) ? icon : w;
}

/* The height of one row of the table. */
static float overview_row_h(void) {
    return ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
}

/* The height of the two share bars with the space above each. */
static float overview_bars_h(void) {
    return 2.0f * (overview_icon_size() + ImGui::GetStyle().ItemSpacing.y);
}

/* The height of the table for a number of teams: the icons, the teams, the
 * neutral row and the totals. */
static float overview_table_h(int teams) {
    return (float)(teams + 3) * overview_row_h();
}

float lv_imgui_overview_dock_height(float dockWidth) {
    const ImGuiStyle &style = ImGui::GetStyle();
    float availW = dockWidth - style.WindowPadding.x * 2.0f;
    float h = overview_table_h(s_last_rows);
    /* In a narrow dock the bars are under the table. */
    if (availW < OVERVIEW_TABLE_MAX_W + OVERVIEW_BARS_MIN_W) {
        h += overview_bars_h() + style.ItemSpacing.y;
    }
    return h + style.WindowPadding.y * 2.0f + ImGui::GetFrameHeight();
}

/* A number at the right of the current cell. A zero is dimmed, so the eye
 * goes to the teams that own something. */
static void overview_count_cell(int count, bool dim) {
    char text[16];
    snprintf(text, sizeof(text), "%d", count);
    float w = ImGui::CalcTextSize(text).x;
    float room = ImGui::GetContentRegionAvail().x;
    if (room > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (room - w));
    if (dim || count == 0) {
        ImGui::TextDisabled("%s", text);
    } else {
        ImGui::TextUnformatted(text);
    }
}

/* The first cell of a row: a stripe in the row's colour, then the label. */
static void overview_label_cell(ImU32 stripe, const char *label, bool dim) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    dl->AddRectFilled(ImVec2(p.x, p.y + 1.0f),
                      ImVec2(p.x + OVERVIEW_STRIPE_W, p.y + h - 1.0f), stripe, 1.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + OVERVIEW_STRIPE_W +
                         ImGui::GetStyle().ItemInnerSpacing.x);
    if (dim) {
        ImGui::TextDisabled("%s", label);
    } else {
        ImGui::TextUnformatted(label);
    }
}

/* The label of a team's row: the names of its players when they fit in the
 * room there is, else the first of them and how many more there are. */
static void overview_team_label(const LvOverviewRow &row, float room,
                                char *out, size_t outSize) {
    if (row.players <= 1 || ImGui::CalcTextSize(row.names).x <= room) {
        snprintf(out, outSize, "%s", row.names);
        return;
    }
    MessageArgs args = {};
    snprintf(args.string1, sizeof(args.string1), "%s", row.firstName);
    args.number = row.players - 1;
    snprintf(out, outSize, "%s", langGetTextFmt(STR_LV_OVERVIEW_MORE, &args));
}

/* The table, width wide. */
static void overview_table(const LvOverview &ov, float width) {
    const ImGuiStyle &style = ImGui::GetStyle();
    const float countW = overview_count_w();
    const ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_NoSavedSettings |
                                  ImGuiTableFlags_NoHostExtendX;

    const float left = ImGui::GetCursorScreenPos().x;

    if (!ImGui::BeginTable("##overview", 3, flags, ImVec2(width, 0.0f))) {
        return;
    }
    ImGui::TableSetupColumn("##team", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##pills", ImGuiTableColumnFlags_WidthFixed, countW);
    ImGui::TableSetupColumn("##bases", ImGuiTableColumnFlags_WidthFixed, countW);

    /* The icons that head the two count columns, over the right of each
     * column, where the numbers end. */
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float icon = overview_icon_size();
        ImGui::TableNextRow();
        for (int col = 1; col <= 2; col++) {
            ImGui::TableSetColumnIndex(col);
            ImVec2 p = ImGui::GetCursorScreenPos();
            float room = ImGui::GetContentRegionAvail().x;
            float off = (room > icon) ? (room - icon) : 0.0f;
            float down = (ImGui::GetTextLineHeight() - icon) * 0.5f;
            overview_icon(dl, col == 2, ImVec2(p.x + off, p.y + down));
            ImGui::Dummy(ImVec2(room, ImGui::GetTextLineHeight()));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                ImGui::SetTooltip("%s", langGetText(col == 2 ? STR_TABLET_BASES
                                                             : STR_TABLET_PILLBOXES));
            }
        }
    }

    for (int i = 0; i < ov.rowCount; i++) {
        const LvOverviewRow &row = ov.rows[i];
        char label[LV_OVERVIEW_NAMES_LEN];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        float room = ImGui::GetContentRegionAvail().x - OVERVIEW_STRIPE_W -
                     style.ItemInnerSpacing.x;
        overview_team_label(row, room, label, sizeof(label));
        overview_label_cell(overview_team_colour(row.colour), label, false);
        /* The whole team, for a row that had no room for it. */
        if (row.players > 1 && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::SetTooltip("%s", row.names);
        }
        ImGui::TableSetColumnIndex(1);
        overview_count_cell(row.pills, false);
        ImGui::TableSetColumnIndex(2);
        overview_count_cell(row.bases, false);
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    overview_label_cell(OVERVIEW_NEUTRAL_COLOUR, langGetText(STR_MAPEDIT_OWNER_NEUTRAL), true);
    ImGui::TableSetColumnIndex(1);
    overview_count_cell(ov.neutralPills, true);
    ImGui::TableSetColumnIndex(2);
    overview_count_cell(ov.neutralBases, true);

    /* The totals, under a line across the table. */
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float y = floorf(p.y - style.CellPadding.y) + 0.5f;
        dl->AddLine(ImVec2(left, y), ImVec2(left + width, y),
                    ImGui::GetColorU32(ImGuiCol_Separator));
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + OVERVIEW_STRIPE_W + style.ItemInnerSpacing.x);
    ImGui::TextUnformatted(langGetText(STR_LV_OVERVIEW_TOTAL));
    ImGui::TableSetColumnIndex(1);
    overview_count_cell(ov.totalPills, false);
    ImGui::TableSetColumnIndex(2);
    overview_count_cell(ov.totalBases, false);

    ImGui::EndTable();
}

/* One share bar, width wide: the icon of the kind, then a bar cut into the
 * part each team owns, in team order, with the part nobody owns last. */
static void overview_share_bar(const LvOverview &ov, bool isBase, float width) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImGuiStyle &style = ImGui::GetStyle();
    const float icon = overview_icon_size();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const int total = isBase ? ov.totalBases : ov.totalPills;

    overview_icon(dl, isBase, p);

    float x0 = floorf(p.x + icon + style.ItemInnerSpacing.x);
    float x1 = floorf(p.x + width);
    float y0 = floorf(p.y + (icon - OVERVIEW_BAR_H) * 0.5f);
    float y1 = y0 + OVERVIEW_BAR_H;
    if (x1 - x0 >= 8.0f) {
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), ImGui::GetColorU32(ImGuiCol_FrameBg));
        /* Each part ends where the running count says, so the parts meet
         * with no gap and the last one ends at the end of the bar. */
        int run = 0;
        float from = x0;
        for (int i = 0; i <= ov.rowCount; i++) {
            bool neutral = (i == ov.rowCount);
            int count = neutral ? (isBase ? ov.neutralBases : ov.neutralPills)
                                : (isBase ? ov.rows[i].bases : ov.rows[i].pills);
            if (count <= 0) continue;
            run += count;
            float to = floorf(x0 + (x1 - x0) * lvOverviewShare(run, total));
            if (to > from) {
                dl->AddRectFilled(ImVec2(from, y0), ImVec2(to, y1),
                                  neutral ? OVERVIEW_NEUTRAL_COLOUR
                                          : overview_team_colour(ov.rows[i].colour));
            }
            from = to;
        }
    }
    ImGui::Dummy(ImVec2(width, icon));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("%s", langGetText(isBase ? STR_TABLET_BASES : STR_TABLET_PILLBOXES));
    }
}

void lv_imgui_overview_window(void) {
    if (!lv_g_show_overview_window) {
        return;
    }

    /* Playback writes the pillboxes, bases and players on the decode timer,
     * so what is drawn is copied out under its lock, every frame: the copy
     * is then right while playing, scrubbing and stepping. */
    LvOverview ov;
    lv_clientMutexWaitFor();
    lv_screenOverviewSnapshot(&ov);
    lv_clientMutexRelease();
    if (ov.rowCount > 0) s_last_rows = ov.rowCount;

    {
        /* A panel that sat in the dock when the dock was turned off goes
         * back to its default place, the same as a reset. */
        bool to_default = lv_g_reset_window_positions ||
                          lv_imgui_dock_take_float_reset("###overview");
        ImGuiCond cond = to_default ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        const ImGuiStyle &style = ImGui::GetStyle();
        ImGui::SetNextWindowSize(ImVec2(OVERVIEW_DEFAULT_W,
                                        lv_imgui_overview_dock_height(OVERVIEW_DEFAULT_W) +
                                            style.ItemSpacing.y), cond);
        /* Beside the HUD panel, which opens down the left edge. */
        ImGui::SetNextWindowPos(ImVec2(220, 30), cond);
    }

    char title[128];
    snprintf(title, sizeof(title), "%s###overview", langGetText(STR_LV_WIN_OVERVIEW));
    if (ImGui::Begin(title, &lv_g_show_overview_window, ImGuiWindowFlags_NoCollapse)) {
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

        const ImGuiStyle &style = ImGui::GetStyle();
        float availW = ImGui::GetContentRegionAvail().x;
        /* A wide window (a sidebar along the top or the bottom) has the bars
         * beside the table; a narrow one has them under it. */
        bool beside = availW >= OVERVIEW_TABLE_MAX_W + OVERVIEW_BARS_MIN_W;
        float tableW = beside ? OVERVIEW_TABLE_MAX_W : availW;
        if (tableW < 1.0f) tableW = 1.0f;

        overview_table(ov, tableW);

        float barsW;
        if (beside) {
            ImGui::SameLine(0.0f, style.ItemSpacing.x * 2.0f);
            barsW = ImGui::GetContentRegionAvail().x;
            ImGui::BeginGroup();
            /* Level with the first team's row. */
            ImGui::Dummy(ImVec2(barsW, overview_row_h() - style.ItemSpacing.y));
        } else {
            barsW = availW;
        }
        overview_share_bar(ov, false, barsW);
        overview_share_bar(ov, true, barsW);
        if (beside) {
            ImGui::EndGroup();
        }
    }
    ImGui::End();
}
