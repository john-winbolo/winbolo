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
#include "imgui_lv_icons.h"
#include "imgui_lv_panel.h"
#include "imgui_event_overlay.h"
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
    bool lv_screenRewindTargetMs(uint32_t *ms);
    bool lv_screenFastForwardTargetMs(uint32_t *ms);
    uint32_t lv_screenGetTimeRunning(void);
    uint32_t lv_screenServerTickAt(uint32_t ms);
    bool lv_screenHasServerTick(void);
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
    lv_event_overlay_init();
}

/* The height the panel gets in the dock's built layout: its tab, the button
 * row, the seek bar and the time row, and the lane of event markers over
 * the seek bar when there is one. */
#define LV_CONTROLS_BASE_H 125.0f
float lv_imgui_controls_dock_height(float dockWidth) {
    (void)dockWidth;
    return LV_CONTROLS_BASE_H + lv_event_overlay_lane_height();
}

void lv_imgui_controls_update_speed(void) {
    /* Called when speed changes externally */
}

static void update_speed_from_slider(int stop) {
    if (stop < 0) stop = 0;
    if (stop >= SPEED_STOP_COUNT) stop = SPEED_STOP_COUNT - 1;
    lv_updateSpeed(speed_stops[stop], 0);
}

/* End a scrub the seek slider can no longer end itself. The slider pauses
 * playback while its handle is held and starts it again on release. When
 * the slider is not drawn in a frame (the Controls window was closed with
 * Ctrl+1 in the middle of a drag, its dock tab went to the back, the log
 * was closed) no release is ever seen, and playback would stay paused with
 * the slider stuck on the dragged value. */
static void controls_drop_scrub(void) {
    if (!s_is_seeking) return;
    s_is_seeking = false;
    /* A log that was closed has nothing to play. */
    if (s_seek_was_playing && s_lv != NULL && s_lv->isLoaded) {
        lv_windowPlay();
    }
    s_seek_was_playing = false;
}

/* The room left on the line of the last item, after a gap. It is asked
 * before SameLine: a SameLine that is then taken back with NewLine leaves
 * the content of the window wider than what is drawn in it. */
static float controls_room_after(float gap) {
    /* The cursor is at the start of the next line, whose room is the whole
     * width of the content. */
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    return right - (ImGui::GetItemRectMax().x + gap);
}

/* The next item goes on the same line when it has room there (it is w
 * wide), and starts a new line when it has not. */
static void controls_same_line(float w, float gap) {
    if (controls_room_after(gap) >= w) {
        ImGui::SameLine(0.0f, gap);
    }
}

/* The tooltip of the last item, also while it is disabled: a dimmed button
 * still says what it is. */
static void controls_tooltip(const char *text, bool enabled) {
    if (enabled) {
        imguiHandOnHover();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip |
                             ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", text);
    }
}

/* "Play (Ctrl+P, Space)": what a button does, then its key. */
static void controls_key_tip(const char *name, const char *key, char *out, size_t outSize) {
    MessageArgs args = {};
    snprintf(args.string1, sizeof(args.string1), "%s", name);
    snprintf(args.string2, sizeof(args.string2), "%s", key);
    snprintf(out, outSize, "%s", langGetTextFmt(STR_LV_TIP_KEY_FMT, &args));
}

/* The seconds a jump of ms moves the playhead, as the tooltips say it: one
 * decimal under ten seconds, whole seconds from there. */
static void controls_format_jump(uint32_t ms, char *out, size_t outSize) {
    if (ms < 10000u) {
        snprintf(out, outSize, "%u.%u", (unsigned)(ms / 1000u), (unsigned)((ms % 1000u) / 100u));
    } else {
        snprintf(out, outSize, "%u", (unsigned)((ms + 500u) / 1000u));
    }
}

/* The tooltip of Rewind or Fast Forward. Each goes to a snapshot of the
 * log, so how far that is changes as playback moves: "Back 12 seconds
 * (Ctrl+R)", with the seconds worked out this frame from the place the
 * button would really go to (lv_screenRewindTargetMs,
 * lv_screenFastForwardTargetMs). With nowhere to go it is the plain name. */
static void controls_jump_tip(bool known, langid fmt, langid plain, uint32_t ms,
                              const char *key, char *out, size_t outSize) {
    char name[LANG_MSGARG_STRING_LEN];
    if (known) {
        MessageArgs args = {};
        controls_format_jump(ms, args.string1, sizeof(args.string1));
        snprintf(name, sizeof(name), "%s", langGetTextFmt(fmt, &args));
    } else {
        snprintf(name, sizeof(name), "%s", langGetText(plain));
    }
    controls_key_tip(name, key, out, outSize);
}

void lv_imgui_controls_window(void) {
    bool seek_slider_drawn = false;

    if (!lv_g_show_controls_window) {
        controls_drop_scrub();
        return;
    }

    {
        /* A panel that sat in the dock when the dock was turned off goes
         * back to its default place, the same as a reset. */
        bool to_default = lv_g_reset_window_positions ||
                          lv_imgui_dock_take_float_reset("###controls");
        ImGuiCond cond = to_default ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        ImVec2 vp = ImGui::GetMainViewport()->Size;
        /* Room for the lane of event markers over the seek bar. */
        const float h = 120.0f + lv_event_overlay_lane_height();
        ImGui::SetNextWindowSize(ImVec2(420, h), cond);
        ImGui::SetNextWindowPos(ImVec2((vp.x - 420) * 0.5f, vp.y - h - 10), cond);
    }

    char ctrl_title[128];
    snprintf(ctrl_title, sizeof(ctrl_title), "%s###controls", langGetText(STR_LV_WIN_CONTROLS));
    if (lv_panel_begin("###controls", ctrl_title, &lv_g_show_controls_window, ImGuiWindowFlags_NoCollapse)) {
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

        /* The playback buttons: back to the last snapshot, play, pause,
         * on to the next snapshot, and on a live feed back to the live
         * head. Each is a square with a drawn icon (imgui_lv_icons.cpp) and
         * says what it does, with its key, in its tooltip. A button whose
         * action has nothing to act on is dimmed. A button that does not go
         * on the line starts the next one, so none is cut off in a narrow
         * panel. */
        const ImGuiStyle &style = ImGui::GetStyle();
        const float btn = lv_icon_button_side();
        const float btnGap = style.ItemInnerSpacing.x;
        const bool loaded = s_lv->isLoaded != 0;
        const bool live = lv_screenSpecIsLiveMode();
        const bool playing = loaded && s_lv->playIsPlaying != 0;
        /* The decoder has read the last record: nothing is left to play. */
        const bool atEnd = loaded && !live && s_lv->isPlaying == 0;
        uint32_t nowMs = 0, backMs = 0, fwdMs = 0;
        bool backKnown = false, fwdKnown = false;
        if (loaded) {
            /* The snapshot index is written on the playback timer. */
            lv_clientMutexWaitFor();
            nowMs = lv_screenGetTimeRunning();
            backKnown = lv_screenRewindTargetMs(&backMs) && backMs < nowMs;
            fwdKnown = lv_screenFastForwardTargetMs(&fwdMs) && fwdMs > nowMs;
            lv_clientMutexRelease();
        }
        char tip[LANG_MSGARG_STRING_LEN * 2 + 16];

        ImGui::BeginDisabled(!backKnown);
        if (lv_icon_button("rewind", LV_ICON_REWIND)) {
            lv_windowRewind();
        }
        ImGui::EndDisabled();
        controls_jump_tip(backKnown, STR_LV_TIP_BACK_SECS, STR_LV_REWIND, nowMs - backMs,
                          "Ctrl+R", tip, sizeof(tip));
        controls_tooltip(tip, backKnown);

        controls_same_line(btn, btnGap);
        ImGui::BeginDisabled(!loaded || playing || atEnd);
        if (lv_icon_button("play", LV_ICON_PLAY)) {
            lv_windowPlay();
        }
        ImGui::EndDisabled();
        controls_key_tip(langGetText(STR_LV_PLAY), "Ctrl+P, Space", tip, sizeof(tip));
        controls_tooltip(tip, loaded && !playing && !atEnd);

        /* Pause holds playback where it is; nothing moves the playhead and
         * the log stays open. (The Stop this button replaced closed the
         * log; Action > Stop still does.) */
        controls_same_line(btn, btnGap);
        ImGui::BeginDisabled(!playing);
        if (lv_icon_button("pause", LV_ICON_PAUSE)) {
            lv_windowPause();
        }
        ImGui::EndDisabled();
        controls_key_tip(langGetText(STR_LV_PAUSE), "Ctrl+U, Space", tip, sizeof(tip));
        controls_tooltip(tip, playing);

        controls_same_line(btn, btnGap);
        /* A live feed has no snapshot ahead to name; the button still plays
         * on to the head. */
        const bool fwdOn = fwdKnown || (loaded && live);
        ImGui::BeginDisabled(!fwdOn);
        if (lv_icon_button("forward", LV_ICON_FAST_FORWARD)) {
            lv_windowFastForward();
        }
        ImGui::EndDisabled();
        controls_jump_tip(fwdKnown, STR_LV_TIP_FWD_SECS, STR_LV_FAST_FORWARD, fwdMs - nowMs,
                          "Ctrl+F", tip, sizeof(tip));
        controls_tooltip(tip, fwdOn);

        /* Spectator live-DVR: jump back to the live head and resume following it.
         * Shown only on the live spectator feed, not for standalone .wbv playback. */
        if (live) {
            controls_same_line(btn, btnGap);
            if (lv_icon_button("live", LV_ICON_TO_END)) {
                lv_screenSpecJumpToLive();
            }
            controls_tooltip(langGetText(STR_LV_TIP_LIVE), true);
        }

        /* The speed: its label and a slider, after the buttons where there
         * is room. The slider gives up width first, down to half. Under
         * that the two go on a line of their own, and in a panel too narrow
         * for both the label goes and the slider keeps the line; the label
         * is then the slider's tooltip. */
        const float overlayGap = 15.0f;
        {
            const char *speedLbl = langGetText(STR_LV_SPEED_LBL);
            const float labelW = ImGui::CalcTextSize(speedLbl).x + style.ItemSpacing.x;
            const float speedMax = 100.0f, speedMin = 50.0f;
            /* The arrow of the "Overlay events" list goes after the slider
             * when the slider can spare it the room. */
            const float spare = overlayGap + lv_event_overlay_combo_width(false);
            bool label = true;
            float room = controls_room_after(15.0f) - labelW;
            if (room >= speedMin) {
                ImGui::SameLine(0, 15);
            } else {
                room = ImGui::GetContentRegionAvail().x - labelW;
                if (room < speedMin) {
                    label = false;
                    room = ImGui::GetContentRegionAvail().x;
                }
            }
            if (room - spare >= speedMin) {
                room -= spare;
            }
            float speedW = (room < speedMax) ? room : speedMax;
            if (speedW < 16.0f) speedW = 16.0f;
            if (label) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(speedLbl);
                ImGui::SameLine();
            }

            int stop = speed_stop_for((int)s_lv->speed);

            ImGui::PushItemWidth(speedW);
            if (ImGui::SliderInt("##speed", &stop, 0, SPEED_STOP_COUNT - 1, speed_labels[stop])) {
                update_speed_from_slider(stop);
            }
            ImGui::PopItemWidth();
            if (!label) {
                controls_tooltip(speedLbl, true);
            }
        }

        /* "Overlay events": which kinds of event have a marker over the
         * seek bar. After the speed where it has room with its name on it;
         * with less room only its arrow, which says its name in a tooltip;
         * and on a line of its own when not even that goes on the line. */
        {
            const float gap = overlayGap;
            const float room = controls_room_after(gap);
            if (room >= lv_event_overlay_combo_width(true)) {
                ImGui::SameLine(0, gap);
                lv_event_overlay_combo(true);
            } else if (room >= lv_event_overlay_combo_width(false)) {
                ImGui::SameLine(0, gap);
                lv_event_overlay_combo(false);
            } else {
                lv_event_overlay_combo(ImGui::GetContentRegionAvail().x >=
                                       lv_event_overlay_combo_width(true));
            }
        }

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
            /* The event markers, in a lane just over the slider and as wide
             * as it. The lane is an item of its own: it takes no press that
             * is meant for the slider. */
            lv_event_overlay_lane();

            seek_slider_drawn = true;
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
                /* Only when the handle was dropped somewhere the last live
                 * apply did not reach (a backward step inside the throttle
                 * time). A seek to where the world already is would be a
                 * backward one to the decoder, a snapshot restore and a
                 * decode of the whole round up to here, for no change. */
                if (s_seek_ratio != s_seek_applied) {
                    seek_to_ratio(s_seek_ratio);
                    s_seek_applied = s_seek_ratio;
                }
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
                /* A text wider than the panel goes on to a second line. */
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(langGetTextFmt(STR_LV_TIME_FMT, &args));
                MessageArgs args2 = {};
                strncpy(args2.string1, remaining_str, sizeof(args2.string1) - 1);
                /* Each part goes on the line while it has room there. */
                char remaining[LANG_MSGARG_STRING_LEN];
                snprintf(remaining, sizeof(remaining), "%s",
                         langGetTextFmt(STR_LV_TIME_REMAINING, &args2));
                controls_same_line(ImGui::CalcTextSize(remaining).x, 15.0f);
                ImGui::TextDisabled("%s", remaining);
                /* The tick playback is on. The server's own game tick when
                 * the recording states it (lv_screenServerTickAt), so the
                 * number is the one the server and the bots count by. An
                 * older recording has none: then 20 ms steps from the start
                 * of the log, the step of the , and . keys. */
                {
                    uint32_t ms = lv_screenGetTimeRunning();
                    uint32_t tick = lv_screenHasServerTick() ? lv_screenServerTickAt(ms)
                                                             : ms / 20u;
                    char tickText[64];
                    snprintf(tickText, sizeof(tickText), "%s %u",
                             langGetText(STR_DLGSYSINFO_TICK), (unsigned)tick);
                    controls_same_line(ImGui::CalcTextSize(tickText).x, 15.0f);
                    ImGui::TextUnformatted(tickText);
                }
                ImGui::PopTextWrapPos();
            }
        } else {
            ImGui::TextWrapped("%s", langGetText(STR_LV_NO_LOG_LOADED));
        }
    }
    lv_panel_end();
    if (!seek_slider_drawn) {
        controls_drop_scrub();
    }
}
