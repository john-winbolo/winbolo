/*
 * imgui_game_info.cpp - ImGui game information window for Log Viewer
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "imgui_game_info.h"
#include "imgui_main_menu.h"
#include "imgui_context.h"
#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <ctime>

/* Windows headers for ShellExecute */
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <SDL3/SDL.h>
#endif

/* External functions from backend - using C types directly */
extern "C" {
    unsigned char lv_screenGetNumPlayers(void);
    int32_t lv_screenGetGameTimeLeft(void);
    int32_t lv_screenGetGameStartDelay(void);
    void lv_screenGetMapName(char *buffer);
    void lv_screenGetPlayerName(char *buffer, unsigned char player);
}

/* Game info state */
static char s_map_name[256] = "";
static char s_version[32] = "";
static char s_game_type[32] = "";
static char s_hidden_mines[16] = "";
static char s_computer_tanks[32] = "";
static char s_time_limit[64] = "";
static char s_start_delay[64] = "";
static char s_wbn_key[64] = "";
static char s_start_time[64] = "";
static int s_num_players = 0;

/* Game type enum from backend.h */
enum {
    gameOpen = 0,
    gameTournament = 1,
    gameStrictTournament = 2
};

/* AI type enum */
enum {
    aiNone = 0,
    aiYes = 1,
    aiYesAdvantage = 2,
    aiFull = 3
};

void lv_imgui_game_info_init(void) {
    /* Clear all fields initially */
    lv_imgui_game_info_clear();
}

void lv_imgui_game_info_clear(void) {
    s_map_name[0] = '\0';
    s_version[0] = '\0';
    s_game_type[0] = '\0';
    s_hidden_mines[0] = '\0';
    s_computer_tanks[0] = '\0';
    s_time_limit[0] = '\0';
    s_start_delay[0] = '\0';
    s_wbn_key[0] = '\0';
    s_start_time[0] = '\0';
    s_num_players = 0;
}

/* Called from lv_frontEndSetGameInformation in main.c */
void lv_imgui_game_info_set(int clear, unsigned char versionMajor, unsigned char versionMinor, unsigned char versionRevision,
                         char *mapName, unsigned char gameType, int hiddenMines, unsigned char aiType,
                         int32_t startDelay, int32_t timeLimit, unsigned char *wbnKey, int32_t startTime) {
    if (clear) {
        lv_imgui_game_info_clear();
        return;
    }
    
    /* Version string */
    snprintf(s_version, sizeof(s_version), "%d.%d%d", versionMajor, versionMinor, versionRevision);
    
    /* Map name */
    strncpy(s_map_name, mapName, sizeof(s_map_name) - 1);
    s_map_name[sizeof(s_map_name) - 1] = '\0';
    
    /* Game type */
    switch (gameType) {
        case gameOpen:
            snprintf(s_game_type, sizeof(s_game_type), "Open");
            break;
        case gameTournament:
            snprintf(s_game_type, sizeof(s_game_type), "Tournament");
            break;
        case gameStrictTournament:
            snprintf(s_game_type, sizeof(s_game_type), "Strict Tournament");
            break;
        default:
            snprintf(s_game_type, sizeof(s_game_type), "Unknown");
            break;
    }

    /* Hidden mines */
    snprintf(s_hidden_mines, sizeof(s_hidden_mines), "%s", hiddenMines ? "Yes" : "No");

    /* Computer tanks */
    switch (aiType) {
        case aiNone:
            snprintf(s_computer_tanks, sizeof(s_computer_tanks), "No");
            break;
        case aiYes:
            snprintf(s_computer_tanks, sizeof(s_computer_tanks), "Yes");
            break;
        case aiYesAdvantage:
            snprintf(s_computer_tanks, sizeof(s_computer_tanks), "Yes (Advantage)");
            break;
        case aiFull:
            snprintf(s_computer_tanks, sizeof(s_computer_tanks), "Full Advantage");
            break;
        default:
            snprintf(s_computer_tanks, sizeof(s_computer_tanks), "Unknown");
            break;
    }

    /* Time limit */
    if (timeLimit == -1) { /* UNLIMITED_GAME_TIME */
        snprintf(s_time_limit, sizeof(s_time_limit), "Unlimited");
    } else {
        /* Convert ticks to minutes */
        int32_t minutes = timeLimit / 60 / 50; /* 50 ticks per second */
        snprintf(s_time_limit, sizeof(s_time_limit), "About %d minutes", (int)(minutes + 1));
    }

    /* Start delay */
    if (startDelay > 0) {
        int32_t seconds = startDelay / 50; /* 50 ticks per second */
        snprintf(s_start_delay, sizeof(s_start_delay), "%d second%s", (int)seconds, seconds > 1 ? "s" : "");
    } else {
        s_start_delay[0] = '\0';
    }
    
    /* WBN key */
    if (wbnKey && wbnKey[0] != '\0') {
        strncpy(s_wbn_key, (char*)wbnKey, 32);
        s_wbn_key[32] = '\0';
    } else {
        s_wbn_key[0] = '\0';
    }
    
    /* Start time */
    if (startTime > 0) {
        time_t t = startTime;
        struct tm tm_buf;
#ifdef _WIN32
        if (gmtime_s(&tm_buf, &t) == 0) {
            strftime(s_start_time, sizeof(s_start_time), "%a %b %d %H:%M:%S %Y GMT", &tm_buf);
        }
#else
        if (gmtime_r(&t, &tm_buf) != NULL) {
            strftime(s_start_time, sizeof(s_start_time), "%a %b %d %H:%M:%S %Y GMT", &tm_buf);
        }
#endif
    } else {
        s_start_time[0] = '\0';
    }
    
    s_num_players = 0;
}

/* Update dynamic fields (called periodically) */
void imgui_game_info_update(void) {
    s_num_players = lv_screenGetNumPlayers();
    
    int32_t timeLeft = lv_screenGetGameTimeLeft();
    if (timeLeft != -1) { /* UNLIMITED_GAME_TIME */
        int32_t minutes = timeLeft / 60 / 50;
        snprintf(s_time_limit, sizeof(s_time_limit), "About %d minutes", (int)(minutes + 1));
    }

    int32_t startDelay = lv_screenGetGameStartDelay();
    if (startDelay > 0) {
        int32_t seconds = startDelay / 50;
        snprintf(s_start_delay, sizeof(s_start_delay), "%d second%s", (int)seconds, seconds > 1 ? "s" : "");
    } else {
        s_start_delay[0] = '\0';
    }
}

void lv_imgui_game_info_window(void) {
    if (!lv_g_show_game_info_window) {
        return;
    }
    
    {
        ImGuiCond cond = lv_g_reset_window_positions ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        ImVec2 vp = ImGui::GetMainViewport()->Size;
        ImGui::SetNextWindowSize(ImVec2(280, 180), cond);
        ImGui::SetNextWindowPos(ImVec2(vp.x - 280 - 10, 30), cond);
    }

    if (ImGui::Begin("Game Information", &lv_g_show_game_info_window, ImGuiWindowFlags_NoCollapse)) {
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

        /* Static fields */
        ImGui::Text("Map Name: %s", s_map_name);
        ImGui::Text("Version: %s", s_version);
        ImGui::Text("Game Type: %s", s_game_type);
        ImGui::Text("Hidden Mines: %s", s_hidden_mines);
        ImGui::Text("Computer Tanks: %s", s_computer_tanks);
        
        /* Dynamic fields - update periodically */
        imgui_game_info_update();
        
        ImGui::Text("Time Limit: %s", s_time_limit);
        ImGui::Text("Start Delay: %s", s_start_delay);
        ImGui::Text("Players: %d", s_num_players);
        
        /* WBN key - clickable link */
        if (s_wbn_key[0] != '\0' && strlen(s_wbn_key) == 32) {
            ImGui::Text("WBN Key:");
            ImGui::SameLine();
            ImGui::TextLink(s_wbn_key);
            if (ImGui::IsItemClicked()) {
                /* Open URL in browser */
                char url[128];
                snprintf(url, sizeof(url), "http://www.winbolo.net/gamelog.php?key=%s", s_wbn_key);
#ifdef _WIN32
                ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOW);
#else
                SDL_OpenURL(url);
#endif
            }
        }
        
        ImGui::Text("Start Time: %s", s_start_time);
    }
    ImGui::End();
}