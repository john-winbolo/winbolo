/*
 * imgui_dialogs.cpp - ImGui modal dialogs for Log Viewer
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This implements ImGui-based modal dialogs to replace Win32 dialogs:
 * - Team Colours dialog: Assign colours to player teams
 * - About dialog: Version and copyright information
 */

#include "imgui_dialogs.h"
#include "imgui.h"
#include "platform_config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

/* Headers for opening URLs */
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <SDL3/SDL.h>
#endif

/* External functions from main.c/backend */
extern "C" {
    /* Team colours array - defined in main.c */
    extern unsigned char tc[17];

    /* Version info - from backend or resource */
    extern const char* g_version_string;
}

/* Constants */
#define MAX_PLAYERS 16

/* Colour names for team colours dialog */
static const char* colour_names[] = {
    "Grey",
    "Khaki", 
    "Green",
    "Pink",
    "Yellow",
    "Light Blue",
    "Orange",
    "Light Purple",
    "Aqua",
    "Light Green",
    "Light Grey",
    "Red",
    "Blue",
    "Brown",
    "Light Pink",
    "Pale Green",
    "Purple"
};

#define NUM_COLOURS 17

/* Dialog state */
static bool s_show_team_colours = false;
static bool s_show_about = false;

/* Team Colours dialog state */
static int s_team_colours[17] = {0};
static bool s_team_colours_loaded = false;

/* Version information */
static const char* s_version = "1.0.0";
static const char* s_copyright = "Copyright (c) 1998-2026 John Morrison";
static const char* s_website = "https://github.com/winbolo/logviewer";

/* Forward declarations */
static void render_team_colours_dialog(void);
static void render_about_dialog(void);
static void load_team_colours_from_tc(void);
static void save_team_colours_to_tc(void);

/**
 * Initialize the dialogs module.
 */
void imgui_dialogs_init(void) {
    char val[256];

    /* Load team colours from preferences */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        char key[64];
        snprintf(key, sizeof(key), "Team Colour %d", i + 1);
        
        /* Default colours - matches original getDef() function */
        int def_colours[] = {11, 12, 2, 4, 16, 13, 8, 3, 6, 1, 5, 7, 9, 14, 15, 0};
        char def_val[16];
        snprintf(def_val, sizeof(def_val), "%d", def_colours[i]);
        
        platform_config_get_string("LOGVIEWER", key, def_val, val, sizeof(val));
        s_team_colours[i] = atoi(val);
        if (s_team_colours[i] < 0 || s_team_colours[i] >= NUM_COLOURS) {
            s_team_colours[i] = def_colours[i];
        }
    }
    
    /* Neutral colour */
    platform_config_get_string("LOGVIEWER", "Neutral Colour", "10", val, sizeof(val));
    s_team_colours[16] = atoi(val);
    if (s_team_colours[16] < 0 || s_team_colours[16] >= NUM_COLOURS) {
        s_team_colours[16] = 16;
    }
    
    s_team_colours_loaded = true;
}

/**
 * Render all open dialogs.
 */
void imgui_dialogs_render(void) {
    /* Open popups at the start of the frame (outside of menu context) */
    static bool was_team_colours_open = false;
    static bool was_about_open = false;

    /* Open popup on transition from closed to open */
    if (s_show_team_colours && !was_team_colours_open) {
        ImGui::OpenPopup("Team Colours");
    }
    was_team_colours_open = s_show_team_colours;

    if (s_show_about && !was_about_open) {
        ImGui::OpenPopup("About");
    }
    was_about_open = s_show_about;

    /* Render each dialog */
    if (s_show_team_colours) {
        render_team_colours_dialog();
    }
    if (s_show_about) {
        render_about_dialog();
    }
}

/**
 * Save dialog state to preferences.
 */
void imgui_dialogs_save(void) {
    char val[64];

    /* Save team colours */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        char key[64];
        snprintf(key, sizeof(key), "Team Colour %d", i + 1);
        snprintf(val, sizeof(val), "%d", s_team_colours[i]);
        platform_config_set_string("LOGVIEWER", key, val);
    }
    
    /* Neutral colour */
    snprintf(val, sizeof(val), "%d", s_team_colours[16]);
    platform_config_set_string("LOGVIEWER", "Neutral Colour", val);
    
    /* Save to disk */
    platform_config_save();
}

/**
 * Show the Team Colours dialog.
 */
void imgui_show_team_colours_dialog(void) {
    /* Load current colours from tc array */
    load_team_colours_from_tc();
    s_show_team_colours = true;
    /* OpenPopup will be called in render function */
}

/**
 * Show the About dialog.
 */
void imgui_show_about_dialog(void) {
    s_show_about = true;
    /* OpenPopup will be called in render function */
}

/* ============================================================================
 * Team Colours Dialog Implementation
 * ============================================================================ */

static void load_team_colours_from_tc(void) {
    /* Copy from global tc array */
    for (int i = 0; i < 17; i++) {
        s_team_colours[i] = (int)tc[i];
    }
}

static void save_team_colours_to_tc(void) {
    /* Copy to global tc array */
    for (int i = 0; i < 17; i++) {
        tc[i] = (unsigned char)s_team_colours[i];
    }
}

static void render_team_colours_dialog(void) {
    /* Center the modal */
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(350, 400), ImGuiCond_FirstUseEver);
    
    if (ImGui::BeginPopupModal("Team Colours", &s_show_team_colours, 0)) {
        ImGui::Text("Assign colours to each player team:");
        ImGui::Separator();
        
        /* Player team colours */
        for (int i = 0; i < MAX_PLAYERS; i++) {
            ImGui::Text("Player %d:", i + 1);
            ImGui::SameLine(100);
            
            char combo_id[32];
            snprintf(combo_id, sizeof(combo_id), "##team%d", i);
            
            /* Current selection */
            const char* current_colour = colour_names[s_team_colours[i]];
            
            if (ImGui::BeginCombo(combo_id, current_colour)) {
                for (int j = 0; j < NUM_COLOURS; j++) {
                    bool is_selected = (s_team_colours[i] == j);
                    if (ImGui::Selectable(colour_names[j], is_selected)) {
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
        
        /* Neutral colour */
        ImGui::Text("Neutral:");
        ImGui::SameLine(100);
        
        const char* neutral_colour = colour_names[s_team_colours[16]];
        if (ImGui::BeginCombo("##neutral", neutral_colour)) {
            for (int j = 0; j < NUM_COLOURS; j++) {
                bool is_selected = (s_team_colours[16] == j);
                if (ImGui::Selectable(colour_names[j], is_selected)) {
                    s_team_colours[16] = j;
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        
        ImGui::Separator();
        
        /* Buttons */
        if (ImGui::Button("OK", ImVec2(80, 0))) {
            /* Apply changes */
            save_team_colours_to_tc();
            
            /* Save to preferences */
            for (int i = 0; i < MAX_PLAYERS; i++) {
                char key[64];
                char val[16];
                snprintf(key, sizeof(key), "Team Colour %d", i + 1);
                snprintf(val, sizeof(val), "%d", s_team_colours[i]);
                platform_config_set_string("LOGVIEWER", key, val);
            }
            char val[16];
            snprintf(val, sizeof(val), "%d", s_team_colours[16]);
            platform_config_set_string("LOGVIEWER", "Neutral Colour", val);
            platform_config_save();
            
            s_show_team_colours = false;
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::SameLine();
        
        if (ImGui::Button("Cancel", ImVec2(80, 0))) {
            s_show_team_colours = false;
            ImGui::CloseCurrentPopup();
        }
        
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
    
    if (ImGui::BeginPopupModal("About", &s_show_about, ImGuiWindowFlags_NoResize)) {
        /* Title */
        ImGui::Text("WinBolo Log Viewer");
        ImGui::Text("Version: %s", s_version);
        
        ImGui::Separator();
        
        /* Copyright */
        ImGui::TextWrapped("%s", s_copyright);
        
        ImGui::Separator();
        
        /* License */
        ImGui::TextWrapped("This program is free software; you can redistribute it");
        ImGui::TextWrapped("and/or modify it under the terms of the GNU General");
        ImGui::TextWrapped("Public License as published by the Free Software Foundation.");
        
        ImGui::Separator();
        
        /* Website link */
        ImGui::Text("Website:");
        ImGui::SameLine();
        ImGui::TextLink(s_website);
        if (ImGui::IsItemClicked()) {
            /* Open URL in browser - Windows only for now */
            #ifdef _WIN32
            ShellExecuteA(NULL, "open", s_website, NULL, NULL, SW_SHOW);
            #else
            SDL_OpenURL(s_website);
            #endif
        }
        
        ImGui::Separator();
        
        /* Close button */
        if (ImGui::Button("OK", ImVec2(80, 0))) {
            s_show_about = false;
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
}