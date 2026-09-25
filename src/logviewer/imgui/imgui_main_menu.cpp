/*
 * imgui_main_menu.cpp - ImGui main menu bar for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "imgui_main_menu.h"
#include "imgui_dialogs.h"
#include "imgui.h"
#include "platform_config.h"
#include "../../gui/lang.h"
#include "../../gui/sdl3/dialogs/imgui_wbn_browser.h"
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

/* External functions from main.c - using C types directly */
extern "C" {
    #include "logviewer.h"
    #include "../backend.h"
    #include "../draw.h"
    #include "../../winbolonet/http.h"
    void lv_windowOpenFile(char *cmdLine);
    void lv_windowSaveMap(void);
    void lv_windowPlay(void);
    void lv_windowPause(void);
    void lv_windowStop(int corruptLog);
    void lv_windowRewind(void);
    void lv_windowFastForward(void);
    void lv_windowNeedRedraw(void);
    void lv_screenTankCentred(int enabled);
    int lv_dnsSetEnabled(int enabled);
    void lv_imgui_events_clear(void);
    void lv_soundSetVolume(int pct);
}

static LogViewerState *s_lv = nullptr;

/* Open the WinBolo.net log browser as a blocking modal. The dialog creates
 * its own ImGui context, takes over the window, and returns the user's
 * choice. We save/restore LogViewer's ImGui context, window size+title,
 * and re-init HTTP (the dialog destroys it on close). Exposed (C-linkage)
 * so the compact-mode popup menu can reuse the same flow. */
static void open_wbn_browser_modal(void);
extern "C" void lv_imgui_open_wbn_browser(void) { open_wbn_browser_modal(); }
static void open_wbn_browser_modal(void) {
    if (!s_lv || !s_lv->window || !s_lv->renderer) return;

    ImGuiContext *saved_ctx = ImGui::GetCurrentContext();

    int saved_w = 0, saved_h = 0;
    SDL_GetWindowSize(s_lv->window, &saved_w, &saved_h);
    char saved_title[256];
    const char *cur_title = SDL_GetWindowTitle(s_lv->window);
    SDL_strlcpy(saved_title, cur_title ? cur_title : "", sizeof(saved_title));

    WbnBrowserResult res = imguiWbnBrowserShow(s_lv->window, s_lv->renderer);

    ImGui::SetCurrentContext(saved_ctx);
    SDL_SetWindowSize(s_lv->window, saved_w, saved_h);
    SDL_SetWindowTitle(s_lv->window, saved_title);
    httpCreate();   /* dialog destroyed it on the way out */

    switch (res.action) {
    case WBN_BROWSER_PLAY_FILE:
        lv_windowOpenFile(res.filePath);
        break;
    case WBN_BROWSER_PLAY_MEMORY:
        lv_windowStop(0);
        if (lv_screenLoadMapFromMemory(res.memoryData, res.memorySize)) {
            s_lv->isLoaded = TRUE;
            lv_imgui_events_clear();
            lv_windowNeedRedraw();
        }
        /* memoryData ownership transferred — don't free */
        break;
    case WBN_BROWSER_OPEN_LOCAL:
        lv_windowOpenFile(NULL);
        break;
    case WBN_BROWSER_CLOSE:
    default:
        break;
    }
}

/* Window visibility state */
bool lv_g_show_controls_window = true;
bool lv_g_show_game_info_window = true;
bool lv_g_show_events_window = true;
bool lv_g_show_item_info_window = true;
bool lv_g_show_comments_window = false;  /* Opt-in: WBN comments are noisy if you don't want them */
/* The scenario panel only draws while a recording has a list for it, so a
   plain recording never shows it whatever this says. */
bool lv_g_show_scenario_panel_window = true;
bool lv_g_reset_window_positions = false;

/* Options state */
static bool s_tank_centred = false;
static bool s_dns_lookups = false;
static bool s_mode_information = true;  /* true = Information, false = Select Team */

/* Forward declarations for dialog callbacks (Phase 3) */
static void show_team_colours_dialog(void);
static void show_about_dialog(void);

void lv_imgui_main_menu_init(struct LogViewerState *lv) {
    s_lv = lv;
    /* Load preferences from config */
    char val[256];
    
    lv_platform_config_get_string("LOGVIEWER", "Tank Centred", "No", val, sizeof(val));
    s_tank_centred = (val[0] == 'Y' || val[0] == 'y');

    /* screen.c owns the flag; the menu only pushes the stored value in. */
    lv_platform_config_get_string("LOGVIEWER", "Hide Lobby", "Yes", val, sizeof(val));
    lv_screenSetHideLobby((val[0] == 'Y' || val[0] == 'y') ? 1 : 0);

    lv_platform_config_get_string("LOGVIEWER", "DNS Lookups", "No", val, sizeof(val));
    s_dns_lookups = (val[0] == 'Y' || val[0] == 'y');
    
    lv_platform_config_get_string("LOGVIEWER", "Mode", "Information", val, sizeof(val));
    s_mode_information = (val[0] == '\0' || val[0] == 'I' || val[0] == 'i');
    
    lv_platform_config_get_string("LOGVIEWER", "Window.Controls.Visible", "Yes", val, sizeof(val));
    lv_g_show_controls_window = (val[0] == 'Y' || val[0] == 'y');
    
    lv_platform_config_get_string("LOGVIEWER", "Window.Events.Visible", "Yes", val, sizeof(val));
    lv_g_show_events_window = (val[0] == 'Y' || val[0] == 'y');
    
    lv_platform_config_get_string("LOGVIEWER", "Window.GameInformation.Visible", "Yes", val, sizeof(val));
    lv_g_show_game_info_window = (val[0] == 'Y' || val[0] == 'y');
    
    lv_platform_config_get_string("LOGVIEWER", "Window.ItemInformation.Visible", "Yes", val, sizeof(val));
    lv_g_show_item_info_window = (val[0] == 'Y' || val[0] == 'y');

    lv_platform_config_get_string("LOGVIEWER", "Window.Comments.Visible", "No", val, sizeof(val));
    lv_g_show_comments_window = (val[0] == 'Y' || val[0] == 'y');

    lv_platform_config_get_string("LOGVIEWER", "Window.ScenarioPanel.Visible", "Yes", val, sizeof(val));
    lv_g_show_scenario_panel_window = (val[0] == 'Y' || val[0] == 'y');
}

void lv_imgui_main_menu_save(void) {
    lv_platform_config_set_string("LOGVIEWER", "Tank Centred", s_tank_centred ? "Yes" : "No");
    lv_platform_config_set_string("LOGVIEWER", "Hide Lobby",
                                  lv_screenGetHideLobby() ? "Yes" : "No");
    lv_platform_config_set_string("LOGVIEWER", "DNS Lookups", s_dns_lookups ? "Yes" : "No");
    lv_platform_config_set_string("LOGVIEWER", "Mode", s_mode_information ? "Information" : "Select Teams");
    lv_platform_config_set_string("LOGVIEWER", "Window.Controls.Visible", lv_g_show_controls_window ? "Yes" : "No");
    lv_platform_config_set_string("LOGVIEWER", "Window.Events.Visible", lv_g_show_events_window ? "Yes" : "No");
    lv_platform_config_set_string("LOGVIEWER", "Window.GameInformation.Visible", lv_g_show_game_info_window ? "Yes" : "No");
    lv_platform_config_set_string("LOGVIEWER", "Window.ItemInformation.Visible", lv_g_show_item_info_window ? "Yes" : "No");
    lv_platform_config_set_string("LOGVIEWER", "Window.Comments.Visible", lv_g_show_comments_window ? "Yes" : "No");
    lv_platform_config_set_string("LOGVIEWER", "Window.ScenarioPanel.Visible", lv_g_show_scenario_panel_window ? "Yes" : "No");
}

static void zoom_at_center(int stepIndex) {
    int w = 0, h = 0;
    if (s_lv && s_lv->window) SDL_GetWindowSize(s_lv->window, &w, &h);
    lv_drawSetZoomStep(stepIndex, w / 2, h / 2);
}

void lv_imgui_zoom_at_center(int stepIndex) { zoom_at_center(stepIndex); }

void lv_imgui_set_mode_information(int isInformation) {
    s_mode_information = isInformation ? true : false;
}

int lv_imgui_get_tank_centred(void) { return s_tank_centred ? 1 : 0; }

void lv_imgui_toggle_tank_centred(void) {
    s_tank_centred = !s_tank_centred;
    lv_screenTankCentred(s_tank_centred ? 1 : 0);
}

/* No cached copy: screen.c holds the flag, and enabling it can move the
 * playhead, so the menu always reads the live value back. */
void lv_imgui_toggle_hide_lobby(void) {
    lv_screenSetHideLobby(lv_screenGetHideLobby() ? 0 : 1);
}

int lv_imgui_get_dns_lookups(void) { return s_dns_lookups ? 1 : 0; }

void lv_imgui_toggle_dns_lookups(void) {
    s_dns_lookups = !s_dns_lookups;
    if (s_dns_lookups) {
        s_dns_lookups = lv_dnsSetEnabled(1) ? true : false;
    } else {
        lv_dnsSetEnabled(0);
    }
}

static void handle_keyboard_shortcuts(int *clicked) {
    /* Skip shortcuts when ImGui wants the keyboard (e.g., text input focused) */
    if (ImGui::GetIO().WantCaptureKeyboard) return;

    ImGuiIO& io = ImGui::GetIO();

    /* Zoom shortcuts: +/= to zoom in, - to zoom out (no modifier required) */
    if (!io.KeyCtrl && !io.KeyAlt) {
        if (ImGui::IsKeyPressed(ImGuiKey_Equal, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false)) {
            zoom_at_center(lv_drawGetZoomStepIndex() + 1);
            *clicked = 1;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false)) {
            zoom_at_center(lv_drawGetZoomStepIndex() - 1);
            *clicked = 1;
        }
    }

    bool ctrl = io.KeyCtrl;
    if (!ctrl) return;

    /* File */
    if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        lv_windowOpenFile(NULL);
        *clicked = 1;
    }

    /* Action */
    if (ImGui::IsKeyPressed(ImGuiKey_P, false)) {
        if (s_lv->isLoaded && !s_lv->playIsPlaying) { lv_windowPlay(); *clicked = 1; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_U, false)) {
        if (s_lv->isLoaded && s_lv->playIsPlaying) { lv_windowPause(); *clicked = 1; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        if (s_lv->isLoaded) { lv_windowStop(0); *clicked = 1; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        if (s_lv->isLoaded) { lv_windowFastForward(); *clicked = 1; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        if (s_lv->isLoaded) { lv_windowRewind(); *clicked = 1; }
    }

    /* Options */
    if (ImGui::IsKeyPressed(ImGuiKey_T, false)) {
        s_tank_centred = !s_tank_centred;
        lv_screenTankCentred(s_tank_centred ? 1 : 0);
        *clicked = 1;
    }

    /* Windows */
    if (ImGui::IsKeyPressed(ImGuiKey_1, false)) {
        lv_g_show_controls_window = !lv_g_show_controls_window;
        *clicked = 1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_2, false)) {
        lv_g_show_events_window = !lv_g_show_events_window;
        *clicked = 1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_3, false)) {
        lv_g_show_game_info_window = !lv_g_show_game_info_window;
        *clicked = 1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_4, false)) {
        lv_g_show_item_info_window = !lv_g_show_item_info_window;
        *clicked = 1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_5, false)) {
        lv_g_show_comments_window = !lv_g_show_comments_window;
        *clicked = 1;
    }
}

int lv_imgui_main_menu_bar(void) {
    int clicked = 0;

    handle_keyboard_shortcuts(&clicked);

#ifndef __APPLE__
    if (ImGui::BeginMainMenuBar()) {
        /* File Menu */
        if (ImGui::BeginMenu(langGetText(STR_MENU_FILE))) {
            if (ImGui::MenuItem(langGetText(STR_LV_MENU_OPEN_WBN))) {
                lv_imgui_open_wbn_browser();
                clicked = 1;
            }
            if (ImGui::MenuItem(langGetText(STR_LV_MENU_OPEN), "Ctrl+O")) {
                lv_windowOpenFile(NULL);
                clicked = 1;
            }
            if (ImGui::MenuItem(langGetText(STR_MENU_SAVE_MAP), NULL, false, s_lv->isLoaded != 0)) {
                lv_windowSaveMap();
                clicked = 1;
            }
            ImGui::Separator();
            if (s_lv->fromMainMenu) {
                if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_RETURN))) {
                    SDL_Event quit_event;
                    SDL_zero(quit_event);
                    quit_event.type = SDL_EVENT_QUIT;
                    SDL_PushEvent(&quit_event);
                    clicked = 1;
                }
            } else {
                if (ImGui::MenuItem(langGetText(STR_MENU_EXIT))) {
                    logViewerRequestAppQuit();
                    clicked = 1;
                }
            }
            ImGui::EndMenu();
        }

        /* Action Menu */
        if (ImGui::BeginMenu(langGetText(STR_LV_MENU_ACTION))) {
            if (ImGui::MenuItem(langGetText(STR_LV_PLAY), "Ctrl+P", false, s_lv->isLoaded != 0 && !s_lv->playIsPlaying)) {
                lv_windowPlay();
                clicked = 1;
            }
            if (ImGui::MenuItem(langGetText(STR_LV_PAUSE), "Ctrl+U", false, s_lv->isLoaded != 0 && s_lv->playIsPlaying)) {
                lv_windowPause();
                clicked = 1;
            }
            if (ImGui::MenuItem(langGetText(STR_LV_STOP), "Ctrl+S", false, s_lv->isLoaded != 0)) {
                lv_windowStop(0);
                clicked = 1;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(langGetText(STR_LV_FAST_FORWARD), "Ctrl+F", false, s_lv->isLoaded != 0)) {
                lv_windowFastForward();
                clicked = 1;
            }
            if (ImGui::MenuItem(langGetText(STR_LV_REWIND), "Ctrl+R", false, s_lv->isLoaded != 0)) {
                lv_windowRewind();
                clicked = 1;
            }
            ImGui::EndMenu();
        }

        /* Options Menu */
        if (ImGui::BeginMenu(langGetText(STR_MAPEDIT_MENU_OPTIONS))) {
            /* Zoom submenu */
            if (ImGui::BeginMenu(langGetText(STR_LV_ZOOM))) {
                int curStep = lv_drawGetZoomStepIndex();
                int stepCount = lv_drawGetZoomStepCount();
                for (int i = 0; i < stepCount; i++) {
                    float val = lv_drawGetZoomStepValue(i);
                    char label[32];
                    if (val == (float)(int)val) {
                        snprintf(label, sizeof(label), "%dx", (int)val);
                    } else {
                        snprintf(label, sizeof(label), "%.1fx", val);
                    }
                    if (ImGui::MenuItem(label, NULL, i == curStep)) {
                        zoom_at_center(i);
                        clicked = 1;
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem(langGetText(STR_LV_ZOOM_IN), "+", false, curStep < stepCount - 1)) {
                    zoom_at_center(curStep + 1);
                    clicked = 1;
                }
                if (ImGui::MenuItem(langGetText(STR_LV_ZOOM_OUT), "-", false, curStep > 0)) {
                    zoom_at_center(curStep - 1);
                    clicked = 1;
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            /* Mode submenu */
            if (ImGui::BeginMenu(langGetText(STR_LV_MODE))) {
                if (ImGui::MenuItem(langGetText(STR_LV_MODE_INFO), "Ctrl+I", s_mode_information)) {
                    s_mode_information = true;
                    clicked = 1;
                }
                if (ImGui::MenuItem(langGetText(STR_LV_SELECT_TEAM), "Ctrl+C", !s_mode_information)) {
                    s_mode_information = false;
                    clicked = 1;
                }
                ImGui::EndMenu();
            }

            if (ImGui::MenuItem(langGetText(STR_LV_USE_TEAM_COLOURS), NULL,
                                s_lv->useTeamColours != 0,
                                !s_lv->gameView)) {
                s_lv->useTeamColours = s_lv->useTeamColours ? 0 : 1;
                clicked = 1;
            }
            if (ImGui::MenuItem(langGetText(STR_LV_TANK_CENTRED), "Ctrl+T", s_tank_centred)) {
                s_tank_centred = !s_tank_centred;
                lv_screenTankCentred(s_tank_centred ? 1 : 0);
                clicked = 1;
            }
            if (ImGui::MenuItem(langGetText(STR_LV_HIDE_LOBBY), NULL,
                                lv_screenGetHideLobby() != 0)) {
                lv_imgui_toggle_hide_lobby();
                clicked = 1;
            }
            if (ImGui::MenuItem(langGetText(STR_MENU_SOUND_EFFECTS), NULL, s_lv->isSoundsPlaying != 0)) {
                s_lv->isSoundsPlaying = s_lv->isSoundsPlaying ? 0 : 1;
                clicked = 1;
            }
            {
                int vol = s_lv->soundVolume;
                const float sliderW = 160.0f;
                ImGui::TextUnformatted(langGetText(STR_MENU_VOLUME));
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - sliderW);
                ImGui::SetNextItemWidth(sliderW);
                if (ImGui::SliderInt("##volume", &vol, 0, 100, "%d%%")) {
                    if (vol < 0) vol = 0;
                    if (vol > 100) vol = 100;
                    s_lv->soundVolume = vol;
                    lv_soundSetVolume(vol);
                    clicked = 1;
                }
            }
            if (ImGui::MenuItem(langGetText(STR_LV_DNS_LOOKUPS), NULL, s_dns_lookups)) {
                s_dns_lookups = !s_dns_lookups;
                if (s_dns_lookups) {
                    s_dns_lookups = lv_dnsSetEnabled(1) ? true : false;
                } else {
                    lv_dnsSetEnabled(0);
                }
                clicked = 1;
            }

            ImGui::Separator();

            if (ImGui::MenuItem(langGetText(STR_LV_TEAM_COLOURS))) {
                show_team_colours_dialog();
                clicked = 1;
            }

            ImGui::EndMenu();
        }

        /* Windows Menu */
        if (ImGui::BeginMenu(langGetText(STR_LV_MENU_WINDOWS))) {
            if (ImGui::MenuItem(langGetText(STR_LV_WIN_CONTROLS), "Ctrl+1", lv_g_show_controls_window)) {
                lv_g_show_controls_window = !lv_g_show_controls_window;
                clicked = 1;
            }
            if (ImGui::MenuItem(langGetText(STR_LV_WIN_EVENTS), "Ctrl+2", lv_g_show_events_window)) {
                lv_g_show_events_window = !lv_g_show_events_window;
                clicked = 1;
            }
            if (ImGui::MenuItem(langGetText(STR_LV_WIN_GAMEINFO), "Ctrl+3", lv_g_show_game_info_window)) {
                lv_g_show_game_info_window = !lv_g_show_game_info_window;
                clicked = 1;
            }
            if (ImGui::MenuItem(langGetText(STR_LV_WIN_ITEMINFO), "Ctrl+4", lv_g_show_item_info_window)) {
                lv_g_show_item_info_window = !lv_g_show_item_info_window;
                clicked = 1;
            }
            if (ImGui::MenuItem(langGetText(STR_LV_WIN_COMMENTS), "Ctrl+5", lv_g_show_comments_window)) {
                lv_g_show_comments_window = !lv_g_show_comments_window;
                clicked = 1;
            }
            if (ImGui::MenuItem(langGetText(STR_SCNPANEL_SETTINGS_TITLE), NULL, lv_g_show_scenario_panel_window)) {
                lv_g_show_scenario_panel_window = !lv_g_show_scenario_panel_window;
                clicked = 1;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(langGetText(STR_LV_RESET_WINDOWS))) {
                lv_g_reset_window_positions = true;
                clicked = 1;
            }
            ImGui::EndMenu();
        }

        /* Help Menu */
        if (ImGui::BeginMenu(langGetText(STR_MENU_HELP))) {
            if (ImGui::MenuItem(langGetText(STR_MENU_HELP))) {
                /* Open help file - Phase 3 */
                clicked = 1;
            }
            if (ImGui::MenuItem(langGetText(STR_MENU_ABOUT))) {
                show_about_dialog();
                clicked = 1;
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
#endif /* __APPLE__ */

    return clicked;
}

float lv_imgui_get_menu_bar_height(void) {
#ifdef __APPLE__
    /* macOS uses a native NSMenu — the in-window strip is suppressed and
     * the viewport reclaims those pixels. */
    return 0.0f;
#else
    /* Return the height of the main menu bar.
     * ImGui stores this after BeginMainMenuBar() is called.
     *
     * IMPORTANT: This function is called from lv_drawMainScreen() BEFORE
     * lv_imgui_main_menu_bar() renders the menu. We need to return a cached
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
#endif /* __APPLE__ */
}

void lv_imgui_cache_menu_bar_height(void) {
    /* Call this after rendering the menu bar to cache the actual height.
     * This ensures the next frame has an accurate value. */
    ImGuiStyle& style = ImGui::GetStyle();
    float frame_height = ImGui::GetFrameHeight();
    float calculated_height = frame_height + style.FramePadding.y * 2.0f;
    (void)calculated_height;  /* Suppress unused variable warning */
}

int lv_imgui_get_mode_information(void) {
    /* Return the current mode state.
     * Returns 1 if Information mode, 0 if Select Team mode. */
    return s_mode_information ? 1 : 0;
}

/* Dialog functions are now implemented in imgui_dialogs.cpp */
static void show_team_colours_dialog(void) {
    lv_imgui_show_team_colours_dialog();
}

static void show_about_dialog(void) {
    lv_imgui_show_about_dialog();
}