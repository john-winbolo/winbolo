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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>

/* External functions from main.c - using C types directly */
extern "C" {
    void windowPlay(void);
    void windowPause(void);
    void windowStop(int corruptLog);
    void windowRewind(void);
    void windowFastForward(void);
    void windowNeedRedraw(void);
    void screenGetTime(char *buffer);
    void updateSpeed(unsigned char speed, int updateSlider);
    void screenGetLogProgress(size_t *currentPos, size_t *totalSize, uint32_t *currentTime);
    void screenSeekToPosition(float ratio);
    void clientMutexWaitFor(void);
    void clientMutexRelease(void);
    void drawDirtyScreen(void);

    /* External state variables from main.c */
    /* Note: bool in global.h is BYTE (unsigned char), not C++ bool */
    extern unsigned char playIsPlaying;
    extern unsigned char isLoaded;
    extern unsigned char doubleSpeed;
    extern unsigned char speed;
    extern int timerSleep;
}

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

void imgui_controls_init(void) {
    /* Load speed from preferences */
    char val[256];
    platform_config_get_string("LOGVIEWER", "Playback Speed", "1", val, sizeof(val));
    int spd = atoi(val);
    if (spd < 1 || spd > 9) {
        spd = 1;
    }
    speed = (unsigned char)spd;
    updateSpeed(speed, 0);
}

void imgui_controls_update_speed(void) {
    /* Called when speed changes externally */
}

static void update_speed_from_slider(int new_speed) {
    if (new_speed < 1) new_speed = 1;
    if (new_speed > 9) new_speed = 9;

    speed = (unsigned char)new_speed;

    /* Update timerSleep based on speed (matching original logic) */
    switch (speed) {
        case 2:
            timerSleep = 18;
            break;
        case 3:
            timerSleep = 16;
            break;
        case 4:
            timerSleep = 14;
            break;
        case 5:
            timerSleep = 12;
            break;
        case 6:
            timerSleep = 10;
            break;
        case 7:
            timerSleep = 8;
            break;
        case 8:
            timerSleep = 6;
            break;
        case 9:
            timerSleep = 1;
            break;
        case 1:
        default:
            timerSleep = 20;
            speed = 1;
            break;
    }
}

void imgui_controls_window(void) {
    if (!g_show_controls_window) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(420, 120), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Controls", &g_show_controls_window, ImGuiWindowFlags_NoCollapse)) {
        /* Reposition window relative to right/bottom edge when viewport is resized */
        float dx, dy;
        if (imgui_context_get_resize_delta(&dx, &dy)) {
            ImVec2 pos = ImGui::GetWindowPos();
            ImVec2 size = ImGui::GetWindowSize();
            ImVec2 vp = ImGui::GetMainViewport()->Size;
            ImVec2 new_pos = pos;
            if (pos.x + size.x * 0.5f > (vp.x - dx) * 0.5f) new_pos.x = pos.x + dx;
            if (pos.y + size.y * 0.5f > (vp.y - dy) * 0.5f) new_pos.y = pos.y + dy;
            /* Clamp to keep fully on screen */
            if (new_pos.x < 0) new_pos.x = 0;
            if (new_pos.y < 0) new_pos.y = 0;
            if (new_pos.x + size.x > vp.x) new_pos.x = vp.x - size.x;
            if (new_pos.y + size.y > vp.y) new_pos.y = vp.y - size.y;
            if (new_pos.x != pos.x || new_pos.y != pos.y) ImGui::SetWindowPos(new_pos);
        }

        /* Playback buttons row */
        if (!isLoaded) ImGui::BeginDisabled();

        if (ImGui::Button("<< Rew", ImVec2(60, 0))) {
            windowRewind();
        }
        ImGui::SameLine();

        /* Play/Pause toggle button */
        if (playIsPlaying) {
            if (ImGui::Button("Pause", ImVec2(50, 0))) {
                windowPause();
            }
        } else {
            if (ImGui::Button("Play >", ImVec2(50, 0))) {
                windowPlay();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Stop", ImVec2(50, 0))) {
            windowStop(0);
        }

        ImGui::SameLine();
        if (ImGui::Button("Fwd >>", ImVec2(60, 0))) {
            windowFastForward();
        }

        if (!isLoaded) ImGui::EndDisabled();

        /* Speed slider - same line as buttons */
        ImGui::SameLine(0, 15);
        ImGui::Text("Speed:");
        ImGui::SameLine();

        int spd = (int)speed;
        if (spd < 1) spd = 1;
        if (spd > 9) spd = 9;

        ImGui::PushItemWidth(100);
        if (ImGui::SliderInt("##speed", &spd, 1, 9, speed_labels[spd - 1])) {
            update_speed_from_slider(spd);
        }
        ImGui::PopItemWidth();

        /* Seek slider row */
        if (isLoaded) {
            size_t currentPos = 0, totalSize = 0;
            uint32_t currentTime = 0;
            screenGetLogProgress(&currentPos, &totalSize, &currentTime);

            /* Calculate current ratio for display */
            float displayRatio = 0.0f;
            if (totalSize > 0) {
                displayRatio = (float)currentPos / (float)totalSize;
            }

            /* Only update slider position when not actively seeking */
            if (!s_is_seeking) {
                s_seek_ratio = displayRatio;
            }

            /* Current time */
            format_time(s_current_time, sizeof(s_current_time), currentTime);

            /* Estimate total time from byte position ratio */
            char total_time_str[32] = "--:--";
            char remaining_str[32] = "--:--";
            if (displayRatio > 0.01f && currentTime > 0) {
                uint32_t estimatedTotal = (uint32_t)((float)currentTime / displayRatio);
                uint32_t remaining = estimatedTotal > currentTime ? estimatedTotal - currentTime : 0;
                format_time(total_time_str, sizeof(total_time_str), estimatedTotal);
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
                unsigned char wasPlaying = playIsPlaying;
                if (wasPlaying) {
                    windowPause();
                }
                clientMutexWaitFor();
                drawDirtyScreen();
                screenSeekToPosition(s_seek_ratio);
                drawDirtyScreen();
                clientMutexRelease();
                windowNeedRedraw();
                if (wasPlaying) {
                    windowPlay();
                }
            }

            ImGui::PopItemWidth();

            /* Time display row */
            ImGui::Text("%s / %s", s_current_time, total_time_str);
            ImGui::SameLine(0, 15);
            ImGui::TextDisabled("-%s remaining", remaining_str);
        } else {
            ImGui::Text("No log loaded");
        }
    }
    ImGui::End();
}
