/*
 * imgui_scenario_panel.cpp - a scenario's panel square in the Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The list the followed player would have had on screen at the playhead,
 * drawn through the same scenario_panel_draw.cpp the game uses, at its
 * native 128 pixels with nothing faded. The row is chosen by
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

/* The panel's side in screen pixels: one pixel a panel unit. */
#define LV_SCN_PANEL_PX ((float)SCN_PANEL_UNITS)

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
        ImGuiCond cond = lv_g_reset_window_positions ? ImGuiCond_Always
                                                     : ImGuiCond_FirstUseEver;
        ImVec2 vp = ImGui::GetMainViewport()->Size;
        /* Left of Game Information, which sits 280 wide against the right
         * edge. */
        ImGui::SetNextWindowPos(ImVec2(vp.x - 280 - 10 - LV_SCN_PANEL_PX - 30,
                                       30),
                                cond);
    }

    char title[128];
    snprintf(title, sizeof(title), "%s###lvscnpanel",
             langGetText(STR_SCNPANEL_SETTINGS_TITLE));
    if (ImGui::Begin(title, &lv_g_show_scenario_panel_window,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse)) {
        ImVec2          origin = ImGui::GetCursorScreenPos();
        ScnPanelDrawEnv env;

        memset(&env, 0, sizeof(env));
        env.playerName = panelPlayerName;
        env.ctx        = NULL;
        env.tick       = tick;
        env.scale      = 1.0f;
        env.alpha      = 1.0f;
        env.tiles      = (void *)lv_drawGetTilesTexture();
        /* The square's room in the layout, so the window sizes to it. */
        ImGui::Dummy(ImVec2(LV_SCN_PANEL_PX, LV_SCN_PANEL_PX));
        scnPanelDraw(&s_list, origin.x, origin.y, &env);
    }
    ImGui::End();
}
