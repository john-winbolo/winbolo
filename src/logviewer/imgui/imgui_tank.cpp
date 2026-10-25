/*
 * imgui_tank.cpp - ImGui Tank window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

/*
 * What the selected tank holds, drawn as the game draws it for the player:
 * the four gauges of the game's status column (shells, mines, armour and
 * trees). The well the gauges stand in, with the four small pictures under
 * them, is cut out of the game's own background picture, and the green bars
 * are worked out as the game works them out (sdl3RenderTankBarsTex): 80
 * pixels for a full tank, each gauge against its own cap from the rules of
 * the recorded game.
 *
 * Beside the gauges (under them in a narrow window) the same four values
 * are given as numbers, with the state of the tank's builder and how many
 * pillboxes the tank carries. The line for the pillboxes always starts with
 * the game's status picture of a pillbox in a tank (the small green ring of
 * the game's pillbox panel) in place of its words, when the tiles are
 * loaded. In a small panel each of the four lines starts with the game's
 * small picture for what it counts in place of the word, and the builder's
 * line with the game's picture of a builder. While
 * the builder is dead his line has the parachute the new one flies in on and
 * the time until he is in the tank, in words and in pictures alike; no line
 * says "dead". A panel with no room for all of the text shows
 * none of it, only the gauges (lvTankLayoutPick has the rule). Nothing is
 * ever cut off, so the panel does not scroll.
 *
 * The selected tank is the one a click on a tank on the map picks, or a
 * click on a tank in the HUD panel. Everything is read off the game's state
 * every frame, so it follows a seek in either direction.
 */

#include "imgui_tank.h"
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
    #include "../lv_tank_status.h"
    #include "../lv_builder_status.h"
    #include "../lv_pill_numbers.h"
    #include "../lv_lag.h"
    #include "../../gui/positions.h"
    #include "../../gui/tiles.h"
    #include "../../gui/sdl3/map_colours.h"  /* mapColourTeam */

    /* External functions from backend - using C types directly. The viewer's
     * own headers cannot be included here beside lang.h: its lv_global.h and
     * the game's global.h, which lang.h brings in, share an include guard. */
    void lv_clientMutexWaitFor(void);
    void lv_clientMutexRelease(void);
    void lv_screenTankStatusSnapshot(LvTankStatus *out);
    void lv_screenDeselectTank(void);
    bool lv_screenLagShown(void);
    void lv_screenPlayerLagSnapshot(int slot, LvPlayerLag *out);
    void lv_screenLagViewSet(int slot);
    int  lv_screenLagViewSlot(void);
    bool lv_screenLagViewStatus(char *name, size_t nameSize, int *behindMs, bool *drawn);
    void lv_drawDirtyScreen(void);
    SDL_Texture *lv_drawGetStatusBackgroundTexture(void);
    void lv_drawTeamPanelColour(int palette, float *r, float *g, float *b);
    SDL_Texture *lv_drawGetTilesTexture(void);
}

/* The part of the game's background picture the gauges stand in: the black
 * well of the status column from its left bevel to its right one, from a
 * little above the gauges to under the four pictures at their feet. The
 * numbers are the game's (gui/sdl3/overview_hud_layout.cpp cuts the same
 * slice for the full screen view's HUD). */
#define TANK_WELL_LEFT   438
#define TANK_WELL_RIGHT  493
#define TANK_WELL_PAD    4
#define TANK_WELL_FOOT_H 18
#define TANK_SLICE_X TANK_WELL_LEFT
#define TANK_SLICE_Y (STATUS_TANK_BARS_TOP - TANK_WELL_PAD)
#define TANK_SLICE_W (TANK_WELL_RIGHT - TANK_WELL_LEFT)
#define TANK_SLICE_H (TANK_WELL_PAD + STATUS_TANK_BARS_HEIGHT + TANK_WELL_FOOT_H)

/* The whole-number scales the gauges are drawn at. The floating window
 * opens at 2, the scale the HUD panel opens at. */
#define TANK_DEFAULT_SCALE 2
#define TANK_MAX_SCALE     4

/* The floating window's default width: room for the numbers beside the
 * gauges. */
#define TANK_DEFAULT_W 300.0f

/* The game's green. */
#define TANK_BAR_COLOUR IM_COL32(0, 255, 0, 255)

/* The thin edge round the well, as the HUD panel has round its grids. */
#define TANK_EDGE_COLOUR IM_COL32(110, 110, 110, 255)

/* Where each gauge starts across the well, in the game's pixels from the
 * first one, and the name of what it measures. */
static const int k_gauge_x[LV_TANK_GAUGE_COUNT] = {
    0, STATUS_TANK_MINES, STATUS_TANK_ARMOUR, STATUS_TANK_TREES
};
static const langid k_gauge_name[LV_TANK_GAUGE_COUNT] = {
    STR_TABLET_SHELLS, STR_TABLET_MINES, STR_TABLET_ARMOUR, STR_TABLET_TREES
};

/* The lines of text beside the gauges: the four values, the builder and the
 * pillboxes carried. */
#define TANK_TEXT_LINES 6

static float tank_header_h(void) {
    return ceilf(ImGui::GetTextLineHeightWithSpacing());
}

/* The most pillboxes a tank can carry: all there are on a map. The line for
 * them is measured with it, so its width does not move. */
#define TANK_PILLS_MOST 16

/* The builder's line while he is alive: in the tank or out of it. */
static const char *tank_builder_text(const LvTankStatus &st) {
    return langGetText(st.builderOut ? STR_LV_TANK_BUILDER_OUT : STR_LV_TANK_BUILDER_IN);
}

/* What a dead builder's line says after the parachute: the time until the
 * new builder is in the tank, as the marker over the tank on the map has it
 * (lvBuilderDeadText; both take the time from lv_screenBuilderReturn). With
 * the time not known it is nothing, and the parachute stands alone. */
static void tank_builder_dead_time(const LvTankStatus &st, char *out, size_t outSize) {
    lvBuilderDeadText(st.builderBack != 0, st.builderBackMs, st.builderBackApprox != 0,
                      out, outSize);
}

/* The start of a dead builder's line in words: "Builder:", the word of the
 * line for a builder in his tank with its colon. A language whose line has
 * no colon gets nothing, and the line is the parachute and the time. */
static void tank_builder_word(char *out, size_t outSize) {
    bool colon = false;
    out[0] = '\0';
    lvTankAfterColon(langGetText(STR_LV_TANK_BUILDER_IN), &colon);
    if (colon && outSize > 2) {
        lvTankBeforeColon(langGetText(STR_LV_TANK_BUILDER_IN), out, outSize - 1);
        strcat(out, ":");
    }
}

/* The side of the parachute's square in a line of words: a line of text
 * high, to a whole pixel. */
static float tank_chute_side(void) {
    return floorf(ImGui::GetTextLineHeight());
}

/* A dead builder's line in words is "Builder:", the parachute and the time.
 * Its width at its widest: the time is measured as the longest it is. */
static float tank_builder_dead_w(void) {
    const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
    char word[LANG_MSGARG_STRING_LEN];
    tank_builder_word(word, sizeof(word));
    float w = tank_chute_side() + gap + ImGui::CalcTextSize(LV_BUILDER_COUNTDOWN_WIDEST).x;
    if (word[0] != '\0') w += ImGui::CalcTextSize(word).x + gap;
    return w;
}

/* The width the builder's line needs, whatever it says at the moment: the
 * widest of the builder's three states. */
static float tank_builder_w(void) {
    static const langid k_builder[] = { STR_LV_TANK_BUILDER_IN, STR_LV_TANK_BUILDER_OUT };
    float w = tank_builder_dead_w();
    for (langid id : k_builder) {
        float builder = ImGui::CalcTextSize(langGetText(id)).x;
        if (builder > w) w = builder;
    }
    return w;
}

/* The width the two lines under the four values need written in words: the
 * builder's, and the line for the pillboxes carried with the most a tank
 * can carry. */
static float tank_text_w(void) {
    MessageArgs args = {};
    args.number = TANK_PILLS_MOST;
    float w = ImGui::CalcTextSize(langGetTextFmt(STR_LV_TANK_PILLS, &args)).x;
    float builder = tank_builder_w();
    return (builder > w) ? builder : w;
}

/* The height of a block of so many lines of text, to the foot of the last. */
static float tank_lines_h(int lines) {
    return (float)lines * ImGui::GetTextLineHeight() +
           (float)(lines - 1) * ImGui::GetStyle().ItemSpacing.y;
}

static float tank_text_h(void) {
    return tank_lines_h(TANK_TEXT_LINES);
}

/* True when the text goes beside the gauges at the default scale in a
 * window with availW of room across. */
static bool tank_text_beside(float availW) {
    return availW >= TANK_DEFAULT_SCALE * TANK_SLICE_W + 2.0f +
                         ImGui::GetStyle().ItemSpacing.x * 2.0f + tank_text_w();
}

/* The lag section under the gauges: its line, its graph and its check box.
 * Only a recording with lag records has it. */
static float tank_lag_graph_h(void) {
    return ceilf(ImGui::GetTextLineHeight() * 2.0f);
}

/* withGraph is FALSE for a bot, which has no round trip to plot. */
static float tank_lag_h(bool withGraph) {
    const ImGuiStyle &style = ImGui::GetStyle();
    float h = style.ItemSpacing.y + ImGui::GetTextLineHeight() + style.ItemSpacing.y +
              ImGui::GetFrameHeight();
    if (withGraph) {
        h += tank_lag_graph_h() + style.ItemSpacing.y;
    }
    return h;
}

float lv_imgui_tank_dock_height(float dockWidth) {
    const ImGuiStyle &style = ImGui::GetStyle();
    float availW = dockWidth - style.WindowPadding.x * 2.0f;
    float h = tank_header_h() + (float)(TANK_DEFAULT_SCALE * TANK_SLICE_H) + 2.0f;
    if (!tank_text_beside(availW)) {
        h += style.ItemSpacing.y + tank_text_h();
    }
    if (lv_screenLagShown()) {
        /* The dock is sized for the largest section, the one with a graph. */
        h += tank_lag_h(true);
    }
    /* One more: the heights are whole pixels and the text's may not be. */
    return ceilf(h) + 1.0f + style.WindowPadding.y * 2.0f + ImGui::GetFrameHeight();
}

/* The colour of the tank's team: the one every panel draws the team in
 * (lv_drawTeamPanelColour). */
static ImU32 tank_team_colour(int colour) {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    lv_drawTeamPanelColour(colour, &r, &g, &b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f));
}

/* "Shells: 12 / 40" for one gauge. */
static void tank_gauge_text(const LvTankStatus &st, int gauge, char *out, size_t outSize) {
    MessageArgs args = {};
    snprintf(args.string1, sizeof(args.string1), "%s", langGetText(k_gauge_name[gauge]));
    args.number = st.alive ? st.value[gauge] : 0;
    args.number2 = st.full[gauge];
    snprintf(out, outSize, "%s", langGetTextFmt(STR_LV_TANK_STOCK_FMT, &args));
}

/* The same line with no word: ": 12 / 40". The picture stands for the word. */
static void tank_gauge_numbers(const LvTankStatus &st, int gauge, char *out, size_t outSize) {
    MessageArgs args = {};
    args.number = st.alive ? st.value[gauge] : 0;
    args.number2 = st.full[gauge];
    snprintf(out, outSize, "%s", langGetTextFmt(STR_LV_TANK_STOCK_FMT, &args));
}

/* The width of the widest of the four lines written with their words. Each
 * is measured with its cap for the value too, the most digits it has in
 * play, so the answer does not move as the tank fires and refuels. */
static float tank_words_w(const LvTankStatus &st) {
    float widest = 0.0f;
    for (int g = 0; g < LV_TANK_GAUGE_COUNT; g++) {
        MessageArgs args = {};
        snprintf(args.string1, sizeof(args.string1), "%s", langGetText(k_gauge_name[g]));
        args.number = (st.value[g] > st.full[g]) ? st.value[g] : st.full[g];
        args.number2 = st.full[g];
        float w = ImGui::CalcTextSize(langGetTextFmt(STR_LV_TANK_STOCK_FMT, &args)).x;
        if (w > widest) widest = w;
    }
    return widest;
}

/* What a line with a picture is made of across: the picture, the colon, a
 * gap and the column the numbers end in. The column is as wide as the widest
 * numbers the four lines can show (each with its cap for the value too), so
 * it does not move as the tank fires and refuels. */
struct TankIconMetrics {
    float picW;     /* a gauge's picture */
    float pillSize; /* the pillbox's picture; 0: that line keeps its words */
    float iconW;    /* the column the pictures stand in */
    float colonW;   /* 0 when the lines have no colon */
    float gap;
    float columnW;
    float lineW;    /* all of it */
    float builderH; /* the builder's picture; 0: his line keeps its words */
    float builderW;
    float chuteSide;    /* the parachute's square */
    float builderLineW; /* his line after the picture, at its widest state */
};

/* The builder's line goes after his picture when each of its states in
 * words has its state after a colon ("Builder: in tank"), as in English.
 * Returns the width of the widest state, the time after a dead builder's
 * parachute among them, or under 0 when one has no colon. */
static float tank_builder_state_w(void) {
    static const langid k_builder[] = { STR_LV_TANK_BUILDER_IN, STR_LV_TANK_BUILDER_OUT };
    float w = ImGui::CalcTextSize(LV_BUILDER_COUNTDOWN_WIDEST).x;
    for (langid id : k_builder) {
        bool colon = false;
        float state = ImGui::CalcTextSize(lvTankAfterColon(langGetText(id), &colon)).x;
        if (!colon) return -1.0f;
        if (state > w) w = state;
    }
    return w;
}

/* The tiles when they are loaded and hold the status picture of a pillbox
 * in a tank. */
static SDL_Texture *tank_tiles(float *tw, float *th) {
    SDL_Texture *atlas = lv_drawGetTilesTexture();
    *tw = 0.0f;
    *th = 0.0f;
    if (atlas != nullptr && SDL_GetTextureSize(atlas, tw, th) &&
        *tw >= (float)(STATUS_PILLBOX_TANKGOOD_X + STATUS_ITEM_SIZE_X) &&
        *th >= (float)(STATUS_PILLBOX_TANKGOOD_Y + STATUS_ITEM_SIZE_Y)) {
        return atlas;
    }
    return nullptr;
}

static TankIconMetrics tank_icon_metrics(const LvTankStatus &st) {
    TankIconMetrics m;
    bool anyColon = false;
    m.picW = lvTankIconWidth(ImGui::GetTextLineHeight());
    m.iconW = m.picW;
    m.pillSize = 0.0f;
    m.gap = ImGui::GetStyle().ItemInnerSpacing.x;
    m.columnW = 0.0f;
    {
        /* The pillboxes carried go after a picture too, when the tiles are
         * loaded and the line has its number after a colon. */
        float tw = 0.0f, th = 0.0f;
        MessageArgs args = {};
        bool colon = false;
        args.number = TANK_PILLS_MOST;
        const char *most = lvTankAfterColon(langGetTextFmt(STR_LV_TANK_PILLS, &args), &colon);
        if (colon && tank_tiles(&tw, &th) != nullptr) {
            m.pillSize = lvTankPillIconSize(ImGui::GetTextLineHeight(), STATUS_ITEM_SIZE_Y);
            if (m.pillSize > m.iconW) m.iconW = m.pillSize;
            m.columnW = ImGui::CalcTextSize(most).x;
            anyColon = true;
        }
    }
    for (int g = 0; g < LV_TANK_GAUGE_COUNT; g++) {
        MessageArgs args = {};
        bool colon = false;
        args.number = (st.value[g] > st.full[g]) ? st.value[g] : st.full[g];
        args.number2 = st.full[g];
        float w = ImGui::CalcTextSize(
            lvTankStockNumbers(langGetTextFmt(STR_LV_TANK_STOCK_FMT, &args), &colon)).x;
        if (w > m.columnW) m.columnW = w;
        if (colon) anyColon = true;
    }
    m.colonW = anyColon ? ImGui::CalcTextSize(":").x : 0.0f;
    m.lineW = lvTankIconNumberX(m.iconW, m.colonW, m.gap, m.columnW, m.columnW) + m.columnW;
    {
        /* The builder: his picture stands in the middle of the column the
         * others have, which is the wider one at every size of the font. */
        const float lineH = ImGui::GetTextLineHeight();
        const float stateW = tank_builder_state_w();
        m.builderH = 0.0f;
        m.builderW = 0.0f;
        m.chuteSide = 0.0f;
        m.builderLineW = 0.0f;
        if (stateW >= 0.0f) {
            m.builderH = lvTankBuilderIconH(lineH);
            m.builderW = lvTankBuilderIconW(lineH);
            if (m.builderW > m.iconW) m.iconW = m.builderW;
            m.chuteSide = lvTankChuteIconSide(lineH, m.iconW);
            m.lineW = lvTankIconNumberX(m.iconW, m.colonW, m.gap, m.columnW, m.columnW) +
                      m.columnW;
            m.builderLineW = lvTankBuilderLineW(m.iconW, ImGui::CalcTextSize(":").x, m.gap,
                                                stateW);
        }
    }
    return m;
}

/* The background picture when it is loaded and holds the well; its size. */
static SDL_Texture *tank_background(float *tw, float *th) {
    SDL_Texture *bg = lv_drawGetStatusBackgroundTexture();
    *tw = 0.0f;
    *th = 0.0f;
    if (bg != nullptr && SDL_GetTextureSize(bg, tw, th) &&
        *tw >= (float)(TANK_SLICE_X + TANK_SLICE_W) && *th >= (float)(TANK_SLICE_Y + TANK_SLICE_H)) {
        return bg;
    }
    return nullptr;
}

/* The well and its four gauges with the top left at origin. The scale is a
 * whole number but in a room smaller than the well, where it is under 1 and
 * the picture is drawn smoothed. It is an item of the window; each gauge
 * has a tooltip. */
static void tank_gauges(const LvTankStatus &st, ImVec2 origin, float scale) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float s = scale;
    float tw = 0.0f, th = 0.0f;
    SDL_Texture *bg = tank_background(&tw, &th);
    ImVec2 p0(floorf(origin.x) + 1.0f, floorf(origin.y) + 1.0f);
    ImVec2 p1(p0.x + s * TANK_SLICE_W, p0.y + s * TANK_SLICE_H);

    dl->AddRect(ImVec2(p0.x - 1.0f, p0.y - 1.0f), ImVec2(p1.x + 1.0f, p1.y + 1.0f),
                TANK_EDGE_COLOUR);
    if (bg != nullptr) {
        /* Pixel art drawn several times its size: point sampled, or it
         * blurs. Drawn under its size it is left smoothed, or whole columns
         * of it drop out. */
        const bool nearest = (s >= 1.0f);
        if (nearest) imguiPushNearestSamplingOn(dl);
        dl->AddImage((ImTextureID)(size_t)bg, p0, p1,
                     ImVec2((float)TANK_SLICE_X / tw, (float)TANK_SLICE_Y / th),
                     ImVec2((float)(TANK_SLICE_X + TANK_SLICE_W) / tw,
                            (float)(TANK_SLICE_Y + TANK_SLICE_H) / th));
        if (nearest) imguiPopNearestSamplingOn(dl);
    } else {
        /* No background picture: the well is black in it, and the gauges
         * still read from the left as the tooltips name them. */
        dl->AddRectFilled(p0, p1, IM_COL32_BLACK);
    }

    /* The gauges stand on the line the game stands them on and grow up. */
    const float left = p0.x + s * (float)(STATUS_TANK_SHELLS - TANK_SLICE_X);
    const float bottom = p0.y + s * (float)(TANK_WELL_PAD + STATUS_TANK_BARS_HEIGHT);
    for (int g = 0; g < LV_TANK_GAUGE_COUNT; g++) {
        float x0 = left + s * (float)k_gauge_x[g];
        float x1 = x0 + s * (float)STATUS_TANK_BARS_WIDTH;
        int value = st.alive ? st.value[g] : 0;
        float h = lvTankGaugeHeight(value, st.full[g], BAR_TANK_FULL_PIXELS,
                                    STATUS_TANK_BARS_HEIGHT);
        if (h > 0.0f) {
            dl->AddRectFilled(ImVec2(x0, floorf(bottom - s * h)), ImVec2(x1, bottom),
                              TANK_BAR_COLOUR);
        }

        /* The room a gauge answers the mouse in: its own column of the
         * well, down to the picture under it. */
        const float pitch = s * (float)(STATUS_TANK_BARS_WIDTH + STATUS_TANK_BARS_GAP);
        float hx0 = x0 - (pitch - s * (float)STATUS_TANK_BARS_WIDTH) * 0.5f;
        ImGui::PushID(g);
        ImGui::SetCursorScreenPos(ImVec2(hx0, p0.y));
        ImGui::InvisibleButton("##gauge", ImVec2(pitch, p1.y - p0.y));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            char tip[LANG_MSGARG_STRING_LEN];
            tank_gauge_text(st, g, tip, sizeof(tip));
            ImGui::SetTooltip("%s", tip);
        }
        ImGui::PopID();
    }

    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(s * TANK_SLICE_W + 2.0f, s * TANK_SLICE_H + 2.0f));
}

/* The four values, each after the small picture the game has under that
 * gauge in place of its word: "<picture>: 12 / 40". The picture is as tall
 * as a line of text and sits on the line as a letter would; the colons are
 * under each other and the numbers end under each other. The word is the
 * picture's tooltip. */
static void tank_icon_lines(const LvTankStatus &st, SDL_Texture *bg, float tw, float th) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const TankIconMetrics m = tank_icon_metrics(st);
    const float lineH = ImGui::GetTextLineHeight();
    const float iconW = m.iconW;
    const float picW = m.picW;
    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    char line[LV_TANK_GAUGE_COUNT][LANG_MSGARG_STRING_LEN];
    const char *numbers[LV_TANK_GAUGE_COUNT];
    bool colon[LV_TANK_GAUGE_COUNT];
    float numW[LV_TANK_GAUGE_COUNT];

    for (int g = 0; g < LV_TANK_GAUGE_COUNT; g++) {
        tank_gauge_numbers(st, g, line[g], sizeof(line[g]));
        numbers[g] = lvTankStockNumbers(line[g], &colon[g]);
        numW[g] = ImGui::CalcTextSize(numbers[g]).x;
    }

    const float cellY = (float)(STATUS_TANK_BARS_TOP + STATUS_TANK_BARS_HEIGHT + LV_TANK_ICON_CELL_DY);
    for (int g = 0; g < LV_TANK_GAUGE_COUNT; g++) {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImVec2 p0(floorf(pos.x), floorf(pos.y));
        ImVec2 p1(p0.x + picW, p0.y + lineH);
        const float cellX = (float)(STATUS_TANK_SHELLS + k_gauge_x[g] + LV_TANK_ICON_CELL_DX);
        const float numX = lvTankIconNumberX(iconW, m.colonW, m.gap, m.columnW, numW[g]);

        /* The picture is an item, so it has the tooltip; the rest of the
         * line is one more, so the window knows how wide the lines are. */
        ImGui::PushID(g);
        ImGui::Dummy(ImVec2(iconW, lineH));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::SetTooltip("%s", langGetText(k_gauge_name[g]));
        }
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::Dummy(ImVec2(m.lineW - iconW, lineH));
        ImGui::PopID();

        imguiPushNearestSamplingOn(dl);
        dl->AddImage((ImTextureID)(size_t)bg, p0, p1,
                     ImVec2(cellX / tw, cellY / th),
                     ImVec2((cellX + (float)LV_TANK_ICON_CELL_W) / tw,
                            (cellY + (float)LV_TANK_ICON_CELL_H) / th));
        imguiPopNearestSamplingOn(dl);
        if (colon[g]) {
            dl->AddText(ImVec2(p0.x + iconW, p0.y), textCol, ":");
        }
        dl->AddText(ImVec2(p0.x + numX, p0.y), textCol, numbers[g]);
    }
}

/* The numbers of the pillboxes the tank carries, for their tooltip:
 * "Numbers: 3, 5". Empty when it carries none. */
static void tank_pill_numbers(const LvTankStatus &st, char *dest, size_t size) {
    char list[LV_PILL_NUMBER_LIST_MAX];
    dest[0] = '\0';
    if (lvPillNumberListText(st.carriedPillNumbers, list, sizeof(list)) <= 0) return;
    MessageArgs args = {};
    snprintf(args.string1, sizeof(args.string1), "%s", list);
    snprintf(dest, size, "%s", langGetTextFmt(STR_LV_PILL_NUMBERS_LIST, &args));
}

/* The pillboxes carried, after the game's status picture of a pillbox in a
 * tank: the small green ring the game's pillbox panel shows for one of the
 * player's own pillboxes that a tank carries (STATUS_PILLBOX_TANKGOOD, the
 * cell sdl3DrawStatusPillbox draws for pillTankGood), as it is in the tile
 * sheet: "<picture>: 2", in the same columns as the four lines over it. The
 * whole line in words is the picture's tooltip. With none carried it is
 * dimmed, as in words. */
static void tank_pill_icon_line(const LvTankStatus &st, const TankIconMetrics &m) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float lineH = ImGui::GetTextLineHeight();
    float tw = 0.0f, th = 0.0f;
    SDL_Texture *atlas = tank_tiles(&tw, &th);
    MessageArgs args = {};
    char line[LANG_MSGARG_STRING_LEN];
    bool colon = false;
    args.number = st.carriedPills;
    snprintf(line, sizeof(line), "%s", langGetTextFmt(STR_LV_TANK_PILLS, &args));
    const char *number = lvTankAfterColon(line, &colon);
    const ImU32 col = ImGui::GetColorU32(st.carriedPills > 0 ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 p0(floorf(pos.x), floorf(pos.y));
    const float top = p0.y + floorf((lineH - m.pillSize) * 0.5f);
    const float numX = lvTankIconNumberX(m.iconW, m.colonW, m.gap, m.columnW,
                                         ImGui::CalcTextSize(number).x);

    ImGui::PushID("pills");
    ImGui::Dummy(ImVec2(m.iconW, lineH));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        char numbers[LANG_MSGARG_STRING_LEN * 2];
        tank_pill_numbers(st, numbers, sizeof(numbers));
        if (numbers[0] != '\0') {
            ImGui::SetTooltip("%s\n%s", line, numbers);
        } else {
            ImGui::SetTooltip("%s", line);
        }
    }
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::Dummy(ImVec2(m.lineW - m.iconW, lineH));
    /* The number after the picture names the pillboxes too. */
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        char numbers[LANG_MSGARG_STRING_LEN * 2];
        tank_pill_numbers(st, numbers, sizeof(numbers));
        if (numbers[0] != '\0') ImGui::SetTooltip("%s", numbers);
    }
    ImGui::PopID();

    if (atlas != nullptr) {
        imguiPushNearestSamplingOn(dl);
        dl->AddImage((ImTextureID)(size_t)atlas, ImVec2(p0.x, top),
                     ImVec2(p0.x + m.pillSize, top + m.pillSize),
                     ImVec2((float)STATUS_PILLBOX_TANKGOOD_X / tw, (float)STATUS_PILLBOX_TANKGOOD_Y / th),
                     ImVec2((float)(STATUS_PILLBOX_TANKGOOD_X + STATUS_ITEM_SIZE_X) / tw,
                            (float)(STATUS_PILLBOX_TANKGOOD_Y + STATUS_ITEM_SIZE_Y) / th));
        imguiPopNearestSamplingOn(dl);
    }
    if (colon) {
        dl->AddText(ImVec2(p0.x + m.iconW, p0.y), col, ":");
    }
    dl->AddText(ImVec2(p0.x + numX, p0.y), col, number);
}

/* The line for the pillboxes carried among lines in words: the same picture
 * in place of the words before the colon ("Pillboxes carried"), then the
 * colon and the number. False when the tiles are not loaded or the line has
 * no colon: it then keeps its words. */
static bool tank_pill_words_metrics(TankIconMetrics *out) {
    float tw = 0.0f, th = 0.0f;
    MessageArgs args = {};
    bool colon = false;
    args.number = TANK_PILLS_MOST;
    const char *most = lvTankAfterColon(langGetTextFmt(STR_LV_TANK_PILLS, &args), &colon);
    if (!colon || tank_tiles(&tw, &th) == nullptr) return false;
    TankIconMetrics m = {};
    m.pillSize = lvTankPillIconSize(ImGui::GetTextLineHeight(), STATUS_ITEM_SIZE_Y);
    m.iconW = m.pillSize;
    m.colonW = ImGui::CalcTextSize(":").x;
    m.gap = ImGui::GetStyle().ItemInnerSpacing.x;
    m.columnW = ImGui::CalcTextSize(most).x;
    m.lineW = lvTankIconNumberX(m.iconW, m.colonW, m.gap, m.columnW, m.columnW) + m.columnW;
    *out = m;
    return true;
}

/* The builder's line after the game's picture of a builder in place of the
 * word: "<builder>: in tank" or ": out" as the line in words has it. He is
 * the standing builder of the map, in the game's own colours, at a whole
 * number of times his 3 by 4 pixels. While he is dead the picture is the
 * parachute the new one flies in on, and after the colon is the time until
 * he is in the tank: "<parachute>: 0:07". With that time not known the
 * parachute is all there is, with no colon. The colon is under the colons
 * of the lines over it. The word ("Builder") is the picture's tooltip. With
 * no tiles loaded a drawn figure stands in. */
static void tank_builder_icon_line(const LvTankStatus &st, const TankIconMetrics &m) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float lineH = ImGui::GetTextLineHeight();
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    const float colonW = ImGui::CalcTextSize(":").x;
    const float textX = m.iconW + colonW + m.gap;
    char line[LANG_MSGARG_STRING_LEN];
    char word[LANG_MSGARG_STRING_LEN];
    char back[32];
    snprintf(line, sizeof(line), "%s", tank_builder_text(st));
    const char *state = lvTankAfterColon(line, nullptr);
    lvTankBeforeColon(line, word, sizeof(word));
    if (st.builderDead) {
        tank_builder_dead_time(st, back, sizeof(back));
        state = back;
    }
    float lineW = textX + ImGui::CalcTextSize(state).x;
    if (lineW < m.builderLineW) lineW = m.builderLineW;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 p0(floorf(pos.x), floorf(pos.y));

    ImGui::PushID("builder");
    ImGui::Dummy(ImVec2(m.iconW, lineH));
    if (word[0] != '\0' && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("%s", word);
    }
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::Dummy(ImVec2(lineW - m.iconW, lineH));
    ImGui::PopID();

    const bool chute = (st.builderDead != 0);
    const float w = chute ? m.chuteSide : m.builderW;
    const float h = chute ? m.chuteSide : m.builderH;
    const ImVec2 i0(p0.x + lvTankIconCentre(m.iconW, w), p0.y + lvTankIconCentre(lineH, h));
    if (!lv_builder_picture(dl, chute, i0, ImVec2(i0.x + w, i0.y + h))) {
        const float side = lvTankChuteIconSide(lineH, m.iconW);
        const ImVec2 f0(p0.x + lvTankIconCentre(m.iconW, side),
                        p0.y + lvTankIconCentre(lineH, side));
        lv_icon_draw(dl, LV_ICON_PERSON, f0, ImVec2(f0.x + side, f0.y + side), col);
    }
    if (state[0] != '\0') {
        dl->AddText(ImVec2(p0.x + m.iconW, p0.y), col, ":");
        dl->AddText(ImVec2(p0.x + textX, p0.y), col, state);
    }
}

/* The builder's line in words. Alive it is the words alone. Dead it is
 * "Builder:", the parachute at the height of the line, and the time until
 * the new builder is in the tank; with that time not known, "Builder:" and
 * the parachute. */
static void tank_builder_words_line(const LvTankStatus &st) {
    if (!st.builderDead) {
        ImGui::TextUnformatted(tank_builder_text(st));
        return;
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float lineH = ImGui::GetTextLineHeight();
    const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
    const float side = tank_chute_side();
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    char word[LANG_MSGARG_STRING_LEN];
    char back[32];
    tank_builder_word(word, sizeof(word));
    tank_builder_dead_time(st, back, sizeof(back));
    const float wordW = (word[0] != '\0') ? ImGui::CalcTextSize(word).x + gap : 0.0f;
    float lineW = wordW + side;
    if (back[0] != '\0') lineW += gap + ImGui::CalcTextSize(back).x;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 p0(floorf(pos.x), floorf(pos.y));

    ImGui::Dummy(ImVec2(lineW, lineH));
    if (word[0] != '\0') {
        dl->AddText(p0, col, word);
    }
    const ImVec2 i0(p0.x + wordW, p0.y + lvTankIconCentre(lineH, side));
    if (!lv_builder_picture(dl, true, i0, ImVec2(i0.x + side, i0.y + side))) {
        lv_icon_draw(dl, LV_ICON_PERSON, i0, ImVec2(i0.x + side, i0.y + side), col);
    }
    if (back[0] != '\0') {
        dl->AddText(ImVec2(p0.x + wordW + side + gap, p0.y), col, back);
    }
}

/* The lines of text: the four values, the builder, the pillboxes. With
 * icons the four values are written after the game's small pictures (with
 * no background picture loaded they keep their words), and so are the
 * builder and the pillboxes. The pillboxes have their picture in words too,
 * whenever the tiles are loaded. */
static void tank_text(const LvTankStatus &st, bool icons) {
    float tw = 0.0f, th = 0.0f;
    SDL_Texture *bg = icons ? tank_background(&tw, &th) : nullptr;
    bool pillIcon = false;
    TankIconMetrics m = {};
    if (st.hasStock && bg != nullptr) {
        tank_icon_lines(st, bg, tw, th);
        m = tank_icon_metrics(st);
        pillIcon = (m.pillSize > 0.0f);
    } else if (st.hasStock) {
        for (int g = 0; g < LV_TANK_GAUGE_COUNT; g++) {
            char line[LANG_MSGARG_STRING_LEN];
            tank_gauge_text(st, g, line, sizeof(line));
            ImGui::TextUnformatted(line);
        }
    } else {
        /* A recording made before the tanks' resources were recorded. It
         * wraps at the width the block was measured with (tank_room). */
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tank_text_w());
        ImGui::TextDisabled("%s", langGetText(STR_LV_TANK_NO_STOCK));
        ImGui::PopTextWrapPos();
    }
    if (m.builderH > 0.0f) {
        tank_builder_icon_line(st, m);
    } else {
        tank_builder_words_line(st);
    }
    /* The pillboxes' line only while the tank carries one. The room for it
     * stays (tank_room counts it), so the panel does not jump when a
     * pillbox is picked up or put down. */
    if (st.carriedPills <= 0) return;
    TankIconMetrics pm = {};
    if (pillIcon) {
        tank_pill_icon_line(st, m);
    } else if (tank_pill_words_metrics(&pm)) {
        /* Among lines in words too: the picture says what is counted. */
        tank_pill_icon_line(st, pm);
    } else {
        MessageArgs args = {};
        args.number = st.carriedPills;
        ImGui::TextUnformatted(langGetTextFmt(STR_LV_TANK_PILLS, &args));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            char numbers[LANG_MSGARG_STRING_LEN * 2];
            tank_pill_numbers(st, numbers, sizeof(numbers));
            if (numbers[0] != '\0') ImGui::SetTooltip("%s", numbers);
        }
    }
}

/* The room under the name and the sizes of what goes in it, for
 * lvTankLayoutPick: all from the size of the window's content and from the
 * font, none of it from what was drawn before. */
static LvTankRoom tank_room(const LvTankStatus &st, ImVec2 avail) {
    const ImGuiStyle &style = ImGui::GetStyle();
    const float otherW = tank_text_w();
    LvTankRoom r;
    r.availW = avail.x;
    r.availH = avail.y;
    r.wellW = TANK_SLICE_W;
    r.wellH = TANK_SLICE_H;
    r.wellEdge = 2.0f;
    r.maxScale = TANK_MAX_SCALE;
    r.wordsMinScale = TANK_DEFAULT_SCALE;
    r.gapBeside = style.ItemSpacing.x * 2.0f;
    r.gapUnder = style.ItemSpacing.y;
    r.iconsW = 0.0f;
    r.iconsH = 0.0f;
    if (st.hasStock) {
        float tw = 0.0f, th = 0.0f;
        float words = tank_words_w(st);
        r.wordsW = (words > otherW) ? words : otherW;
        r.wordsH = tank_text_h();
        if (tank_background(&tw, &th) != nullptr) {
            /* With its picture the line for the pillboxes is one of the
             * short ones, and the builder's line is counted with his
             * picture in place of the word. A line that keeps its words (a
             * language with no colon in it) is counted in words. */
            const TankIconMetrics m = tank_icon_metrics(st);
            const float builder = (m.builderH > 0.0f) ? m.builderLineW : tank_builder_w();
            float other = builder;
            if (m.pillSize <= 0.0f && otherW > other) other = otherW;
            r.iconsW = (m.lineW > other) ? m.lineW : other;
            r.iconsH = tank_text_h();
        }
    } else {
        /* One wrapped note in place of the four values, then the two lines. */
        float noteH = ImGui::CalcTextSize(langGetText(STR_LV_TANK_NO_STOCK), nullptr, false,
                                          otherW).y;
        r.wordsW = otherW;
        r.wordsH = noteH + style.ItemSpacing.y + tank_lines_h(2);
    }
    return r;
}

/* The selected player's lag: the round-trip time and how far behind the
 * present the player's screen was, the round-trip time across the recording
 * with a line at the playhead, and the check box that shows the map as that
 * player's screen showed it. */
static void tank_lag(const LvTankStatus &st, const LvPlayerLag &lag, int viewSlot) {
    const ImGuiStyle &style = ImGui::GetStyle();
    const float graphH = tank_lag_graph_h();
    const float w = ImGui::GetContentRegionAvail().x;
    MessageArgs args = {};

    if (lag.has && lag.bot) {
        /* A bot has no round trip, so only the rewind is shown. */
        args.number = lag.behindMs;
        ImGui::TextUnformatted(langGetTextFmt(STR_LV_LAG_LINE_BOT, &args));
    } else if (lag.has) {
        args.number = lag.rttMs;
        args.number2 = lag.behindMs;
        ImGui::TextUnformatted(langGetTextFmt(STR_LV_LAG_LINE, &args));
    } else {
        ImGui::TextDisabled("%s", langGetText(STR_LV_LAG_NONE));
    }

    /* The graph is of the round-trip time, which a bot does not have. */
    if (lag.has && !lag.bot && w > 0.0f) {
        ImVec2 g0 = ImGui::GetCursorScreenPos();
        ImGui::PlotLines("##lag_graph", lag.graph, LV_LAG_GRAPH_SAMPLES, 0, nullptr, 0.0f,
                         lag.graphMax, ImVec2(w, graphH));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            args = {};
            args.number = (int)lag.graphMax;
            ImGui::SetTooltip("%s", langGetTextFmt(STR_LV_LAG_GRAPH_TIP, &args));
        }
        /* The playhead, across the plot's inner box. */
        float x = g0.x + style.FramePadding.x +
                  lag.playhead * (w - style.FramePadding.x * 2.0f);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(floorf(x) + 0.5f, g0.y + style.FramePadding.y),
                                            ImVec2(floorf(x) + 0.5f,
                                                   g0.y + graphH - style.FramePadding.y),
                                            IM_COL32(255, 255, 255, 200));
    } else if (!(lag.has && lag.bot)) {
        /* No record yet: keep the graph's room so the panel does not move. */
        ImGui::Dummy(ImVec2(w > 0.0f ? w : 1.0f, graphH));
    }

    bool on = (viewSlot >= 0 && viewSlot == (int)st.slot);
    /* With no record for this player yet the view has no rewind to draw, so
       the box cannot be ticked. A view already on can still be turned off. */
    const bool noRecord = !lag.has && !on;
    ImGui::BeginDisabled(noRecord);
    if (ImGui::Checkbox(langGetText(STR_LV_LAG_VIEW), &on)) {
        lv_clientMutexWaitFor();
        lv_screenLagViewSet(on ? (int)st.slot : -1);
        lv_clientMutexRelease();
        lv_drawDirtyScreen();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", langGetText(STR_LV_LAG_VIEW_TIP));
    }
}

void lv_imgui_lag_banner_at(float x, float y, float w) {
    char name[LANG_MSGARG_STRING_LEN];
    int behind = 0;
    bool drawn = false;
    bool on;

    lv_clientMutexWaitFor();
    on = lv_screenLagViewStatus(name, sizeof(name), &behind, &drawn);
    lv_clientMutexRelease();
    if (!on) {
        return;
    }

    MessageArgs args = {};
    snprintf(args.string1, sizeof(args.string1), "%s", name);
    args.number = behind;
    char text[LANG_MSGARG_STRING_LEN * 2];
    snprintf(text, sizeof(text), "%s",
             langGetTextFmt(drawn ? STR_LV_LAG_BANNER : STR_LV_LAG_BANNER_WAIT, &args));

    /* At the top of the map, in the middle, 8 pixels down. */
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + x + w * 0.5f, vp->Pos.y + y + 8.0f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    if (ImGui::Begin("##lag_banner", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoDocking)) {
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 210, 90, 255));
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton(langGetText(STR_LV_LAG_OFF))) {
            lv_clientMutexWaitFor();
            lv_screenLagViewSet(-1);
            lv_clientMutexRelease();
            lv_drawDirtyScreen();
        }
    }
    ImGui::End();
}

void lv_imgui_tank_window(void) {
    if (!lv_g_show_tank_window) {
        return;
    }

    {
        /* A panel that sat in the dock when the dock was turned off goes
         * back to its default place, the same as a reset. */
        bool to_default = lv_g_reset_window_positions ||
                          lv_imgui_dock_take_float_reset("###tank");
        ImGuiCond cond = to_default ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        ImGui::SetNextWindowSize(ImVec2(TANK_DEFAULT_W,
                                        lv_imgui_tank_dock_height(TANK_DEFAULT_W)), cond);
        /* Beside the HUD panel, under where the Overview panel opens. */
        ImGui::SetNextWindowPos(ImVec2(220, 250), cond);
    }

    char title[128];
    snprintf(title, sizeof(title), "%s###tank", langGetText(STR_LV_WIN_TANK));
    /* No scroll bar: what does not go into the window is not drawn at all
     * (lvTankLayoutPick), so there is nothing to scroll to. */
    if (ImGui::Begin(title, &lv_g_show_tank_window,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse)) {
        lv_imgui_keep_window_on_screen();

        /* Playback writes the players on the decode timer, so what is drawn
         * is copied out under its lock, every frame. */
        LvTankStatus st;
        LvPlayerLag lag;
        int lagViewSlot;
        lv_clientMutexWaitFor();
        lv_screenTankStatusSnapshot(&st);
        lv_screenPlayerLagSnapshot(st.selected ? (int)st.slot : -1, &lag);
        lagViewSlot = lv_screenLagViewSlot();
        lv_clientMutexRelease();

        if (!st.selected) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", langGetText(STR_LV_TANK_HINT));
            ImGui::PopTextWrapPos();
        } else {
            const ImGuiStyle &style = ImGui::GetStyle();

            /* The name, after a stripe in the colour of the tank's team,
             * and at the right the X that deselects the tank. A dead tank
             * says so: its gauges are then empty, as the game's are. */
            {
                char name[LANG_MSGARG_STRING_LEN];
                if (st.alive) {
                    snprintf(name, sizeof(name), "%s", st.name);
                } else {
                    MessageArgs args = {};
                    snprintf(args.string1, sizeof(args.string1), "%s", st.name);
                    snprintf(args.string2, sizeof(args.string2), "%s",
                             langGetText(STR_LV_HUD_DEAD));
                    snprintf(name, sizeof(name), "%s",
                             langGetTextFmt(STR_LV_HUD_NAME_TEAM, &args));
                }
                if (lv_name_header("deselect", st.hasTeam != 0, tank_team_colour(st.colour), -1,
                                   st.chip, name, nullptr, langGetText(STR_LV_TIP_DESELECT))) {
                    /* No tank is selected: the panel shows its hint and the
                     * map has no mark. A one-player view that is on stays
                     * on; the X of the HUD panel is the one that leaves it. */
                    lv_clientMutexWaitFor();
                    lv_screenDeselectTank();
                    lv_clientMutexRelease();
                }
            }

            /* What the room under the name has space for: the text with
             * words, with the small pictures, or none of it; beside the
             * well or under it; and the scale of the well. */
            /* The lag section keeps its room under the rest. */
            ImVec2 avail = ImGui::GetContentRegionAvail();
            if (lag.recorded) {
                avail.y -= tank_lag_h(!(lag.has && lag.bot));
            }
            const LvTankRoom room = tank_room(st, avail);
            const LvTankLayout lay = lvTankLayoutPick(&room);
            const bool icons = (lay.text == LV_TANK_TEXT_ICONS);

            ImVec2 origin = ImGui::GetCursorScreenPos();
            tank_gauges(st, origin, lay.scale);
            if (lay.text == LV_TANK_TEXT_NONE) {
                /* No room for all of the text: none of it. It comes back
                 * when the panel is large enough again. */
            } else if (lay.beside) {
                ImGui::SameLine(0.0f, room.gapBeside);
                ImGui::BeginGroup();
                tank_text(st, icons);
                ImGui::EndGroup();
            } else {
                tank_text(st, icons);
            }
            if (lag.recorded) {
                tank_lag(st, lag, lagViewSlot);
            }
        }
    }
    ImGui::End();
}
