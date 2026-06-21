/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_controller_disconnect.cpp
 * Purpose:       "Controller Disconnected" dialog.  Shown
 *                when the active gamepad drops while the
 *                game is in controller mode.  The only ways
 *                out are reconnecting a controller (caller
 *                requests close) or pressing "Continue with
 *                keyboard and mouse", which turns controller
 *                mode off so the menu bar returns.
 *********************************************************/

#include <cfloat>                 /* FLT_MIN (full-width button) */

#include "imgui.h"
#include "imgui_controller_disconnect.h"
#include "imgui_dialog_utils.h"   /* imguiLoadSvgIcon */

extern "C" {
#include "../sdl3draw.h"          /* sdl3DrawGetRenderer */
#include "../../ui_mode.h"
#include "../../gamefront.h"
#include "../../lang.h"
}

static const int ICON_SIZE = 48;

static bool         s_open        = false;
static bool         s_pendingOpen = false;
static bool         s_pendingClose = false;
static bool         s_focusBtn    = false;
/* Loaded lazily on first render (renderer may not exist when Open is
   requested); kept for the lifetime of the process. */
static SDL_Texture *s_icon        = nullptr;

void controllerDisconnectOpen(void) {
    s_pendingOpen  = true;
    s_pendingClose = false;
}

void controllerDisconnectClose(void) {
    s_pendingOpen = false;
    if (s_open) s_pendingClose = true;   /* processed inside the popup scope */
}

bool controllerDisconnectIsOpen(void) {
    return s_open || s_pendingOpen;
}

bool controllerDisconnectRender(void) {
    if (s_pendingOpen) {
        ImGui::OpenPopup(langGetText(STR_CTRL_DISC_TITLE));
        ImGui::SetNavCursorVisible(true);
        s_pendingOpen  = false;
        s_pendingClose = false;
        s_open         = true;
        s_focusBtn     = true;
    }
    if (!s_open) return false;

    if (!s_icon) {
        SDL_Renderer *r = sdl3DrawGetRenderer();
        if (r) s_icon = imguiLoadSvgIcon(r, "data/ui/controller-disconnect.svg", ICON_SIZE);
    }

    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                                   vp->Pos.y + vp->Size.y * 0.5f),
                            ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_AlwaysAutoResize;

    bool keyboardChosen = false;

    /* No &p_open / no B / no Escape: the controller is gone, so the only
       deliberate dismissals are reconnecting one (s_pendingClose) or
       choosing keyboard. */
    if (ImGui::BeginPopupModal(langGetText(STR_CTRL_DISC_TITLE), nullptr, flags)) {
        /* Reconnect requested close while the popup was on the stack. */
        if (s_pendingClose) {
            ImGui::CloseCurrentPopup();
            s_open         = false;
            s_pendingClose = false;
            ImGui::EndPopup();
            return false;
        }

        const float wrap = 340.0f;

        if (s_icon) {
            ImGui::Image((ImTextureID)s_icon, ImVec2((float)ICON_SIZE, (float)ICON_SIZE));
            ImGui::SameLine();
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::TextUnformatted(langGetText(STR_CTRL_DISC_MESSAGE));
        ImGui::PopTextWrapPos();

        ImGui::Spacing();
        ImGui::Spacing();

        ImGuiStyle &style = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(style.FramePadding.x, 8.0f));
        if (s_focusBtn) {
            ImGui::SetKeyboardFocusHere();
            s_focusBtn = false;
        }
        if (ImGui::Button(langGetText(STR_CTRL_DISC_BUTTON), ImVec2(-FLT_MIN, 0.0f))) {
            /* Switch to keyboard/mouse: turning controller mode off makes the
               menu bar return.  Persist the choice so a later reconnect does
               not silently drop back into controller mode. */
            uiControllerModeSet(CONTROLLER_MODE_OFF);
            gameFrontSaveCurrentPrefs();
            keyboardChosen = true;
            ImGui::CloseCurrentPopup();
            s_open = false;
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    } else {
        s_open = false;
    }

    return keyboardChosen;
}
