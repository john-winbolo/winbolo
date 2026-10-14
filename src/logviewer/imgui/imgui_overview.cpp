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
 * the names of its players and then the number of pillboxes and of bases it
 * owns at the playback time, each number in front of the map's picture of a
 * pillbox or a base in the team's colour. Then the ones nobody owns, with
 * the game's red pillbox and its neutral base, while there is one of them
 * (lvOverviewHasNeutral), then the totals. Under the
 * table (beside it when the window is wide) two thin bars show the same
 * numbers as shares: one for the pillboxes and one for the bases, each cut
 * into the teams' colours.
 *
 * The colours are the ones the Team Colours dialog has: a team's own, and
 * the dialog's Neutral colour for the stripe and the bar part of the things
 * nobody owns.
 *
 * The numbers are read off the game's state every frame
 * (lv_screenOverviewSnapshot), so they follow a seek in either direction.
 */

#include "imgui_overview.h"
#include "imgui_main_menu.h"
#include "imgui_dialogs.h"
#include "imgui_dock.h"
#include "imgui_context.h"
#include "imgui_lv_icons.h"
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
    unsigned char lv_screenTeamColour(int team);
    SDL_Texture *lv_drawGetTilesTexture(void);
    void lv_drawTeamPanelColour(int palette, float *r, float *g, float *b);
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

/* How many team rows the dock gives room for when no log says: before the
 * first log, and at the start of a log, before its first player is in. */
#define OVERVIEW_DEFAULT_ROWS 2

/* How many rows the table has now (lvOverviewTableRows), read under the
 * decoder's lock. The dock asks for a height before the window is drawn in
 * it, so the count is read for the dock too, and not kept from the last
 * drawing: a count that was kept was the last log's for one frame after
 * another log was loaded. With no team yet it is OVERVIEW_DEFAULT_ROWS
 * teams and the Neutral row. */
static int overview_rows_now(void) {
    LvOverview ov;
    lv_clientMutexWaitFor();
    lv_screenOverviewSnapshot(&ov);
    lv_clientMutexRelease();
    if (ov.rowCount <= 0) return lvOverviewTableRows(OVERVIEW_DEFAULT_ROWS, true);
    return lvOverviewTableRows(ov.rowCount, lvOverviewHasNeutral(&ov));
}

/* The colour a team is drawn in: the one every panel draws the team in
 * (lv_drawTeamPanelColour). colour is the team's palette entry. */
static ImU32 overview_team_colour(int colour) {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    lv_drawTeamPanelColour(colour, &r, &g, &b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f));
}

/* The colour of things nobody owns, in the stripe and in the share bars:
 * the Neutral entry of the Team Colours dialog. The pictures of a neutral
 * pillbox and base are the game's own and do not take it. */
static ImU32 overview_neutral_colour(void) {
    return overview_team_colour(lv_screenTeamColour(-1));
}

/* The side of the picture of a pillbox or a base: the size the game's 12x12
 * status icons had here, at the largest whole number of times that fits in
 * a line of text, so the table and the bars keep their sizes. */
static float overview_icon_size(void) {
    int scale = (int)(ImGui::GetTextLineHeight() / (float)STATUS_ITEM_SIZE_Y);
    if (scale < 1) scale = 1;
    return (float)(scale * STATUS_ITEM_SIZE_Y);
}

/* The picture of a pillbox or a base, with its top left at p: the map's own
 * picture, made smaller (lv_item_picture; the events lane uses the same). It
 * is in the colour of team; with team -1 it is the one no team holds (the
 * game's red pillbox, the game's neutral base). plain is the colour of the
 * shape drawn when the game's sheets are not there. */
static void overview_icon(ImDrawList *dl, bool isBase, int team, ImU32 plain, ImVec2 p) {
    const float size = overview_icon_size();

    p.x = floorf(p.x);
    p.y = floorf(p.y);
    if (!lv_item_picture(dl, isBase, team, p, ImVec2(p.x + size, p.y + size))) {
        /* No sheet: a plain shape still tells the two columns apart. */
        if (isBase) {
            dl->AddCircleFilled(ImVec2(p.x + size * 0.5f, p.y + size * 0.5f), size * 0.5f, plain);
        } else {
            dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), plain);
        }
    }
}

/* The width a count column needs: three digits, a gap and the picture. */
static float overview_count_w(void) {
    return lvOverviewCountColW(ImGui::CalcTextSize("000").x, overview_icon_size(),
                               ImGui::GetStyle().ItemInnerSpacing.x);
}

/* The height of one row of the table. */
static float overview_row_h(void) {
    return ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
}

/* The height of the two share bars with the space above each. */
static float overview_bars_h(void) {
    return 2.0f * (overview_icon_size() + ImGui::GetStyle().ItemSpacing.y);
}

/* The height of the table for a number of rows (lvOverviewTableRows). */
static float overview_table_h(int rows) {
    return (float)rows * overview_row_h();
}

float lv_imgui_overview_dock_height(float dockWidth) {
    const ImGuiStyle &style = ImGui::GetStyle();
    float availW = dockWidth - style.WindowPadding.x * 2.0f;
    float h = overview_table_h(overview_rows_now());
    /* In a narrow dock the bars are under the table. */
    if (availW < OVERVIEW_TABLE_MAX_W + OVERVIEW_BARS_MIN_W) {
        h += overview_bars_h() + style.ItemSpacing.y;
    }
    return h + style.WindowPadding.y * 2.0f + ImGui::GetFrameHeight();
}

/* What a count cell has after its number. */
enum OverviewPicture {
    OVERVIEW_PIC_NONE, /* the totals: the number alone */
    OVERVIEW_PIC_PILL,
    OVERVIEW_PIC_BASE
};

/* A count cell: the number, then the picture of what is counted, in the
 * colour of team (-1: the one no team holds). The picture is at the right
 * of the cell and the number ends in front of it, so the numbers of all the
 * rows end at one place (lvOverviewCountPlace). A zero is dimmed, so the eye
 * goes to the teams that own something; the picture is not. */
static void overview_count_cell(int count, bool dim, OverviewPicture picture, bool isBase,
                                int team, ImU32 plain) {
    char text[16];
    snprintf(text, sizeof(text), "%d", count);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float lineH = ImGui::GetTextLineHeight();
    const float icon = overview_icon_size();
    const float room = ImGui::GetContentRegionAvail().x;
    float numX = 0.0f, iconX = 0.0f;
    lvOverviewCountPlace(room, ImGui::CalcTextSize(text).x, icon,
                         ImGui::GetStyle().ItemInnerSpacing.x, &numX, &iconX);
    dl->AddText(ImVec2(floorf(p.x + numX), p.y),
                ImGui::GetColorU32((dim || count == 0) ? ImGuiCol_TextDisabled : ImGuiCol_Text),
                text);
    if (picture != OVERVIEW_PIC_NONE) {
        overview_icon(dl, isBase, team, plain,
                      ImVec2(p.x + iconX, p.y + (lineH - icon) * 0.5f));
    }
    /* The cell is one item: its tooltip is the word for what it counts. */
    ImGui::Dummy(ImVec2(room, lineH));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("%s", langGetText(isBase ? STR_TABLET_BASES : STR_TABLET_PILLBOXES));
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

/* The room the chip of the row's player number index takes in front of the
 * name, with its gap: 0 for a person, and for a bot when the game's picture
 * is not there. */
static float overview_chip_room(const LvOverviewRow &row, int index) {
    if (index < 0 || index >= LV_OVERVIEW_MAX_TEAMS) return 0.0f;
    const float w = lv_bot_chip_width(row.chip[index]);
    return (w > 0.0f) ? w + lv_bot_chip_gap() : 0.0f;
}

/* The names of the row's first `count` players on one line, "A, B, C", with
 * the chip in front of each bot. With dl NULL nothing is drawn. Returns the
 * width. */
static float overview_names_run(ImDrawList *dl, ImVec2 at, ImU32 col,
                                const LvOverviewRow &row, int count) {
    /* The whole list of a team with no bot: one piece of text. */
    if (row.bots == 0 && count >= row.players) {
        if (dl != nullptr) dl->AddText(at, col, row.names);
        return ImGui::CalcTextSize(row.names).x;
    }
    float x = at.x;
    for (int i = 0; i < count && i < row.players && i < LV_OVERVIEW_MAX_TEAMS; i++) {
        size_t len = 0;
        const char *name = lvOverviewName(&row, i, &len);
        if (name == nullptr) break;
        if (i > 0) {
            if (dl != nullptr) dl->AddText(ImVec2(x, at.y), col, LV_OVERVIEW_NAME_SEP);
            x += ImGui::CalcTextSize(LV_OVERVIEW_NAME_SEP).x;
        }
        const float chip = overview_chip_room(row, i);
        if (chip > 0.0f) {
            if (dl != nullptr) lv_bot_chip_draw(dl, row.chip[i], ImVec2(x, at.y));
            x += chip;
        }
        if (dl != nullptr) dl->AddText(ImVec2(x, at.y), col, name, name + len);
        x += ImGui::CalcTextSize(name, name + len).x;
    }
    return x - at.x;
}

/* What is put where names are left out. */
#define OVERVIEW_ELLIPSIS "..."

/* The label of a team's row: the names of its players, "A, B, C", with the
 * chip of a bot in front of its name. When they do not fit in the room
 * there is, the names from the start that do and then "..." ("A, B, ...");
 * when not even the first fits, that name cut short with "...". Drawn at
 * `at`; returns the width, and in *cut whether anything was left out (the
 * tooltip of the row then has the whole list). */
static float overview_team_label(ImDrawList *dl, ImVec2 at, ImU32 col,
                                 const LvOverviewRow &row, float room, bool *cut) {
    float widths[LV_OVERVIEW_MAX_TEAMS];
    int n = 0;
    *cut = false;
    for (int i = 0; i < row.players && i < LV_OVERVIEW_MAX_TEAMS; i++) {
        size_t len = 0;
        const char *name = lvOverviewName(&row, i, &len);
        if (name == nullptr) break;
        widths[n++] = overview_chip_room(row, i) + ImGui::CalcTextSize(name, name + len).x;
    }
    if (n == 0) return 0.0f;
    const float sepW = ImGui::CalcTextSize(LV_OVERVIEW_NAME_SEP).x;
    const float ellW = ImGui::CalcTextSize(OVERVIEW_ELLIPSIS).x;
    const int shown = lvOverviewNamesFit(widths, n, sepW, ellW, room, cut);
    if (!*cut) {
        return overview_names_run(dl, at, col, row, row.players);
    }
    if (shown > 0) {
        /* "A, B, ...": whole names, then what says there are more. */
        float x = at.x + overview_names_run(dl, at, col, row, shown);
        dl->AddText(ImVec2(x, at.y), col, LV_OVERVIEW_NAME_SEP OVERVIEW_ELLIPSIS);
        return x + sepW + ellW - at.x;
    }
    /* Not even the first name: its chip, then as much of it as goes in,
     * with "..." for the rest. Whole UTF-8 characters are taken off. */
    const float chip = overview_chip_room(row, 0);
    float x = at.x;
    if (chip > 0.0f && chip <= room) {
        lv_bot_chip_draw(dl, row.chip[0], at);
        x += chip;
    }
    size_t len = 0;
    const char *name = lvOverviewName(&row, 0, &len);
    char part[LV_OVERVIEW_NAMES_LEN + 4];
    if (name == nullptr) return x - at.x;
    while (len > 0) {
        len--;
        while (len > 0 && ((unsigned char)name[len] & 0xC0) == 0x80) len--;
        snprintf(part, sizeof(part), "%.*s" OVERVIEW_ELLIPSIS, (int)len, name);
        const float w = ImGui::CalcTextSize(part).x;
        if (w <= room - (x - at.x)) {
            dl->AddText(ImVec2(x, at.y), col, part);
            return x + w - at.x;
        }
    }
    /* Not even "..." goes in: nothing more. */
    return x - at.x;
}

/* The first cell of a team's row: a stripe in the team's colour, then the
 * names of its players. The cell is one item, so the tooltip of the row is
 * asked of it. Returns whether any name was left out or cut. */
static bool overview_team_cell(ImU32 stripe, const LvOverviewRow &row, float room) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    const float lead = OVERVIEW_STRIPE_W + ImGui::GetStyle().ItemInnerSpacing.x;
    bool cut = false;
    dl->AddRectFilled(ImVec2(p.x, p.y + 1.0f),
                      ImVec2(p.x + OVERVIEW_STRIPE_W, p.y + h - 1.0f), stripe, 1.0f);
    const float w = overview_team_label(dl, ImVec2(p.x + lead, p.y),
                                        ImGui::GetColorU32(ImGuiCol_Text), row, room, &cut);
    ImGui::Dummy(ImVec2(lead + w, h));
    return cut;
}

/* The table, width wide. */
static void overview_table(const LvOverview &ov, float width) {
    const ImGuiStyle &style = ImGui::GetStyle();
    const float countW = overview_count_w();
    const ImU32 plain = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_NoSavedSettings |
                                  ImGuiTableFlags_NoHostExtendX;

    const float left = ImGui::GetCursorScreenPos().x;

    if (!ImGui::BeginTable("##overview", 3, flags, ImVec2(width, 0.0f))) {
        return;
    }
    /* The count columns keep their width; the names have what is left. */
    ImGui::TableSetupColumn("##team", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##pills", ImGuiTableColumnFlags_WidthFixed, countW);
    ImGui::TableSetupColumn("##bases", ImGuiTableColumnFlags_WidthFixed, countW);

    for (int i = 0; i < ov.rowCount; i++) {
        const LvOverviewRow &row = ov.rows[i];
        const ImU32 colour = overview_team_colour(row.colour);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        float room = ImGui::GetContentRegionAvail().x - OVERVIEW_STRIPE_W -
                     style.ItemInnerSpacing.x;
        const bool cut = overview_team_cell(colour, row, room);
        /* The whole team, for a row that had no room for it. */
        if ((cut || row.players > 1) && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            if (ImGui::BeginTooltip()) {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float w = overview_names_run(ImGui::GetWindowDrawList(), at,
                                                   ImGui::GetColorU32(ImGuiCol_Text), row,
                                                   row.players);
                ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
                ImGui::EndTooltip();
            }
        }
        /* The number, then the picture in the team's colour. */
        ImGui::TableSetColumnIndex(1);
        overview_count_cell(row.pills, false, OVERVIEW_PIC_PILL, false, row.team, colour);
        ImGui::TableSetColumnIndex(2);
        overview_count_cell(row.bases, false, OVERVIEW_PIC_BASE, true, row.team, colour);
    }

    /* The ones no team holds, with the game's red pillbox and its neutral
     * base: only while there is one. */
    if (lvOverviewHasNeutral(&ov)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        overview_label_cell(overview_neutral_colour(), langGetText(STR_MAPEDIT_OWNER_NEUTRAL),
                            true);
        ImGui::TableSetColumnIndex(1);
        overview_count_cell(ov.neutralPills, true, OVERVIEW_PIC_PILL, false, -1, plain);
        ImGui::TableSetColumnIndex(2);
        overview_count_cell(ov.neutralBases, true, OVERVIEW_PIC_BASE, true, -1, plain);
    }

    /* The totals, under a line across the table: the numbers alone, under
     * the numbers of the rows. */
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
    overview_count_cell(ov.totalPills, false, OVERVIEW_PIC_NONE, false, -1, plain);
    ImGui::TableSetColumnIndex(2);
    overview_count_cell(ov.totalBases, false, OVERVIEW_PIC_NONE, true, -1, plain);

    ImGui::EndTable();
}

/* One share bar, width wide: the icon of the kind, then a bar cut into the
 * part each team owns, in team order, with the part nobody owns last. The
 * mouse over a part draws a white line round it and says how many that is
 * and whose: "3 pillboxes (Rider, ErYan)". */
static void overview_share_bar(const LvOverview &ov, bool isBase, float width) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImGuiStyle &style = ImGui::GetStyle();
    const float icon = overview_icon_size();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const int total = isBase ? ov.totalBases : ov.totalPills;

    overview_icon(dl, isBase, -1, ImGui::GetColorU32(ImGuiCol_TextDisabled), p);

    float x0 = floorf(p.x + icon + style.ItemInnerSpacing.x);
    float x1 = floorf(p.x + width);
    float y0 = floorf(p.y + (icon - OVERVIEW_BAR_H) * 0.5f);
    float y1 = y0 + OVERVIEW_BAR_H;
    ImGui::Dummy(ImVec2(width, icon));
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
    const float mouseX = ImGui::GetIO().MousePos.x;
    int hoverPart = -1, hoverCount = 0;
    float hoverFrom = 0.0f, hoverTo = 0.0f;
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
                                  neutral ? overview_neutral_colour()
                                          : overview_team_colour(ov.rows[i].colour));
                if (hovered && mouseX >= from && mouseX < to) {
                    hoverPart = i;
                    hoverCount = count;
                    hoverFrom = from;
                    hoverTo = to;
                }
            }
            from = to;
        }
    }
    if (hoverPart >= 0) {
        /* Over the parts beside it, so the line shows on both sides. */
        dl->AddRect(ImVec2(hoverFrom - 1.0f, y0 - 1.0f), ImVec2(hoverTo + 1.0f, y1 + 1.0f),
                    IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
        MessageArgs args = {};
        args.number = hoverCount;
        const int id = isBase ? (hoverCount == 1 ? STR_LV_OVERVIEW_BAR_BASE : STR_LV_OVERVIEW_BAR_BASES)
                              : (hoverCount == 1 ? STR_LV_OVERVIEW_BAR_PILL : STR_LV_OVERVIEW_BAR_PILLS);
        char what[LANG_MSGARG_STRING_LEN];
        snprintf(what, sizeof(what), "%s", langGetTextFmt(id, &args));
        const char *whose = (hoverPart == ov.rowCount) ? langGetText(STR_MAPEDIT_OWNER_NEUTRAL)
                                                       : ov.rows[hoverPart].names;
        ImGui::SetTooltip("%s (%s)", what, whose);
    } else if (hovered) {
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
