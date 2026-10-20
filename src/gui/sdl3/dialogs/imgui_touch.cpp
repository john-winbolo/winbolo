/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          imgui_touch.cpp
 * Purpose:       iOS touch handling for every ImGui context.
 *                See imgui_touch.h.
 *********************************************************/

#if defined(__IPHONEOS__)

#include <math.h>
#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_touch.h"

/* ---- On-screen keyboard ----
 *
 * The ImGui SDL3 backend opens the keyboard with plain SDL_StartTextInput,
 * which on iOS capitalises the first letter and autocorrects. That changes
 * what a player types into a password, an address or a name. The backend
 * only starts text input when none is active, so starting it here first,
 * with the properties wanted, from the handler ImGui calls when a field
 * gains or loses focus, takes its place. The backend's own handler then
 * runs as before for the input area and for stopping. */

static void (*s_backendSetImeData)(ImGuiContext *ctx, ImGuiViewport *viewport,
                                   ImGuiPlatformImeData *data) = nullptr;
static bool s_passwordWanted = false;
static bool s_activeIsPassword = false;

extern "C" void imguiTouchSetPasswordInput(bool password) {
    s_passwordWanted = password;
}

static void touchSetImeData(ImGuiContext *ctx, ImGuiViewport *viewport,
                            ImGuiPlatformImeData *data) {
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window != nullptr && data != nullptr &&
        (data->WantVisible || data->WantTextInput)) {
        /* A keyboard of the other kind is already up: close it so it can be
         * opened again as the kind this field needs. */
        if (SDL_TextInputActive(window) && s_activeIsPassword != s_passwordWanted) {
            SDL_StopTextInput(window);
        }
        if (!SDL_TextInputActive(window)) {
            SDL_PropertiesID props = SDL_CreateProperties();
            if (props != 0) {
                SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER,
                                      s_passwordWanted
                                          ? SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN
                                          : SDL_TEXTINPUT_TYPE_TEXT);
                SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_CAPITALIZATION_NUMBER,
                                      SDL_CAPITALIZE_NONE);
                SDL_SetBooleanProperty(props, SDL_PROP_TEXTINPUT_AUTOCORRECT_BOOLEAN,
                                       false);
                SDL_StartTextInputWithProperties(window, props);
                SDL_DestroyProperties(props);
                s_activeIsPassword = s_passwordWanted;
            }
        }
    }
    if (s_backendSetImeData != nullptr) {
        s_backendSetImeData(ctx, viewport, data);
    }
}

static void touchInstallImeHandler(void) {
    ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
    if (pio.Platform_SetImeDataFn == touchSetImeData ||
        pio.Platform_SetImeDataFn == nullptr) {
        return;
    }
    /* Every context runs the same backend, so one saved handler serves
     * them all. */
    s_backendSetImeData = pio.Platform_SetImeDataFn;
    pio.Platform_SetImeDataFn = touchSetImeData;
}

/* ---- Drag to scroll ----
 *
 * ImGui scrolls a window with a mouse wheel or its scrollbar, and a finger
 * has neither: SDL makes a touch into a left mouse button and sends no
 * wheel events for it. Once a finger has moved further up or down than
 * across, by more than a tap's wobble, the nearest window under it that
 * can scroll follows the finger until it lifts. The widget the finger
 * landed on is released, so lifting does not click it; a tap that does not
 * move still clicks. A drag that starts on the scrollbar, in a text field
 * or as a drag and drop is left alone, and so is a mouse or trackpad. */

/* Movement, in ImGui units (points on iOS), before a press becomes a
 * scroll. */
static const float kTouchScrollThreshold = 10.0f;

/* The window being scrolled, kept by ID and looked up each frame, so a
 * window that closes mid-drag simply ends the scroll. */
static ImGuiContext *s_scrollCtx = nullptr;
static ImGuiID       s_scrollWindowId = 0;

static ImGuiWindow *touchScrollableWindowAt(ImGuiWindow *w) {
    while (w != nullptr) {
        bool canScroll = w->ScrollMax.y > 0.0f &&
                         (w->Flags & ImGuiWindowFlags_NoScrollWithMouse) == 0;
        if (canScroll) return w;
        if ((w->Flags & ImGuiWindowFlags_ChildWindow) == 0) return nullptr;
        w = w->ParentWindow;
    }
    return nullptr;
}

static void touchDragScroll(void) {
    ImGuiContext &g = *GImGui;
    ImGuiIO &io = g.IO;

    if (s_scrollCtx != &g) {
        s_scrollCtx = &g;
        s_scrollWindowId = 0;
    }
    if (!io.MouseDown[0]) {
        s_scrollWindowId = 0;
        return;
    }

    if (s_scrollWindowId == 0) {
        /* Fingers only: a mouse or trackpad on an iPad has a wheel and
         * drags to select, as on the desktop. */
        if (io.MouseSource != ImGuiMouseSource_TouchScreen) return;
        if (g.DragDropActive || g.MovingWindow != nullptr) return;
        ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
        if (fabsf(d.y) < kTouchScrollThreshold || fabsf(d.y) < fabsf(d.x)) return;
        ImGuiWindow *w = touchScrollableWindowAt(g.HoveredWindow);
        if (w == nullptr) return;
        if (g.ActiveId != 0) {
            if (g.ActiveId == ImGui::GetWindowScrollbarID(w, ImGuiAxis_Y)) return;
            if (g.InputTextState.ID == g.ActiveId) return;
            ImGui::ClearActiveID();
        }
        s_scrollWindowId = w->ID;
        /* Catch up with the distance moved before the threshold. */
        ImGui::SetScrollY(w, w->Scroll.y - d.y);
        return;
    }

    ImGuiWindow *w = ImGui::FindWindowByID(s_scrollWindowId);
    if (w == nullptr) {
        s_scrollWindowId = 0;
        return;
    }
    if (g.ActiveId != 0) ImGui::ClearActiveID();
    ImGui::SetScrollY(w, w->Scroll.y - io.MouseDelta.y);
}

extern "C" void imguiTouchFrame(void) {
    if (ImGui::GetCurrentContext() == nullptr) return;
    touchInstallImeHandler();
    touchDragScroll();
}

#endif /* __IPHONEOS__ */
