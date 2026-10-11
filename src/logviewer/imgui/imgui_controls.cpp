/*
 * imgui_controls.cpp - ImGui playback controls window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "imgui_controls.h"
#include "imgui_main_menu.h"
#include "imgui_dock.h"
#include "imgui_context.h"
#include "imgui.h"
#include "platform_config.h"
#include "../../gui/lang.h"
#include "../../gui/ui_mode.h"
#include "../../gui/sdl3/dialogs/imgui_dialog_utils.h"

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
    bool lv_screenSpecIsLiveMode(void);
    void lv_screenSpecJumpToLive(void);
    void lv_clientMutexWaitFor(void);
    void lv_clientMutexRelease(void);
    void lv_drawDirtyScreen(void);
}

static LogViewerState *s_lv = nullptr;

/* Speed labels for display */
/* The speed slider's stops from slowest to fastest: the speed value each one
 * sets, and its label. The slow values are numbered after the fast ones (see
 * logviewer.h), so the slider goes by this table, not by the value. */
static const unsigned char speed_stops[] = {
    LV_SPEED_TENTH, LV_SPEED_QUARTER, LV_SPEED_HALF, 1, 2, 3, 4, 5, 6, 7, 8, 9
};
static const char* speed_labels[] = {
    "0.1x", "0.25x", "0.5x",
    "1x", "1.2x", "1.4x", "1.6x", "1.8x", "2x", "2.2x", "2.4x", "Fast"
};
#define SPEED_STOP_COUNT ((int)(sizeof(speed_stops) / sizeof(speed_stops[0])))
#define SPEED_STOP_NORMAL 3

/* The slider stop that sets the given speed value; 1x when there is none. */
static int speed_stop_for(int speed) {
    for (int i = 0; i < SPEED_STOP_COUNT; i++) {
        if (speed_stops[i] == speed) return i;
    }
    return SPEED_STOP_NORMAL;
}

/* Current time string buffer */
static char s_current_time[32] = "00:00";

/* Seek slider state */
static float s_seek_ratio = 0.0f;
static bool s_is_seeking = false;
/* Live scrub: the ratio the world was last moved to, when that was, and
 * whether playback has to resume once the handle is dropped. */
static float s_seek_applied = 0.0f;
static double s_seek_applied_at = 0.0;
static bool s_seek_was_playing = false;

/* Least time between two backward seeks while the handle is dragged. Same
 * value as the recap reel's RECAP_SCRUB_BACK_MS. */
#define LV_SCRUB_BACK_SECS 0.090

/* Move the world to a slider ratio. Playback must already be paused: the
 * seek walks the log on this thread while lv_windowTimer ticks it on the
 * timer thread. */
static void seek_to_ratio(float ratio) {
    lv_clientMutexWaitFor();
    lv_drawDirtyScreen();
    lv_screenSeekToPosition(ratio);
    lv_drawDirtyScreen();
    lv_clientMutexRelease();
    lv_windowNeedRedraw();
}

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
    if (spd < 1 || spd > LV_SPEED_MAX) {
        spd = 1;
    }
    lv_updateSpeed((unsigned char)spd, 0);
}

void lv_imgui_controls_update_speed(void) {
    /* Called when speed changes externally */
}

static void update_speed_from_slider(int stop) {
    if (stop < 0) stop = 0;
    if (stop >= SPEED_STOP_COUNT) stop = SPEED_STOP_COUNT - 1;
    lv_updateSpeed(speed_stops[stop], 0);
}

void lv_imgui_controls_window(void) {
    if (!lv_g_show_controls_window) {
        return;
    }

    {
        /* A panel that sat in the dock when the dock was turned off goes
         * back to its default place, the same as a reset. */
        bool to_default = lv_g_reset_window_positions ||
                          lv_imgui_dock_take_float_reset("###controls");
        ImGuiCond cond = to_default ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        ImVec2 vp = ImGui::GetMainViewport()->Size;
        ImGui::SetNextWindowSize(ImVec2(420, 120), cond);
        ImGui::SetNextWindowPos(ImVec2((vp.x - 420) * 0.5f, vp.y - 120 - 10), cond);
    }

    char ctrl_title[128];
    snprintf(ctrl_title, sizeof(ctrl_title), "%s###controls", langGetText(STR_LV_WIN_CONTROLS));
    if (ImGui::Begin(ctrl_title, &lv_g_show_controls_window, ImGuiWindowFlags_NoCollapse)) {
        /* In the dock the dock places the window; moving it here would pull
         * it straight back out. */
        if (!ImGui::IsWindowDocked()) {
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
        }

        /* Playback buttons row */
        bool controlsDisabled = !s_lv->isLoaded;
        if (controlsDisabled) ImGui::BeginDisabled();

        if (ImGui::Button(langGetText(STR_LV_REW_BTN), ImVec2(60, 0))) {
            lv_windowRewind();
        }
        imguiHandOnHover();
        ImGui::SameLine();

        /* Play/Pause toggle button */
        if (s_lv->playIsPlaying) {
            if (ImGui::Button(langGetText(STR_LV_PAUSE), ImVec2(50, 0))) {
                lv_windowPause();
            }
            imguiHandOnHover();
        } else {
            if (ImGui::Button(langGetText(STR_LV_PLAY_BTN), ImVec2(50, 0))) {
                lv_windowPlay();
            }
            imguiHandOnHover();
        }

        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_LV_STOP), ImVec2(50, 0))) {
            lv_windowStop(0);
        }
        imguiHandOnHover();

        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_LV_FWD_BTN), ImVec2(60, 0))) {
            lv_windowFastForward();
        }
        imguiHandOnHover();

        if (controlsDisabled) ImGui::EndDisabled();

        /* Spectator live-DVR: jump back to the live head and resume following it.
         * Shown only on the live spectator feed, not for standalone .wbv playback. */
        if (lv_screenSpecIsLiveMode()) {
            ImGui::SameLine();
            if (ImGui::Button("Live", ImVec2(50, 0))) {
                lv_screenSpecJumpToLive();
            }
            imguiHandOnHover();
        }

        /* Speed slider - same line as buttons */
        ImGui::SameLine(0, 15);
        /* In the dock the panel is as narrow as the dock is. The slider gives
         * up width first, down to half; under that the speed row goes below
         * the buttons instead of being cut off at the edge. */
        float speedW = 100.0f;
        if (ImGui::IsWindowDocked()) {
            float labelW = ImGui::CalcTextSize(langGetText(STR_LV_SPEED_LBL)).x +
                           ImGui::GetStyle().ItemSpacing.x;
            float room = ImGui::GetContentRegionAvail().x - labelW;
            if (room < 50.0f) {
                ImGui::NewLine();
                room = ImGui::GetContentRegionAvail().x - labelW;
            }
            if (room < speedW) speedW = (room > 50.0f) ? room : 50.0f;
        }
        ImGui::TextUnformatted(langGetText(STR_LV_SPEED_LBL));
        ImGui::SameLine();

        int stop = speed_stop_for((int)s_lv->speed);

        ImGui::PushItemWidth(speedW);
        if (ImGui::SliderInt("##speed", &stop, 0, SPEED_STOP_COUNT - 1, speed_labels[stop])) {
            update_speed_from_slider(stop);
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
                /* Idle: the applied value is wherever the log actually is,
                 * so the next drag measures its first step from the truth. */
                s_seek_applied = displayRatio;
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

            /* Seek slider - full width.  Always-visible in-slider time
             * label so controller / touch users can read progress without
             * a hover tooltip. */
            ImGui::PushItemWidth(-1);

            char seek_fmt[64];
            if (totalTime > 0) {
                snprintf(seek_fmt, sizeof(seek_fmt), "%s / %s  (%.1f%%)",
                         s_current_time, total_time_str, displayRatio * 100.0f);
            } else {
                snprintf(seek_fmt, sizeof(seek_fmt), "%s", s_current_time);
            }
            /* NoRoundToFormat is essential here: seek_fmt is a pre-rendered
             * label ("12:34 / 51:32 (0.4%)"), not a numeric printf format.
             * Without this flag ImGui rounds the dragged value by round-tripping
             * it through seek_fmt -- which parses back to 0 -- pinning the
             * slider (and every seek) to the start of the log. */
            if (ImGui::SliderFloat("##seek", &s_seek_ratio, 0.0f, 1.0f, seek_fmt,
                                   ImGuiSliderFlags_NoRoundToFormat)) {
                if (!s_is_seeking) {
                    /* Freeze playback for the whole scrub rather than
                     * pausing and resuming around every applied seek: that
                     * removes and re-adds the SDL playback timers. */
                    s_is_seeking = true;
                    s_seek_was_playing = s_lv->playIsPlaying != 0;
                    if (s_seek_was_playing) {
                        lv_windowPause();
                    }
                    s_seek_applied_at = 0.0;
                }
            }

            /* Live scrub: the world follows the handle as it is dragged, not
             * only when it is dropped. A forward seek continues the decode
             * from where it stands, so it costs only the ticks the handle
             * crossed and runs every frame. A backward seek has to restore a
             * snapshot and decode forward again, so those are throttled and
             * only the newest one in a burst is paid for. */
            if (s_is_seeking && ImGui::IsItemActive()) {
                double now = ImGui::GetTime();
                bool apply = false;
                if (s_seek_ratio > s_seek_applied) {
                    apply = true;
                } else if (s_seek_ratio < s_seek_applied) {
                    apply = (now - s_seek_applied_at >= LV_SCRUB_BACK_SECS);
                }
                if (apply) {
                    seek_to_ratio(s_seek_ratio);
                    s_seek_applied = s_seek_ratio;
                    s_seek_applied_at = now;
                }
            }

            /* IsItemDeactivated, not ...AfterEdit: this also has to resume
             * playback, and a release ImGui does not count as an edit would
             * otherwise leave the log paused. The seek lands exactly on the
             * dropped value; it decodes nothing when the last live apply
             * already got there. */
            if (s_is_seeking && ImGui::IsItemDeactivated()) {
                s_is_seeking = false;
                seek_to_ratio(s_seek_ratio);
                s_seek_applied = s_seek_ratio;
                if (s_seek_was_playing) {
                    lv_windowPlay();
                }
                s_seek_was_playing = false;
            }

            ImGui::PopItemWidth();

            /* Desktop keeps the existing time display row.  In compact
             * (tablet/Deck) mode the in-slider label above already shows
             * elapsed/total/percent, so suppress this row to avoid
             * double-display. */
            if (!(uiModeIsTablet() || uiModeIsSteamDeck())) {
                MessageArgs args = {};
                strncpy(args.string1, s_current_time, sizeof(args.string1) - 1);
                strncpy(args.string2, total_time_str, sizeof(args.string2) - 1);
                ImGui::TextUnformatted(langGetTextFmt(STR_LV_TIME_FMT, &args));
                ImGui::SameLine(0, 15);
                MessageArgs args2 = {};
                strncpy(args2.string1, remaining_str, sizeof(args2.string1) - 1);
                ImGui::TextDisabled("%s", langGetTextFmt(STR_LV_TIME_REMAINING, &args2));
            }
        } else {
            ImGui::TextUnformatted(langGetText(STR_LV_NO_LOG_LOADED));
        }
    }
    ImGui::End();
}
