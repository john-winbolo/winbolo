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
#include "../../gui/lang.h"

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
    
    /* Game type — copy from the localized string table. */
    {
        langid id;
        switch (gameType) {
            case gameOpen:             id = STR_DLGGAMEINFO_OPEN;   break;
            case gameTournament:       id = STR_DLGGAMEINFO_TOURN;  break;
            case gameStrictTournament: id = STR_DLGGAMEINFO_STRICT; break;
            default:                   id = STR_UNKNOWN;            break;
        }
        snprintf(s_game_type, sizeof(s_game_type), "%s", langGetText(id));
    }

    /* Hidden mines */
    snprintf(s_hidden_mines, sizeof(s_hidden_mines), "%s",
             langGetText(hiddenMines ? STR_YES : STR_NO));

    /* Computer tanks */
    {
        langid id;
        switch (aiType) {
            case aiNone:         id = STR_NO;                  break;
            case aiYes:          id = STR_YES;                 break;
            case aiYesAdvantage: id = STR_DLGGAMEINFO_AIADV;   break;
            case aiFull:         id = STR_DLGGAMEINFO_FULLADV; break;
            default:             id = STR_UNKNOWN;             break;
        }
        snprintf(s_computer_tanks, sizeof(s_computer_tanks), "%s", langGetText(id));
    }

    /* Time limit */
    if (timeLimit == -1) { /* UNLIMITED_GAME_TIME */
        snprintf(s_time_limit, sizeof(s_time_limit), "%s",
                 langGetText(STR_DLGGAMEINFO_UNLIMITED));
    } else {
        /* Convert ticks to minutes */
        int32_t minutes = timeLimit / 60 / 50; /* 50 ticks per second */
        MessageArgs args = {};
        args.number = (int)(minutes + 1);
        snprintf(s_time_limit, sizeof(s_time_limit), "%s",
                 langGetTextFmt(STR_DLGGAMEINFO_TIMEREMAINING, &args));
    }

    /* Start delay */
    if (startDelay > 0) {
        int32_t seconds = startDelay / 50; /* 50 ticks per second */
        MessageArgs args = {};
        args.number = (int)seconds;
        snprintf(s_start_delay, sizeof(s_start_delay), "%s",
                 langGetTextFmt(STR_LV_SECONDS, &args));
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
        MessageArgs args = {};
        args.number = (int)(minutes + 1);
        snprintf(s_time_limit, sizeof(s_time_limit), "%s",
                 langGetTextFmt(STR_DLGGAMEINFO_TIMEREMAINING, &args));
    }

    int32_t startDelay = lv_screenGetGameStartDelay();
    if (startDelay > 0) {
        int32_t seconds = startDelay / 50;
        MessageArgs args = {};
        args.number = (int)seconds;
        snprintf(s_start_delay, sizeof(s_start_delay), "%s",
                 langGetTextFmt(STR_LV_SECONDS, &args));
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

    char gi_title[128];
    snprintf(gi_title, sizeof(gi_title), "%s###gameinfo", langGetText(STR_LV_WIN_GAMEINFO));
    if (ImGui::Begin(gi_title, &lv_g_show_game_info_window, ImGuiWindowFlags_NoCollapse)) {
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

        /* Static fields — use a single MessageArgs reused across rows
         * since each ImGui::Text call consumes the rendered string
         * before the next overwrites the format buffer. */
        {
            MessageArgs args = {};
            strncpy(args.string1, s_map_name, sizeof(args.string1) - 1);
            ImGui::TextUnformatted(langGetTextFmt(STR_LV_INFO_MAP, &args));
        }
        {
            MessageArgs args = {};
            strncpy(args.string1, s_version, sizeof(args.string1) - 1);
            ImGui::TextUnformatted(langGetTextFmt(STR_LV_VERSION_FMT, &args));
        }
        {
            MessageArgs args = {};
            strncpy(args.string1, s_game_type, sizeof(args.string1) - 1);
            ImGui::TextUnformatted(langGetTextFmt(STR_LV_INFO_GAMETYPE, &args));
        }
        {
            MessageArgs args = {};
            strncpy(args.string1, s_hidden_mines, sizeof(args.string1) - 1);
            ImGui::TextUnformatted(langGetTextFmt(STR_LV_INFO_HIDDENMINES, &args));
        }
        {
            MessageArgs args = {};
            strncpy(args.string1, s_computer_tanks, sizeof(args.string1) - 1);
            ImGui::TextUnformatted(langGetTextFmt(STR_LV_INFO_COMPUTER_TANKS, &args));
        }

        /* Dynamic fields - update periodically */
        imgui_game_info_update();

        {
            MessageArgs args = {};
            strncpy(args.string1, s_time_limit, sizeof(args.string1) - 1);
            ImGui::TextUnformatted(langGetTextFmt(STR_LV_INFO_TIME_LIMIT, &args));
        }
        {
            MessageArgs args = {};
            strncpy(args.string1, s_start_delay, sizeof(args.string1) - 1);
            ImGui::TextUnformatted(langGetTextFmt(STR_LV_INFO_START_DELAY, &args));
        }
        {
            MessageArgs args = {};
            args.number = s_num_players;
            ImGui::TextUnformatted(langGetTextFmt(STR_DLGGAMEINFO_NUMPLAYERS, &args));
        }

        /* WBN key - clickable link */
        if (s_wbn_key[0] != '\0' && strlen(s_wbn_key) == 32) {
            ImGui::TextUnformatted(langGetText(STR_LV_INFO_WBN_KEY));
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

        {
            MessageArgs args = {};
            strncpy(args.string1, s_start_time, sizeof(args.string1) - 1);
            ImGui::TextUnformatted(langGetTextFmt(STR_LV_INFO_START_TIME, &args));
        }
    }
    ImGui::End();
}