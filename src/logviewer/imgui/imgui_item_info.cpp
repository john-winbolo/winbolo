/*
 * imgui_item_info.cpp - ImGui item information window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "imgui_item_info.h"
#include "imgui_main_menu.h"
#include "imgui_dock.h"
#include "imgui_context.h"
#include "imgui_dialogs.h"
#include "imgui.h"
#include "../../gui/lang.h"
#include "../../gui/sdl3/dialogs/imgui_dialog_utils.h"

#include <cstdio>
#include <cstring>

/* External functions from backend - using C types directly */
extern "C" {
    void lv_screenCentreOnSelectedItem(void);
    void lv_screenGetPlayerName(char *buffer, unsigned char player, size_t destSize);
}

/* Item info state */
static unsigned char s_item_type = 0;      /* 0 = none, 1 = base, 2 = pillbox */
static unsigned char s_item_number = 0;
static unsigned char s_owner = 0;
static unsigned char s_x = 0;
static unsigned char s_y = 0;
static unsigned char s_armour = 0;
static unsigned char s_shells = 0;
static unsigned char s_mines = 0;
static int s_in_tank = 0;

/* Cached display strings */
static char s_owner_text[80] = "";
static char s_location_text[32] = "";

void lv_imgui_item_info_init(void) {
    s_item_type = 0;
    s_item_number = 0;
    s_owner = 0;
    s_x = 0;
    s_y = 0;
    s_armour = 0;
    s_shells = 0;
    s_mines = 0;
    s_in_tank = 0;
    s_owner_text[0] = '\0';
    s_location_text[0] = '\0';
}

void lv_imgui_item_info_update(unsigned char itemType, unsigned char itemNumber, unsigned char owner, 
                            unsigned char x, unsigned char y, unsigned char armour, unsigned char shells, 
                            unsigned char mines, int inTank) {
    /* Update state */
    s_item_type = itemType;
    s_item_number = itemNumber;
    s_owner = owner;
    s_x = x;
    s_y = y;
    s_armour = armour;
    s_shells = shells;
    s_mines = mines;
    s_in_tank = inTank;
    
    /* Update display strings */
    if (itemType == 0) {
        s_owner_text[0] = '\0';
        s_location_text[0] = '\0';
    } else {
        /* Owner text */
        if (owner == 0xFF) { /* NEUTRAL */
            snprintf(s_owner_text, sizeof(s_owner_text), "%s",
                     langGetText(STR_MAPEDIT_OWNER_NEUTRAL));
        } else {
            char player_name[PLAYER_NAME_LEN];
            lv_screenGetPlayerName(player_name, owner, sizeof(player_name));
            MessageArgs args = {};
            strncpy(args.string1, player_name, sizeof(args.string1) - 1);
            args.number = owner;
            snprintf(s_owner_text, sizeof(s_owner_text), "%s",
                     langGetTextFmt(STR_LV_OWNER_FMT, &args));
        }

        /* Location text */
        MessageArgs args = {};
        args.number = x;
        args.number2 = y;
        snprintf(s_location_text, sizeof(s_location_text), "%s",
                 langGetTextFmt(STR_LV_POSITION_XY, &args));
    }
}

void lv_imgui_item_info_window(void) {
    if (!lv_g_show_item_info_window) {
        return;
    }
    
    {
        /* A panel that sat in the dock when the dock was turned off goes
         * back to its default place, the same as a reset. */
        bool to_default = lv_g_reset_window_positions ||
                          lv_imgui_dock_take_float_reset("###iteminfo");
        ImGuiCond cond = to_default ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        ImVec2 vp = ImGui::GetMainViewport()->Size;
        ImGui::SetNextWindowSize(ImVec2(280, 150), cond);
        ImGui::SetNextWindowPos(ImVec2(vp.x - 280 - 10, 30 + 180 + 5), cond);
    }

    char ii_title[128];
    snprintf(ii_title, sizeof(ii_title), "%s###iteminfo", langGetText(STR_LV_WIN_ITEMINFO));
    if (ImGui::Begin(ii_title, &lv_g_show_item_info_window, ImGuiWindowFlags_NoCollapse)) {
        lv_imgui_keep_window_on_screen();

        if (s_item_type == 0) {
            /* No item selected */
            ImGui::TextUnformatted(langGetText(STR_LV_NO_ITEM_SELECTED));
        } else {
            /* Item type header. The number counts from 1, as the game's
             * own lists of pillboxes and bases do; the item is held by its
             * place in the list, from 0. */
            {
                MessageArgs args = {};
                args.number = s_item_number + 1;
                ImGui::TextUnformatted(langGetTextFmt(
                    s_item_type == 1 ? STR_MAPEDIT_INSP_BASE_HASH
                                     : STR_MAPEDIT_INSP_PILL_HASH,
                    &args));
            }

            ImGui::Separator();

            /* Location */
            {
                MessageArgs args = {};
                strncpy(args.string1, s_location_text, sizeof(args.string1) - 1);
                ImGui::TextUnformatted(langGetTextFmt(STR_LV_LOCATION_FMT, &args));
            }

            /* Owner */
            {
                MessageArgs args = {};
                strncpy(args.string1, s_owner_text, sizeof(args.string1) - 1);
                ImGui::TextUnformatted(langGetTextFmt(STR_LV_OWNER_LBL, &args));
            }

            /* Armour */
            {
                MessageArgs args = {};
                args.number = s_armour;
                ImGui::TextUnformatted(langGetTextFmt(STR_LV_ARMOUR_FMT, &args));
            }

            /* Shells (bases only) */
            if (s_item_type == 1) {
                MessageArgs args = {};
                args.number = s_shells;
                ImGui::TextUnformatted(langGetTextFmt(STR_LV_SHELLS_FMT, &args));
                args.number = s_mines;
                ImGui::TextUnformatted(langGetTextFmt(STR_LV_MINES_FMT, &args));
            }

            /* In tank status */
            {
                MessageArgs args = {};
                strncpy(args.string1, langGetText(s_in_tank ? STR_YES : STR_NO),
                        sizeof(args.string1) - 1);
                ImGui::TextUnformatted(langGetTextFmt(STR_LV_IN_TANK_FMT, &args));
            }

            /* Center on map button */
            ImGui::Spacing();
            if (s_in_tank) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button(langGetText(STR_LV_CENTER_ON_MAP), ImVec2(-1, 0))) {
                lv_screenCentreOnSelectedItem();
            }
            imguiHandOnHover();
            if (s_in_tank) {
                ImGui::EndDisabled();
            }
        }
    }
    ImGui::End();
}

void lv_imgui_item_info_pill_tooltip(int number, const char *ownerName, int teamColour,
                                     int armour) {
    char owner[LANG_MSGARG_STRING_LEN];

    if (number <= 0) return;
    /* The owner as the HUD panel names one: the player, and the team's
     * colour after it when the player is in a team. */
    if (ownerName == nullptr) {
        snprintf(owner, sizeof(owner), "%s", langGetText(STR_MAPEDIT_OWNER_NEUTRAL));
    } else {
        const char *team = (teamColour >= 0) ? lv_imgui_team_colour_name(teamColour) : "";
        if (team == nullptr || team[0] == '\0') {
            snprintf(owner, sizeof(owner), "%s", ownerName);
        } else {
            MessageArgs args = {};
            snprintf(args.string1, sizeof(args.string1), "%s", ownerName);
            snprintf(args.string2, sizeof(args.string2), "%s", team);
            snprintf(owner, sizeof(owner), "%s", langGetTextFmt(STR_LV_HUD_NAME_TEAM, &args));
        }
    }

    if (!ImGui::BeginTooltip()) return;
    {
        MessageArgs args = {};
        args.number = number;
        ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_INSP_PILL_HASH, &args));
    }
    {
        MessageArgs args = {};
        snprintf(args.string1, sizeof(args.string1), "%s", owner);
        ImGui::TextUnformatted(langGetTextFmt(STR_LV_OWNER_LBL, &args));
    }
    {
        MessageArgs args = {};
        args.number = armour;
        ImGui::TextUnformatted(langGetTextFmt(STR_LV_ARMOUR_FMT, &args));
    }
    ImGui::EndTooltip();
}
