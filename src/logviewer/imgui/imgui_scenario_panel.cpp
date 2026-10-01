/*
 * imgui_scenario_panel.cpp - a scenario's panel square in the Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * The list the followed player would have had on screen at the playhead,
 * drawn through the same scenario_panel_draw.cpp the game uses, with nothing
 * faded, in a square window that resizes from 128 pixels (one a panel unit)
 * up to the shorter side of the viewport. The row is chosen by
 * lv_screenFollowedPanelRow and the timer counts on the server's tick at the
 * playhead, which is the clock the scenario set its targets on.
 */

#include "imgui_scenario_panel.h"
#include "imgui_main_menu.h"
#include "imgui.h"
#include "../../gui/lang.h"
/* Plain C, with nothing but the public scenario_panel.h behind it. */
#include "../lv_presentation.h"
#include "../../gui/sdl3/scenario_panel_draw.h"

#include <cstdio>
#include <cstring>

/* External functions from the backend. draw.h is not included by name: it
 * resolves to src/gui/sdl3/draw.h, whose bolo headers clash with the
 * viewer's. */
extern "C" {
    struct SDL_Texture *lv_drawGetTilesTexture(void);
    uint32_t lv_screenGetTimeRunning(void);
    bool lv_playersIsInUse(BYTE playerNumber);
    void lv_playersGetPlayerName(BYTE playerNum, char *dest, size_t destSize);
    void lv_clientMutexWaitFor(void);
    void lv_clientMutexRelease(void);
}

/* The square's side in screen pixels. Starts at one pixel a panel unit; the
 * preference and a resize drag change it. */
static int s_side = LV_SCN_PANEL_SIDE_MIN;

int lv_imgui_scenario_panel_side(void) {
    return s_side;
}

void lv_imgui_scenario_panel_set_side(int side) {
    s_side = (side < LV_SCN_PANEL_SIDE_MIN) ? LV_SCN_PANEL_SIDE_MIN : side;
}

/* What the size callback needs: the window's size around its content (title
 * bar and padding) and the range the content's side may take. */
typedef struct {
    ImVec2 frame;
    float  sideMin;
    float  sideMax;
} LvScnPanelFit;

/* Keeps the content square while the window is resized. The edge the drag
 * moves decides the side: whichever of width and height changed the more. */
static void lvScnPanelSquare(ImGuiSizeCallbackData *data) {
    const LvScnPanelFit *fit = (const LvScnPanelFit *)data->UserData;
    float dx = data->DesiredSize.x - data->CurrentSize.x;
    float dy = data->DesiredSize.y - data->CurrentSize.y;
    float side = (dx * dx >= dy * dy) ? data->DesiredSize.x - fit->frame.x
                                      : data->DesiredSize.y - fit->frame.y;

    if (side < fit->sideMin) side = fit->sideMin;
    if (side > fit->sideMax) side = fit->sideMax;
    data->DesiredSize = ImVec2(side + fit->frame.x, side + fit->frame.y);
}

static float lvScnPanelMin(float a, float b) {
    return (a < b) ? a : b;
}

/* Read by the callback inside Begin, and by the window to turn its size back
 * into a side. */
static LvScnPanelFit s_fit;

/* The row's bytes as they stood under the lock, and the list parsed from
 * them. Static because the list is several kilobytes. */
static uint8_t      s_bytes[SCN_PANEL_MAX];
static uint16_t     s_len = 0;
static ScnPanelList s_list;

/* The name a slot is playing under, for a name primitive, or NULL for a slot
 * nobody holds. One buffer a slot, since a list may name several. */
static const char *panelPlayerName(void *ctx, uint8_t slot) {
    static char names[MAX_TANKS][64];

    (void)ctx;
    if (slot >= MAX_TANKS || !lv_playersIsInUse(slot)) {
        return NULL;
    }
    lv_playersGetPlayerName(slot, names[slot], sizeof(names[slot]));
    return names[slot];
}

void lv_imgui_scenario_panel_window(bool logLoaded) {
    const LvPresPanelRow *row;
    bool                  have;
    uint32_t              tick = 0;

    /* Reset Windows puts the square back to its smallest. Ahead of the
     * returns below, so the reset takes whether or not the panel is drawn
     * on the frame it was asked for. */
    if (lv_g_reset_window_positions) {
        s_side = LV_SCN_PANEL_SIDE_MIN;
    }

    if (!logLoaded || !lv_g_show_scenario_panel_window) {
        return;
    }

    /* Playback writes the stores on the decode timer, so the row is copied
     * out under its lock and parsed from the copy. */
    lv_clientMutexWaitFor();
    row  = lv_screenFollowedPanelRow();
    have = (row != NULL && row->len <= SCN_PANEL_MAX);
    if (have) {
        s_len = row->len;
        memcpy(s_bytes, row->bytes, row->len);
        tick = lv_screenServerTickAt(lv_screenGetTimeRunning());
    }
    lv_clientMutexRelease();
    if (!have || scnPanelParse(s_bytes, s_len, &s_list) != SCN_PANEL_OK) {
        return;
    }

    {
        LvScnPanelFit    &fit   = s_fit;
        const ImGuiStyle &style = ImGui::GetStyle();
        ImGuiCond cond = lv_g_reset_window_positions ? ImGuiCond_Always
                                                     : ImGuiCond_FirstUseEver;
        ImVec2 vp = ImGui::GetMainViewport()->Size;
        float  side;

        /* Title bar and padding: the window is this much bigger than the
         * square it holds. */
        fit.frame   = ImVec2(style.WindowPadding.x * 2.0f,
                             style.WindowPadding.y * 2.0f +
                                 ImGui::GetFrameHeight());
        fit.sideMin = (float)LV_SCN_PANEL_SIDE_MIN;
        fit.sideMax = lvScnPanelMin(vp.x - fit.frame.x, vp.y - fit.frame.y);
        if (fit.sideMax < fit.sideMin) fit.sideMax = fit.sideMin;

        side = (float)s_side;
        if (side > fit.sideMax) side = fit.sideMax;

        /* Left of Game Information, which sits 280 wide against the right
         * edge, and no further left than the viewport's own edge. */
        {
            float x = vp.x - 280 - 10 - side - 30;
            if (x < 0.0f) x = 0.0f;
            ImGui::SetNextWindowPos(ImVec2(x, 30), cond);
        }
        ImGui::SetNextWindowSize(ImVec2(side + fit.frame.x, side + fit.frame.y),
                                 cond);
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(fit.sideMin + fit.frame.x, fit.sideMin + fit.frame.y),
            ImVec2(fit.sideMax + fit.frame.x, fit.sideMax + fit.frame.y),
            lvScnPanelSquare, &fit);
    }

    char title[128];
    snprintf(title, sizeof(title), "%s###lvscnpanel",
             langGetText(STR_SCNPANEL_SETTINGS_TITLE));
    if (ImGui::Begin(title, &lv_g_show_scenario_panel_window,
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse)) {
        ImVec2          origin = ImGui::GetCursorScreenPos();
        ImVec2          avail  = ImGui::GetContentRegionAvail();
        float           side   = lvScnPanelMin(avail.x, avail.y);
        ScnPanelDrawEnv env;

        if (side < 1.0f) side = 1.0f;
        memset(&env, 0, sizeof(env));
        env.playerName = panelPlayerName;
        env.ctx        = NULL;
        env.tick       = tick;
        env.scale      = side / (float)SCN_PANEL_UNITS;
        env.alpha      = 1.0f;
        env.tiles      = (void *)lv_drawGetTilesTexture();
        /* The square's room in the layout. */
        ImGui::Dummy(ImVec2(side, side));
        scnPanelDraw(&s_list, origin.x, origin.y, &env);
        /* Kept for the preference, which is saved on exit. Taken from the
         * window's size by the same sum that sets it, so a restart puts it
         * back exactly. A side at the viewport's limit and below the one
         * kept is the size constraint shrinking the window to fit a smaller
         * viewport, not the user resizing it, and is not kept: otherwise one
         * session in a small window would shrink the square for good. */
        {
            ImVec2 win     = ImGui::GetWindowSize();
            float  kept    = lvScnPanelMin(win.x - s_fit.frame.x,
                                           win.y - s_fit.frame.y);
            int    keptInt = (int)(kept + 0.5f);
            bool   clamped = kept >= s_fit.sideMax - 0.5f && keptInt < s_side;

            if (!clamped) {
                lv_imgui_scenario_panel_set_side(keptInt);
            }
        }
    }
    ImGui::End();
}
