/*
 * imgui_main_menu.cpp - ImGui main menu bar for Log Viewer
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "imgui_main_menu.h"
#include "imgui_dialogs.h"
#include "imgui.h"
#include "platform_config.h"
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

/* External functions from main.c - using C types directly */
extern "C" {
    void windowOpenFile(char *cmdLine);
    void windowSaveMap(void);
    void windowPlay(void);
    void windowPause(void);
    void windowStop(int corruptLog);
    void windowRewind(void);
    void windowFastForward(void);
    void screenTankCentred(int enabled);
    int dnsSetEnabled(int enabled);
    
    /* External state variables from main.c */
    /* Note: bool in global.h is BYTE (unsigned char), not C++ bool */
    extern unsigned char playIsPlaying;
    extern unsigned char isLoaded;
    extern unsigned char isSoundsPlaying;
    extern unsigned char useTeamColours;
    extern unsigned char doubleSpeed;
    extern unsigned char speed;
    
    /* Team colours array */
    extern unsigned char tc[17];
}

/* Window visibility state */
bool g_show_controls_window = true;
bool g_show_game_info_window = true;
bool g_show_events_window = true;
bool g_show_item_info_window = true;

/* Options state */
static bool s_tank_centred = false;
static bool s_dns_lookups = false;
static bool s_mode_information = true;  /* true = Information, false = Select Team */

/* Forward declarations for dialog callbacks (Phase 3) */
static void show_team_colours_dialog(void);
static void show_about_dialog(void);

void imgui_main_menu_init(void) {
    /* Load preferences from config */
    char val[256];
    
    platform_config_get_string("LOGVIEWER", "Tank Centred", "No", val, sizeof(val));
    s_tank_centred = (val[0] == 'Y' || val[0] == 'y');
    
    platform_config_get_string("LOGVIEWER", "DNS Lookups", "No", val, sizeof(val));
    s_dns_lookups = (val[0] == 'Y' || val[0] == 'y');
    
    platform_config_get_string("LOGVIEWER", "Mode", "Information", val, sizeof(val));
    s_mode_information = (val[0] == '\0' || val[0] == 'I' || val[0] == 'i');
    
    platform_config_get_string("LOGVIEWER", "Window.Controls.Visible", "Yes", val, sizeof(val));
    g_show_controls_window = (val[0] == 'Y' || val[0] == 'y');
    
    platform_config_get_string("LOGVIEWER", "Window.Events.Visible", "Yes", val, sizeof(val));
    g_show_events_window = (val[0] == 'Y' || val[0] == 'y');
    
    platform_config_get_string("LOGVIEWER", "Window.GameInformation.Visible", "Yes", val, sizeof(val));
    g_show_game_info_window = (val[0] == 'Y' || val[0] == 'y');
    
    platform_config_get_string("LOGVIEWER", "Window.ItemInformation.Visible", "Yes", val, sizeof(val));
    g_show_item_info_window = (val[0] == 'Y' || val[0] == 'y');
}

void imgui_main_menu_save(void) {
    platform_config_set_string("LOGVIEWER", "Tank Centred", s_tank_centred ? "Yes" : "No");
    platform_config_set_string("LOGVIEWER", "DNS Lookups", s_dns_lookups ? "Yes" : "No");
    platform_config_set_string("LOGVIEWER", "Mode", s_mode_information ? "Information" : "Select Teams");
    platform_config_set_string("LOGVIEWER", "Window.Controls.Visible", g_show_controls_window ? "Yes" : "No");
    platform_config_set_string("LOGVIEWER", "Window.Events.Visible", g_show_events_window ? "Yes" : "No");
    platform_config_set_string("LOGVIEWER", "Window.GameInformation.Visible", g_show_game_info_window ? "Yes" : "No");
    platform_config_set_string("LOGVIEWER", "Window.ItemInformation.Visible", g_show_item_info_window ? "Yes" : "No");
}

static void handle_keyboard_shortcuts(int *clicked) {
    /* Skip shortcuts when ImGui wants the keyboard (e.g., text input focused) */
    if (ImGui::GetIO().WantCaptureKeyboard) return;

    ImGuiIO& io = ImGui::GetIO();
    bool ctrl = io.KeyCtrl;
    if (!ctrl) return;

    /* File */
    if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        windowOpenFile(NULL);
        *clicked = 1;
    }

    /* Action */
    if (ImGui::IsKeyPressed(ImGuiKey_P, false)) {
        if (isLoaded && !playIsPlaying) { windowPlay(); *clicked = 1; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_U, false)) {
        if (isLoaded && playIsPlaying) { windowPause(); *clicked = 1; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        if (isLoaded) { windowStop(0); *clicked = 1; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        if (isLoaded) { windowFastForward(); *clicked = 1; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        if (isLoaded) { windowRewind(); *clicked = 1; }
    }

    /* Options */
    if (ImGui::IsKeyPressed(ImGuiKey_T, false)) {
        s_tank_centred = !s_tank_centred;
        screenTankCentred(s_tank_centred ? 1 : 0);
        *clicked = 1;
    }

    /* Windows */
    if (ImGui::IsKeyPressed(ImGuiKey_1, false)) {
        g_show_controls_window = !g_show_controls_window;
        *clicked = 1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_2, false)) {
        g_show_events_window = !g_show_events_window;
        *clicked = 1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_3, false)) {
        g_show_game_info_window = !g_show_game_info_window;
        *clicked = 1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_4, false)) {
        g_show_item_info_window = !g_show_item_info_window;
        *clicked = 1;
    }
}

int imgui_main_menu_bar(void) {
    int clicked = 0;

    handle_keyboard_shortcuts(&clicked);

    if (ImGui::BeginMainMenuBar()) {
        /* File Menu */
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open", "Ctrl+O")) {
                windowOpenFile(NULL);
                clicked = 1;
            }
            if (ImGui::MenuItem("Save Map", NULL, false, isLoaded != 0)) {
                windowSaveMap();
                clicked = 1;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                SDL_Event quit_event;
                SDL_zero(quit_event);
                quit_event.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit_event);
                clicked = 1;
            }
            ImGui::EndMenu();
        }
        
        /* Action Menu */
        if (ImGui::BeginMenu("Action")) {
            if (ImGui::MenuItem("Play", "Ctrl+P", false, isLoaded != 0 && !playIsPlaying)) {
                windowPlay();
                clicked = 1;
            }
            if (ImGui::MenuItem("Pause", "Ctrl+U", false, isLoaded != 0 && playIsPlaying)) {
                windowPause();
                clicked = 1;
            }
            if (ImGui::MenuItem("Stop", "Ctrl+S", false, isLoaded != 0)) {
                windowStop(0);
                clicked = 1;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Fast Forward", "Ctrl+F", false, isLoaded != 0)) {
                windowFastForward();
                clicked = 1;
            }
            if (ImGui::MenuItem("Rewind", "Ctrl+R", false, isLoaded != 0)) {
                windowRewind();
                clicked = 1;
            }
            ImGui::EndMenu();
        }
        
        /* Options Menu */
        if (ImGui::BeginMenu("Options")) {
            /* Mode submenu */
            if (ImGui::BeginMenu("Mode")) {
                if (ImGui::MenuItem("Information", "Ctrl+I", s_mode_information)) {
                    s_mode_information = true;
                    clicked = 1;
                }
                if (ImGui::MenuItem("Select Team", "Ctrl+C", !s_mode_information)) {
                    s_mode_information = false;
                    clicked = 1;
                }
                ImGui::EndMenu();
            }
            
            /* Use value-based MenuItem to avoid C4739 warning with extern int variables */
            if (ImGui::MenuItem("Use Team Colours", NULL, useTeamColours != 0)) {
                if (useTeamColours != 0) {
                    useTeamColours = 0;
                } else {
                    useTeamColours = 1;
                }
                clicked = 1;
            }
            if (ImGui::MenuItem("Tank Centred", "Ctrl+T", s_tank_centred)) {
                s_tank_centred = !s_tank_centred;
                screenTankCentred(s_tank_centred ? 1 : 0);
                clicked = 1;
            }
            if (ImGui::MenuItem("Sound Effects", NULL, isSoundsPlaying != 0)) {
                if (isSoundsPlaying != 0) {
                    isSoundsPlaying = 0;
                } else {
                    isSoundsPlaying = 1;
                }
                clicked = 1;
            }
            if (ImGui::MenuItem("DNS Lookups", NULL, s_dns_lookups)) {
                s_dns_lookups = !s_dns_lookups;
                if (s_dns_lookups) {
                    s_dns_lookups = dnsSetEnabled(1) ? true : false;
                } else {
                    dnsSetEnabled(0);
                }
                clicked = 1;
            }
            
            ImGui::Separator();
            
            if (ImGui::MenuItem("Team Colours")) {
                show_team_colours_dialog();
                clicked = 1;
            }
            
            ImGui::EndMenu();
        }
        
        /* Windows Menu */
        if (ImGui::BeginMenu("Windows")) {
            if (ImGui::MenuItem("Controls", "Ctrl+1", g_show_controls_window)) {
                g_show_controls_window = !g_show_controls_window;
                clicked = 1;
            }
            if (ImGui::MenuItem("Events", "Ctrl+2", g_show_events_window)) {
                g_show_events_window = !g_show_events_window;
                clicked = 1;
            }
            if (ImGui::MenuItem("Game Information", "Ctrl+3", g_show_game_info_window)) {
                g_show_game_info_window = !g_show_game_info_window;
                clicked = 1;
            }
            if (ImGui::MenuItem("Item Information", "Ctrl+4", g_show_item_info_window)) {
                g_show_item_info_window = !g_show_item_info_window;
                clicked = 1;
            }
            ImGui::EndMenu();
        }
        
        /* Help Menu */
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Help")) {
                /* Open help file - Phase 3 */
                clicked = 1;
            }
            if (ImGui::MenuItem("About")) {
                show_about_dialog();
                clicked = 1;
            }
            ImGui::EndMenu();
        }
        
        ImGui::EndMainMenuBar();
    }
    
    return clicked;
}

float imgui_get_menu_bar_height(void) {
    /* Return the height of the main menu bar.
     * ImGui stores this after BeginMainMenuBar() is called.
     *
     * IMPORTANT: This function is called from drawMainScreen() BEFORE
     * imgui_main_menu_bar() renders the menu. We need to return a cached
     * value from the previous frame, or a reasonable default.
     */
    static float cached_height = 0.0f;
    static bool height_cached = false;
    
    /* Get the actual menu bar height from ImGui's internal state */
    ImGuiStyle& style = ImGui::GetStyle();
    float frame_height = ImGui::GetFrameHeight();
    
    /* The menu bar height is: frame height + top padding + bottom padding
     * (ItemSpacing.y is the gap between menu bar and content below) */
    float calculated_height = frame_height + style.FramePadding.y * 2.0f;
    
    /* If we have a cached value from a previous frame, use it */
    if (height_cached) {
        return cached_height;
    }
    
    /* Otherwise return the calculated height */
    cached_height = calculated_height;
    height_cached = true;
    return calculated_height;
}

void imgui_cache_menu_bar_height(void) {
    /* Call this after rendering the menu bar to cache the actual height.
     * This ensures the next frame has an accurate value. */
    ImGuiStyle& style = ImGui::GetStyle();
    float frame_height = ImGui::GetFrameHeight();
    float calculated_height = frame_height + style.FramePadding.y * 2.0f;
    (void)calculated_height;  /* Suppress unused variable warning */
}

int imgui_get_mode_information(void) {
    /* Return the current mode state.
     * Returns 1 if Information mode, 0 if Select Team mode. */
    return s_mode_information ? 1 : 0;
}

/* Dialog functions are now implemented in imgui_dialogs.cpp */
static void show_team_colours_dialog(void) {
    imgui_show_team_colours_dialog();
}

static void show_about_dialog(void) {
    imgui_show_about_dialog();
}