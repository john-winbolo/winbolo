/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          imgui_touch.cpp
 * Purpose:       iOS touch handling for every ImGui context.
 *                See imgui_touch.h.
 *********************************************************/

#if defined(__IPHONEOS__) || defined(__ANDROID__)

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

#if defined(__ANDROID__)
/* The Back key hides the Android keyboard and leaves SDL's text input on,
 * so a field focused after that would get no keyboard: the backend starts
 * text input only when none is active. Seen per frame (touchKeyboardLift),
 * acted on in the handler below. */
static bool s_keyboardWasUp = false;      /* up since this text input began */
static bool s_keyboardDismissed = false;  /* then hidden with the input still on */
#endif

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
#if defined(__ANDROID__)
        /* A keyboard the Back key hid, with the text input still on: a tap
         * on a field asks for it again. Only a tap: the window moving back
         * when the keyboard went changes the field's position and brings
         * this handler here too, and a request made during the hide is
         * dropped. Ending and starting the text input instead posts a hide
         * and a show together, which Android drops as well. */
        if (SDL_TextInputActive(window) && s_keyboardDismissed &&
            ImGui::GetIO().MouseClicked[0]) {
            androidDeviceShowKeyboard();
            s_keyboardDismissed = false;
            s_keyboardWasUp = false;
        }
#endif
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

#if defined(__ANDROID__)
/* ---- The field above the keyboard ----
 *
 * On iOS SDL lifts its view when the keyboard would cover the text input
 * area (dialogResetTextInputArea leaves the area set for a lower field).
 * On Android nothing is lifted: a full-screen window is neither panned nor
 * resized for the keyboard, which simply covers the lower part of it. So
 * the window holding the field being typed in is moved up by what the
 * keyboard covers of the field, and moved back when the keyboard goes or
 * the typing ends. The keyboard's height comes from the driver in window
 * pixels; ImGui's units are the renderer's when a logical presentation is
 * set, so it is scaled through the display size. */

/* The window moved up, by ID, how far, and the top it was moved to, so it
 * can be put back. A window whose position is set every frame is back at
 * its own place by the next frame, and its top no longer matches; it then
 * has nothing to put back. */
static ImGuiID s_liftWindowId = 0;
static float   s_liftAmount = 0.0f;
static float   s_liftTop = 0.0f;

static void touchLiftRestore(ImGuiWindow *w) {
    if (w != nullptr && s_liftAmount > 0.0f && w->Pos.y == s_liftTop) {
        ImGui::SetWindowPos(w, ImVec2(w->Pos.x, w->Pos.y + s_liftAmount),
                            ImGuiCond_Always);
    }
    s_liftWindowId = 0;
    s_liftAmount = 0.0f;
}

static void touchKeyboardLift(void) {
    ImGuiContext &g = *GImGui;
    ImGuiWindow *lifted =
        (s_liftWindowId != 0) ? ImGui::FindWindowByID(s_liftWindowId) : nullptr;
    if (s_liftWindowId != 0 && lifted == nullptr) {
        /* The window closed mid-lift; nothing to put back. */
        s_liftWindowId = 0;
        s_liftAmount = 0.0f;
    }

    /* The keyboard's height: 0 while it is down, below 0 where Android does
     * not report it (before Android 11), in which case nothing here moves. */
    SDL_Window *window = SDL_GetKeyboardFocus();
    const int keyboardPx = androidDeviceKeyboardHeight();
    if (window == nullptr || !SDL_TextInputActive(window)) {
        s_keyboardWasUp = false;
        s_keyboardDismissed = false;
    } else if (keyboardPx > 0) {
        s_keyboardWasUp = true;
        s_keyboardDismissed = false;
    } else if (keyboardPx == 0 && s_keyboardWasUp) {
        s_keyboardDismissed = true;
    }

    /* The lift is undone when the keyboard is down or the text input has
     * ended, not when the field loses the active ID: a finger pressed on a
     * button in the lifted window takes the active ID, and the window has to
     * stay where it is until the finger lifts, so the release lands on that
     * button. */
    const ImGuiIO &io = g.IO;
    const bool inputOn = window != nullptr && SDL_TextInputActive(window) &&
                         io.WantTextInput;
    if (!inputOn || keyboardPx <= 0) {
        if (lifted == nullptr || !io.MouseDown[0]) {
            touchLiftRestore(lifted);
        }
        return;
    }

    const bool typing = g.PlatformImeData.WantVisible && g.ActiveId != 0 &&
                        g.ActiveIdWindow != nullptr;
    if (!typing) return;

    ImGuiWindow *root = g.ActiveIdWindow->RootWindow;
    if (lifted != nullptr && lifted != root) {
        touchLiftRestore(lifted);
    }

    int pxW = 0, pxH = 0;
    SDL_GetWindowSizeInPixels(window, &pxW, &pxH);
    if (pxH <= 0) return;
    const float keyboardTop = io.DisplaySize.y - (float)keyboardPx * io.DisplaySize.y / (float)pxH;
    const float fieldBottom = g.PlatformImeData.InputPos.y +
                              g.PlatformImeData.InputLineHeight +
                              g.Style.FramePadding.y + g.Style.ItemSpacing.y;
    const float overlap = fieldBottom - keyboardTop;
    if (overlap <= 0.0f) return;
    /* What is already applied is counted only while the window is still at
     * the top it was moved to; a window placed again by its own code since
     * then has none, so the amount stays what the window has actually been
     * moved. */
    const float applied =
        (lifted == root && s_liftWindowId == root->ID && root->Pos.y == s_liftTop)
            ? s_liftAmount
            : 0.0f;
    const float before = root->Pos.y;
    ImGui::SetWindowPos(root, ImVec2(root->Pos.x, root->Pos.y - overlap),
                        ImGuiCond_Always);
    if (root->Pos.y == before) return;
    s_liftWindowId = root->ID;
    s_liftAmount = applied + (before - root->Pos.y);
    s_liftTop = root->Pos.y;
}
#endif /* __ANDROID__ */

extern "C" void imguiTouchFrame(void) {
    if (ImGui::GetCurrentContext() == nullptr) return;
    touchInstallImeHandler();
    touchDragScroll();
#if defined(__ANDROID__)
    touchKeyboardLift();
#endif
}

#endif /* __IPHONEOS__ || __ANDROID__ */
