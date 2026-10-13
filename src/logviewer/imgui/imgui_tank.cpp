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
 * pillboxes the tank carries.
 *
 * The selected tank is the viewer's "self" player: the one a click on a
 * tank in the HUD panel picks, the one a Select Team click on a tank on the
 * map picks, and the one Tank Centred follows. Everything is read off the
 * game's state every frame, so it follows a seek in either direction.
 */

#include "imgui_tank.h"
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
    #include "../lv_tank_status.h"
    #include "../../gui/positions.h"
    #include "../../gui/sdl3/map_colours.h"  /* mapColourTeam */

    /* External functions from backend - using C types directly. The viewer's
     * own headers cannot be included here beside lang.h: its lv_global.h and
     * the game's global.h, which lang.h brings in, share an include guard. */
    void lv_clientMutexWaitFor(void);
    void lv_clientMutexRelease(void);
    void lv_screenTankStatusSnapshot(LvTankStatus *out);
    SDL_Texture *lv_drawGetStatusBackgroundTexture(void);
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

/* The stripe in the team's colour at the start of the name. */
#define TANK_STRIPE_W 4.0f

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

/* The width the lines of text need, near enough: the longest of them is the
 * line for the pillboxes carried. */
static float tank_text_w(void) {
    MessageArgs args = {};
    args.number = 16;
    float w = ImGui::CalcTextSize(langGetTextFmt(STR_LV_TANK_PILLS, &args)).x;
    float builder = ImGui::CalcTextSize(langGetText(STR_LV_TANK_BUILDER_IN)).x;
    return (builder > w) ? builder : w;
}

static float tank_text_h(void) {
    return TANK_TEXT_LINES * ImGui::GetTextLineHeightWithSpacing();
}

/* True when the text goes beside the gauges at the default scale in a
 * window with availW of room across. */
static bool tank_text_beside(float availW) {
    return availW >= TANK_DEFAULT_SCALE * TANK_SLICE_W + ImGui::GetStyle().ItemSpacing.x * 2.0f +
                         tank_text_w();
}

float lv_imgui_tank_dock_height(float dockWidth) {
    const ImGuiStyle &style = ImGui::GetStyle();
    float availW = dockWidth - style.WindowPadding.x * 2.0f;
    float h = tank_header_h() + (float)(TANK_DEFAULT_SCALE * TANK_SLICE_H) + 2.0f;
    if (!tank_text_beside(availW)) {
        h += style.ItemSpacing.y + tank_text_h();
    }
    return h + style.WindowPadding.y * 2.0f + ImGui::GetFrameHeight();
}

/* The colour of the tank's team: the HUD panel's (imgui_hud.cpp). */
static ImU32 tank_team_colour(int colour) {
    SDL_FColor c = mapColourTeam(colour);
    if (!lvHudRedToOrange(&c.r, &c.g, &c.b)) {
        lvHudReadableColour(&c.r, &c.g, &c.b);
    }
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 1.0f));
}

/* "Shells: 12 / 40" for one gauge. */
static void tank_gauge_text(const LvTankStatus &st, int gauge, char *out, size_t outSize) {
    MessageArgs args = {};
    snprintf(args.string1, sizeof(args.string1), "%s", langGetText(k_gauge_name[gauge]));
    args.number = st.alive ? st.value[gauge] : 0;
    args.number2 = st.full[gauge];
    snprintf(out, outSize, "%s", langGetTextFmt(STR_LV_TANK_STOCK_FMT, &args));
}

/* The well and its four gauges with the top left at origin, at a whole
 * scale. It is an item of the window; each gauge has a tooltip. */
static void tank_gauges(const LvTankStatus &st, ImVec2 origin, int scale) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float s = (float)scale;
    SDL_Texture *bg = lv_drawGetStatusBackgroundTexture();
    float tw = 0.0f, th = 0.0f;
    ImVec2 p0(floorf(origin.x) + 1.0f, floorf(origin.y) + 1.0f);
    ImVec2 p1(p0.x + s * TANK_SLICE_W, p0.y + s * TANK_SLICE_H);

    dl->AddRect(ImVec2(p0.x - 1.0f, p0.y - 1.0f), ImVec2(p1.x + 1.0f, p1.y + 1.0f),
                TANK_EDGE_COLOUR);
    if (bg != nullptr && SDL_GetTextureSize(bg, &tw, &th) &&
        tw >= (float)(TANK_SLICE_X + TANK_SLICE_W) && th >= (float)(TANK_SLICE_Y + TANK_SLICE_H)) {
        /* Pixel art drawn several times its size: point sampled, or it blurs. */
        imguiPushNearestSamplingOn(dl);
        dl->AddImage((ImTextureID)(size_t)bg, p0, p1,
                     ImVec2((float)TANK_SLICE_X / tw, (float)TANK_SLICE_Y / th),
                     ImVec2((float)(TANK_SLICE_X + TANK_SLICE_W) / tw,
                            (float)(TANK_SLICE_Y + TANK_SLICE_H) / th));
        imguiPopNearestSamplingOn(dl);
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

/* The lines of text: the four values, the builder, the pillboxes. */
static void tank_text(const LvTankStatus &st) {
    if (st.hasStock) {
        for (int g = 0; g < LV_TANK_GAUGE_COUNT; g++) {
            char line[LANG_MSGARG_STRING_LEN];
            tank_gauge_text(st, g, line, sizeof(line));
            ImGui::TextUnformatted(line);
        }
    } else {
        /* A recording made before the tanks' resources were recorded. */
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", langGetText(STR_LV_TANK_NO_STOCK));
        ImGui::PopTextWrapPos();
    }
    ImGui::TextUnformatted(langGetText(st.builderDead ? STR_LV_TANK_BUILDER_DEAD :
                                       st.builderOut  ? STR_LV_TANK_BUILDER_OUT :
                                                        STR_LV_TANK_BUILDER_IN));
    {
        MessageArgs args = {};
        args.number = st.carriedPills;
        if (st.carriedPills > 0) {
            ImGui::TextUnformatted(langGetTextFmt(STR_LV_TANK_PILLS, &args));
        } else {
            ImGui::TextDisabled("%s", langGetTextFmt(STR_LV_TANK_PILLS, &args));
        }
    }
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
    if (ImGui::Begin(title, &lv_g_show_tank_window, ImGuiWindowFlags_NoCollapse)) {
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

        /* Playback writes the players on the decode timer, so what is drawn
         * is copied out under its lock, every frame. */
        LvTankStatus st;
        lv_clientMutexWaitFor();
        lv_screenTankStatusSnapshot(&st);
        lv_clientMutexRelease();

        if (!st.selected) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", langGetText(STR_LV_TANK_HINT));
            ImGui::PopTextWrapPos();
        } else {
            const ImGuiStyle &style = ImGui::GetStyle();

            /* The name, after a stripe in the colour of the tank's team. A
             * dead tank says so: its gauges are then empty, as the game's
             * are. */
            {
                ImDrawList *dl = ImGui::GetWindowDrawList();
                ImVec2 top = ImGui::GetCursorScreenPos();
                const float lh = ImGui::GetTextLineHeight();
                char name[LANG_MSGARG_STRING_LEN];
                if (st.hasTeam) {
                    dl->AddRectFilled(ImVec2(top.x, top.y + 1.0f),
                                      ImVec2(top.x + TANK_STRIPE_W, top.y + lh - 1.0f),
                                      tank_team_colour(st.colour), 1.0f);
                }
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
                ImGui::SetCursorScreenPos(ImVec2(top.x + TANK_STRIPE_W + style.ItemInnerSpacing.x,
                                                 top.y));
                ImGui::TextUnformatted(name);
                ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + tank_header_h()));
            }

            ImVec2 avail = ImGui::GetContentRegionAvail();
            bool beside = tank_text_beside(avail.x);
            /* The largest whole scale the room has for the well, with the
             * text under it when the text is not beside it. Never under 1:
             * a window too small for that scrolls. */
            float roomH = avail.y - 2.0f;
            if (!beside) roomH -= style.ItemSpacing.y + tank_text_h();
            int scale = 1;
            for (int s = 2; s <= TANK_MAX_SCALE; s++) {
                float needW = (float)(s * TANK_SLICE_W) + 2.0f;
                if (beside) needW += style.ItemSpacing.x * 2.0f + tank_text_w();
                if (needW > avail.x || (float)(s * TANK_SLICE_H) > roomH) break;
                scale = s;
            }

            ImVec2 origin = ImGui::GetCursorScreenPos();
            tank_gauges(st, origin, scale);
            if (beside) {
                ImGui::SameLine(0.0f, style.ItemSpacing.x * 2.0f);
                ImGui::BeginGroup();
                tank_text(st);
                ImGui::EndGroup();
            } else {
                tank_text(st);
            }
        }
    }
    ImGui::End();
}
