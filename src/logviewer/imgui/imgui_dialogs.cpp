/*
 * imgui_dialogs.cpp - ImGui modal dialogs for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This implements ImGui-based modal dialogs to replace Win32 dialogs:
 * - Team Colours dialog: Assign colours to player teams
 * - About dialog: Version and copyright information
 */

#include "imgui_dialogs.h"
#include "imgui.h"
#include "platform_config.h"
#include "../../gui/lang.h"
#include "../../gui/sdl3/dialogs/imgui_dialog_utils.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
    #include "logviewer.h"
    #include "../lv_team_colour.h"
    #include "../../gui/sdl3/map_colours.h"  /* mapColourTeam */

    void lv_drawDirtyScreen(void);
}

static LogViewerState *s_lv = nullptr;

/* Constants */
#define MAX_PLAYERS 16

/* Colour name lang IDs for team colours dialog. Resolved via
 * langGetText() at draw time so language changes take effect on the
 * next frame. */
static const langid colour_name_ids[] = {
    STR_LV_COL_GREY,
    STR_LV_COL_KHAKI,
    STR_LV_COL_GREEN,
    STR_LV_COL_PINK,
    STR_LV_COL_YELLOW,
    STR_LV_COL_LIGHTBLUE,
    STR_LV_COL_ORANGE,
    STR_LV_COL_LIGHTPURPLE,
    STR_LV_COL_AQUA,
    STR_LV_COL_LIGHTGREEN,
    STR_LV_COL_LIGHTGREY,
    STR_LV_COL_RED,
    STR_LV_COL_BLUE,
    STR_LV_COL_BROWN,
    STR_LV_COL_LIGHTPINK,
    STR_LV_COL_PALEGREEN,
    STR_LV_COL_PURPLE
};

#define NUM_COLOURS 17

/* The name of a team colour, for the panels that say which team a thing
 * belongs to. The teams have no names of their own in a log; the colour a
 * team is drawn in is what the user knows it by. */
const char *lv_imgui_team_colour_name(int colour) {
    if (colour < 0 || colour >= NUM_COLOURS) return "";
    return langGetText(colour_name_ids[colour]);
}

/* Dialog state */
static bool s_show_team_colours = false;
static bool s_show_about = false;

/* Live-spectator "Leave spectating?" confirm state. s_spec_leave_confirmed
 * latches on Yes and is consumed by lv_imgui_spectator_leave_confirmed(). */
static bool s_show_spec_leave = false;
static bool s_spec_leave_confirmed = false;

/* Team Colours dialog state */
static int s_team_colours[17] = {0};
static bool s_team_colours_loaded = false;

/* Forward declarations */
static void render_team_colours_dialog(void);
static void render_about_dialog(void);
static void render_spectator_leave_dialog(void);
static void load_team_colours_from_tc(void);
static void save_team_colours_to_tc(void);

/**
 * Initialize the dialogs module.
 */
void lv_imgui_dialogs_init(struct LogViewerState *lv) {
    s_lv = lv;
    char val[256];

    /* Load team colours from preferences. A stored old default with no
     * mark reads as the new default (lvTeamColourStoredPref). */
    lv_platform_config_get_string("LOGVIEWER", LV_TEAM_COLOUR_MARK_KEY, "", val, sizeof(val));
    const int marked = (strcmp(val, LV_TEAM_COLOUR_MARK) == 0);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        char key[64];
        snprintf(key, sizeof(key), "Team Colour %d", i + 1);

        /* The defaults are the viewer's one table (lv_team_colour.h). */
        char def_val[16];
        snprintf(def_val, sizeof(def_val), "%d", (int)lvTeamColourDefault(i));

        lv_platform_config_get_string("LOGVIEWER", key, def_val, val, sizeof(val));
        s_team_colours[i] = lvTeamColourStoredPref(i, atoi(val), marked);
    }

    /* Neutral colour */
    {
        char def_val[16];
        snprintf(def_val, sizeof(def_val), "%d",
                 (int)lvTeamColourDefault(LV_TEAM_COLOUR_NEUTRAL));
        lv_platform_config_get_string("LOGVIEWER", LV_TEAM_COLOUR_NEUTRAL_KEY, def_val, val,
                                      sizeof(val));
        s_team_colours[16] = lvTeamColourStoredPref(LV_TEAM_COLOUR_NEUTRAL, atoi(val), marked);
    }

    s_team_colours_loaded = true;
}

/**
 * Render all open dialogs.
 */
void lv_imgui_dialogs_render(void) {
    /* Open popups at the start of the frame (outside of menu context) */
    static bool was_team_colours_open = false;
    static bool was_about_open = false;

    /* Open popup on transition from closed to open. Use a stable
     * widget id (not the localized title) so language changes don't
     * lose the popup state. */
    if (s_show_team_colours && !was_team_colours_open) {
        ImGui::OpenPopup("###team_colours");
    }
    was_team_colours_open = s_show_team_colours;

    if (s_show_about && !was_about_open) {
        ImGui::OpenPopup("###about");
    }
    was_about_open = s_show_about;

    static bool was_spec_leave_open = false;
    if (s_show_spec_leave && !was_spec_leave_open) {
        ImGui::OpenPopup("###spec_leave");
    }
    was_spec_leave_open = s_show_spec_leave;

    /* Render each dialog */
    if (s_show_team_colours) {
        render_team_colours_dialog();
    }
    if (s_show_about) {
        render_about_dialog();
    }
    if (s_show_spec_leave) {
        render_spectator_leave_dialog();
    }
}

/**
 * Save dialog state to preferences.
 */
void lv_imgui_dialogs_save(void) {
    char val[64];

    /* Save team colours */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        char key[64];
        snprintf(key, sizeof(key), "Team Colour %d", i + 1);
        snprintf(val, sizeof(val), "%d", s_team_colours[i]);
        lv_platform_config_set_string("LOGVIEWER", key, val);
    }
    
    /* Neutral colour */
    snprintf(val, sizeof(val), "%d", s_team_colours[16]);
    lv_platform_config_set_string("LOGVIEWER", LV_TEAM_COLOUR_NEUTRAL_KEY, val);
    lv_platform_config_set_string("LOGVIEWER", LV_TEAM_COLOUR_MARK_KEY, LV_TEAM_COLOUR_MARK);
    
    /* Save to disk */
    lv_platform_config_save();
}

/**
 * Show the Team Colours dialog.
 */
void lv_imgui_show_team_colours_dialog(void) {
    /* Load current colours from tc array */
    load_team_colours_from_tc();
    s_show_team_colours = true;
    /* OpenPopup will be called in render function */
}

/**
 * Show the About dialog.
 */
void lv_imgui_show_about_dialog(void) {
    s_show_about = true;
    /* OpenPopup will be called in render function */
}

/**
 * Arm the live-spectator leave-confirm modal.
 */
void lv_imgui_spectator_leave_request(void) {
    s_show_spec_leave = true;
    s_spec_leave_confirmed = false;
    /* OpenPopup will be called in render function */
}

/**
 * Consume the confirm latch: true once after Yes, false otherwise.
 */
bool lv_imgui_spectator_leave_confirmed(void) {
    bool confirmed = s_spec_leave_confirmed;
    s_spec_leave_confirmed = false;
    return confirmed;
}

/* ============================================================================
 * Team Colours Dialog Implementation
 * ============================================================================ */

static void load_team_colours_from_tc(void) {
    /* Copy from LogViewerState tc array */
    for (int i = 0; i < 17; i++) {
        s_team_colours[i] = (int)s_lv->tc[i];
    }
}

static void save_team_colours_to_tc(void) {
    /* Copy to LogViewerState tc array */
    for (int i = 0; i < 17; i++) {
        s_lv->tc[i] = (unsigned char)s_team_colours[i];
    }
}

/* A square in a palette colour, as the map draws that colour, in front of
 * the colour's name. In a row with a combo box it is moved down to the
 * middle of the box. */
static void team_colour_swatch(int entry, bool besideCombo) {
    const ImGuiStyle &style = ImGui::GetStyle();
    SDL_FColor c = mapColourTeam(entry);
    float side = ImGui::GetTextLineHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    if (besideCombo) p.y += style.FramePadding.y;
    dl->AddRectFilled(p, ImVec2(p.x + side, p.y + side),
                      ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 1.0f)));
    dl->AddRect(p, ImVec2(p.x + side, p.y + side), ImGui::GetColorU32(ImGuiCol_Border));
    ImGui::Dummy(ImVec2(side, besideCombo ? ImGui::GetFrameHeight() : side));
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
}

static void render_team_colours_dialog(void) {
    /* Center the modal */
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(350, 400), ImGuiCond_FirstUseEver);
    
    char team_title[128];
    snprintf(team_title, sizeof(team_title), "%s###team_colours", langGetText(STR_LV_TEAM_COLOURS));
    if (ImGui::BeginPopupModal(team_title, &s_show_team_colours, 0)) {
        ImGui::TextUnformatted(langGetText(STR_LV_ASSIGN_COLOURS_HINT));
        ImGui::Separator();

        /* Neutral colour, first, above the teams */
        ImGui::TextUnformatted(langGetText(STR_LV_NEUTRAL_LBL));
        ImGui::SameLine(100);

        const char* neutral_colour = langGetText(colour_name_ids[s_team_colours[16]]);
        team_colour_swatch(s_team_colours[16], true);
        if (ImGui::BeginCombo("##neutral", neutral_colour)) {
            for (int j = 0; j < NUM_COLOURS; j++) {
                bool is_selected = (s_team_colours[16] == j);
                team_colour_swatch(j, false);
                if (ImGui::Selectable(langGetText(colour_name_ids[j]), is_selected)) {
                    s_team_colours[16] = j;
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Separator();

        /* Colours belong to reusable alliance team slots, not player numbers. */
        for (int i = 0; i < MAX_PLAYERS; i++) {
            MessageArgs lblArgs = {};
            lblArgs.number = i + 1;
            ImGui::TextUnformatted(langGetTextFmt(STR_DLGLOBBY_TEAM_HEADER, &lblArgs));
            ImGui::SameLine(100);

            char combo_id[32];
            snprintf(combo_id, sizeof(combo_id), "##team%d", i);

            /* Current selection */
            const char* current_colour = langGetText(colour_name_ids[s_team_colours[i]]);

            team_colour_swatch(s_team_colours[i], true);
            if (ImGui::BeginCombo(combo_id, current_colour)) {
                for (int j = 0; j < NUM_COLOURS; j++) {
                    bool is_selected = (s_team_colours[i] == j);
                    team_colour_swatch(j, false);
                    if (ImGui::Selectable(langGetText(colour_name_ids[j]), is_selected)) {
                        s_team_colours[i] = j;
                    }
                    if (is_selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        }

        ImGui::Separator();

        /* Buttons */
        if (ImGui::Button(langGetText(STR_OK), ImVec2(80, 0))) {
            /* Apply changes. The pillboxes and bases of the map are kept
             * from frame to frame, so the map is told to draw them again:
             * the new colours then show on the next frame. */
            save_team_colours_to_tc();
            lv_drawDirtyScreen();
            
            /* Save to preferences */
            for (int i = 0; i < MAX_PLAYERS; i++) {
                char key[64];
                char val[16];
                snprintf(key, sizeof(key), "Team Colour %d", i + 1);
                snprintf(val, sizeof(val), "%d", s_team_colours[i]);
                lv_platform_config_set_string("LOGVIEWER", key, val);
            }
            char val[16];
            snprintf(val, sizeof(val), "%d", s_team_colours[16]);
            lv_platform_config_set_string("LOGVIEWER", LV_TEAM_COLOUR_NEUTRAL_KEY, val);
            lv_platform_config_set_string("LOGVIEWER", LV_TEAM_COLOUR_MARK_KEY,
                                          LV_TEAM_COLOUR_MARK);
            lv_platform_config_save();
            
            s_show_team_colours = false;
            ImGui::CloseCurrentPopup();
        }
        imguiHandOnHover();
        
        ImGui::SameLine();

        if (ImGui::Button(langGetText(STR_CANCEL), ImVec2(80, 0))) {
            s_show_team_colours = false;
            ImGui::CloseCurrentPopup();
        }
        imguiHandOnHover();

        ImGui::EndPopup();
    }
}

/* ============================================================================
 * About Dialog Implementation
 * ============================================================================ */

static void render_about_dialog(void) {
    /* Center the modal */
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(350, 180), ImGuiCond_FirstUseEver);
    
    char about_title[128];
    snprintf(about_title, sizeof(about_title), "%s###about", langGetText(STR_MENU_ABOUT));
    if (ImGui::BeginPopupModal(about_title, &s_show_about, ImGuiWindowFlags_NoResize)) {
        /* Title */
        ImGui::TextUnformatted(langGetText(STR_LV_ABOUT_TITLE));
        {
            /* The game's version, from cmake/bolo_version.cmake: the viewer
               ships with it and has no version of its own. */
            MessageArgs args = {};
            strncpy(args.string1, WINBOLO_DISPLAY_VERSION, sizeof(args.string1) - 1);
            ImGui::TextUnformatted(langGetTextFmt(STR_LV_VERSION_FMT, &args));
        }

        ImGui::Separator();

        /* Copyright */
        ImGui::Text("Copyright 1998-%s John Morrison", WINBOLO_BUILD_YEAR);

        ImGui::Separator();

        /* Licence: the game's about box one-liner */
        ImGui::TextWrapped("%s", langGetText(STR_DLGABOUT_LICENCE_NOTICE));

        ImGui::Separator();

        /* Website link */
        ImGui::TextUnformatted(langGetText(STR_LV_WEBSITE_LBL));
        ImGui::SameLine();
        ImGui::TextLinkOpenURL("www.winbolo.com", "https://www.winbolo.com/");

        ImGui::Separator();

        /* Close button */
        if (ImGui::Button(langGetText(STR_OK), ImVec2(80, 0))) {
            s_show_about = false;
            ImGui::CloseCurrentPopup();
        }
        imguiHandOnHover();

        ImGui::EndPopup();
    }
}

/* ============================================================================
 * Spectator Leave-Confirm Dialog Implementation
 * ============================================================================ */

static void render_spectator_leave_dialog(void) {
    /* Center the modal */
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    /* No p_open close button: the only exits are Yes/No (or Esc/click-away,
     * which fall through to the else and clear the flag = keep watching).
     * The "###spec_leave" suffix is an ImGui window-id tag and stays
     * verbatim; only the visible text before it is localized. */
    char title[128];
    snprintf(title, sizeof(title), "%s###spec_leave",
             langGetText(STR_LV_SPEC_LEAVE_CONFIRM));
    if (ImGui::BeginPopupModal(title, NULL,
                               ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(langGetText(STR_LV_SPEC_LEAVE_CONFIRM));
        ImGui::Separator();

        if (ImGui::Button(langGetText(STR_YES), ImVec2(80, 0))) {
            s_spec_leave_confirmed = true;
            s_show_spec_leave = false;
            ImGui::CloseCurrentPopup();
        }
        imguiHandOnHover();

        ImGui::SameLine();

        if (ImGui::Button(langGetText(STR_NO), ImVec2(80, 0))) {
            s_show_spec_leave = false;
            ImGui::CloseCurrentPopup();
        }
        imguiHandOnHover();

        ImGui::EndPopup();
    } else {
        /* Popup closed by Esc or click-away without a button press: drop the
         * logical flag so a later request re-opens it. Treated as cancel. */
        s_show_spec_leave = false;
    }
}
