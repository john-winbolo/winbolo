/*
 * imgui_controls.cpp - ImGui playback controls window for Log Viewer
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "imgui_controls.h"
#include "imgui_main_menu.h"
#include "imgui_context.h"
#include "imgui.h"
#include "platform_config.h"
#include "../../gui/lang.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>

/* External functions from main.c - using C types directly */
extern "C" {
    #include "logviewer.h"
    void lv_windowPlay(void);
    void lv_windowPause(void);
    void lv_windowStop(int corruptLog);
    void lv_windowRewind(void);
    void lv_windowFastForward(void);
    void lv_windowNeedRedraw(void);
    void lv_screenGetTime(char *buffer);
    void lv_updateSpeed(unsigned char speed, int updateSlider);
    void lv_screenGetLogProgress(size_t *currentPos, size_t *totalSize, uint32_t *currentTime, uint32_t *totalTime);
    void lv_screenSeekToPosition(float ratio);
    void lv_clientMutexWaitFor(void);
    void lv_clientMutexRelease(void);
    void lv_drawDirtyScreen(void);
}

static LogViewerState *s_lv = nullptr;

/* Speed labels for display */
static const char* speed_labels[] = {
    "1x", "1.2x", "1.4x", "1.6x", "1.8x", "2x", "2.2x", "2.4x", "Fast"
};

/* Current time string buffer */
static char s_current_time[32] = "00:00";

/* Seek slider state */
static float s_seek_ratio = 0.0f;
static bool s_is_seeking = false;

/* Forward declaration for speed update helper */
static void update_speed_from_slider(int new_speed);

/* Format time from milliseconds into MM:SS */
static void format_time(char *buf, size_t buf_size, unsigned int time_ms) {
    double secs = time_ms / 1000.0;
    double mins = floor(secs / 60.0);
    secs = floor(secs - (mins * 60.0));
    snprintf(buf, buf_size, "%02d:%02d", (int)mins, (int)secs);
}

void lv_imgui_controls_init(struct LogViewerState *lv) {
    s_lv = lv;
    /* Load speed from preferences */
    char val[256];
    lv_platform_config_get_string("LOGVIEWER", "Playback Speed", "1", val, sizeof(val));
    int spd = atoi(val);
    if (spd < 1 || spd > 9) {
        spd = 1;
    }
    lv_updateSpeed((unsigned char)spd, 0);
}

void lv_imgui_controls_update_speed(void) {
    /* Called when speed changes externally */
}

static void update_speed_from_slider(int new_speed) {
    if (new_speed < 1) new_speed = 1;
    if (new_speed > 9) new_speed = 9;
    lv_updateSpeed((unsigned char)new_speed, 0);
}

void lv_imgui_controls_window(void) {
    if (!lv_g_show_controls_window) {
        return;
    }

    {
        ImGuiCond cond = lv_g_reset_window_positions ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        ImVec2 vp = ImGui::GetMainViewport()->Size;
        ImGui::SetNextWindowSize(ImVec2(420, 120), cond);
        ImGui::SetNextWindowPos(ImVec2((vp.x - 420) * 0.5f, vp.y - 120 - 10), cond);
    }

    char ctrl_title[128];
    snprintf(ctrl_title, sizeof(ctrl_title), "%s###controls", langGetText(STR_LV_WIN_CONTROLS));
    if (ImGui::Begin(ctrl_title, &lv_g_show_controls_window, ImGuiWindowFlags_NoCollapse)) {
        /* Reposition window relative to right/bottom edge when viewport is resized */
        float dx, dy;
        if (lv_imgui_context_get_resize_delta(&dx, &dy)) {
            ImVec2 pos = ImGui::GetWindowPos();
            ImVec2 vp_old = ImGui::GetMainViewport()->Size;
            float old_vp_x = vp_old.x - dx;
            float old_vp_y = vp_old.y - dy;
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

        /* Playback buttons row */
        bool controlsDisabled = !s_lv->isLoaded;
        if (controlsDisabled) ImGui::BeginDisabled();

        if (ImGui::Button(langGetText(STR_LV_REW_BTN), ImVec2(60, 0))) {
            lv_windowRewind();
        }
        ImGui::SameLine();

        /* Play/Pause toggle button */
        if (s_lv->playIsPlaying) {
            if (ImGui::Button(langGetText(STR_LV_PAUSE), ImVec2(50, 0))) {
                lv_windowPause();
            }
        } else {
            if (ImGui::Button(langGetText(STR_LV_PLAY_BTN), ImVec2(50, 0))) {
                lv_windowPlay();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_LV_STOP), ImVec2(50, 0))) {
            lv_windowStop(0);
        }

        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_LV_FWD_BTN), ImVec2(60, 0))) {
            lv_windowFastForward();
        }

        if (controlsDisabled) ImGui::EndDisabled();

        /* Speed slider - same line as buttons */
        ImGui::SameLine(0, 15);
        ImGui::TextUnformatted(langGetText(STR_LV_SPEED_LBL));
        ImGui::SameLine();

        int spd = (int)s_lv->speed;
        if (spd < 1) spd = 1;
        if (spd > 9) spd = 9;

        ImGui::PushItemWidth(100);
        if (ImGui::SliderInt("##speed", &spd, 1, 9, speed_labels[spd - 1])) {
            update_speed_from_slider(spd);
        }
        ImGui::PopItemWidth();

        /* Seek slider row */
        if (s_lv->isLoaded) {
            size_t currentPos = 0, totalSize = 0;
            uint32_t currentTime = 0, totalTime = 0;
            lv_screenGetLogProgress(&currentPos, &totalSize, &currentTime, &totalTime);
            (void)currentPos; (void)totalSize;

            /* totalTime comes from a one-shot scan at load, so the bar is
             * stable from the moment the file opens — no more byte-ratio
             * drift while waiting for the first player to join. */
            float displayRatio = 0.0f;
            if (totalTime > 0) {
                displayRatio = (float)currentTime / (float)totalTime;
                if (displayRatio > 1.0f) displayRatio = 1.0f;
            }

            /* Only update slider position when not actively seeking */
            if (!s_is_seeking) {
                s_seek_ratio = displayRatio;
            }

            /* Current time */
            format_time(s_current_time, sizeof(s_current_time), currentTime);

            char total_time_str[32] = "--:--";
            char remaining_str[32] = "--:--";
            if (totalTime > 0) {
                uint32_t remaining = totalTime > currentTime ? totalTime - currentTime : 0;
                format_time(total_time_str, sizeof(total_time_str), totalTime);
                format_time(remaining_str, sizeof(remaining_str), remaining);
            }

            /* Seek slider - full width */
            ImGui::PushItemWidth(-1);

            if (ImGui::SliderFloat("##seek", &s_seek_ratio, 0.0f, 1.0f, "")) {
                s_is_seeking = true;
            }

            /* When user releases the slider, perform the seek */
            if (s_is_seeking && ImGui::IsItemDeactivatedAfterEdit()) {
                s_is_seeking = false;
                unsigned char wasPlaying = s_lv->playIsPlaying;
                if (wasPlaying) {
                    lv_windowPause();
                }
                lv_clientMutexWaitFor();
                lv_drawDirtyScreen();
                lv_screenSeekToPosition(s_seek_ratio);
                lv_drawDirtyScreen();
                lv_clientMutexRelease();
                lv_windowNeedRedraw();
                if (wasPlaying) {
                    lv_windowPlay();
                }
            }

            ImGui::PopItemWidth();

            /* Time display row */
            {
                MessageArgs args = {};
                strncpy(args.string1, s_current_time, sizeof(args.string1) - 1);
                strncpy(args.string2, total_time_str, sizeof(args.string2) - 1);
                ImGui::TextUnformatted(langGetTextFmt(STR_LV_TIME_FMT, &args));
            }
            ImGui::SameLine(0, 15);
            {
                MessageArgs args = {};
                strncpy(args.string1, remaining_str, sizeof(args.string1) - 1);
                ImGui::TextDisabled("%s", langGetTextFmt(STR_LV_TIME_REMAINING, &args));
            }
        } else {
            ImGui::TextUnformatted(langGetText(STR_LV_NO_LOG_LOADED));
        }
    }
    ImGui::End();
}
