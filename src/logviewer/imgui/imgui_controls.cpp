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
#include "imgui_event_overlay.h"
#include "imgui_clip_gif.h"
#include "imgui.h"
#include "imgui_internal.h"  /* SetKeyOwner: the wheel over the seek bar */
#include "platform_config.h"
#include "../../gui/lang.h"
#include "../../gui/ui_mode.h"
#include "../../gui/sdl3/dialogs/imgui_dialog_utils.h"
#include "../../gui/sdl3/dialogs/clip_gif_controls.h"  /* clipGifToggleButton: the movie camera */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>

/* External functions from main.c - using C types directly */
extern "C" {
    #include "logviewer.h"
    #include "../lv_time_fmt.h"
    #include "../lv_controls_layout.h"
    #include "../lv_zoom_text.h"
    float lv_drawZoomPercent(void);
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
    void lv_screenSeekToTimeMs(uint32_t ms);
    uint32_t lv_screenWindowStartMs(void);
    uint32_t lv_screenWindowLenMs(void);
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

/* Move the world to an exact time of the log. Playback must already be
 * paused, as for seek_to_ratio. */
static void seek_to_ms(uint32_t ms) {
    lv_clientMutexWaitFor();
    lv_drawDirtyScreen();
    lv_screenSeekToTimeMs(ms);
    lv_drawDirtyScreen();
    lv_clientMutexRelease();
    lv_windowNeedRedraw();
}

/* Shift and a drag of the seek bar: playback moves one tick of the log for
 * each LV_CONTROLS_FINE_DRAG_PX_PER_TICK pixels of the mouse, so it can be
 * put on one tick (lvControlsDragTargetMs).
 *
 * The drag is then measured from an anchor, not from where the mouse is on
 * the bar: the place playback was at and the x of the mouse when Shift went
 * down, or when the drag started with Shift down. Playback does not move at
 * that moment. When Shift is let go in the middle of the drag a new anchor
 * is set there and the drag goes on from it at the bar's ordinary rate (the
 * length of the log one pixel of the bar stands for) until the button is
 * let go. The handle is by then not under the mouse, so the ordinary way,
 * the handle goes where the mouse is, would make it jump.
 *
 * While the drag is measured like this ImGui only draws the slider
 * (ImGuiSliderFlags_ReadOnly) and the value is worked out here. A drag in
 * which Shift is never down does not come here at all. The steps are
 * lvFineDragStep's (lv_controls_layout.h), which the game's replay bar uses
 * too. */
static LvFineDrag s_rel = { false, false, 0, 0.0f, 0 };
/* The pixels the handle of the seek bar can move over, from the last frame
 * the bar was drawn. */
static float s_seek_usable_px = 0.0f;

/* Before the slider is drawn: sets or moves the anchor, and puts the value
 * the drag gives in s_seek_ratio. True when the slider is to be drawn
 * read-only in this frame. */
static bool seek_rel_drag_update(ImGuiID seek_id) {
    const ImGuiIO &io = ImGui::GetIO();
    const ImGuiContext &g = *ImGui::GetCurrentContext();
    const uint32_t startMs = lv_screenWindowStartMs();
    const uint32_t lenMs = lv_screenWindowLenMs();
    const bool active = ImGui::GetActiveID() == seek_id;

    if (lenMs == 0) {
        s_rel.on = false;
        return false;
    }
    if (!active) {
        s_rel.on = false;
        /* A press on the bar with Shift down starts a fine drag: it must
         * not first put the handle where the press was. Ctrl and a press is
         * the slider's text box, left alone. */
        return io.KeyShift && !io.KeyCtrl;
    }
    if (g.ActiveIdSource != ImGuiInputSource_Mouse || ImGui::TempInputIsActive(seek_id)) {
        s_rel.on = false;
        return false;
    }
    if (io.MouseDown[0]) {
        /* The world is where the handle is: no seek is owed. */
        const bool at_world = s_is_seeking && s_seek_ratio == s_seek_applied;
        /* Where the handle is, for the first anchor (Shift down in a drag
         * that is not yet measured). The sum is lv_screenSeekToPosition's
         * own, so the anchor is the very time the drag last asked for. */
        uint32_t handleMs = 0;
        if (!s_rel.on && io.KeyShift) {
            if (s_is_seeking) {
                handleMs = startMs + (uint32_t)(s_seek_ratio * (float)lenMs);
            } else {
                handleMs = lv_screenGetTimeRunning();
                if (handleMs < startMs) handleMs = startMs;
                if (handleMs > startMs + lenMs) handleMs = startMs + lenMs;
            }
        }
        bool anchored = false;
        if (lvFineDragStep(&s_rel, io.KeyShift, io.MousePos.x, handleMs, startMs, lenMs,
                           s_seek_usable_px, &anchored)) {
            s_seek_ratio = (float)(s_rel.targetMs - startMs) / (float)lenMs;
            if (anchored && at_world) {
                /* The same time written as another fraction: still no seek
                 * owed, or the scrub would go back to where it is. */
                s_seek_applied = s_seek_ratio;
            }
        }
    }
    /* On the frame the button is let go the value stays what it was. */
    return s_rel.on;
}

/* Shift and the wheel over the seek bar, or over the lane of event markers
 * on it: one tick of the log for each notch, wheel up back in time
 * (lvControlsWheelStep). a, b: the corners of the bar and the lane. The
 * plain wheel is left to ImGui, as before.
 *
 * The seek is to an exact time, as the , and . keys make it. It is a seek
 * like any other, so it wins over a running Rewind or Fast Forward
 * animation and starts none. Play and Pause stay as they are. */
static float s_wheel_carry = 0.0f;

static void seek_wheel_step(const ImVec2 &a, const ImVec2 &b) {
    const ImGuiIO &io = ImGui::GetIO();
    if (!io.KeyShift || s_is_seeking ||
        !ImGui::IsWindowHovered() || !ImGui::IsMouseHoveringRect(a, b)) {
        s_wheel_carry = 0.0f;
        return;
    }
    /* The wheel is the seek bar's while Shift is held over it: the panel
     * does not scroll too. ImGui scrolls sideways with Shift, so both ways
     * of the wheel are taken. */
    const ImGuiID owner = ImGui::GetID("##seekwheel");
    ImGui::SetKeyOwner(ImGuiKey_MouseWheelX, owner);
    ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, owner);
    /* Some systems turn Shift and the wheel into the sideways wheel
     * themselves: the notch is in whichever of the two moved. */
    const float wheel = (io.MouseWheel != 0.0f) ? io.MouseWheel : io.MouseWheelH;
    if (wheel == 0.0f) {
        return;
    }
    const uint32_t startMs = lv_screenWindowStartMs();
    const uint32_t lenMs = lv_screenWindowLenMs();
    if (lenMs == 0) {
        return;
    }
    const LvWheelStep step = lvControlsWheelStep(wheel, &s_wheel_carry,
                                                 lv_screenGetTimeRunning(), startMs,
                                                 startMs + lenMs, 20u);
    if (!step.moved) {
        return;
    }
    const bool wasPlaying = s_lv != nullptr && s_lv->playIsPlaying != 0;
    if (wasPlaying) {
        lv_windowPause();
    }
    lv_clientMutexWaitFor();
    lv_drawDirtyScreen();
    lv_screenSeekToTimeMs(step.targetMs);
    lv_drawDirtyScreen();
    lv_clientMutexRelease();
    lv_windowNeedRedraw();
    if (wasPlaying) {
        lv_windowPlay();
    }
}

/* Forward declaration for speed update helper */
static void update_speed_from_slider(int new_speed);

/* A length of playback time as text, the way every time of the viewer is
 * written (lv_time_fmt.h). The time playback is at has its hundredths, so a
 * jump to an event shows the event's own time; the length of the log and the
 * time left are to the second. */
static void format_time(char *buf, size_t buf_size, unsigned int time_ms, bool hundredths) {
    lvTimeFormat(time_ms, hundredths ? 1 : 0, buf, buf_size);
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

/* Puts the next item at x on its line of a row whose places were worked out
 * beforehand (lv_controls_layout.h). baseX is the cursor's x at the start of
 * the row; curLine is the line the last item was on, -1 before the first. */
static void controls_place(float baseX, int *curLine, int line, float x) {
    if (line == *curLine) {
        ImGui::SameLine(baseX + x, 0.0f);
    } else {
        ImGui::SetCursorPosX(baseX + x);
        *curLine = line;
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

    /* The GIF window goes over this panel; with the panel shut it goes to
     * the bottom of the screen. Set again below while the panel shows. */
    lv_clip_gif_set_anchor(false, 0.0f, 0.0f, 0.0f, 0.0f);
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
    if (ImGui::Begin(ctrl_title, &lv_g_show_controls_window, ImGuiWindowFlags_NoCollapse)) {
        lv_imgui_keep_window_on_screen();
        {
            const ImVec2 wp = ImGui::GetWindowPos();
            const ImVec2 ws = ImGui::GetWindowSize();
            lv_clip_gif_set_anchor(true, wp.x, wp.y, wp.x + ws.x, wp.y + ws.y);
        }

        /* The top row: the playback buttons (back to the last snapshot,
         * play or pause, on to the next snapshot, and on a live feed back to
         * the live head), the speed and the "Overlay events" list. Each
         * button has a drawn icon (imgui_lv_icons.cpp) and says what it
         * does, with its key, in its tooltip. A button whose action has
         * nothing to act on is dimmed. Where each item goes is worked out
         * first, every frame, from the width of the panel
         * (lvControlsTopLayout): the buttons are twice as wide as high where
         * there is room, the row takes more lines where it must, and each
         * line is in the middle of the panel. */
        const ImGuiStyle &style = ImGui::GetStyle();
        const float btn = lv_icon_button_side();
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
        const char *speedLbl = langGetText(STR_LV_SPEED_LBL);

        LvControlsTopIn topIn;
        topIn.contentW = ImGui::GetContentRegionAvail().x;
        /* The fourth button: back to the live head on a live feed, else the
         * movie camera of recording mode where this build makes GIFs. */
        const bool recordButton = !live && lv_clip_gif_available();
        topIn.buttons = (live || recordButton) ? 4 : 3;
        topIn.btnSide = btn;
        topIn.btnGap = style.ItemInnerSpacing.x;
        topIn.labelW = ImGui::CalcTextSize(speedLbl).x + style.ItemSpacing.x;
        topIn.speedMin = 50.0f;
        topIn.speedMax = 100.0f;
        topIn.comboFullW = lv_event_overlay_combo_width(true);
        topIn.comboArrowW = lv_event_overlay_combo_width(false);
        topIn.gap = 15.0f;
        const LvControlsTop top = lvControlsTopLayout(&topIn);
        const float topX = ImGui::GetCursorPosX();
        int topLine = -1;

        controls_place(topX, &topLine, top.btnLine[0], top.btnX[0]);
        ImGui::BeginDisabled(!backKnown);
        if (lv_icon_button("rewind", LV_ICON_REWIND, btn, top.btnW)) {
            lv_windowRewind();
        }
        ImGui::EndDisabled();
        controls_jump_tip(backKnown, STR_LV_TIP_BACK_SECS, STR_LV_REWIND, nowMs - backMs,
                          "Ctrl+R", tip, sizeof(tip));
        controls_tooltip(tip, backKnown);

        /* One button for play and pause, as the game's replay has it
         * (lvPlayToggle): the pause icon while the log plays, else the play
         * icon. Pause holds playback where it is; nothing moves the
         * playhead and the log stays open. (The Stop the pause button
         * replaced closed the log; Action > Stop still does.) One ID for
         * both, so a press is not lost as the icon changes. */
        controls_place(topX, &topLine, top.btnLine[1], top.btnX[1]);
        {
            const LvPlayToggle pt = lvPlayToggle(loaded, playing, atEnd);
            ImGui::BeginDisabled(!pt.enabled);
            if (lv_icon_button("playpause", pt.pause ? LV_ICON_PAUSE : LV_ICON_PLAY,
                               btn, top.btnW)) {
                if (pt.pause) {
                    lv_windowPause();
                } else {
                    lv_windowPlay();
                }
            }
            ImGui::EndDisabled();
            char keys[64];
            snprintf(keys, sizeof(keys), "%s, %s", pt.pause ? "Ctrl+U" : "Ctrl+P",
                     langGetText(STR_OSK_SPACE));
            controls_key_tip(langGetText(pt.pause ? STR_LV_PAUSE : STR_LV_PLAY), keys, tip,
                             sizeof(tip));
            controls_tooltip(tip, pt.enabled);
        }

        controls_place(topX, &topLine, top.btnLine[2], top.btnX[2]);
        /* A live feed has no snapshot ahead to name; the button still plays
         * on to the head. */
        const bool fwdOn = fwdKnown || (loaded && live);
        ImGui::BeginDisabled(!fwdOn);
        if (lv_icon_button("forward", LV_ICON_FAST_FORWARD, btn, top.btnW)) {
            lv_windowFastForward();
        }
        ImGui::EndDisabled();
        controls_jump_tip(fwdKnown, STR_LV_TIP_FWD_SECS, STR_LV_FAST_FORWARD, fwdMs - nowMs,
                          "Ctrl+F", tip, sizeof(tip));
        controls_tooltip(tip, fwdOn);

        /* Spectator live-DVR: jump back to the live head and resume following it.
         * Shown only on the live spectator feed, not for standalone .wbv playback. */
        if (live) {
            controls_place(topX, &topLine, top.btnLine[3], top.btnX[3]);
            if (lv_icon_button("live", LV_ICON_TO_END, btn, top.btnW)) {
                lv_screenSpecJumpToLive();
            }
            controls_tooltip(langGetText(STR_LV_TIP_LIVE), true);
        }

        /* Recording mode, as the game's replay has it: the movie camera
         * turns on the GIF controls (the GIF window, the GIF's span on the
         * timeline and the crop frame on the map). Pressed while the GIF
         * window is open, and held still while a GIF is made. */
        if (recordButton) {
            controls_place(topX, &topLine, top.btnLine[3], top.btnX[3]);
            const bool canToggle = lv_clip_gif_can_toggle_mode();
            ImGui::BeginDisabled(!canToggle);
            if (clipGifToggleButton("##recmode", lv_clip_gif_tools_shown(),
                                    CLIP_GIF_GLYPH_CAMERA, NULL,
                                    ImVec2(top.btnW, btn))) {
                lv_clip_gif_toggle_mode();
            }
            ImGui::EndDisabled();
            controls_key_tip(langGetText(STR_REEL_RECORD_MODE_TIP), "Ctrl+G", tip,
                             sizeof(tip));
            controls_tooltip(tip, canToggle);
        }

        /* The speed: its label and a slider. In a panel too narrow for both
         * the label goes and is the slider's tooltip. */
        {
            if (top.label) {
                controls_place(topX, &topLine, top.speedLine, top.labelX);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(speedLbl);
            }
            controls_place(topX, &topLine, top.speedLine, top.speedX);

            int stop = speed_stop_for((int)s_lv->speed);

            ImGui::PushItemWidth(top.speedW);
            if (ImGui::SliderInt("##speed", &stop, 0, SPEED_STOP_COUNT - 1, speed_labels[stop])) {
                update_speed_from_slider(stop);
            }
            ImGui::PopItemWidth();
            if (!top.label) {
                controls_tooltip(speedLbl, true);
            }
        }

        /* "Overlay events": which kinds of event have a marker over the
         * seek bar. With its name on it where it has room; with less room
         * only its arrow, which says its name in a tooltip. */
        controls_place(topX, &topLine, top.comboLine, top.comboX);
        lv_event_overlay_combo(top.comboFull);

        /* The GIF controls of recording mode are in the GIF window
         * (lv_clip_gif_render); here only the GIF's span on the timeline. */
        const bool gifTools = lv_clip_gif_tools_shown();

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
            format_time(s_current_time, sizeof(s_current_time), currentTime, true);

            char total_time_str[32] = "--:--";
            char remaining_str[32] = "--:--";
            if (totalTime > 0) {
                uint32_t remaining = totalTime > currentTime ? totalTime - currentTime : 0;
                format_time(total_time_str, sizeof(total_time_str), totalTime, false);
                format_time(remaining_str, sizeof(remaining_str), remaining, false);
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
             * label ("12:34.56 / 51:32 (0.4%)"), not a numeric printf format.
             * Without this flag ImGui rounds the dragged value by round-tripping
             * it through seek_fmt -- which parses back to 0 -- pinning the
             * slider (and every seek) to the start of the log. */
            /* The event markers, in a lane just over the slider and as wide
             * as it. The lane is an item of its own: it takes no press that
             * is meant for the slider. */
            const ImVec2 seek_top = ImGui::GetCursorScreenPos();
            lv_event_overlay_lane();

            seek_slider_drawn = true;
            /* Shift and a drag: the value comes from seek_rel_drag_update
             * and the slider is only drawn. */
            ImGuiSliderFlags seek_flags = ImGuiSliderFlags_NoRoundToFormat;
            if (seek_rel_drag_update(ImGui::GetID("##seek"))) {
                seek_flags |= ImGuiSliderFlags_ReadOnly;
            }
            bool seek_changed = ImGui::SliderFloat("##seek", &s_seek_ratio, 0.0f, 1.0f, seek_fmt,
                                                   seek_flags);
            const ImVec2 seek_min = ImGui::GetItemRectMin();
            const ImVec2 seek_max = ImGui::GetItemRectMax();
            {
                /* As ImGui's own slider has it: two pixels at each end, and
                 * the handle is all on the bar. */
                const float bar = ImGui::GetItemRectSize().x - 4.0f;
                const float grab = ImGui::GetStyle().GrabMinSize;
                s_seek_usable_px = bar - ((grab < bar) ? grab : bar);
            }
            /* A read-only slider reports no change: a drag that is measured
             * from an anchor starts the scrub by being held. */
            if (seek_changed || (s_rel.on && ImGui::IsItemActive())) {
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
                    if (s_rel.on) {
                        seek_to_ms(s_rel.targetMs);
                    } else {
                        seek_to_ratio(s_seek_ratio);
                    }
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
                    if (s_rel.on) {
                        seek_to_ms(s_rel.targetMs);
                    } else {
                        seek_to_ratio(s_seek_ratio);
                    }
                    s_seek_applied = s_seek_ratio;
                }
                if (s_seek_was_playing) {
                    lv_windowPlay();
                }
                s_seek_was_playing = false;
            }
            if (!ImGui::IsItemActive()) {
                s_rel.on = false;
            }
            seek_wheel_step(seek_top, ImGui::GetItemRectMax());

            /* The GIF's span over the slider, after the slider's own state
             * is read above. While the slider is pressed or dragged the span
             * goes with it, as in the game's replay. */
            if (gifTools) {
                const uint32_t seekMs = (uint32_t)(s_seek_ratio * (float)totalTime);
                lv_clip_gif_seek_marker(seek_min.x, seek_min.y, seek_max.x, seek_max.y,
                                        s_is_seeking, seekMs, totalTime);
            }

            ImGui::PopItemWidth();

            /* Desktop keeps the existing time display row.  In compact
             * (tablet/Deck) mode the in-slider label above already shows
             * elapsed/total/percent, so suppress this row to avoid
             * double-display. */
            if (!(uiModeIsTablet() || uiModeIsSteamDeck())) {
                /* The bottom row: the time, the time left, the tick and
                 * the zoom of the map. Each goes on the line while it has
                 * room there, and each line is in the middle of the panel
                 * (lvControlsFlow). The zoom is the first to give way: its
                 * number alone, then nothing (lvControlsZoomForm). */
                char part[4][LANG_MSGARG_STRING_LEN * 2];
                float partW[4], partX[4];
                int partLine[4];
                int parts = 3;
                MessageArgs args = {};
                strncpy(args.string1, s_current_time, sizeof(args.string1) - 1);
                strncpy(args.string2, total_time_str, sizeof(args.string2) - 1);
                snprintf(part[0], sizeof(part[0]), "%s", langGetTextFmt(STR_LV_TIME_FMT, &args));
                MessageArgs args2 = {};
                strncpy(args2.string1, remaining_str, sizeof(args2.string1) - 1);
                snprintf(part[1], sizeof(part[1]), "%s",
                         langGetTextFmt(STR_LV_TIME_REMAINING, &args2));
                /* The tick playback is on. The server's own game tick when
                 * the recording states it (lv_screenServerTickAt), so the
                 * number is the one the server and the bots count by. An
                 * older recording has none: then 20 ms steps from the start
                 * of the log, the step of the , and . keys. */
                {
                    /* A live feed adds to the tick table on the timer
                     * thread, so the table is read under the mutex. */
                    lv_clientMutexWaitFor();
                    uint32_t ms = lv_screenGetTimeRunning();
                    uint32_t tick = lv_screenHasServerTick() ? lv_screenServerTickAt(ms)
                                                             : ms / 20u;
                    lv_clientMutexRelease();
                    snprintf(part[2], sizeof(part[2]), "%s %u",
                             langGetText(STR_DLGSYSINFO_TICK), (unsigned)tick);
                }
                for (int i = 0; i < 3; i++) {
                    partW[i] = ImGui::CalcTextSize(part[i]).x;
                }
                {
                    char pct[24];
                    lvZoomPercentText(lv_drawZoomPercent(), pct, sizeof(pct));
                    snprintf(part[3], sizeof(part[3]), "%s %s", langGetText(STR_LV_ZOOM), pct);
                    const float fullW = ImGui::CalcTextSize(part[3]).x;
                    const float shortW = ImGui::CalcTextSize(pct).x;
                    const LvZoomForm form = lvControlsZoomForm(partW, 3, fullW, shortW, 15.0f,
                                                               ImGui::GetContentRegionAvail().x);
                    if (form == LV_ZOOM_FORM_SHORT) {
                        snprintf(part[3], sizeof(part[3]), "%s", pct);
                    }
                    if (form != LV_ZOOM_FORM_NONE) {
                        partW[3] = (form == LV_ZOOM_FORM_FULL) ? fullW : shortW;
                        parts = 4;
                    }
                }
                lvControlsFlow(partW, parts, 15.0f, ImGui::GetContentRegionAvail().x, partLine,
                               partX);
                const float rowX = ImGui::GetCursorPosX();
                int rowLine = -1;
                /* A text wider than the panel goes on to a second line. */
                ImGui::PushTextWrapPos(0.0f);
                for (int i = 0; i < parts; i++) {
                    controls_place(rowX, &rowLine, partLine[i], partX[i]);
                    if (i == 1) {
                        ImGui::TextDisabled("%s", part[i]);
                    } else {
                        ImGui::TextUnformatted(part[i]);
                    }
                    /* The zoom: what the number is and what changes it. A
                     * click does nothing; the viewer has no "back to 100%"
                     * of its own (the Zoom menu has the 1x step). */
                    if (i == 3 && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                        ImGui::SetTooltip("%s", langGetText(STR_LV_ZOOM_TIP));
                    }
                }
                ImGui::PopTextWrapPos();
            }
        } else {
            ImGui::TextWrapped("%s", langGetText(STR_LV_NO_LOG_LOADED));
        }
    }
    ImGui::End();
    if (!seek_slider_drawn) {
        controls_drop_scrub();
    }
}
